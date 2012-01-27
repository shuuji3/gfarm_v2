#include <pthread.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <netdb.h> /* for NI_MAXHOST, NI_NUMERICHOST, etc */
#include <sys/socket.h>
#include <netinet/in.h>

#include <gfarm/gfarm.h>

#include "gfutil.h"
#include "queue.h"
#include "thrsubr.h"

#include "gfp_xdr.h"
#include "io_fd.h"
#include "auth.h"
#include "gfnetdb.h"

#include "user.h"
#include "host.h"
#include "mdhost.h"
#include "abstract_host.h"
#include "watcher.h"
#include "peer_watcher.h"
#include "peer.h"
#include "local_peer.h"
#include "remote_peer.h"
#include "process.h"

#include "protocol_state.h"
#include "peer_impl.h"

#define BACK_CHANNEL_DIAG(peer) (peer_get_auth_id_type(peer) == \
	GFARM_AUTH_ID_TYPE_SPOOL_HOST ? "back_channel" : "gfmd_channel")

struct local_peer {
	struct peer base;

	struct gfp_xdr *conn;
	gfp_xdr_async_peer_t async; /* used by {back|gfmd}_channel */

	struct peer_watcher *readable_watcher;
	struct watcher_event *readable_event;

	struct remote_peer *child_peers;
	pthread_mutex_t child_peers_mutex;
};

static struct local_peer *local_peer_table;
static int local_peer_table_size;
static gfarm_int64_t local_peer_id = 1;
static pthread_mutex_t local_peer_table_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char local_peer_table_diag[] = "local_peer_table";

struct peer *
local_peer_to_peer(struct local_peer *local_peer)
{
	return (&local_peer->base);
}

static struct local_peer *
local_peer_downcast_to_local_peer(struct peer *peer)
{
	return ((struct local_peer *)peer);
}

static struct remote_peer *
local_peer_downcast_to_remote_peer(struct peer *peer)
{
	gflog_fatal(GFARM_MSG_UNFIXED,
	    "downcasting local_peer %p to remote_peer", peer);
	return (NULL);
}

static struct gfp_xdr *
local_peer_get_conn(struct peer *peer)
{
	return (peer_to_local_peer(peer)->conn);
}

static gfp_xdr_async_peer_t
local_peer_get_async(struct peer *peer)
{
	return (peer_to_local_peer(peer)->async);
}

static int
local_peer_get_fd(struct local_peer *local_peer)
{
	int fd = local_peer - local_peer_table;

	if (fd < 0 || fd >= local_peer_table_size)
		gflog_fatal(GFARM_MSG_1000288,
		    "local_peer_get_fd: invalid peer pointer");
	return (fd);
}

static gfarm_error_t
local_peer_get_port(struct peer *peer, int *portp)
{
	struct sockaddr_in sin;
	socklen_t slen = sizeof(sin);

	if (getpeername(local_peer_get_fd(peer_to_local_peer(peer)),
	    (struct sockaddr *)&sin, &slen) != 0) {
		return (gfarm_errno_to_error(errno));
	} else if (sin.sin_family != AF_INET) {
		return (
		    GFARM_ERR_ADDRESS_FAMILY_NOT_SUPPORTED_BY_PROTOCOL_FAMILY);
	} else {
		*portp = (int)ntohs(sin.sin_port);
		return (GFARM_ERR_NO_ERROR);
	}
}

#if 0

struct peer *
local_peer_by_fd(int fd)
{
	static const char diag[] = "local_peer_by_fd";

	gfarm_mutex_lock(&local_peer_table_mutex, diag, local_peer_table_diag);
	if (fd < 0 || fd >= local_peer_table_size ||
	    local_peer_table[fd].conn == NULL)
		return (NULL);
	gfarm_mutex_unlock(&local_peer_table_mutex, diag,
	    local_peer_table_diag);
	return (&local_peer_table[fd]);
}

/* NOTE: caller of this function should acquire giant_lock as well */
gfarm_error_t
local_peer_free_by_fd(int fd)
{
	struct peer *peer = local_peer_by_fd(fd);

	if (peer == NULL)
		return (GFARM_ERR_BAD_FILE_DESCRIPTOR);
	peer_free(peer);
	return (GFARM_ERR_NO_ERROR);
}

#endif /* 0 */

static int
local_peer_is_busy(struct peer *peer)
{
	struct local_peer *local_peer = peer_to_local_peer(peer);

	return (watcher_event_is_active(local_peer->readable_event));
}

