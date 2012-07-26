/*
 * $Id$
 */

#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <pwd.h>

#include <gfarm/gflog.h>
#include <gfarm/gfarm_config.h>
#include <gfarm/error.h>
#include <gfarm/gfarm_misc.h>
#include <gfarm/gfs.h>

#include "gfutil.h"
#include "thrsubr.h"

#include "gfp_xdr.h"
#include "gfm_proto.h"
#include "gfs_proto.h"
#include "auth.h"
#include "gfm_client.h"
#include "config.h"

#include "peer_watcher.h"
#include "peer.h"
#include "local_peer.h"
#include "remote_peer.h"
#include "subr.h"
#include "rpcsubr.h"
#include "thrpool.h"
#include "callout.h"
#include "abstract_host.h"
#include "host.h"
#include "mdhost.h"
#include "inode.h"
#include "journal_file.h"
#include "db_journal.h"
#include "relay.h"
#include "gfmd_channel.h"
#include "gfmd.h" /* protocol_service(), gfmd_terminate() */


struct gfmdc_journal_send_closure {
	struct mdhost *host;
	void *data;
	int end;
	pthread_mutex_t send_mutex;
	pthread_cond_t send_end_cond;
};

struct gfmdc_journal_sync_info {
	gfarm_uint64_t seqnum;
	int nservers;
	int nrecv_threads, slave_index;
	gfarm_error_t file_sync_error;
	pthread_mutex_t sync_mutex;
	pthread_cond_t sync_end_cond;
	pthread_mutex_t async_mutex;
	pthread_cond_t async_wait_cond;
	struct gfmdc_journal_send_closure *closures;
};

static struct peer_watcher *gfmdc_recv_watcher;
static struct thread_pool *gfmdc_send_thread_pool;
static struct thread_pool *journal_sync_thread_pool;
static struct gfmdc_journal_sync_info journal_sync_info;

static const char SYNC_MUTEX_DIAG[]	= "jorunal_sync_info.sync_mutex";
static const char SYNC_END_COND_DIAG[]	= "jorunal_sync_info.sync_end_cond";
static const char SEND_MUTEX_DIAG[]	= "send_closure.mutex";
static const char SEND_END_COND_DIAG[]	= "send_closure.end_cond";
static const char ASYNC_MUTEX_DIAG[]	= "journal_sync_info.async_mutex";
static const char ASYNC_WAIT_COND_DIAG[] = "journal_sync_info.async_wait_cond";
static const char REQUEST_WAIT_INFO_MUTEX[] = "request_sync_info.mutex";
static const char REQUEST_WAIT_INFO_COND[] = "request_sync_info.cond";

#define GFMDC_CONNECT_INTERVAL	30

static gfarm_error_t
gfmdc_server_get_request(struct peer *peer, size_t size,
	const char *diag, const char *format, ...)
{
	gfarm_error_t e;
	va_list ap;

	va_start(ap, format);
	e = async_server_vget_request(peer, size, diag, format, &ap);
	va_end(ap);

	return (e);
}

static gfarm_error_t
gfmdc_server_put_reply(struct mdhost *mh,
	struct peer *peer, gfp_xdr_xid_t xid,
	const char *diag, gfarm_error_t errcode, char *format, ...)
{
	gfarm_error_t e;
	va_list ap;

	va_start(ap, format);
	e = async_server_vput_reply(
	    mdhost_to_abstract_host(mh), peer, xid, gfp_xdr_vsend,
	    diag, errcode, format, &ap);
	va_end(ap);

	return (e);
}

static gfarm_error_t
gfmdc_client_recv_result(struct peer *peer, struct mdhost *mh,
	size_t size, const char *diag, const char *format, ...)
{
	gfarm_error_t e, errcode;
	va_list ap;

	va_start(ap, format);
	e = async_client_vrecv_result(peer,
	    mdhost_to_abstract_host(mh), size, diag,
	    &format, &errcode, &ap);
	va_end(ap);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	/*
	 * We just use gfarm_error_t as the errcode,
	 * Note that GFARM_ERR_NO_ERROR == 0.
	 */
	return (errcode);
}

struct request_sync_info {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	gfarm_error_t (*result_op)(struct mdhost *, struct peer *, size_t,
		gfarm_error_t, void *, const char *);
	void *closure;
	int end;
	gfarm_error_t error;
	const char *diag;
};

static void
gfmdc_client_send_request_sync_notify(struct request_sync_info *ri,
	gfarm_error_t e)
{
	ri->error = e;
	gfarm_mutex_lock(&ri->mutex, ri->diag, REQUEST_WAIT_INFO_MUTEX);
	ri->end = 1;
	gfarm_cond_signal(&ri->cond, ri->diag, REQUEST_WAIT_INFO_COND);
	gfarm_mutex_unlock(&ri->mutex, ri->diag, REQUEST_WAIT_INFO_MUTEX);
}

static gfarm_error_t
gfmdc_client_vsend_request_sync_result(void *p, void *arg, size_t size)
{
	gfarm_error_t e;
	struct peer *peer = p;
	struct mdhost *mh = peer_get_mdhost(peer);
	struct request_sync_info *ri = arg;

	if ((e = ri->result_op(mh, peer, size, GFARM_ERR_NO_ERROR,
	    ri->closure, ri->diag)) != GFARM_ERR_NO_ERROR)
		gflog_error(GFARM_MSG_UNFIXED, "%s : %s",
		    mdhost_get_name(mh), gfarm_error_string(e));
	gfmdc_client_send_request_sync_notify(ri, e);
	return (e);
}

static void
gfmdc_client_vsend_request_sync_disconnect(void *p, void *arg)
{
	gfarm_error_t e;
	struct peer *peer = p;
	struct mdhost *mh = peer_get_mdhost(peer);
	struct request_sync_info *ri = arg;

	/* FIXME better to set the reason of disconnection */
	e = GFARM_ERR_CONNECTION_ABORTED;
	gflog_error(GFARM_MSG_UNFIXED, "%s : %s",
	    mh != NULL ? mdhost_get_name(mh) : "", gfarm_error_string(e));
	(void)ri->result_op(mh, peer, 0, e, ri->closure, ri->diag);
	gfmdc_client_send_request_sync_notify(ri, e);
}

