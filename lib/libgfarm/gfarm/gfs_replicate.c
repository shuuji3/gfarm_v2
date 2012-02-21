/*
 * $Id$
 */

#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>

#define GFARM_INTERNAL_USE
#include <gfarm/gfarm.h>

#include "config.h"
#include "host.h"
#include "gfm_client.h"
#include "gfs_client.h"
#include "lookup.h"
#include "schedule.h"
#include "gfs_misc.h"

/*#define V2_4 1*/

struct gfm_replicate_file_from_to_closure {
	const char *srchost;
	const char *dsthost;
	int flags;
};

static gfarm_error_t
gfm_replicate_file_from_to_request(struct gfm_connection *gfm_server,
	void *closure)
{
	struct gfm_replicate_file_from_to_closure *c = closure;
	gfarm_error_t e = gfm_client_replicate_file_from_to_request(
	    gfm_server, c->srchost, c->dsthost, c->flags);

	if (e != GFARM_ERR_NO_ERROR)
		gflog_warning(GFARM_MSG_1001386,
		    "replicate_file_from_to request: %s",
		    gfarm_error_string(e));
	return (e);
}

static gfarm_error_t
gfm_replicate_file_from_to_result(struct gfm_connection *gfm_server,
	void *closure)
{
	gfarm_error_t e = gfm_client_replicate_file_from_to_result(gfm_server);

#if 0 /* DEBUG */
	if (e != GFARM_ERR_NO_ERROR)
		gflog_debug(GFARM_MSG_1001387,
		    "replicate_file_from_to result; %s",
		    gfarm_error_string(e));
#endif
	return (e);
}

gfarm_error_t
gfs_replicate_file_from_to_request(
	const char *file, const char *srchost, const char *dsthost, int flags)
{
	gfarm_error_t e;
	struct gfm_replicate_file_from_to_closure closure;

	if ((flags & GFS_REPLICATE_FILE_WAIT) != 0)
		return (GFARM_ERR_FUNCTION_NOT_IMPLEMENTED);

	closure.srchost = srchost;
	closure.dsthost = dsthost;
	closure.flags = (flags & ~GFS_REPLICATE_FILE_MIGRATE);
	e = gfm_inode_op(file, GFARM_FILE_LOOKUP,
	    gfm_replicate_file_from_to_request,
	    gfm_replicate_file_from_to_result,
	    gfm_inode_success_op_connection_free,
	    NULL,
	    &closure);

	/*
	 * XXX GFS_REPLICATE_FILE_MIGRATE is not implemented by gfmd for now.
	 * So, we do it by client side.
	 */
	if (e == GFARM_ERR_NO_ERROR &&
	    (flags & GFS_REPLICATE_FILE_MIGRATE) != 0)
		e = gfs_replica_remove_by_file(file, srchost);
	return (e);
}

gfarm_error_t
gfs_replicate_file_to_request(
	const char *file, const char *dsthost, int flags)
{
	char *srchost;
	int srcport;
	gfarm_error_t e, e2;
	GFS_File gf;

	e = gfs_pio_open(file, GFARM_FILE_RDONLY, &gf);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	e = gfarm_schedule_file(gf, &srchost, &srcport);
	e2 = gfs_pio_close(gf);
	if (e == GFARM_ERR_NO_ERROR) {
		e = gfs_replicate_file_from_to_request(file, srchost,
		    dsthost, flags);
		free(srchost);
	}

	return (e != GFARM_ERR_NO_ERROR ? e : e2);
}

gfarm_error_t
gfs_replicate_file_from_to(
	const char *file, const char *srchost, const char *dsthost, int flags)
{
	return (gfs_replicate_file_from_to_request(file, srchost, dsthost,
	   flags /* | GFS_REPLICATE_FILE_WAIT */));
}

gfarm_error_t
gfs_replicate_file_to(const char *file, const char *dsthost, int flags)
{
	return (gfs_replicate_file_to_request(file, dsthost,
	   flags /* | GFS_REPLICATE_FILE_WAIT */));
}


/* XXX FIXME */
static gfarm_error_t
gfs_replicate_from_to_internal(GFS_File gf, char *srchost, int srcport,
	char *dsthost, int dstport)
{
	gfarm_error_t e;
	struct gfm_connection *gfm_server = gfs_pio_metadb(gf);
	struct gfs_connection *gfs_server;
	int retry = 0;

	for (;;) {
		if ((e = gfs_client_connection_acquire_by_host(gfm_server,
		    dsthost, dstport, &gfs_server, NULL))
			!= GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001388,
				"acquirement of client connection failed: %s",
				gfarm_error_string(e));
			return (e);
		}

		if (gfs_client_pid(gfs_server) == 0)
			e = gfarm_client_process_set(gfs_server, gfm_server);
		if (e == GFARM_ERR_NO_ERROR) {
			e = gfs_client_replica_add_from(gfs_server,
			    srchost, srcport, gfs_pio_fileno(gf));
			if (gfs_client_is_connection_error(e) && ++retry<=1) {
				gfs_client_connection_free(gfs_server);
				continue;
			}
		}

		break;
	}
	gfs_client_connection_free(gfs_server);
	return (e);
}

