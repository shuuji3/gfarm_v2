/*
 * $Id$
 */

#include <string.h>

#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#include <openssl/evp.h>
#include <assert.h>

#include <gfarm/gfarm.h>

#include "queue.h"

#include "config.h"
#include "gfm_client.h"
#include "gfs_client.h"
#include "gfs_io.h"
#include "gfs_pio.h"
#include "filesystem.h"
#include "gfs_failover.h"
#include "gfs_file_list.h"


static struct gfs_connection *
get_storage_context(struct gfs_file_section_context *vc)
{
	if (vc == NULL)
		return (NULL);
	return (vc->storage_context);
}

int
gfm_client_connection_should_failover(struct gfm_connection *gfm_server,
	gfarm_error_t e)
{
	return (gfarm_filesystem_has_multiple_servers(
		    gfarm_filesystem_get_by_connection(gfm_server))
		&& gfm_client_is_connection_error(e));
}

static gfarm_error_t
gfs_pio_reopen(GFS_File gf)
{
	gfarm_error_t e;
	struct gfm_connection *gfm_server;
	int fd, type;
	gfarm_ino_t ino;
	char *real_url = NULL;

	if ((e = gfm_open_fd_with_ino(gf->url, gf->open_flags,
	    &gfm_server, &fd, &type, &real_url, &ino)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "reopen operation on file descriptor for URL (%s) "
		    "failed: %s",
		    gf->url,
		    gfarm_error_string(e));
		free(real_url);
		return (e);
	} else if (type != GFS_DT_REG || ino != gf->ino) {
		e = GFARM_ERR_STALE_FILE_HANDLE;
	} else {
		gf->fd = fd;
		/* storage_context is null in scheduling */
		if (get_storage_context(gf->view_context) != NULL)
			e = (*gf->ops->view_reopen)(gf);
	}
	if (real_url) {
		free(gf->url);
		gf->url = real_url;
	}

	if (e != GFARM_ERR_NO_ERROR) {
		(void)gfm_close_fd(gfm_server, fd); /* ignore result */
		gf->fd = -1;
		gfm_client_connection_free(gfm_server);
		gf->error = e;
		gflog_debug(GFARM_MSG_UNFIXED,
		    "reopen operation on pio for URL (%s) failed: %s",
		    gf->url,
		    gfarm_error_string(e));
	}

	return (e);
}

static int
close_on_server(GFS_File gf, void *closure)
{
	gfarm_error_t e;
	struct gfs_file_section_context *vc = gf->view_context;
	struct gfs_connection *sc;

	/* new connection must be acquired. */
	assert(gf->gfm_server != (struct gfm_connection *)closure);

	gfm_client_connection_free(gf->gfm_server);

	if ((sc = get_storage_context(vc)) == NULL) {
		/* In the case of gfs_file just in scheduling */
		gf->fd = -1;
		return (1);
	}

	if ((e = gfs_client_close(sc, gf->fd)) != GFARM_ERR_NO_ERROR)
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfs_client_close: %s", gfarm_error_string(e));
	if (e == GFARM_ERR_GFMD_FAILED_OVER)
		e = GFARM_ERR_NO_ERROR;
	if (e != GFARM_ERR_NO_ERROR)
		gf->error = e;
	gf->fd = -1;

	return (1);
}

struct reset_and_reopen_info {
	struct gfm_connection *gfm_server;
	int must_retry;
};

static int
reset_and_reopen(GFS_File gf, void *closure)
{
	gfarm_error_t e = GFARM_ERR_NO_ERROR;
	struct reset_and_reopen_info *ri = closure;
	struct gfm_connection *gfm_server = ri->gfm_server;
	struct gfm_connection *gfm_server1;
	struct gfs_connection *sc;
	int fc = gfarm_filesystem_failover_count(
		gfarm_filesystem_get_by_connection(gfm_server));

	/* increment ref count of gfm_server */
	gf->gfm_server = gfm_server;

	if ((e = gfm_client_connection_acquire(gfm_client_hostname(gfm_server),
	    gfm_client_port(gfm_server), gfm_client_username(gfm_server),
	    &gfm_server1)) != GFARM_ERR_NO_ERROR) {
		gf->error = e;
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfm_client_connection_acquire: %s",
		    gfarm_error_string(e));
		return (1);
	}

	if (gfm_server != gfm_server1) {
		gfm_client_connection_free(gfm_server1);
		gflog_debug(GFARM_MSG_UNFIXED,
		    "reconnected to other gfmd or gfmd restarted");
		ri->must_retry = 1;
		return (0);
	}

	if ((sc = get_storage_context(gf->view_context)) != NULL) {
		/*
		 * pid will be 0 if gfarm_client_process_reset() resulted
		 * in failure at reset_and_reopen() previously called with
		 * the same gfs_connection.
		 */
		if (gfs_client_pid(sc) == 0) {
			gf->error = GFARM_ERR_CONNECTION_ABORTED;
			gflog_debug(GFARM_MSG_UNFIXED,
			    "%s", gfarm_error_string(gf->error));
			return (1);
		}

		/* reset pid */
		if (fc > gfs_client_connection_failover_count(sc)) {
			gfs_client_connection_set_failover_count(sc, fc);
			/*
			 * gfs_file just in scheduling is not related to
			 * gfs_server.
			 * In that case, gfarm_client_process_reset() is
			 * called in gfs_pio_open_section().
			 */
			e = gfarm_client_process_reset(sc, gfm_server);
			if (e != GFARM_ERR_NO_ERROR) {
				gf->error = e;
				gflog_debug(GFARM_MSG_UNFIXED,
				    "gfarm_client_process_reset: %s",
				    gfarm_error_string(e));
				return (1);
			}
		}
	}

	/* reopen file */
	if (gfs_pio_error(gf) != GFARM_ERR_STALE_FILE_HANDLE &&
	    (e = gfs_pio_reopen(gf)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfs_pio_reopen: %s", gfarm_error_string(e));
	}

	return (1);
}