static gfarm_error_t
gfmdc_client_vsend_request_sync(struct mdhost *mh, struct peer *peer,
	gfarm_error_t (*result_op)(struct mdhost *, struct peer *, size_t,
		gfarm_error_t, void *, const char *),
	void *closure, const char *diag, gfarm_int32_t command,
	const char *format, va_list *app)
{
	gfarm_error_t e;
	struct request_sync_info ri;

	gfarm_mutex_init(&ri.mutex, diag, REQUEST_WAIT_INFO_MUTEX);
	gfarm_cond_init(&ri.cond, diag, REQUEST_WAIT_INFO_COND);
	ri.result_op = result_op;
	ri.closure = closure;
	ri.end = 0;
	ri.error = GFARM_ERR_NO_ERROR;
	ri.diag = diag;

	e = async_client_vsend_request(mdhost_to_abstract_host(mh),
	    peer, diag,
	    gfmdc_client_vsend_request_sync_result,
	    gfmdc_client_vsend_request_sync_disconnect, &ri,
#ifdef COMPAT_GFARM_2_3
	    NULL,
#endif
	    command, format, app);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_UNFIXED,
		    "%s : %s", mdhost_get_name(mh), gfarm_error_string(e));
		goto end;
	}

	gfarm_mutex_lock(&ri.mutex, diag, REQUEST_WAIT_INFO_MUTEX);
	while (!ri.end) {
		gfarm_cond_wait(&ri.cond, &ri.mutex, diag,
		    REQUEST_WAIT_INFO_COND);
	}
	gfarm_mutex_unlock(&ri.mutex, diag, REQUEST_WAIT_INFO_MUTEX);
	e = ri.error;
	if (e != GFARM_ERR_NO_ERROR)
		gflog_error(GFARM_MSG_UNFIXED,
		    "%s : %s", mdhost_get_name(mh), gfarm_error_string(e));
end:
	gfarm_mutex_destroy(&ri.mutex, diag, REQUEST_WAIT_INFO_MUTEX);
	gfarm_cond_destroy(&ri.cond, diag, REQUEST_WAIT_INFO_COND);
	return (e);
}

static gfarm_error_t
gfmdc_slave_vsend_request_sync(
	gfarm_error_t (*result_op)(struct mdhost *, struct peer *, size_t,
		gfarm_error_t, void *, const char *),
	void *closure, const char *diag, gfarm_int32_t command,
	const char *format, va_list *app)
{
	return (gfmdc_client_vsend_request_sync(mdhost_lookup_master(),
	    NULL, result_op, closure, diag, command, format, app));
}

static gfarm_error_t
gfmdc_slave_send_request_sync(
	gfarm_error_t (*result_op)(struct mdhost *, struct peer *, size_t,
		gfarm_error_t, void *, const char *),
	void *closure, const char *diag, gfarm_int32_t command,
	const char *format, ...)
{
	gfarm_error_t e;
	va_list ap;

	va_start(ap, format);
	e = gfmdc_slave_vsend_request_sync(result_op, closure, diag,
	    command, format, &ap);
	va_end(ap);
	return (e);
}

struct request_async_info {
	gfarm_error_t (*result_op)(struct mdhost *, struct peer *, size_t,
		gfarm_error_t, void *, const char *);
	void *closure;
	const char *diag;
};

static gfarm_error_t
gfmdc_client_vsend_request_async_result(void *p, void *arg, size_t size)
{
	gfarm_error_t e;
	struct peer *peer = p;
	struct mdhost *mh = peer_get_mdhost(peer);
	struct request_async_info *ri = arg;

	e = ri->result_op(mh, peer, size, GFARM_ERR_NO_ERROR, ri->closure,
	    ri->diag);
	free(ri);
	return (e);
}

static void
gfmdc_client_vsend_request_async_disconnect(void *p, void *arg)
{
	gfarm_error_t e;
	struct peer *peer = p;
	struct mdhost *mh = peer_get_mdhost(peer);
	struct request_async_info *ri = arg;

	/* FIXME better to set the reason of disconnection */
	e = GFARM_ERR_CONNECTION_ABORTED;
	gflog_error(GFARM_MSG_UNFIXED, "%s : %s",
	    mh != NULL ? mdhost_get_name(mh) : "", gfarm_error_string(e));
	(void)ri->result_op(mh, peer, 0, e, ri->closure, ri->diag);
	free(ri);
}

static gfarm_error_t
gfmdc_client_vsend_request_async(struct mdhost *mh, struct peer *peer,
	gfarm_error_t (*result_op)(struct mdhost *, struct peer *, size_t,
		gfarm_error_t, void *, const char *),
	void *closure, const char *diag, gfarm_int32_t command,
	const char *format, va_list *app)
{
	gfarm_error_t e;
	struct request_async_info *ri;

	GFARM_MALLOC(ri);
	if (ri == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_error(GFARM_MSG_UNFIXED,
		    "%s", gfarm_error_string(e));
		return (e);
	}

	ri->result_op = result_op;
	ri->closure = closure;
	ri->diag = diag;

	e = async_client_vsend_request(mdhost_to_abstract_host(mh),
	    peer, diag,
	    gfmdc_client_vsend_request_async_result,
	    gfmdc_client_vsend_request_async_disconnect, ri,
#ifdef COMPAT_GFARM_2_3
	    NULL,
#endif
	    command, format, app);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_UNFIXED,
		    "%s : %s", mdhost_get_name(mh), gfarm_error_string(e));
	}
	return (e);
}

static gfarm_error_t
gfmdc_client_send_request_async(struct mdhost *mh, struct peer *peer,
	gfarm_error_t (*result_op)(struct mdhost *, struct peer *, size_t,
		gfarm_error_t, void *, const char *),
	void *closure, const char *diag, gfarm_int32_t command,
	const char *format, ...)
{
	gfarm_error_t e;
	va_list ap;

	va_start(ap, format);
	e = gfmdc_client_vsend_request_async(mh, peer, result_op, closure,
	    diag, command, format, &ap);
	va_end(ap);
	return (e);
}

static gfarm_error_t
gfmdc_slave_vsend_request_async(
	gfarm_error_t (*result_op)(struct mdhost *, struct peer *, size_t,
		gfarm_error_t, void *, const char *),
	void *closure, const char *diag, gfarm_int32_t command,
	const char *format, va_list *app)
{
	return (gfmdc_client_vsend_request_async(mdhost_lookup_master(),
	    NULL, result_op, closure, diag, command, format, app));
}

static gfarm_error_t
gfmdc_slave_send_request_async(
	gfarm_error_t (*result_op)(struct mdhost *, struct peer *, size_t,
		gfarm_error_t, void *, const char *),
	void *closure, const char *diag, gfarm_int32_t command,
	const char *format, ...)
{
	gfarm_error_t e;
	va_list ap;

	va_start(ap, format);
	e = gfmdc_slave_vsend_request_async(result_op, closure, diag,
	    command, format, &ap);
	va_end(ap);
	return (e);
}

static void
gfmdc_journal_recv_end_signal(const char *diag)
{
	gfarm_mutex_lock(&journal_sync_info.sync_mutex, diag,
	    SYNC_MUTEX_DIAG);
	--journal_sync_info.nrecv_threads;
	gfarm_mutex_unlock(&journal_sync_info.sync_mutex, diag,
	    SYNC_MUTEX_DIAG);
	gfarm_cond_signal(&journal_sync_info.sync_end_cond, diag,
	    SYNC_END_COND_DIAG);
}

static void
gfmdc_journal_send_closure_reset(struct gfmdc_journal_send_closure *c,
	struct mdhost *mh)
{
	c->host = mh;
	c->data = NULL;
}