static gfarm_error_t
gfs_replicate_to_internal(char *file, char *dsthost, int dstport, int migrate)
{
	char *srchost;
	int srcport;
	gfarm_error_t e, e2;
	GFS_File gf;

	e = gfs_pio_open(file, GFARM_FILE_RDONLY, &gf);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001389,
			"gfs_pio_open(%s) failed: %s",
			file,
			gfarm_error_string(e));
		return (e);
	}

	e = gfarm_schedule_file(gf, &srchost, &srcport);
	if (e == GFARM_ERR_NO_ERROR) {

#ifndef V2_4
		e = gfs_replicate_from_to_internal(gf, srchost, srcport,
			dsthost, dstport);
#else
		e = gfs_replicate_file_from_to(file, srchost, dsthost,
		    GFS_REPLICATE_FILE_FORCE
		    /* | GFS_REPLICATE_FILE_WAIT */ /* XXX NOTYET */);
#endif
		if (e == GFARM_ERR_NO_ERROR && migrate)
			e = gfs_replica_remove_by_file(file, srchost);
		free(srchost);
	}
	e2 = gfs_pio_close(gf);

	if (e != GFARM_ERR_NO_ERROR || e2 != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001390,
			"error occurred in gfs_replicate_to_internal(): %s",
			gfarm_error_string(e != GFARM_ERR_NO_ERROR ? e : e2));
	}

	return (e != GFARM_ERR_NO_ERROR ? e : e2);
}

gfarm_error_t
gfs_replicate_to_local(GFS_File gf, char *srchost, int srcport)
{
	gfarm_error_t e;
	struct gfm_connection *gfm_server = gfs_pio_metadb(gf);
	char *self;
	int port;

	e = gfm_host_get_canonical_self_name(gfm_server, &self, &port);
	if (e == GFARM_ERR_NO_ERROR) {
		e = gfs_replicate_from_to_internal(gf, srchost, srcport,
		    self, port);
	}

	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001391,
			"error occurred in gfs_replicate_to_local(): %s",
			gfarm_error_string(e));
	}

	return (e);
}

gfarm_error_t
gfs_replicate_to(char *file, char *dsthost, int dstport)
{
	return (gfs_replicate_to_internal(file, dsthost, dstport, 0));
}

gfarm_error_t
gfs_migrate_to(char *file, char *dsthost, int dstport)
{
	return (gfs_replicate_to_internal(file, dsthost, dstport, 1));
}

gfarm_error_t
gfs_replicate_from_to(char *file, char *srchost, int srcport,
	char *dsthost, int dstport)
{
#ifndef V2_4
	gfarm_error_t e, e2;
	GFS_File gf;

	if (srchost == NULL)
		return (gfs_replicate_to(file, dsthost, dstport));

	e = gfs_pio_open(file, GFARM_FILE_RDONLY, &gf);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001392,
			"gfs_pio_open(%s) failed: %s",
			file,
			gfarm_error_string(e));
		return (e);
	}

	e = gfs_replicate_from_to_internal(gf, srchost, srcport,
		dsthost, dstport);
	e2 = gfs_pio_close(gf);

	if (e != GFARM_ERR_NO_ERROR || e2 != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001393,
			"replication failed (%s) from (%s:%d) to (%s:%d): %s",
			file, srchost, srcport, dsthost, dstport,
			gfarm_error_string(e != GFARM_ERR_NO_ERROR ? e : e2));
	}

	return (e != GFARM_ERR_NO_ERROR ? e : e2);
#else
	return (gfs_replicate_file_from_to(file, srchost, dsthost,
	    GFS_REPLICATE_FILE_FORCE
	    /* | GFS_REPLICATE_FILE_WAIT */ /* XXX NOTYET */));

#endif
}

gfarm_error_t
gfs_migrate_from_to(char *file, char *srchost, int srcport,
	char *dsthost, int dstport)
{
	gfarm_error_t e;

	e = gfs_replicate_from_to(file, srchost, srcport, dsthost, dstport);
	return (e != GFARM_ERR_NO_ERROR ? e :
		gfs_replica_remove_by_file(file, srchost));
}