static int
reset_and_reopen_all(struct gfm_connection *gfm_server,
	struct gfs_file_list *gfl)
{
	struct reset_and_reopen_info ri;

	ri.gfm_server = gfm_server;
	ri.must_retry = 0;

	gfs_pio_file_list_foreach(gfl, reset_and_reopen, &ri);

	return (ri.must_retry == 0);
}

static int
set_error(GFS_File gf, void *closure)
{
	gf->error = *(gfarm_error_t *)closure;
	return (1);
}

#define NUM_FAILOVER_RETRY 3

static gfarm_error_t
failover0(struct gfm_connection *gfm_server, const char *host0, int port,
	const char *user0)
{
	gfarm_error_t e;
	struct gfarm_filesystem *fs;
	struct gfs_file_list *gfl;
	char *host = NULL, *user = NULL;
	int fc, i, ok = 0;

	if (gfm_server) {
		fs = gfarm_filesystem_get_by_connection(gfm_server);
		if ((host = strdup(gfm_client_hostname(gfm_server))) ==  NULL) {
			e = GFARM_ERR_NO_MEMORY;
			gflog_debug(GFARM_MSG_UNFIXED,
			    "%s", gfarm_error_string(e));
			goto error_all;
		}
		if ((user = strdup(gfm_client_username(gfm_server))) ==  NULL) {
			e = GFARM_ERR_NO_MEMORY;
			gflog_debug(GFARM_MSG_UNFIXED,
			    "%s", gfarm_error_string(e));
			goto error_all;
		}
		port = gfm_client_port(gfm_server);
		/*
		 * This connection is already purged but ensure to be purged
		 * and force to create new connection in next connection
		 * acquirement.
		 */
		gfm_client_purge_from_cache(gfm_server);
	} else {
		fs = gfarm_filesystem_get(host, port);
		if ((host = strdup(host0)) ==  NULL) {
			e = GFARM_ERR_NO_MEMORY;
			gflog_debug(GFARM_MSG_UNFIXED,
			    "%s", gfarm_error_string(e));
			goto error_all;
		}
		if ((user = strdup(user0)) ==  NULL) {
			e = GFARM_ERR_NO_MEMORY;
			gflog_debug(GFARM_MSG_UNFIXED,
			    "%s", gfarm_error_string(e));
			goto error_all;
		}
	}
	gfl = gfarm_filesystem_opened_file_list(fs);
	fc = gfarm_filesystem_failover_count(fs);

	/* must be set failover_detected to 0 before acquire connection. */
	gfarm_filesystem_set_failover_detected(fs, 0);

	for (i = 0; i < NUM_FAILOVER_RETRY; ++i) {
		/* reconnect to gfmd */
		if ((e = gfm_client_connection_and_process_acquire(
		    host, port, user, &gfm_server)) != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_UNFIXED,
			    "gfm_client_connection_acquire failed : %s",
			    gfarm_error_string(e));
		}
		/*
		 * close fd without accessing to gfmd from client
		 * and release gfm_connection.
		 */
		gfs_pio_file_list_foreach(gfl, close_on_server, gfm_server);

		if (e != GFARM_ERR_NO_ERROR)
			goto error_all;

		gfarm_filesystem_set_failover_count(fs, fc + 1);
		/* reset processes and reopen files */
		ok = reset_and_reopen_all(gfm_server, gfl);
		gfm_client_connection_free(gfm_server);
		if (ok)
			break;
	}

	if (ok) {
		e = GFARM_ERR_NO_ERROR;
		gflog_notice(GFARM_MSG_UNFIXED,
		    "connection to metadb server was failed over successfully");
	} else {
		e = GFARM_ERR_OPERATION_TIMED_OUT;
		gflog_debug(GFARM_MSG_UNFIXED,
		    "falied to fail over: %s", gfarm_error_string(e));
	}

	free(host);
	free(user);

	return (e);

error_all:

	free(host);
	free(user);

	gfs_pio_file_list_foreach(gfl, set_error, &e);

	return (e);
}

static gfarm_error_t
failover(struct gfm_connection *gfm_server)
{
	return (failover0(gfm_server, NULL, 0, NULL));
}