static void
gfmdc_client_journal_syncsend_free(struct gfmdc_journal_send_closure *c)
{
	static const char diag[] = "gfmdc_client_journal_syncsend_free";

	free(c->data);
	c->data = NULL;
	gfarm_mutex_lock(&c->send_mutex, diag, SEND_MUTEX_DIAG);
	c->end = 1;
	gfarm_mutex_unlock(&c->send_mutex, diag, SEND_MUTEX_DIAG);
	gfarm_cond_signal(&c->send_end_cond, diag, SEND_END_COND_DIAG);
}

static void
gfmdc_client_journal_asyncsend_free(struct gfmdc_journal_send_closure *c)
{
	free(c->data);
	c->data = NULL;
	free(c);
}

static gfarm_error_t
gfmdc_client_journal_send_result_common(struct peer *peer, size_t size,
	struct gfmdc_journal_send_closure *c, const char *diag)
{
	gfarm_error_t e;

	if ((e = gfmdc_client_recv_result(peer, c->host, size, diag, ""))
	    != GFARM_ERR_NO_ERROR)
		gflog_error(GFARM_MSG_1002976,
		    "%s : %s", mdhost_get_name(c->host),
		    gfarm_error_string(e));
	return (e);
}

static gfarm_error_t
gfmdc_client_journal_syncsend_result(struct mdhost *mh, struct peer *peer,
	size_t size, gfarm_error_t e, void *closure, const char *diag)
{
	struct gfmdc_journal_send_closure *c = closure;

	if (e == GFARM_ERR_NO_ERROR)
		e = gfmdc_client_journal_send_result_common(peer, size, c,
		    diag);
	gfmdc_client_journal_syncsend_free(c);
	return (e);
}

static gfarm_error_t
gfmdc_client_journal_asyncsend_result(struct mdhost *mh, struct peer *peer,
	size_t size, gfarm_error_t e, void *closure, const char *diag)
{
	struct gfmdc_journal_send_closure *c = closure;

	if (e == GFARM_ERR_NO_ERROR)
		e = gfmdc_client_journal_send_result_common(peer, size, c,
		    diag);
	gfmdc_client_journal_asyncsend_free(c);
	return (e);
}

static int
gfmdc_wait_journal_syncsend(struct gfmdc_journal_send_closure *c)
{
	int r, in_time = 1;
	struct timespec ts;
	static const char diag[] = "gfmdc_wait_journal_syncsend";

#ifdef HAVE_CLOCK_GETTIME
	clock_gettime(CLOCK_REALTIME, &ts);
#else
	struct timeval tv;

	gettimeofday(&tv, NULL);
	ts.tv_sec = tv.tv_sec + gfarm_get_journal_sync_slave_timeout();
	ts.tv_nsec = tv.tv_usec * 1000;
#endif
	ts.tv_sec += gfarm_get_journal_sync_slave_timeout();

	gfarm_mutex_lock(&c->send_mutex, diag, SEND_MUTEX_DIAG);
	while (!c->end && in_time)
		in_time = gfarm_cond_timedwait(&c->send_end_cond,
		    &c->send_mutex, &ts, diag, SEND_END_COND_DIAG);
	if ((r = c->end) == 0)
		c->end = 1;
	gfarm_mutex_unlock(&c->send_mutex, diag, SEND_MUTEX_DIAG);
	return (r);
}

static gfarm_error_t
gfmdc_client_journal_send(gfarm_uint64_t *to_snp,
	gfarm_error_t (*result_op)(struct mdhost *, struct peer *, size_t,
		gfarm_error_t, void *, const char *),
	struct gfmdc_journal_send_closure *c)
{
	gfarm_error_t e;
	int data_len, no_rec;
	char *data;
	gfarm_uint64_t min_seqnum, from_sn, to_sn, lf_sn;
	struct journal_file_reader *reader;
	struct mdhost *mh = c->host;
	static const char diag[] = "GFM_PROTO_JOURNAL_SEND";

	lf_sn = mdhost_get_last_fetch_seqnum(mh);
	min_seqnum = lf_sn == 0 ? 0 : lf_sn + 1;
	reader = mdhost_get_journal_file_reader(mh);
	assert(reader);
	e = db_journal_fetch(reader, min_seqnum, &data, &data_len, &from_sn,
	    &to_sn, &no_rec, mdhost_get_name(mh));
	if (e != GFARM_ERR_NO_ERROR) {
		mdhost_set_seqnum_state_by_error(mh, e);
		gflog_debug(GFARM_MSG_1002977,
		    "reading journal to send to %s: %s",
		    mdhost_get_name(mh), gfarm_error_string(e));
		return (e);
	} else if (no_rec) {
		mdhost_set_seqnum_ok(mh);
		*to_snp = 0;
		return (GFARM_ERR_NO_ERROR);
	}
	mdhost_set_seqnum_ok(mh);
	mdhost_set_last_fetch_seqnum(mh, to_sn);
	c->data = data;

	if ((e = gfmdc_client_send_request_async(mh, NULL, result_op,
	    c, diag, GFM_PROTO_JOURNAL_SEND, "llb", from_sn, to_sn,
	    (size_t)data_len, data)) != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1002978,
		    "%s : %s", mdhost_get_name(mh), gfarm_error_string(e));
		free(data);
		c->data = NULL;
		return (e);
	}
	*to_snp = to_sn;

	return (e);
}

static gfarm_error_t
gfmdc_client_journal_syncsend(gfarm_uint64_t *to_snp,
	struct gfmdc_journal_send_closure *c)
{
	gfarm_error_t e;

	c->end = 0;
	e = gfmdc_client_journal_send(to_snp,
	    gfmdc_client_journal_syncsend_result, c);
	if (e == GFARM_ERR_NO_ERROR && *to_snp > 0 &&
	    !gfmdc_wait_journal_syncsend(c))
		return (GFARM_ERR_OPERATION_TIMED_OUT);
	assert(c->data == NULL);
	return (e);
}

static gfarm_error_t
gfmdc_client_journal_asyncsend(gfarm_uint64_t *to_snp,
	struct gfmdc_journal_send_closure *c)
{
	return (gfmdc_client_journal_send(to_snp,
	    gfmdc_client_journal_asyncsend_result, c));
}

static gfarm_error_t
gfmdc_server_journal_send(struct mdhost *mh, struct peer *peer,
	gfp_xdr_xid_t xid, size_t size)
{
	gfarm_error_t e, er;
	gfarm_uint64_t from_sn, to_sn;
	unsigned char *recs = NULL;
	size_t recs_len;
	static const char diag[] = "GFM_PROTO_JOURNAL_SEND";

	if ((er = gfmdc_server_get_request(peer, size, diag, "llB",
	    &from_sn, &to_sn, &recs_len, &recs))
	    == GFARM_ERR_NO_ERROR)
		er = db_journal_recvq_enter(from_sn, to_sn, recs_len, recs);
#ifdef DEBUG_JOURNAL
	if (er == GFARM_ERR_NO_ERROR)
		gflog_debug(GFARM_MSG_1002979,
		    "from %s : recv journal %llu to %llu",
		    mdhost_get_name(mh), (unsigned long long)from_sn,
		    (unsigned long long)to_sn);
#endif
	e = gfmdc_server_put_reply(mh, peer, xid, diag, er, "");
	return (e);
}