void
local_peer_get_numeric_name(struct local_peer *local_peer,
	char *hostbuf, size_t hostlen)
{
	struct sockaddr_in sin;
	socklen_t slen = sizeof(sin);
	int err;

	if (getpeername(local_peer_get_fd(local_peer),
	    (struct sockaddr *)&sin, &slen) != 0)
		err = errno;
	else if ((err = gfarm_getnameinfo((struct sockaddr *)&sin, slen,
	    hostbuf, hostlen, NULL, 0, NI_NUMERICHOST | NI_NUMERICSERV) != 0))
		;
	else
		return;

	snprintf(hostbuf, hostlen, "<not-socket>");
	gflog_error(GFARM_MSG_1003276,
	    "unable to convert peer address to string: %s", strerror(err));
}

static void
local_peer_notice_disconnected(struct peer *peer,
	const char *hostname, const char *username)
{
	char hostbuf[NI_MAXHOST];

	if (hostname == NULL) {
		/*
		 * IP address must be logged instead of (maybe faked) hostname
		 * in case of an authentication failure.
		 */
		local_peer_get_numeric_name(peer_to_local_peer(peer),
		    hostbuf, sizeof(hostbuf));
		hostname = hostbuf;
	}

	gflog_notice(GFARM_MSG_1000286,
	    "(%s@%s) disconnected", username, hostname);
}

static void
local_peer_shutdown(struct peer *peer)
{
	int fd = local_peer_get_fd(peer_to_local_peer(peer));
	int rv = shutdown(fd, SHUT_RDWR);

	if (rv == -1)
		gflog_warning(GFARM_MSG_1002766,
		    "%s(%s) : shutdown(%d): %s", BACK_CHANNEL_DIAG(peer),
		    peer_get_hostname(peer), fd, strerror(errno));
}

/* NOTE: caller of this function should acquire giant_lock as well */
void
local_peer_shutdown_all(void)
{
	int i;
	struct peer *peer;
	static const char diag[] = "local_peer_shutdown_all";

	/* We never unlock this mutex any more */
	gfarm_mutex_lock(&local_peer_table_mutex, diag, local_peer_table_diag);

	for (i = 0; i < local_peer_table_size; i++) {
		peer = &local_peer_table[i].base;
		if (peer->process == NULL)
			continue;

		gflog_notice(GFARM_MSG_1000287, "(%s@%s) shutting down",
		    peer->username, peer->hostname);
#if 0		/* we don't really have to do this at shutdown */
		peer_unset_pending_new_generation(peer);
#endif
		process_detach_peer(peer->process, peer);
		peer->process = NULL;
	}
}

static void
local_peer_free(struct peer *peer)
{
	struct local_peer *local_peer = peer_to_local_peer(peer);
	static const char diag[] = "local_peer_free";

	gfarm_mutex_lock(&local_peer->child_peers_mutex,
	    diag, "child_peers_mutex");
	remote_peer_for_each_sibling(local_peer->child_peers,
	    remote_peer_free_simply);
	gfarm_mutex_unlock(&local_peer->child_peers_mutex,
	    diag, "child_peers_mutex");

	gfarm_mutex_lock(&local_peer_table_mutex, diag, local_peer_table_diag);

	peer_free_common(peer, diag);

	if (local_peer->async != NULL) {
		/* needs giant_lock and peer_table_lock */
		gfp_xdr_async_peer_free(local_peer->async, peer);
		local_peer->async = NULL;
	}
	if (local_peer->conn != NULL) {
		gfp_xdr_free(local_peer->conn);
		local_peer->conn = NULL;
	}

	local_peer->child_peers = NULL;
	local_peer->readable_watcher = NULL;
	/* We don't free peer->readable_event. */

	gfarm_mutex_unlock(&local_peer_table_mutex,
	    diag, local_peer_table_diag);
}

static struct peer_ops local_peer_ops = {
	local_peer_downcast_to_local_peer,
	local_peer_downcast_to_remote_peer,

	local_peer_get_conn,
	local_peer_get_async,
	local_peer_get_port,
	local_peer_is_busy,
	local_peer_notice_disconnected,
	local_peer_shutdown,
	local_peer_free,
};

static gfarm_error_t
local_peer_alloc0(int fd, struct gfp_xdr *conn,
	struct local_peer **local_peerp)
{
	gfarm_error_t e;
	struct local_peer *local_peer;
	int sockopt;
	static const char diag[] = "local_peer_alloc";