gfarm_error_t
gfm_client_connection_failover_pre_connect(const char *host, int port,
	const char *user)
{
	return (failover0(NULL, host, port, user));
}

gfarm_error_t
gfm_client_connection_failover(struct gfm_connection *gfm_server)
{
	return (failover(gfm_server));
}

gfarm_error_t
gfs_pio_failover(GFS_File gf)
{
	gfarm_error_t e = failover(gf->gfm_server);

	if (e != GFARM_ERR_NO_ERROR)
		gf->error = e;
	return (e);
}

gfarm_error_t
gfm_client_rpc_with_failover(
	gfarm_error_t (*rpc_op)(struct gfm_connection **, void *),
	gfarm_error_t (*post_failover_op)(struct gfm_connection *, void *),
	void (*exit_op)(struct gfm_connection *, gfarm_error_t, void *),
	int (*must_be_warned_op)(gfarm_error_t, void *),
	void *closure)
{
	gfarm_error_t e;
	struct gfm_connection *gfm_server;
	int nretry = 1;

retry:
	gfm_server = NULL;
	e = rpc_op(&gfm_server, closure);
	if (nretry > 0 && gfm_client_connection_should_failover(
	    gfm_server, e)) {
		nretry--;
		if ((e = failover(gfm_server)) != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_UNFIXED,
			    "failover: %s", gfarm_error_string(e));
		} else if (post_failover_op &&
		    (e = post_failover_op(gfm_server, closure))
		    != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_UNFIXED,
			    "post_failover_op: %s", gfarm_error_string(e));
		} else
			goto retry;
	} else if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "rpc_op: %s",
		    gfarm_error_string(e));
		if (nretry == 0 && must_be_warned_op &&
		    must_be_warned_op(e, closure))
			gflog_warning(GFARM_MSG_UNFIXED,
			    "error ocurred at retry for the operation after "
			    "connection to metadb server was failed over, "
			    "so the operation possibly succeeded in the server."
			    " error='%s'",
			    gfarm_error_string(e));
	}
	if (exit_op)
		exit_op(gfm_server, e, closure);

	return (e);
}

struct compound_file_op_info {
	GFS_File gf;
	gfarm_error_t (*request_op)(struct gfm_connection *, void *);
	gfarm_error_t (*result_op)(struct gfm_connection *, void *);
	void (*cleanup_op)(struct gfm_connection *, void *);
	int (*must_be_warned_op)(gfarm_error_t, void *);
	void *closure;
};

static gfarm_error_t
compound_file_op_rpc(struct gfm_connection **gfm_serverp, void *closure)
{
	struct compound_file_op_info *ci = closure;

	*gfm_serverp = ci->gf->gfm_server;
	return (gfm_client_compound_fd_op(*gfm_serverp,
	    ci->gf->fd, ci->request_op, ci->result_op, ci->cleanup_op,
	    ci->closure));
}

static int
compound_file_op_must_be_warned_op(gfarm_error_t e, void *closure)
{
	struct compound_file_op_info *ci = closure;

	return (ci->must_be_warned_op ? ci->must_be_warned_op(e, ci->closure) :
	    0);
}

static gfarm_error_t
compound_file_op(GFS_File gf,
	gfarm_error_t (*request_op)(struct gfm_connection *, void *),
	gfarm_error_t (*result_op)(struct gfm_connection *, void *),
	void (*cleanup_op)(struct gfm_connection *, void *),
	int (*must_be_warned_op)(gfarm_error_t, void *),
	void *closure)
{
	struct compound_file_op_info ci = {
		gf,
		request_op, result_op, cleanup_op, must_be_warned_op,
		closure
	};

	return (gfm_client_rpc_with_failover(compound_file_op_rpc, NULL,
	    NULL, compound_file_op_must_be_warned_op, &ci));
}

gfarm_error_t
gfm_client_compound_file_op_readonly(GFS_File gf,
	gfarm_error_t (*request_op)(struct gfm_connection *, void *),
	gfarm_error_t (*result_op)(struct gfm_connection *, void *),
	void (*cleanup_op)(struct gfm_connection *, void *),
	void *closure)
{
	gfarm_error_t e = compound_file_op(gf, request_op, result_op,
	    cleanup_op, NULL, closure);

	if (e != GFARM_ERR_NO_ERROR)
		gflog_debug(GFARM_MSG_UNFIXED,
		    "compound_file_op: %s", gfarm_error_string(e));
	return (e);
}

gfarm_error_t
gfm_client_compound_file_op_modifiable(GFS_File gf,
	gfarm_error_t (*request_op)(struct gfm_connection *, void *),
	gfarm_error_t (*result_op)(struct gfm_connection *, void *),
	void (*cleanup_op)(struct gfm_connection *, void *),
	int (*must_be_warned_op)(gfarm_error_t, void *),
	void *closure)
{
	gfarm_error_t e = compound_file_op(gf, request_op, result_op,
	    cleanup_op, must_be_warned_op, closure);

	if (e != GFARM_ERR_NO_ERROR)
		gflog_debug(GFARM_MSG_UNFIXED,
		    "compound_file_op: %s", gfarm_error_string(e));
	return (e);
}