static void* gfmdc_journal_first_sync_thread(void *);

static gfarm_error_t
gfmdc_server_journal_ready_to_recv(struct mdhost *mh, struct peer *peer,
	gfp_xdr_xid_t xid, size_t size)
{
	gfarm_error_t e;
	gfarm_uint64_t seqnum;
	int inited = 0;
	struct journal_file_reader *reader;
	static const char diag[] = "GFM_PROTO_JOURNAL_READY_TO_RECV";

	if ((e = gfmdc_server_get_request(peer, size, diag, "l", &seqnum))
	    == GFARM_ERR_NO_ERROR) {
		mdhost_set_last_fetch_seqnum(mh, seqnum);
		mdhost_set_is_recieved_seqnum(mh, 1);
#ifdef DEBUG_JOURNAL
		gflog_debug(GFARM_MSG_1002980,
		    "%s : last_fetch_seqnum=%llu",
		    mdhost_get_name(mh), (unsigned long long)seqnum);
#endif
		reader = mdhost_get_journal_file_reader(mh);
		if ((e = db_journal_reader_reopen_if_needed(&reader,
		    mdhost_get_last_fetch_seqnum(mh), &inited))
		    != GFARM_ERR_NO_ERROR) {
			mdhost_set_seqnum_state_by_error(mh, e);
			gflog_error(GFARM_MSG_1002981,
			    "gfmd_channel(%s) : %s",
			    mdhost_get_name(mh),
			    gfarm_error_string(e));
		}
		/*
		 * if reader is already expired,
		 * db_journal_reader_reopen_if_needed() returns EXPIRED and
		 * sets inited to 1.
		 * if no error occurrs, it also sets inited to 1.
		 */
		if (inited)
			mdhost_set_journal_file_reader(mh, reader);
	}
	e = gfmdc_server_put_reply(mh, peer, xid, diag, e, "l",
	    db_journal_get_current_seqnum());
	if (mdhost_is_sync_replication(mh)) {
		gfarm_mutex_lock(&journal_sync_info.sync_mutex, diag,
		    SYNC_MUTEX_DIAG);
		thrpool_add_job(journal_sync_thread_pool,
		    gfmdc_journal_first_sync_thread, mh);
		gfarm_mutex_unlock(&journal_sync_info.sync_mutex, diag,
		    SYNC_MUTEX_DIAG);
	}
	return (e);
}

static gfarm_error_t
gfmdc_client_journal_ready_to_recv_result(struct mdhost *mh, struct peer *peer,
	size_t size, gfarm_error_t e, void *closure, const char *diag)
{
	gfarm_uint64_t seqnum;

	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	if ((e = gfmdc_client_recv_result(peer, mh, size, diag, "l", &seqnum))
	    != GFARM_ERR_NO_ERROR)
		gflog_error(GFARM_MSG_1002982, "%s : %s",
		    mdhost_get_name(mh), gfarm_error_string(e));
	else
		e = slave_add_initial_db_update_info(seqnum, diag);
	return (e);
}

gfarm_error_t
gfmdc_client_journal_ready_to_recv(struct mdhost *mh)
{
	gfarm_error_t e;
	gfarm_uint64_t seqnum;
	static const char diag[] = "GFM_PROTO_JOURNAL_READY_TO_RECV";

	giant_lock();
	seqnum = db_journal_get_current_seqnum();
	giant_unlock();

	if ((e = gfmdc_slave_send_request_sync(
	    gfmdc_client_journal_ready_to_recv_result, NULL, diag,
	    GFM_PROTO_JOURNAL_READY_TO_RECV, "l", seqnum))
	    != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1002983,
		    "%s : %s", mdhost_get_name(mh), gfarm_error_string(e));
	}
	return (e);
}

static gfarm_error_t
gfmdc_client_remote_peer_alloc_result(struct mdhost *mh, struct peer *peer,
	size_t size, gfarm_error_t e, void *closure, const char *diag)
{
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if ((e = gfmdc_client_recv_result(peer, mh, size, diag, ""))
	    != GFARM_ERR_NO_ERROR)
		gflog_error(GFARM_MSG_UNFIXED, "%s : %s",
		    mdhost_get_name(mh), gfarm_error_string(e));
	return (e);
}

gfarm_error_t
gfmdc_client_remote_peer_alloc(struct peer *peer)
{
	gfarm_error_t e;
	static const char diag[] = "GFM_PROTO_REMOTE_PEER_ALLOC";
	int port;
	/* FIXME get from peer */
	gfarm_int32_t proto_family = GFARM_PROTO_FAMILY_IPV4;
	gfarm_int32_t proto_transport = GFARM_PROTO_TRANSPORT_IP_TCP;

	if ((e = peer_get_port(peer, &port)) != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_UNFIXED,
		    "%s: %s", diag, gfarm_error_string(e));
		return (e);
	}

	if ((e = gfmdc_slave_send_request_sync(
	    gfmdc_client_remote_peer_alloc_result, NULL, diag,
	    GFM_PROTO_REMOTE_PEER_ALLOC, "lissiii",
	    peer_get_id(peer), peer_get_auth_id_type(peer),
	    peer_get_username(peer), peer_get_hostname(peer),
	    proto_family, proto_transport, port)) == GFARM_ERR_NO_ERROR)
		local_peer_set_remote_peer_allocated(peer_to_local_peer(peer),
		    1);
	else
		gflog_error(GFARM_MSG_UNFIXED,
		    "%s: %s", diag, gfarm_error_string(e));
	return (e);
}

static gfarm_error_t
gfmdc_server_remote_peer_alloc(struct mdhost *mh, struct peer *peer,
	gfp_xdr_xid_t xid, size_t size)
{
	gfarm_error_t e;
	gfarm_int64_t remote_peer_id;
	gfarm_int32_t auth_id_type, proto_family, proto_transport, port;
	char *user, *host;
	static const char diag[] = "GFM_PROTO_REMOTE_PEER_ALLOC";

	if ((e = gfmdc_server_get_request(peer, size, diag, "lissiii",
	    &remote_peer_id, &auth_id_type, &user, &host,
	    &proto_family, &proto_transport, &port)) ==
	    GFARM_ERR_NO_ERROR) {
		/* FIXME We don't pass auth_method now */
		e = remote_peer_alloc(peer, remote_peer_id,
		    auth_id_type, user, host, GFARM_AUTH_METHOD_NONE,
		    proto_family, proto_transport, port);
	}
	e = gfmdc_server_put_reply(mh, peer, xid, diag, e, "");
	return (e);
}

