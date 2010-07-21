/*
 * $Id$
 */

#include <stdlib.h>
#include <stdarg.h>
#include <sys/time.h>

#include <gfarm/gflog.h>
#include <gfarm/gfarm_config.h>
#include <gfarm/error.h>
#include <gfarm/gfarm_misc.h>
#include <gfarm/gfs.h>

#include "gfutil.h"

#include "config.h"
#include "gfp_xdr.h"
#include "gfs_proto.h"
#include "auth.h"
#include "host.h"
#include "peer.h"
#include "subr.h"
#include "back_channel.h"

static gfarm_error_t
gfs_client_rpc_back_channel(struct peer *peer, const char *diag, int command,
	const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;
	int errcode;
	struct gfp_xdr *conn;

	if (peer == NULL || (conn = peer_get_conn(peer)) == NULL)
		return (GFARM_ERR_SOCKET_IS_NOT_CONNECTED);

	va_start(ap, format);
	e = gfp_xdr_vrpc(conn, 0, command, &errcode, &format, &ap);
	va_end(ap);
	if (IS_CONNECTION_ERROR(e)) {
		/* back channel is disconnected */
		gflog_warning(GFARM_MSG_1000400,
		    "back channel disconnected: %s",
			      gfarm_error_string(e));
		host_peer_unset(peer_get_host(peer));
	}
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_warning(GFARM_MSG_1000401,
		    "%s: %s", diag, gfarm_error_string(e));
		peer_record_protocol_error(peer);
		return (e);
	}
	if (errcode != 0) {
		/*
		 * We just use gfarm_error_t as the errcode,
		 * Note that GFARM_ERR_NO_ERROR == 0.
		 */
		return (errcode);
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfs_client_fhremove(struct peer *peer, gfarm_ino_t ino, gfarm_uint64_t gen)
{
	return (gfs_client_rpc_back_channel(
			peer, "fhremove", GFS_PROTO_FHREMOVE,
			"ll/", ino, gen));
}

gfarm_error_t
gfs_client_status(struct peer *peer,
	double *loadavg_1min, double *loadavg_5min, double *loadavg_15min,
	gfarm_off_t *disk_used, gfarm_off_t *disk_avail)
{
	return (gfs_client_rpc_back_channel(
			peer, "status", GFS_PROTO_STATUS, "/fffll",
			loadavg_1min, loadavg_5min, loadavg_15min,
			disk_used, disk_avail));
}

void *
remover(void *arg)
{
	struct peer *peer = arg;
	struct host *host = peer_get_host(peer);
	gfarm_error_t e;
	struct timeval now;
	struct timespec timeout;

	gflog_notice(GFARM_MSG_1000402, "heartbeat interval: %d sec",
	    gfarm_metadb_heartbeat_interval);
	while (1) {
		e = host_update_status(host);
		if (peer_had_protocol_error(peer) ||
		    e != GFARM_ERR_NO_ERROR)
			break;

		/* timeout: 3 min */
		gettimeofday(&now, NULL);
		timeout.tv_sec = now.tv_sec + gfarm_metadb_heartbeat_interval;
		timeout.tv_nsec = now.tv_usec * 1000;

		e = host_remove_replica(host, &timeout);
		if (peer_had_protocol_error(peer))
			break;
		if (e == GFARM_ERR_OPERATION_TIMED_OUT ||
		    e == GFARM_ERR_NO_SUCH_FILE_OR_DIRECTORY)
			continue;
		if (e != GFARM_ERR_NO_ERROR)
			break;
	}
	gflog_warning(GFARM_MSG_1000403,
	    "remover: %s", peer_had_protocol_error(peer) ?
		"protocol error" : gfarm_error_string(e));
	host_peer_unset(host);
	giant_lock();
	/*
	 * NOTE: this shouldn't need db_begin()/db_end() at least for now,
	 * because only externalized descriptor needs the calls.
	 */
	peer_free(peer);
	giant_unlock();

	/* this return value won't be used, because this thread is detached */
	return (NULL);
}

gfarm_error_t
gfm_server_switch_back_channel(struct peer *peer, int from_client, int skip)
{
	gfarm_error_t e, e2;
	struct host *h;

#ifdef __GNUC__ /* workaround gcc warning: might be used uninitialized */
	h = NULL;
#endif
	if (skip)
		return (GFARM_ERR_NO_ERROR);

	giant_lock();
	if (from_client)
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
	else if ((h = peer_get_host(peer)) == NULL)
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
	else {
		host_peer_disconnect(h);
		host_peer_set(h, peer);
		e = GFARM_ERR_NO_ERROR;
	}
	giant_unlock();
	e2 = gfm_server_put_reply(peer, "switch_back_channel", e, "");
	if (e2 == GFARM_ERR_NO_ERROR) {
		if (debug_mode)
			gflog_debug(GFARM_MSG_1000404, "gfp_xdr_flush");
		e2 = gfp_xdr_flush(peer_get_conn(peer));
		if (e2 != GFARM_ERR_NO_ERROR)
			gflog_warning(GFARM_MSG_1000405,
			    "back channel protocol flush: %s",
			    gfarm_error_string(e2));
	}

	/* XXX FIXME - make sure there is at most one running remover thread */
	if (e == GFARM_ERR_NO_ERROR && e2 == GFARM_ERR_NO_ERROR)
		e2 = create_detached_thread(remover, peer);
	return (e2);
}
