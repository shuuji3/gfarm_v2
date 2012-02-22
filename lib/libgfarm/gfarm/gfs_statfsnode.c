/*
 * $Id$
 */

#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <gfarm/gfarm.h>

#include "config.h"
#include "gfm_client.h"
#include "gfs_client.h"
#include "lookup.h"
#include "gfs_failover.h"

gfarm_error_t
gfs_statfsnode(char *host, int port,
	gfarm_int32_t *bsize, gfarm_off_t *blocks, gfarm_off_t *bfree,
	gfarm_off_t *bavail, gfarm_off_t *files, gfarm_off_t *ffree,
	gfarm_off_t *favail)
{
	gfarm_error_t e;
	struct gfm_connection *gfm_server;
	struct gfs_connection *gfs_server;
	int nretry = 1;
	const char *path = GFARM_PATH_ROOT;

	if ((e = gfarm_url_parse_metadb(&path, &gfm_server))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002668,
		    "gfarm_url_parse_metadb failed: %s",
		    gfarm_error_string(e));
		return (e);
	}

retry:
	if ((e = gfs_client_connection_and_process_acquire(&gfm_server,
	    host, port, &gfs_server, NULL)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfs_client_connection_acquire_by_host: %s",
		    gfarm_error_string(e));
		gfm_client_connection_free(gfm_server);
		return (e);
	}

	/* "/" is actually not used */
	e = gfs_client_statfs(gfs_server, "/", bsize, blocks,
		bfree, bavail, files, ffree, favail);
	gfs_client_connection_free(gfs_server);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_UNFIXED,
		    "gfs_client_statfs: %s",
		    gfarm_error_string(e));
		if (gfs_client_is_connection_error(e) && nretry-- > 0)
			goto retry;
	}
	gfm_client_connection_free(gfm_server);

	return (e);
}