static gfarm_error_t
gfmdc_client_remote_peer_free_result(struct mdhost *mh, struct peer *peer,
	size_t size, gfarm_error_t e, void *closure, const char *diag)
{
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if ((e = gfmdc_client_recv_result(peer, mh, size, diag, ""))
	    != GFARM_ERR_NO_ERROR)
		gflog_error(GFARM_MSG_UNFIXED, "%s : %s",
		    mdhost_get_name(mh), gfarm_error_string(e));
	return (e);
}

gfarm_error_t
gfmdc_client_remote_peer_free(gfarm_uint64_t peer_id)
{
	gfarm_error_t e;
	static const char diag[] = "GFM_PROTO_REMOTE_PEER_FREE";

	if (mdhost_self_is_master() || !mdhost_is_up(mdhost_lookup_master()))
		return (GFARM_ERR_NO_ERROR);

	if ((e = gfmdc_slave_send_request_async(
	    gfmdc_client_remote_peer_free_result, NULL, diag,
	    GFM_PROTO_REMOTE_PEER_FREE, "l", peer_id))
	    != GFARM_ERR_NO_ERROR)
		gflog_error(GFARM_MSG_UNFIXED,
		    "%s: %s", diag, gfarm_error_string(e));
	return (e);
}

static gfarm_error_t
gfmdc_server_remote_peer_free(struct mdhost *mh, struct peer *peer,
	gfp_xdr_xid_t xid, size_t size)
{
	gfarm_error_t e;
	gfarm_int64_t remote_peer_id;
	static const char diag[] = "GFM_PROTO_REMOTE_PEER_FREE";

	if ((e = gfmdc_server_get_request(peer, size, diag, "l",
	    &remote_peer_id)) == GFARM_ERR_NO_ERROR) {
		/* XXXRELAY this takes giant lock */
		e = remote_peer_free_by_id(peer, remote_peer_id);
	}
	e = gfmdc_server_put_reply(mh, peer, xid, diag, e, "");
	return (e);
}

static gfarm_error_t
gfmdc_server_remote_rpc(struct mdhost *mh, struct peer *peer,
	gfp_xdr_xid_t xid, size_t size)
{
	gfarm_error_t e;
	gfarm_int64_t remote_peer_id;
	struct remote_peer *remote_peer;
	struct peer *rpeer;
	int eof;
	static const char diag[] = "GFM_PROTO_REMOTE_RPC";

	if (debug_mode) {
		gflog_info(GFARM_MSG_UNFIXED,
		    "%s: <%s> start remote rpc receiving from %s",
		    peer_get_hostname(peer), diag,
		    peer_get_service_name(peer));
	}

	if ((e = gfp_xdr_recv_sized(peer_get_conn(peer), 0, &size, &eof, "l",
	    &remote_peer_id)) != GFARM_ERR_NO_ERROR) {
		/* XXXRELAY fix rpc residual */
		e = gfmdc_server_put_reply(mh, peer, xid, diag, e, "");
	} else if (eof) {
		e = gfmdc_server_put_reply(mh, peer, xid, diag,
		    GFARM_ERR_UNEXPECTED_EOF, "");
	} else if ((remote_peer = local_peer_lookup_remote(
	    peer_to_local_peer(peer), remote_peer_id)) == NULL) {
		/* XXXRELAY fix rpc residual */
		e = gfmdc_server_put_reply(mh, peer, xid, diag,
		    GFARM_ERR_INVALID_REMOTE_PEER, "");
	} else {
		rpeer = remote_peer_to_peer(remote_peer);
		peer_add_ref(rpeer);
		remote_peer_clear_db_update_info(remote_peer);
		/* XXXRELAY split this to another thread */
		e = protocol_service(rpeer, xid, &size);
		peer_del_ref(rpeer);
	}
	return (e);
}

static gfarm_error_t
gfmdc_protocol_switch(struct abstract_host *h,
	struct peer *peer, int request, gfp_xdr_xid_t xid, size_t size,
	int *unknown_request)
{
	struct mdhost *mh = abstract_host_to_mdhost(h);
	gfarm_error_t e = GFARM_ERR_NO_ERROR;

	switch (request) {
	case GFM_PROTO_JOURNAL_READY_TO_RECV:
		/* in master */
		e = gfmdc_server_journal_ready_to_recv(mh, peer, xid, size);
		break;
	case GFM_PROTO_JOURNAL_SEND:
		/* in slave */
		e = gfmdc_server_journal_send(mh, peer, xid, size);
		break;
	case GFM_PROTO_REMOTE_PEER_ALLOC:
		/* in master */
		e = gfmdc_server_remote_peer_alloc(mh, peer, xid, size);
		break;
	case GFM_PROTO_REMOTE_PEER_FREE:
		/* in master */
		e = gfmdc_server_remote_peer_free(mh, peer, xid, size);
		break;
	case GFM_PROTO_REMOTE_RPC:
		/* in master */
		e = gfmdc_server_remote_rpc(mh, peer, xid, size);
		break;
	default:
		*unknown_request = 1;
		e = GFARM_ERR_PROTOCOL;
		break;
	}
	return (e);
}

#ifdef COMPAT_GFARM_2_3
static void
gfmdc_channel_free(struct abstract_host *h)
{
}
#endif

static void *
gfmdc_main(void *arg)
{
	struct local_peer *local_peer = arg;

	return (async_server_main(local_peer,
		gfmdc_protocol_switch
#ifdef COMPAT_GFARM_2_3
		,gfmdc_channel_free,
		NULL
#endif
		));
}

static gfarm_error_t
switch_gfmd_channel(struct peer *peer, int from_client,
	int version, const char *diag)
{
	gfarm_error_t e = GFARM_ERR_NO_ERROR;
	struct mdhost *mh = NULL;
	gfp_xdr_async_peer_t async = NULL;

	giant_lock();
	if (peer_get_async(peer) == NULL &&
	    (mh = peer_get_mdhost(peer)) == NULL) {
		gflog_error(GFARM_MSG_1002984,
			"Operation not permitted: peer_get_host() failed");
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
	} else if ((e = gfp_xdr_async_peer_new(&async))
	    != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1002985,
		    "%s: gfp_xdr_async_peer_new(): %s",
		    diag, gfarm_error_string(e));
	}
	giant_unlock();
	if (e == GFARM_ERR_NO_ERROR) {
		struct local_peer *local_peer = peer_to_local_peer(peer);

		local_peer_set_async(local_peer, async);
		local_peer_set_readable_watcher(local_peer, gfmdc_recv_watcher);

		if (mdhost_is_up(mh)) /* throw away old connetion */ {
			gflog_warning(GFARM_MSG_1002986,
			    "gfmd_channel(%s): switching to new connection",
			    mdhost_get_name(mh));
			mdhost_disconnect(mh, NULL);
		}
		mdhost_set_peer(mh, peer, version);
		local_peer_watch_readable(local_peer);
	}
	return (e);
}

