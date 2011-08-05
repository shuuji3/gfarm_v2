#include <stdio.h>	/* config.h needs FILE */
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

#define GFARM_INTERNAL_USE
#include <gfarm/gfarm.h>

#include "gfutil.h"

#include "config.h"
#include "gfm_client.h"
#include "lookup.h"

struct gfm_utimes_closure {
	struct gfarm_timespec atime, mtime;
};

static gfarm_error_t
gfm_utimes_request(struct gfm_connection *gfm_server, void *closure)
{
	struct gfm_utimes_closure *c = closure;
	gfarm_error_t e = gfm_client_futimes_request(gfm_server,
	    c->atime.tv_sec, c->atime.tv_nsec,
	    c->mtime.tv_sec, c->mtime.tv_nsec);

	if (e != GFARM_ERR_NO_ERROR)
		gflog_warning(GFARM_MSG_1000158,
		    "futimes request: %s", gfarm_error_string(e));
	return (e);
}

static gfarm_error_t
gfm_utimes_result(struct gfm_connection *gfm_server, void *closure)
{
	gfarm_error_t e = gfm_client_futimes_result(gfm_server);

#if 1 /* DEBUG */
	if (e != GFARM_ERR_NO_ERROR)
		gflog_debug(GFARM_MSG_1000159,
		    "futimes result: %s", gfarm_error_string(e));
#endif
	return (e);
}

static gfarm_error_t
gfs_utimes_common(const char *path, const struct gfarm_timespec *tsp,
	gfarm_error_t (*inode_op)(const char *, int,
		gfarm_error_t (*request_op)(struct gfm_connection *, void *),
		gfarm_error_t (*result_op)(struct gfm_connection *, void *),
		gfarm_error_t (*success_op)(
		    struct gfm_connection *, void *, int, const char *),
		void (*cleanup_op)(struct gfm_connection *, void *), void *))
{
	struct gfm_utimes_closure closure;

	if (tsp == NULL) {
		struct timeval now;

		gettimeofday(&now, NULL);
		closure.atime.tv_sec  = closure.mtime.tv_sec  =
		    now.tv_sec;
		closure.atime.tv_nsec = closure.mtime.tv_nsec =
		    now.tv_usec * 1000;
	} else {
		closure.atime = tsp[0];
		closure.mtime = tsp[1];
	}

	return ((*inode_op)(path, GFARM_FILE_LOOKUP,
	    gfm_utimes_request,
	    gfm_utimes_result,
	    gfm_inode_success_op_connection_free,
	    NULL,
	    &closure));
}

gfarm_error_t
gfs_utimes(const char *path, const struct gfarm_timespec *tsp)
{
	return (gfs_utimes_common(path, tsp, gfm_inode_op));
}

gfarm_error_t
gfs_lutimes(const char *path, const struct gfarm_timespec *tsp)
{
	return (gfs_utimes_common(path, tsp, gfm_inode_op_no_follow));
}
