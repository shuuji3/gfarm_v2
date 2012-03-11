#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h> /* for NI_MAXHOST */

#include <gfarm/gfarm.h>

#include "gfutil.h"
#include "queue.h"
#include "thrsubr.h"

#include "gfp_xdr.h"
#include "auth.h"
#include "gfm_proto.h"

#include "subr.h"
#include "peer.h"
#include "local_peer.h"
#include "remote_peer.h"

#include "protocol_state.h"
#include "peer_impl.h"

struct remote_peer {
	struct peer super;

	struct local_peer *parent_peer;
	struct remote_peer *next_sibling;	/* must be a remote peer */

	int port, proto_family, proto_transport; /* for gfarm_file_trace */
};

struct peer *
remote_peer_to_peer(struct remote_peer *remote_peer)
{
	return (&remote_peer->super);
}

static struct local_peer *
remote_peer_downcast_to_local_peer(struct peer *peer)
{
	gflog_fatal(GFARM_MSG_UNFIXED,
	    "downcasting remote_peer %p to local_peer", peer);
	return (NULL);
}

static struct remote_peer *
remote_peer_downcast_to_remote_peer(struct peer *peer)
{
	return ((struct remote_peer *)peer);
}

struct gfp_xdr *
remote_peer_get_conn(struct peer *peer)
{
	return (peer_get_conn(local_peer_to_peer(
	    peer_to_remote_peer(peer)->parent_peer)));
}

static gfp_xdr_async_peer_t
remote_peer_get_async(struct peer *peer)
{
	return (peer_get_async(local_peer_to_peer(
	    peer_to_remote_peer(peer)->parent_peer)));
}

static gfarm_error_t
remote_peer_get_port(struct peer *peer, int *portp)
{
	struct remote_peer *remote_peer = peer_to_remote_peer(peer);

	*portp = remote_peer->port;
	return (GFARM_ERR_NO_ERROR);
}

static struct mdhost *
remote_peer_get_mdhost(struct peer *peer)
{
	return (peer_get_mdhost(local_peer_to_peer(
	    peer_to_remote_peer(peer)->parent_peer)));
}

static struct peer *
remote_peer_get_parent(struct peer *peer)
{
	return (local_peer_to_peer(peer_to_remote_peer(peer)->parent_peer));
}

static int
remote_peer_is_busy(struct peer *peer)
{
	return (0); /* XXXRELAY: gfsd back_channel? */
}

static void
remote_peer_notice_disconnected(struct peer *peer,
	const char *hostname, const char *username)
{
	struct remote_peer *remote_peer = peer_to_remote_peer(peer);
	const char *parent_hostname =
	    peer_get_hostname(local_peer_to_peer(remote_peer->parent_peer));
	char hostbuf[NI_MAXHOST];

	if (parent_hostname == NULL) {
		local_peer_get_numeric_name(remote_peer->parent_peer,
		    hostbuf, sizeof(hostbuf));
		parent_hostname = hostbuf;
	}
		
	gflog_notice(GFARM_MSG_UNFIXED,
	    "(%s@%s@%s) disconnected",
	    username, hostname, parent_hostname);
}

static void
remote_peer_shutdown(struct peer *peer)
{
	/* XXX FIXME */
	assert(0);
}

static void
remote_peer_remove_from_children(struct remote_peer **child_peersp,
	void *closure)
{
	struct remote_peer *target = closure;
	struct remote_peer *p, **pp;

	/* XXXRELAY slow */
	for (pp = child_peersp; ; pp = &p->next_sibling) {
		p = *pp;
		assert(p != NULL);
		if (p == target) {
			*pp = p->next_sibling;
			break;
		}
	}
}