gfarm_error_t
gfm_server_switch_gfmd_channel(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip)
{
	gfarm_error_t e, er;
	gfarm_int32_t version;
	gfarm_int64_t cookie;
	struct relayed_request *relay;
	int i;
	static const char diag[] = "GFM_PROTO_SWITCH_GFMD_CHANNEL";

	e = gfm_server_relay_get_request(peer, sizep, skip, &relay, diag,
	    GFM_PROTO_SWITCH_GFMD_CHANNEL, "il", &version, &cookie);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (skip)
		return (GFARM_ERR_NO_ERROR);

	if (relay == NULL) {
		/* do not relay RPC to master gfmd */
		if (from_client) {
			gflog_debug(GFARM_MSG_1002987,
			    "Operation not permitted: from_client");
			er = GFARM_ERR_OPERATION_NOT_PERMITTED;
		} else
			er = GFARM_ERR_NO_ERROR;
	}
	i = 0;
	if ((e = gfm_server_relay_put_reply(peer, xid, sizep, relay,
	    diag, er, "i", &i /*XXX FIXME*/)) != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1002988,
		    "%s: %s", diag, gfarm_error_string(e));
		return (e);
	}
	if ((e = gfp_xdr_flush(peer_get_conn(peer))) != GFARM_ERR_NO_ERROR)
		gflog_warning(GFARM_MSG_1002989,
		    "%s: protocol flush: %s",
		    diag, gfarm_error_string(e));
	if (relay == NULL && debug_mode)
		gflog_debug(GFARM_MSG_1002990, "gfp_xdr_flush");
	return (switch_gfmd_channel(peer, from_client, version, diag));
}

static gfarm_error_t
gfmdc_connect(void)
{
	gfarm_error_t e;
	int port;
	const char *hostname;
	gfarm_int32_t gfmd_knows_me;
	struct gfm_connection *conn = NULL;
	struct mdhost *rhost, *master, *self_host;
	struct peer *peer = NULL;
	char *local_user;
	struct passwd *pwd;
	/* XXX FIXME must be configuable */
	unsigned int sleep_interval = 10;
	/* XXX FIXME must be configuable */
	static unsigned int sleep_max_interval = 40;
	static int hack_to_make_cookie_not_work = 0; /* XXX FIXME */
	static const char *service_user = GFMD_USERNAME;
	static const char diag[] = "gfmdc_connect";

	master = mdhost_lookup_master();
	gfarm_set_auth_id_type(GFARM_AUTH_ID_TYPE_METADATA_HOST);
	hostname = mdhost_get_name(master);
	port = mdhost_get_port(master);

	if ((e = gfarm_global_to_local_username_by_url(GFARM_PATH_ROOT,
	    service_user, &local_user)) != GFARM_ERR_NO_ERROR) {
		gflog_fatal(GFARM_MSG_1002991,
		    "no local user for the global `%s' user.",
		    service_user); /* exit */
	}
	if ((pwd = getpwnam(local_user)) == NULL) {
		gflog_fatal(GFARM_MSG_1002992,
		    "user `%s' is necessary, but doesn't exist.",
		    local_user); /* exit */
	}

	for (;;) {
		/*
		 * XXXRELAY FIXME
		 * multicast should be used always,
		 * (otherwise when a slave gfmd is promoted to master,
		 *  other slaves won't connect to the new master, see #361),
		 * but currently that doesn't work with protocol relay. FIXME.
		 */
		int multicast = gfarm_get_metadb_server_slave_listen() ? 0 : 1;

		/* try connecting to multiple destinations */
		e = gfm_client_connect_with_seteuid(hostname, port,
		    service_user, &conn, NULL, pwd, multicast);
		if (e == GFARM_ERR_NO_ERROR)
			break;
		gflog_error(GFARM_MSG_1002993,
		    "gfmd_channel(%s) : %s",
		    hostname, gfarm_error_string(e));
		if (sleep_interval < sleep_max_interval)
			gflog_error(GFARM_MSG_1002994,
			    "connecting to the master gfmd failed, "
			    "sleep %d sec: %s", sleep_interval,
			    gfarm_error_string(e));
		sleep(sleep_interval);
		if (mdhost_self_is_master())
			break;
		if (sleep_interval < sleep_max_interval)
			sleep_interval *= 2;
	}
	free(local_user);
	if (mdhost_self_is_master()) {
		if (conn)
			gfm_client_connection_free(conn);
		return (GFARM_ERR_NO_ERROR);
	}

	rhost = mdhost_lookup_metadb_server(
	    gfm_client_connection_get_real_server(conn));
	assert(rhost != NULL);
	if (master != rhost) {
		mdhost_set_is_master(master, 0);
		mdhost_set_is_master(rhost, 1);
	}
	hostname = mdhost_get_name(rhost);
	mdhost_set_connection(rhost, conn);

	/* self_host name is equal to metadb_server_host in gfmd.conf */
	self_host = mdhost_lookup_self();
	if (gfm_client_hostname_set(conn, mdhost_get_name(self_host))
	    != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1003426,
		    "gfmd_channel(%s) : %s",
		    hostname, gfarm_error_string(e));
		return (e);
	}

	if ((e = gfm_client_switch_gfmd_channel(conn,
	    GFM_PROTOCOL_VERSION, (gfarm_int64_t)hack_to_make_cookie_not_work,
	    &gfmd_knows_me))
	    != GFARM_ERR_NO_ERROR) {
		if (gfm_client_is_connection_error(e)) {
			gflog_error(GFARM_MSG_1003427,
			    "gfmd_channel(%s) : %s",
			    hostname, gfarm_error_string(e));
		} else {
			gflog_error(GFARM_MSG_1003428,
			    "gfmd_channel(%s) : authorization denied. "
			    "set metadb_server_host properly. : %s",
			    hostname, gfarm_error_string(e));
		}
		return (e);
	}
	if ((e = local_peer_alloc_with_connection(
	    gfm_client_connection_conn(conn), mdhost_to_abstract_host(rhost),
	    GFARM_AUTH_ID_TYPE_METADATA_HOST, &peer)) != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1002996,
		    "gfmd_channel(%s) : %s",
		    hostname, gfarm_error_string(e));
		peer_free(peer);
		return (e);
	}
	if ((e = switch_gfmd_channel(peer, 0, GFM_PROTOCOL_VERSION, diag))
	    != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1002997,
		    "gfmd_channel(%s) : %s",
		    hostname, gfarm_error_string(e));
		return (e);
	}
	if ((e = gfmdc_client_journal_ready_to_recv(rhost))
	    != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1003429,
		    "gfmd_channel(%s) : %s",
		    hostname, gfarm_error_string(e));
		return (e);
	}
	gflog_info(GFARM_MSG_1002998,
	    "gfmd_channel(%s) : connected", hostname);
	return (GFARM_ERR_NO_ERROR);
}

static int
gfmdc_journal_mdhost_can_sync(struct mdhost *mh)
{
	return (!mdhost_is_self(mh) && mdhost_is_up(mh) &&
	    mdhost_get_journal_file_reader(mh) != NULL);
}