	if (fd < 0) {
		gflog_debug(GFARM_MSG_1001580,
			"invalid argument 'fd'");
		return (GFARM_ERR_INVALID_ARGUMENT);
	}
	if (fd >= local_peer_table_size) {
		gflog_debug(GFARM_MSG_1001581,
			"too many open files: fd >= local_peer_table_size");
		return (GFARM_ERR_TOO_MANY_OPEN_FILES);
	}

	/* always deal with reboots or network problems */
	sockopt = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &sockopt, sizeof(sockopt))
	    == -1)
		gflog_warning_errno(GFARM_MSG_1000283, "SO_KEEPALIVE");

	gfarm_mutex_lock(&local_peer_table_mutex, diag, local_peer_table_diag);
	local_peer = &local_peer_table[fd];
	if (local_peer->conn != NULL) { /* must be an implementation error */
		gfarm_mutex_unlock(&local_peer_table_mutex, diag,
		    local_peer_table_diag);
		gflog_debug(GFARM_MSG_1001582,
			"bad file descriptor: conn is not NULL");
		return (GFARM_ERR_BAD_FILE_DESCRIPTOR);
	}

	/* XXX FIXME gfp_xdr requires too much memory */
	if (conn == NULL) {
		e = gfp_xdr_new_socket(fd, &conn);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001583,
			    "gfp_xdr_new_socket() failed: %s",
			    gfarm_error_string(e));
			gfarm_mutex_unlock(&local_peer_table_mutex, diag,
			    local_peer_table_diag);
			return (e);
		}
	}
	local_peer->conn = conn;

	local_peer->async = NULL; /* synchronous protocol by default */

	if (local_peer->readable_event == NULL) {
		e = watcher_fd_readable_event_alloc(fd,
		    &local_peer->readable_event);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_warning(GFARM_MSG_1002767,
			    "peer watching %d: %s", fd, gfarm_error_string(e));
			gfp_xdr_free(local_peer->conn);
			local_peer->conn = NULL;
			gfarm_mutex_unlock(&local_peer_table_mutex, diag,
			    local_peer_table_diag);
			return (e);
		}
	}

	peer_clear_common(&local_peer->base);

	/*
	 * to support remote peer
	 */
	local_peer->child_peers = NULL;
	local_peer->base.peer_id = local_peer_id++;

	*local_peerp = local_peer;

	gfarm_mutex_unlock(&local_peer_table_mutex, diag,
	    local_peer_table_diag);
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
local_peer_alloc(int fd, struct local_peer **local_peerp)
{
	return (local_peer_alloc0(fd, NULL, local_peerp));
}

gfarm_error_t
local_peer_alloc_with_connection(
	struct gfp_xdr *conn, struct abstract_host *host, int id_type,
	struct peer **peerp)
{
	gfarm_error_t e;
	struct local_peer *local_peer;

	if ((e = local_peer_alloc0(gfp_xdr_fd(conn), conn, &local_peer))
	    == GFARM_ERR_NO_ERROR) {
		*peerp = &local_peer->base;
		(*peerp)->host = host;
		(*peerp)->id_type = id_type;
	}
	return (e);
}

/* caller should allocate the storage for username and hostname */
void
local_peer_authorized(struct local_peer *local_peer,
	enum gfarm_auth_id_type id_type, char *username, char *hostname,
	struct sockaddr *addr, enum gfarm_auth_method auth_method,
	struct peer_watcher *readable_watcher)
{
	struct peer *peer = &local_peer->base;
	struct host *h;
	struct mdhost *m;

	peer->id_type = id_type;
	peer->user = NULL;
	peer->username = username;

	switch (id_type) {
	case GFARM_AUTH_ID_TYPE_USER:
		peer->user = user_lookup(username);
		if (peer->user != NULL) {
			free(username);
			peer->username = NULL;
		} else
			peer->username = username;
		/*FALLTHROUGH*/

	case GFARM_AUTH_ID_TYPE_SPOOL_HOST:
		h = host_addr_lookup(hostname, addr);
		if (h == NULL) {
			peer->host = NULL;
		} else {
			peer->host = host_to_abstract_host(h);
		}
		break;

	case GFARM_AUTH_ID_TYPE_METADATA_HOST:
		m = mdhost_lookup(hostname);
		if (m == NULL) {
			peer->host = NULL;
		} else {
			peer->host = mdhost_to_abstract_host(m);
		}
		break;
	}