static void
remote_peer_free(struct peer *peer)
{
	struct remote_peer *remote_peer = peer_to_remote_peer(peer);
	static const char diag[] = "remote_peer_free";

	local_peer_for_child_peers(remote_peer->parent_peer,
	    remote_peer_remove_from_children, remote_peer, diag);
	
	peer_free_common(peer, diag);
	free(remote_peer); /* XXXRELAY is this safe? */
}

gfarm_error_t
remote_peer_free_by_id(struct peer *parent_peer,
	gfarm_int64_t remote_peer_id)
{
	struct remote_peer *remote_peer = local_peer_lookup_remote(
	    peer_to_local_peer(parent_peer), remote_peer_id);

	if (remote_peer != NULL)
		return (GFARM_ERR_INVALID_REMOTE_PEER);

	assert(remote_peer->parent_peer != NULL);
	giant_lock();

	remote_peer_free(&remote_peer->super);

	giant_unlock();

	return (GFARM_ERR_NO_ERROR);
}

void
remote_peer_free_simply(struct remote_peer *remote_peer)
{
	peer_free_common(&remote_peer->super, "remote_peer_free_simply");
	free(remote_peer); /* XXXRELAY is this safe? */
}

static struct peer_ops remote_peer_ops = {
	remote_peer_downcast_to_local_peer,
	remote_peer_downcast_to_remote_peer,

	remote_peer_get_conn,
	remote_peer_get_async,
	remote_peer_get_port,
	remote_peer_get_mdhost,
	remote_peer_get_parent,
	remote_peer_is_busy,
	remote_peer_notice_disconnected,
	remote_peer_shutdown,
	remote_peer_free,
};

gfarm_error_t
remote_peer_alloc(struct peer *parent_peer, gfarm_int64_t remote_peer_id,
	gfarm_int32_t auth_id_type, char *username, char *hostname,
	enum gfarm_auth_method auth_method, int proto_family,
	int proto_transport, int port)
{
	struct local_peer *parent_local_peer = peer_to_local_peer(parent_peer);
	struct remote_peer *remote_peer;
	struct peer *peer;
	static const char diag[] = "remote_peer_alloc";

	if (proto_family != GFARM_PROTO_FAMILY_IPV4 ||
	    proto_transport != GFARM_PROTO_TRANSPORT_IP_TCP ||
	    port <= 0 || port >= 0x10000)
		return (GFARM_ERR_PROTOCOL);	    

	if (local_peer_lookup_remote(parent_local_peer, remote_peer_id) != NULL)
		return (GFARM_ERR_INVALID_REMOTE_PEER);

	GFARM_MALLOC(remote_peer);
	if (remote_peer == NULL)
		return (GFARM_ERR_NO_MEMORY);

	peer = remote_peer_to_peer(remote_peer);
	peer_construct_common(peer, &remote_peer_ops, diag);
	peer->peer_id = remote_peer_id;
	/* We don't pass auth_method for now */
	peer_authorized_common(peer, auth_id_type, username, hostname, NULL,
	    auth_method);

	remote_peer->parent_peer = parent_local_peer;
	remote_peer->proto_family = proto_family;
	remote_peer->proto_transport = proto_transport;
	remote_peer->port = port;

	local_peer_add_child(parent_local_peer,
	    remote_peer, &remote_peer->next_sibling);

	return (GFARM_ERR_NO_ERROR);
}

void
remote_peer_for_each_sibling(struct remote_peer *remote_peer,
	void (*op)(struct remote_peer *))
{
	struct remote_peer *p;

	for (; remote_peer != NULL; remote_peer = p) {
		p = remote_peer->next_sibling;
		(*op)(remote_peer); /* `remote_peer' may be freed here */
	}
}

struct remote_peer *
remote_peer_id_lookup_from_siblings(struct remote_peer *remote_peer,
	gfarm_int64_t remote_peer_id)
{
	for (; remote_peer != NULL; remote_peer = remote_peer->next_sibling) {
		if (remote_peer->super.peer_id == remote_peer_id)
			return (remote_peer);
	}
	return (NULL);
}