static gfarm_error_t
gfmdc_journal_asyncsend(struct mdhost *mh, int *exist_recsp)
{
	gfarm_error_t e;
	gfarm_uint64_t to_sn;
	struct gfmdc_journal_send_closure *c;

	if ((mdhost_is_sync_replication(mh) &&
	    !mdhost_is_in_first_sync(mh)) ||
	    !gfmdc_journal_mdhost_can_sync(mh))
		return (GFARM_ERR_NO_ERROR);
	GFARM_MALLOC(c);
	if (c == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_error(GFARM_MSG_1002999,
		    "%s", gfarm_error_string(e));
		return (e);
	}
	gfmdc_journal_send_closure_reset(c, mh);
	if ((e = gfmdc_client_journal_asyncsend(&to_sn, c))
	    != GFARM_ERR_NO_ERROR) {
		free(c);
		mdhost_disconnect(mh, mdhost_get_peer(mh));
	} else if (to_sn == 0) {
		free(c);
		*exist_recsp = 0;
		return (GFARM_ERR_NO_ERROR);
	}
	*exist_recsp = 0;
	return (GFARM_ERR_NO_ERROR);
}

static int
gfmdc_journal_asyncsend_each_mdhost(struct mdhost *mh, void *closure)
{
	int exist_recs;
	struct mdhost *self = mdhost_lookup_self();

	if (mh != self && !mdhost_is_sync_replication(mh))
		(void)gfmdc_journal_asyncsend(mh, &exist_recs);
	return (1);
}

void *
gfmdc_journal_asyncsend_thread(void *arg)
{
	struct timespec ts;
	static const char diag[] = "gfmdc_journal_asyncsend_thread";

	ts.tv_sec = 0;
	ts.tv_nsec = 500 * 1000 * 1000;

	for (;;) {
		gfarm_mutex_lock(&journal_sync_info.async_mutex, diag,
		    ASYNC_MUTEX_DIAG);
		while (!mdhost_has_async_replication_target()) {
			gfarm_cond_wait(&journal_sync_info.async_wait_cond,
			    &journal_sync_info.async_mutex,
			    diag, ASYNC_WAIT_COND_DIAG);
		}
		mdhost_foreach(gfmdc_journal_asyncsend_each_mdhost, NULL);
		gfarm_mutex_unlock(&journal_sync_info.async_mutex, diag,
		    ASYNC_MUTEX_DIAG);
		nanosleep(&ts, NULL);
	}
	return (NULL);
}

void *
gfmdc_connect_thread(void *arg)
{
	gfarm_error_t e;
	static const char diag[] = "gfmdc_connect_thread";

	for (;;) {
		if (mdhost_get_connection(mdhost_lookup_master()) != NULL) {
			sleep(GFMDC_CONNECT_INTERVAL);
		} else if ((e = gfmdc_connect()) != GFARM_ERR_NO_ERROR) {
			gflog_error(GFARM_MSG_1003430,
			    "gfmd_channel : "
			    "give up to connect to the master gfmd: %s",
			    gfarm_error_string(e));
			gfmd_terminate(diag);
			/* NOTREACHED */
		}
		if (mdhost_self_is_master())
			break;
	}
	return (NULL);
}

static void *
gfmdc_journal_file_sync_thread(void *arg)
{
	static const char diag[] = "db_journal_file_sync_thread";

#ifdef DEBUG_JOURNAL
	gflog_debug(GFARM_MSG_1003000,
	    "journal_file_sync start");
#endif
	journal_sync_info.file_sync_error = db_journal_file_writer_sync();
	gfmdc_journal_recv_end_signal(diag);
#ifdef DEBUG_JOURNAL
	gflog_debug(GFARM_MSG_1003001,
	    "journal_file_sync end");
#endif
	return (NULL);
}

static void *
gfmdc_journal_send_thread(void *closure)
{
	gfarm_error_t e;
	gfarm_uint64_t to_sn;
	struct gfmdc_journal_send_closure *c = closure;
	static const char diag[] = "gfmdc_journal_send_thread";

#ifdef DEBUG_JOURNAL
	gflog_debug(GFARM_MSG_1003002,
	    "journal_syncsend(%s) start", mdhost_get_name(c->host));
#endif
	do {
		if ((e = gfmdc_client_journal_syncsend(&to_sn, c))
		    != GFARM_ERR_NO_ERROR) {
			gflog_error(GFARM_MSG_1003003,
			    "%s : failed to send journal : %s",
			    mdhost_get_name(c->host), gfarm_error_string(e));
			break;
		}
		if (to_sn == 0) {
			gflog_warning(GFARM_MSG_1003004,
			    "%s : no journal records to send (seqnum=%lld) "
			    ": %s", mdhost_get_name(c->host),
			    (unsigned long long)journal_sync_info.seqnum,
			    gfarm_error_string(e));
			break;
		}
	} while (to_sn < journal_sync_info.seqnum);

	if (e != GFARM_ERR_NO_ERROR)
		mdhost_disconnect(c->host, mdhost_get_peer(c->host));
	gfmdc_journal_recv_end_signal(diag);
#ifdef DEBUG_JOURNAL
	gflog_debug(GFARM_MSG_1003005,
	    "journal_syncsend(%s) end", mdhost_get_name(c->host));
#endif
	return (NULL);
}

static void
gfmdc_wait_journal_recv_threads(const char *diag)
{
	gfarm_mutex_lock(&journal_sync_info.sync_mutex, diag,
	    SYNC_MUTEX_DIAG);
	while (journal_sync_info.nrecv_threads > 0)
		gfarm_cond_wait(&journal_sync_info.sync_end_cond,
		    &journal_sync_info.sync_mutex, diag,
		    SYNC_END_COND_DIAG);
	gfarm_mutex_unlock(&journal_sync_info.sync_mutex, diag,
	    SYNC_MUTEX_DIAG);
}

static int
gfmdc_journal_sync_count_host(struct mdhost *mh, void *closure)
{
	int *countp = closure;
	struct mdhost *self = mdhost_lookup_self();

	(*countp) += (mh != self) &&
	    mdhost_is_sync_replication(mh) &&
	    gfmdc_journal_mdhost_can_sync(mh) &&
	    !mdhost_is_in_first_sync(mh);
	return (1);
}

static int
gfmdc_journal_sync_mdhost_add_job(struct mdhost *mh, void *closure)
{
	int i, s = 0;
	struct gfmdc_journal_send_closure *c;
	const char diag[] = "gfmdc_journal_sync_mdhost_add_job";

	i = journal_sync_info.slave_index++;
	(void)gfmdc_journal_sync_count_host(mh, &s);
	if (s == 0)
		return (1);
	c = &journal_sync_info.closures[i];
	assert(c->data == NULL);
	gfarm_mutex_lock(&journal_sync_info.sync_mutex, diag,
	    SYNC_MUTEX_DIAG);
	++journal_sync_info.nrecv_threads;
	gfarm_mutex_unlock(&journal_sync_info.sync_mutex, diag,
	    SYNC_MUTEX_DIAG);
	gfmdc_journal_send_closure_reset(c, mh);
	thrpool_add_job(journal_sync_thread_pool, gfmdc_journal_send_thread, c);
	return (1);
}