	if (peer->host != NULL) {
		free(hostname);
		peer->hostname = NULL;
	} else {
		peer->hostname = hostname;
	}

	switch (id_type) {
	case GFARM_AUTH_ID_TYPE_SPOOL_HOST:
	case GFARM_AUTH_ID_TYPE_METADATA_HOST:
		if (peer->host == NULL)
			gflog_warning(GFARM_MSG_1000284,
			    "unknown host: %s", hostname);
		else
			gflog_debug(GFARM_MSG_1002768,
			    "%s connected from %s",
			    peer_get_service_name(peer),
			    abstract_host_get_name(peer->host));
		break;
	default:
		break;
	}
	/* We don't record auth_method for now */

	local_peer->readable_watcher = readable_watcher;

	if (gfp_xdr_recv_is_ready(local_peer->conn))
		peer_watcher_schedule(readable_watcher, local_peer);
	else
		local_peer_watch_readable(local_peer);
}

void
local_peer_set_async(struct local_peer *local_peer, gfp_xdr_async_peer_t async)
{
	local_peer->async = async;
}

void
local_peer_set_readable_watcher(struct local_peer *local_peer,
	struct peer_watcher *readable_watcher)
{
	local_peer->readable_watcher = readable_watcher;
}

void
local_peer_readable_invoked(struct local_peer *local_peer)
{
	watcher_event_ack(local_peer->readable_event);

	peer_closer_wakeup(&local_peer->base);
}

void
local_peer_watch_readable(struct local_peer *local_peer)
{
	peer_watcher_add_event(local_peer->readable_watcher,
	    local_peer->readable_event, local_peer);
}

struct remote_peer *
local_peer_lookup_remote(struct local_peer *parent_peer,
	gfarm_int64_t remote_peer_id)
{
	struct remote_peer *remote_peer;
	static const char diag[] = "local_peer_lookup_remote";

	gfarm_mutex_lock(&parent_peer->child_peers_mutex,
	    diag, "child_peers_mutex");
	remote_peer = remote_peer_id_lookup_from_siblings(
	    parent_peer->child_peers, remote_peer_id);
	gfarm_mutex_unlock(&parent_peer->child_peers_mutex,
	    diag, "child_peers_mutex");

	return (remote_peer);
}

void
local_peer_add_child(struct local_peer *parent_peer,
	struct remote_peer *remote_peer, struct remote_peer **next_siblingp)
{
	static const char diag[] = "local_peer_add_child";

	gfarm_mutex_lock(&parent_peer->child_peers_mutex,
	    diag, "child_peers_mutex");

	*next_siblingp = parent_peer->child_peers;
	parent_peer->child_peers = remote_peer;

	gfarm_mutex_unlock(&parent_peer->child_peers_mutex,
	    diag, "child_peers_mutex");
}

void
local_peer_for_child_peers(struct local_peer *parent_peer,
	void (*op)(struct remote_peer **, void *), void *closure,
	const char *diag)
{
	gfarm_mutex_lock(&parent_peer->child_peers_mutex,
	    diag, "child_peers_mutex");
	(*op)(&parent_peer->child_peers, closure);
	gfarm_mutex_unlock(&parent_peer->child_peers_mutex,
	    diag, "child_peers_mutex");
}

void
local_peer_init(int max_peers)
{
	int i;
	struct local_peer *local_peer;
	static const char diag[] = "local_peer_init";

	GFARM_MALLOC_ARRAY(local_peer_table, max_peers);
	if (local_peer_table == NULL)
		gflog_fatal(GFARM_MSG_1000278,
		    "peer table: %s", strerror(ENOMEM));
	local_peer_table_size = max_peers;

	for (i = 0; i < local_peer_table_size; i++) {
		local_peer = &local_peer_table[i];

		peer_construct_common(&local_peer->base,
		    &local_peer_ops, diag);

		local_peer->conn = NULL;
		local_peer->async = NULL;

		local_peer->readable_watcher = NULL;
		local_peer->readable_event = NULL;

		/*
		 * to support remote peer
		 */
		local_peer->child_peers = NULL;
		gfarm_mutex_init(&local_peer->child_peers_mutex,
		    diag, "peer:child_peers_mutex");
		local_peer->base.peer_id = 0;
	}
}