/* PREREQUISITE: giant_lock */
static gfarm_error_t
gfmdc_journal_sync_multiple(gfarm_uint64_t seqnum)
{
	int nhosts = 0;
	static const char diag[] = "gfmdc_journal_sync_multiple";

	mdhost_foreach(gfmdc_journal_sync_count_host, &nhosts);
	if (nhosts == 0) {
		if (gfarm_get_journal_sync_file())
			return (db_journal_file_writer_sync());
		return (GFARM_ERR_NO_ERROR);
	}

	assert(journal_sync_info.nrecv_threads == 0);

	journal_sync_info.file_sync_error = GFARM_ERR_NO_ERROR;
	journal_sync_info.seqnum = seqnum;
	if (gfarm_get_journal_sync_file()) {
		journal_sync_info.nrecv_threads = 1;
		thrpool_add_job(journal_sync_thread_pool,
		    gfmdc_journal_file_sync_thread, NULL);
	} else
		journal_sync_info.nrecv_threads = 0;
	journal_sync_info.slave_index = 0;
	mdhost_foreach(gfmdc_journal_sync_mdhost_add_job, NULL);

	gfmdc_wait_journal_recv_threads(diag);

	return (journal_sync_info.file_sync_error);
}

static void*
gfmdc_journal_first_sync_thread(void *closure)
{
#define FIRST_SYNC_DELAY 1
	gfarm_error_t e;
	int do_sync, exist_recs = 1;
	struct mdhost *mh = closure;

	sleep(FIRST_SYNC_DELAY);
#ifdef DEBUG_JOURNAL
	gflog_debug(GFARM_MSG_1003006,
	    "%s : first sync start", mdhost_get_name(mh));
#endif
	giant_lock();
	do_sync = 0;
	if (mdhost_get_last_fetch_seqnum(mh) <
	    db_journal_get_current_seqnum()) {
		/* it's still unknown whether seqnum is ok or out_of_sync */
		if (!mdhost_is_in_first_sync(mh) &&
		    !mdhost_journal_file_reader_is_expired(mh))
			do_sync = 1;
	} else {
		mdhost_set_seqnum_ok(mh);
	}
	giant_unlock();

	if (!do_sync)
		return (NULL);

	giant_lock();
	mdhost_set_is_in_first_sync(mh, 1);
	giant_unlock();

	while (exist_recs) {
		if ((e = gfmdc_journal_asyncsend(mh, &exist_recs))
		    != GFARM_ERR_NO_ERROR) {
			gflog_error(GFARM_MSG_1003007,
			    "%s : %s", mdhost_get_name(mh),
			    gfarm_error_string(e));
			break;
		}
	}
#ifdef DEBUG_JOURNAL
	gflog_debug(GFARM_MSG_1003008,
	    "%s : first sync end", mdhost_get_name(mh));
#endif
	giant_lock();
	mdhost_set_is_in_first_sync(mh, 0);
	giant_unlock();
	return (NULL);
}

void
gfmdc_alloc_journal_sync_info_closures(void)
{
	int i;
	int nsvrs = mdhost_get_count();
	struct gfmdc_journal_sync_info *si = &journal_sync_info;
	struct gfmdc_journal_send_closure *c;
	static const char diag[] = "gfmdc_alloc_journal_sync_info_closures";

	if (si->closures) {
		for (i = 0; i < si->nservers; ++i) {
			c = &si->closures[i];
			gfarm_mutex_destroy(&c->send_mutex, diag,
			    SEND_MUTEX_DIAG);
			gfarm_cond_destroy(&c->send_end_cond, diag,
			    SEND_END_COND_DIAG);
		}
		free(si->closures);
	}

	GFARM_MALLOC_ARRAY(si->closures, nsvrs);
	if (si->closures == NULL)
		gflog_fatal(GFARM_MSG_1003009,
		    "%s", gfarm_error_string(GFARM_ERR_NO_MEMORY));
	for (i = 0; i < nsvrs; ++i) {
		c = &si->closures[i];
		c->data = NULL;
		gfarm_mutex_init(&c->send_mutex, diag, SEND_MUTEX_DIAG);
		gfarm_cond_init(&c->send_end_cond, diag, SEND_END_COND_DIAG);
	}

	si->nservers = nsvrs;

	if (mdhost_has_async_replication_target())
		gfarm_cond_signal(&si->async_wait_cond, diag,
			ASYNC_WAIT_COND_DIAG);
}

static void
gfmdc_sync_init(void)
{
	int thrpool_size, jobq_len;
	struct gfmdc_journal_sync_info *si = &journal_sync_info;
	static const char diag[] = "gfmdc_sync_init";

	thrpool_size = gfarm_get_metadb_server_slave_max_size()
		+ (gfarm_get_journal_sync_file() ? 1 : 0);
	jobq_len = thrpool_size + 1;

	journal_sync_thread_pool = thrpool_new(thrpool_size, jobq_len,
	    "sending and writing journal record");
	if (journal_sync_thread_pool == NULL)
		gflog_fatal(GFARM_MSG_1003010,
		    "thread pool size:%d, queue length:%d: no memory",
		    thrpool_size, jobq_len); /* exit */

	gfarm_mutex_init(&si->sync_mutex, diag, SYNC_MUTEX_DIAG);
	gfarm_cond_init(&si->sync_end_cond, diag, SYNC_END_COND_DIAG);
	gfarm_mutex_init(&si->async_mutex, diag, ASYNC_MUTEX_DIAG);
	gfarm_cond_init(&si->async_wait_cond, diag, ASYNC_WAIT_COND_DIAG);

	si->nrecv_threads = 0;
	gfmdc_alloc_journal_sync_info_closures();
	db_journal_set_sync_op(gfmdc_journal_sync_multiple);
}

void
gfmdc_init(void)
{
	gfmdc_recv_watcher = peer_watcher_alloc(
	    /* XXX FIXME use different config parameter */
	    gfarm_metadb_thread_pool_size, gfarm_metadb_job_queue_length,
	    gfmdc_main, "receiving from gfmd");

	gfmdc_send_thread_pool = thrpool_new(
	    /* XXX FIXME use different config parameter */
	    gfarm_metadb_thread_pool_size, gfarm_metadb_job_queue_length,
	    "sending to gfmd");
	if (gfmdc_send_thread_pool == NULL)
		gflog_fatal(GFARM_MSG_1003011,
		    "gfmd channel thread pool size:"
		    "%d, queue length:%d: no memory",
		    gfarm_metadb_thread_pool_size,
		    gfarm_metadb_job_queue_length);
	gfmdc_sync_init();
}
