/*
 * $Id$
 */

#include <pthread.h>
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#include <gfarm/gfarm.h>

#include "gfutil.h"
#include "hash.h"
#include "thrsubr.h"

#include "auth.h"
#include "gfp_xdr.h"
#include "config.h"

#include "gfm_client.h"
#include "gfm_proto.h"
#include "filesystem.h"

#include "subr.h"
#include "rpcsubr.h"
#include "host.h"
#include "user.h"
#include "peer.h"
#include "abstract_host.h"
#include "metadb_server.h"
#include "mdhost.h"
#include "mdcluster.h"
#include "journal_file.h"
#include "db_access.h"
#include "db_journal.h"
#include "gfmd_channel.h"

/* in-core gfarm_metadb_server */
struct mdhost {
	struct abstract_host ah; /* must be the first member of this struct */

	struct gfarm_metadb_server ms;
	pthread_mutex_t mutex;
	struct gfm_connection *conn;
	int is_recieved_seqnum, is_in_first_sync;
	struct journal_file_reader *jreader;
	gfarm_uint64_t last_fetch_seqnum;
	struct mdcluster *cluster;
};

static struct gfarm_hash_table *mdhost_hashtab;
static int localhost_is_readonly = 0;
static struct mdhost *mdhost_self;

static const char MDHOST_GLOBAL_MUTEX_DIAG[]	= "mdhost_global_mutex";
pthread_mutex_t mdhost_global_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char MDHOST_MUTEX_DIAG[]		= "mdhost_mutex";

#define MDHOST_HASHTAB_SIZE	31

#define FOREACH_MDHOST(it) \
	for (gfarm_hash_iterator_begin(mdhost_hashtab, &(it)); \
	     !gfarm_hash_iterator_is_end(&(it)); \
	     gfarm_hash_iterator_next(&(it)))

static void
mdhost_mutex_lock(struct mdhost *m, const char *diag)
{
	gfarm_mutex_lock(&m->mutex, diag, MDHOST_MUTEX_DIAG);
}

static void
mdhost_mutex_unlock(struct mdhost *m, const char *diag)
{
	gfarm_mutex_unlock(&m->mutex, diag, MDHOST_MUTEX_DIAG);
}

static void
mdhost_global_mutex_lock(const char *diag)
{
	gfarm_mutex_lock(&mdhost_global_mutex, diag, MDHOST_GLOBAL_MUTEX_DIAG);
}

static void
mdhost_global_mutex_unlock(const char *diag)
{
	gfarm_mutex_unlock(&mdhost_global_mutex, diag,
		MDHOST_GLOBAL_MUTEX_DIAG);
}

const char *
mdhost_get_name(struct mdhost *m)
{
	return (gfarm_metadb_server_get_name(&m->ms));
}

int
mdhost_is_master(struct mdhost *m)
{
	int is_master;
	static const char *diag = "mdhost_is_master";

	mdhost_mutex_lock(m, diag);
	is_master = gfarm_metadb_server_is_master(&m->ms);
	mdhost_mutex_unlock(m, diag);
	return (is_master);
}

void
mdhost_set_is_master(struct mdhost *m, int enable)
{
	static const char *diag = "mdhost_set_is_master";

	mdhost_mutex_lock(m, diag);
	gfarm_metadb_server_set_is_master(&m->ms, enable);
	mdhost_mutex_unlock(m, diag);
}

int
mdhost_is_self(struct mdhost *m)
{
	return (gfarm_metadb_server_is_self(&m->ms));
}

struct abstract_host *
mdhost_to_abstract_host(struct mdhost *m)
{
	return (&m->ah);
}

static struct host *
mdhost_downcast_to_host(struct abstract_host *h)
{
	gflog_error(GFARM_MSG_1002925, "downcasting mdhost %p to host", h);
	abort();
	return (NULL);
}

static struct mdhost *
mdhost_downcast_to_mdhost(struct abstract_host *h)
{
	return ((struct mdhost *)h);
}

static const char *
mdhost_name0(struct abstract_host *h)
{
	return (mdhost_get_name(abstract_host_to_mdhost(h)));
}

int
mdhost_get_port(struct mdhost *m)
{
	return (gfarm_metadb_server_get_port(&m->ms));
}

static int
mdhost_port0(struct abstract_host *h)
{
	return (mdhost_get_port(abstract_host_to_mdhost(h)));
}

struct peer *
mdhost_get_peer(struct mdhost *m)
{
	return (abstract_host_get_peer(mdhost_to_abstract_host(m),
	    MDHOST_MUTEX_DIAG));
}

int
mdhost_is_up(struct mdhost *m)
{
	return (abstract_host_is_up(mdhost_to_abstract_host(m)));
}

void
mdhost_activate(struct mdhost *m)
{
	abstract_host_activate(mdhost_to_abstract_host(m), MDHOST_MUTEX_DIAG);
}

static int
mdhost_is_valid(struct mdhost *m)
{
	return (abstract_host_is_valid(mdhost_to_abstract_host(m),
	    MDHOST_MUTEX_DIAG));
}

void
mdhost_set_peer(struct mdhost *m, struct peer *peer, int version)
{
	abstract_host_set_peer(mdhost_to_abstract_host(m), peer, version);
}

static struct mdhost *
mdhost_iterator_access(struct gfarm_hash_iterator *it)
{
	return (*(struct mdhost **)gfarm_hash_entry_data(
	    gfarm_hash_iterator_access(it)));
}

void
mdhost_foreach(int (*func)(struct mdhost *, void *), void *closure)
{
	struct gfarm_hash_iterator it;
	struct mdhost *m;
	struct mdhost *self = mdhost_lookup_self();

	FOREACH_MDHOST(it) {
		m = mdhost_iterator_access(&it);
		if (mdhost_is_valid(m) && m != self && func(m, closure) == 0)
			break;
	}
}

struct gfm_connection *
mdhost_get_connection(struct mdhost *m)
{
	struct gfm_connection *conn;
	static const char diag[] = "mdhost_get_connection";

	mdhost_mutex_lock(m, diag);
	conn = m->conn;
	mdhost_mutex_unlock(m, diag);
	return (conn);
}

void
mdhost_set_connection(struct mdhost *m, struct gfm_connection *conn)
{
	static const char diag[] = "mdhost_set_connection";

	mdhost_mutex_lock(m, diag);
	m->conn = conn;
	mdhost_mutex_unlock(m, diag);
}

int
mdhost_is_default_master(struct mdhost *m)
{
	return (gfarm_metadb_server_is_default_master(&m->ms));
}

void
mdhost_set_is_default_master(struct mdhost *m, int enable)
{
	gfarm_metadb_server_set_is_default_master(&m->ms, enable);
}

struct mdcluster *
mdhost_get_cluster(struct mdhost *m)
{
	return (m->cluster);
}

void
mdhost_set_cluster(struct mdhost *m, struct mdcluster *c)
{
	m->cluster = c;
}

const char *
mdhost_get_cluster_name(struct mdhost *m)
{
	return (m->ms.clustername);
}

struct journal_file_reader *
mdhost_get_journal_file_reader(struct mdhost *m)
{
	struct journal_file_reader *reader;
	static const char *diag = "mdhost_get_journal_file_reader";

	mdhost_mutex_lock(m, diag);
	reader = m->jreader;
	mdhost_mutex_unlock(m, diag);
	return (reader);
}

void
mdhost_set_journal_file_reader(struct mdhost *m,
	struct journal_file_reader *reader)
{
	static const char *diag = "mdhost_set_journal_file_reader";

	mdhost_mutex_lock(m, diag);
	m->jreader = reader;
	mdhost_mutex_unlock(m, diag);
}

gfarm_uint64_t
mdhost_get_last_fetch_seqnum(struct mdhost *m)
{
	gfarm_uint64_t r;
	static const char *diag = "mdhost_get_last_fetch_seqnum";

	mdhost_mutex_lock(m, diag);
	r = m->last_fetch_seqnum;
	mdhost_mutex_unlock(m, diag);
	return (r);
}

void
mdhost_set_last_fetch_seqnum(struct mdhost *m, gfarm_uint64_t seqnum)
{
	static const char *diag = "mdhost_set_last_fetch_seqnum";

	mdhost_mutex_lock(m, diag);
	m->last_fetch_seqnum = seqnum;
	mdhost_mutex_unlock(m, diag);
}

int
mdhost_is_recieved_seqnum(struct mdhost *m)
{
	int r;
	static const char *diag = "mdhost_is_recieved_seqnum";

	mdhost_mutex_lock(m, diag);
	r = m->is_recieved_seqnum;
	mdhost_mutex_unlock(m, diag);
	return (r);
}

void
mdhost_set_is_recieved_seqnum(struct mdhost *m, int flag)
{
	static const char *diag = "mdhost_set_is_recieved_seqnum";

	mdhost_mutex_lock(m, diag);
	m->is_recieved_seqnum = flag;
	mdhost_mutex_unlock(m, diag);
}

int
mdhost_is_in_first_sync(struct mdhost *m)
{
	int r;
	static const char *diag = "mdhost_is_in_first_sync";

	mdhost_mutex_lock(m, diag);
	r = m->is_in_first_sync;
	mdhost_mutex_unlock(m, diag);
	return (r);
}

void
mdhost_set_is_in_first_sync(struct mdhost *m, int flag)
{
	static const char *diag = "mdhost_set_is_in_first_sync";

	mdhost_mutex_lock(m, diag);
	m->is_in_first_sync = flag;
	mdhost_mutex_unlock(m, diag);
}

static void
mdhost_self_change_to_readonly(void)
{
	localhost_is_readonly = 1;
	gflog_warning(GFARM_MSG_1002926,
	    "changed to read-only mode");
}

static void
mdhost_invalidate(struct mdhost *m)
{
	abstract_host_invalidate(mdhost_to_abstract_host(m));
}

int
mdhost_self_is_master_candidate(void)
{
	return (gfarm_metadb_server_is_master_candidate(
		&mdhost_lookup_self()->ms));
}

static void
mdhost_validate(struct mdhost *m)
{
	abstract_host_validate(mdhost_to_abstract_host(m));
}

static void
mdhost_set_peer_locked(struct abstract_host *h, struct peer *peer)
{
}

static void
mdhost_set_peer_unlocked(struct abstract_host *h, struct peer *peer)
{
}

static void
mdhost_unset_peer(struct abstract_host *h, struct peer *peer)
{
}

static gfarm_error_t
mdhost_disable(struct abstract_host *h, void **closurep)
{
	return (GFARM_ERR_NO_ERROR);
}

static void
mdhost_disabled(struct abstract_host *h, struct peer *peer, void *closure)
{
	struct mdhost *m;
	struct gfm_connection *conn;

	if (!gfarm_get_metadb_replication_enabled())
		return;

	m = abstract_host_to_mdhost(h);
	conn = mdhost_get_connection(m);

	if (conn) {
		gfm_client_connection_unset_conn(conn);
		gfm_client_connection_free(conn);
		mdhost_set_connection(m, NULL);
		peer_invoked(peer);
	}
	m->is_recieved_seqnum = 0;
	if (m->jreader)
		journal_file_reader_close(m->jreader);
}

struct abstract_host_ops mdhost_ops = {
	mdhost_downcast_to_host,
	mdhost_downcast_to_mdhost,
	mdhost_name0,
	mdhost_port0,
	mdhost_set_peer_locked,
	mdhost_set_peer_unlocked,
	mdhost_unset_peer,
	mdhost_disable,
	mdhost_disabled,
};

static struct mdhost *
mdhost_new(struct gfarm_metadb_server *ms)
{
	struct mdhost *m;
	static const char *diag = "mdhost_new";

	if ((m = malloc(sizeof(struct mdhost))) == NULL)
		return (NULL);
	abstract_host_init(&m->ah, &mdhost_ops, diag);
	m->ms = *ms;
	gfarm_mutex_init(&m->mutex, diag, MDHOST_MUTEX_DIAG);
	mdhost_validate(m);
	m->conn = NULL;
	m->jreader = NULL;
	m->last_fetch_seqnum = 0;
	m->is_recieved_seqnum = 0;
	m->is_in_first_sync = 0;
	m->cluster = NULL;

	return (m);
}

static struct mdhost *
mdhost_lookup_internal(const char *hostname)
{
	struct gfarm_hash_entry *entry;

	entry = gfarm_hash_lookup(mdhost_hashtab, &hostname,
		sizeof(hostname));
	if (entry == NULL)
		return (NULL);
	return (*(struct mdhost **)gfarm_hash_entry_data(entry));
}

struct mdhost *
mdhost_lookup(const char *hostname)
{
	struct mdhost *m = mdhost_lookup_internal(hostname);

	return (m && mdhost_is_valid(m) ? m : NULL);
}

struct mdhost *
mdhost_lookup_metadb_server(struct gfarm_metadb_server *ms)
{
	struct gfarm_hash_iterator it;
	struct mdhost *m, *mm = NULL;
	static const char diag[] = "mdhost_lookup_metadb_server";

	mdhost_global_mutex_lock(diag);
	FOREACH_MDHOST(it) {
		m = mdhost_iterator_access(&it);
		if (mdhost_is_valid(m) && &m->ms == ms) {
			mm = m;
			break;
		}
	}
	mdhost_global_mutex_unlock(diag);
	return (mm);
}

struct mdhost *
mdhost_lookup_master(void)
{
	struct gfarm_hash_iterator it;
	struct mdhost *m, *mm = NULL;
	static const char diag[] = "mdhost_lookup_master";

	mdhost_global_mutex_lock(diag);
	FOREACH_MDHOST(it) {
		m = mdhost_iterator_access(&it);
		if (mdhost_is_valid(m) && mdhost_is_master(m)) {
			mm = m;
			break;
		}
	}
	if (mm == NULL)
		abort();
	mdhost_global_mutex_unlock(diag);
	return (mm);
}


struct mdhost *
mdhost_lookup_self(void)
{
	return (mdhost_self);
}

int
mdhost_self_is_master(void)
{
	struct mdhost *m = mdhost_lookup_self();

	return (mdhost_is_master(m));
}

/* giant_lock should be held before calling this */
void
mdhost_disconnect(struct mdhost *m, struct peer *peer)
{
	if (abstract_host_get_peer_unlocked(mdhost_to_abstract_host(m)) != NULL)
		gflog_warning(GFARM_MSG_1002927,
		    "disconnect gfmd %s", mdhost_get_name(m));
	return (abstract_host_disconnect(&m->ah, peer, MDHOST_MUTEX_DIAG));
}

void
mdhost_set_self_as_master(void)
{
	struct gfarm_hash_iterator it;
	struct mdhost *m, *s = mdhost_lookup_self();

	FOREACH_MDHOST(it) {
		m = mdhost_iterator_access(&it);
		if (!mdhost_is_valid(m))
			continue;
		if (mdhost_is_master(m))
			mdhost_disconnect(m, NULL);
		gfarm_metadb_server_set_is_master(&m->ms, m == s);
	}
	localhost_is_readonly = 0;
}

int
mdhost_self_is_readonly(void)
{
	return (localhost_is_readonly);
}

gfarm_error_t
mdhost_enter(struct gfarm_metadb_server *ms, struct mdhost **mpp)
{
	struct gfarm_hash_entry *entry;
	int created;
	struct mdhost *mh;
	gfarm_error_t e;

	mh = mdhost_lookup_internal(ms->name);
	if (mh) {
		if (mdhost_is_valid(mh))
			return (GFARM_ERR_ALREADY_EXISTS);

		mdhost_validate(mh);
		if (mpp)
			*mpp = mh;
		/* copy ms to mh except name */
		free(ms->name);
		ms->name = mh->ms.name;
		if (gfarm_get_metadb_replication_enabled())
			free(mh->ms.clustername);
		mh->ms = *ms;
		return (GFARM_ERR_NO_ERROR);
	}

	mh = mdhost_new(ms);
	if (mh == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_error(GFARM_MSG_1002928,
		    "%s", gfarm_error_string(e));
		return (e);
	}
	if (gfarm_get_metadb_replication_enabled() &&
	    (e = mdcluster_get_or_create_by_mdhost(mh)) != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1002929,
		    "%s", gfarm_error_string(e));
		free(mh);
		return (e);
	}
	entry = gfarm_hash_enter(mdhost_hashtab,
	    &mh->ms.name, sizeof(mh->ms.name),
	    sizeof(struct mdhost *), &created);
	if (entry == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_error(GFARM_MSG_1002930,
		    "%s", gfarm_error_string(e));
		free(mh);
		return (e);
	}
	if (!created) {
		gflog_debug(GFARM_MSG_1002931,
		    "Entry %s already exists", ms->name);
		free(mh);
		return (GFARM_ERR_ALREADY_EXISTS);
	}
	*(struct mdhost **)gfarm_hash_entry_data(entry) = mh;

	if (mpp)
		*mpp = mh;
	return (GFARM_ERR_NO_ERROR);
}

int
mdhost_is_sync_replication(struct mdhost *mh)
{
	struct mdhost *mmh = mdhost_lookup_master();

	assert(mh != mmh);
	return (mh->cluster == mmh->cluster);
}

int
mdhost_get_flags(struct mdhost *mh)
{
	return (mh->ms.flags);
}

int
mdhost_has_async_replication_target(void)
{
	struct gfarm_hash_iterator it;
	struct mdhost *mh;
	struct mdhost *mmh = mdhost_lookup_master();

	if (mdhost_get_count() == 1)
		return (0);
	FOREACH_MDHOST(it) {
		mh = mdhost_iterator_access(&it);
		if (!mdhost_is_valid(mh))
			continue;
		if (mh != mmh && !mdhost_is_sync_replication(mh))
			return (1);
	}
	return (0);
}

void
mdhost_add_one(void *closure, struct gfarm_metadb_server *ms)
{
	gfarm_error_t e = mdhost_enter(ms, NULL);

	if (e != GFARM_ERR_NO_ERROR)
		gflog_warning(GFARM_MSG_1002932,
		    "mdhost_add_one: %s", gfarm_error_string(e));
}

gfarm_error_t
mdhost_modify_in_cache(struct mdhost *mh, struct gfarm_metadb_server *ms)
{
	int cluster_changed = strcmp(mh->ms.clustername, ms->clustername) != 0;
	static const char diag[] = "mdhost_modify_in_cache";

	if (cluster_changed)
		mdcluster_remove_mdhost(mh);
	free(mh->ms.clustername);
	mh->ms.clustername = strdup_ck(ms->clustername, diag);
	mh->ms.port = ms->port;
	mh->ms.flags = ms->flags;
	if (cluster_changed)
		return (mdcluster_get_or_create_by_mdhost(mh));
	return (GFARM_ERR_NO_ERROR);
}

/* PREREQUISITE: giant_lock */
gfarm_error_t
mdhost_remove_in_cache(const char *name)
{
	struct mdhost *m;
	gfarm_error_t e;

	m = mdhost_lookup(name);
	if (m == NULL) {
		e = GFARM_ERR_NO_SUCH_OBJECT;
		gflog_debug(GFARM_MSG_1002933,
		    "%s: %s", gfarm_error_string(e), name);
		return (e);
	}
	if (m->conn)
		mdhost_disconnect(m, NULL);
	mdcluster_remove_mdhost(m);
	mdhost_invalidate(m);

	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
metadb_server_reply(struct mdhost *m, struct peer *peer)
{
	struct gfarm_metadb_server *ms, tms;
	struct gfp_xdr *xdr = peer_get_conn(peer);

	ms = &m->ms;
	tms.tflags = ms->tflags;
	if (!mdhost_is_master(m) && mdhost_is_sync_replication(m))
		gfarm_metadb_server_set_is_sync_replication(&tms, 1);
	if (mdhost_is_up(m))
		gfarm_metadb_server_set_is_active(&tms, 1);

	return (gfp_xdr_send(xdr, "sisii",
	    ms->name, ms->port, ms->clustername ? ms->clustername : "",
	    ms->flags, tms.tflags));
}

int
mdhost_get_count(void)
{
	struct gfarm_hash_iterator it;
	int n = 0;

	FOREACH_MDHOST(it) {
		if (mdhost_is_valid(mdhost_iterator_access(&it)))
			++n;
	}
	return (n);
}

static gfarm_error_t
metadb_server_get0(struct peer *peer, int (*match_op)(
	struct mdhost *, void *), void *closure, const char *diag)
{
	gfarm_error_t e, e2;
	gfarm_int32_t nhosts, nmatch, i;
	struct gfarm_hash_iterator it;
	struct mdhost *mh, **match;

	nhosts = mdhost_get_count();
	assert(nhosts > 0); /* self host must be exist */

	GFARM_MALLOC_ARRAY(match, nhosts);
	nmatch = 0;

	if (match == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_error(GFARM_MSG_1002934,
		    "%s", gfarm_error_string(e));
	} else {
		i = 0;
		FOREACH_MDHOST(it) {
			if (i >= nhosts) /* always false due to giant_lock */
				break;
			mh = mdhost_iterator_access(&it);
			if (mdhost_is_valid(mh) && match_op(mh, closure))
				match[i++] = mh;
		}
		nmatch = i;
		e = GFARM_ERR_NO_ERROR;
	}
	e2 = gfm_server_put_reply(peer, diag, e, "i", nmatch);
	if (e2 != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002935,
		    "%s: gfm_server_put_reply: %s",
		    diag, gfarm_error_string(e2));
	} else if (e == GFARM_ERR_NO_ERROR) {
		i = 0;
		for (i = 0; i < nmatch; ++i) {
			mh = match[i];
			if ((e2 = metadb_server_reply(mh, peer))
			    != GFARM_ERR_NO_ERROR) {
				gflog_debug(GFARM_MSG_1002936,
				    "%s: metadb_server_reply: %s",
				    diag, gfarm_error_string(e));
				break;
			}
		}
	}
	free(match);

	return (e2);
}

static gfarm_error_t
metadb_server_get(struct peer *peer, int (*match_op)(
	struct mdhost *, void *), void *closure, const char *diag)
{
	gfarm_error_t e;

	if (!gfarm_get_metadb_replication_enabled()) {
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		(void)gfm_server_put_reply(peer, diag, e, "");
		gflog_debug(GFARM_MSG_1002937,
		    "%s: gfm_server_put_reply: %s",
		    diag, gfarm_error_string(e));
		return (e);
	}

	giant_lock();
	e = metadb_server_get0(peer, match_op, closure, diag);
	giant_unlock();

	return (e);
}

static int
match_all(struct mdhost *mh, void *closure)
{
	return (1);
}

static int
match_hostname(struct mdhost *mh, void *closure)
{
	return (strcmp(mdhost_get_name(mh), (char *)closure) == 0);
}

gfarm_error_t
gfm_server_metadb_server_get(struct peer *peer, int from_client, int skip)
{
	gfarm_error_t e;
	char *name;
	static const char diag[] = "GFM_PROTO_METADB_SERVER_GET";

	if ((e = gfm_server_get_request(peer, diag, "s", &name))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002938,
		    "%s: get_request failure: %s",
		    diag, gfarm_error_string(e));
	}
	if (skip) {
		e = GFARM_ERR_NO_ERROR;
		goto end;
	}
	e = metadb_server_get(peer, match_hostname, name, diag);
end:
	free(name);
	return (e);
}

#ifdef DEBUG_MDCLUSTER
static int
mdcluster_dump(struct mdcluster *c, void *closure)
{
	gflog_debug(0, "cluster=%s", mdcluster_get_name(c));
	return (1);
}
#endif

gfarm_error_t
gfm_server_metadb_server_get_all(struct peer *peer, int from_client, int skip)
{
	static const char diag[] = "GFM_PROTO_METADB_SERVER_GET_ALL";

	if (skip)
		return (GFARM_ERR_NO_ERROR);
#ifdef DEBUG_CLUSTER
	mdcluster_foreach(mdcluster_dump, NULL);
#endif
	return (metadb_server_get(peer, match_all, NULL, diag));
}

static gfarm_error_t
metadb_server_recv(struct peer *peer, struct gfarm_metadb_server *ms)
{
	gfarm_error_t e;
	static const char diag[] = "metadb_server_recv";

	if ((e = gfm_server_get_request(peer, diag, "sisi",
	    &ms->name, &ms->port, &ms->clustername, &ms->flags))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002939,
		    "get_request failure: %s",
		    gfarm_error_string(e));
	}
	return (e);
}

static gfarm_error_t
metadb_server_verify(struct gfarm_metadb_server *ms, const char *diag)
{
	if (ms->name == NULL || strlen(ms->name) == 0) {
		gflog_debug(GFARM_MSG_1002940, "%s: name is empty",
		    diag);
		return (GFARM_ERR_INVALID_ARGUMENT);
	}
	if (strlen(ms->name) > GFARM_HOST_NAME_MAX) {
		gflog_debug(GFARM_MSG_1002941, "%s: too long hostname: %s",
		    diag, ms->name);
		return (GFARM_ERR_INVALID_ARGUMENT);
	}
	if (ms->clustername &&
	    strlen(ms->clustername) > GFARM_CLUSTER_NAME_MAX) {
		gflog_debug(GFARM_MSG_1002942,
		    "%s: %s: too long clustername: %s",
		    diag, ms->name, ms->clustername);
		return (GFARM_ERR_INVALID_ARGUMENT);
	}
	if (ms->port <= 0 || ms->port >= 65536) {
		gflog_debug(GFARM_MSG_1002943,
		    "%s: %s: invalid port number: %d",
		    diag, ms->name, ms->port);
		return (GFARM_ERR_INVALID_ARGUMENT);
	}
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
mdhost_fix_default_master(struct mdhost *new_mmh, const char *diag)
{
	gfarm_error_t e;
	struct gfarm_hash_iterator it;
	struct mdhost *mh;
	struct gfarm_metadb_server *ms;

	FOREACH_MDHOST(it) {
		mh = mdhost_iterator_access(&it);
		if (!mdhost_is_valid(mh))
			continue;
		if (mh == new_mmh || !mdhost_is_default_master(mh))
			continue;
		mdhost_set_is_default_master(mh, 0);
		ms = db_mdhost_dup(&mh->ms, sizeof(*ms));
		if ((e = db_mdhost_modify(ms, 0)) != GFARM_ERR_NO_ERROR) {
			gflog_error(GFARM_MSG_1002944,
			    "%s: db_mdhost_modify failed: %s", diag,
			    gfarm_error_string(e));
			return (e);
		}
	}
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
mdhost_updated(void)
{
	gfarm_error_t e;
	int i, n = mdhost_get_count();
	struct mdhost *mh;
	struct gfarm_metadb_server **mss;
	struct gfarm_filesystem *fs;
	struct gfarm_hash_iterator it;

	GFARM_MALLOC_ARRAY(mss, n);
	if (mss == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_debug(GFARM_MSG_1002945,
		    "mdhost_updated: %s", gfarm_error_string(e));
		return (e);
	}
	i = 0;
	FOREACH_MDHOST(it) {
		mh = mdhost_iterator_access(&it);
		if (mdhost_is_valid(mh) && i < n)
			mss[i++] = &mh->ms;
	}
	fs = gfarm_filesystem_get_default();
	gfarm_filesystem_set_metadb_server_list(fs, mss, i);
	free(mss);
	gfmdc_alloc_journal_sync_info_closures();

	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
mdhost_db_modify_default_master(struct mdhost *mh,
	struct gfarm_metadb_server *ms, const char *diag)
{
	gfarm_error_t e;

	if ((e = db_begin(diag)) != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1002946, "db_begin failed: %s",
		    gfarm_error_string(e));
	} else if ((e = db_mdhost_modify(ms, 0)) != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1002947, "db_mdhost_modify failed: %s",
		    gfarm_error_string(e));
	} else if ((e = mdhost_fix_default_master(mh, diag))
	    != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1002948, "db_mdhost_modify failed: %s",
		    gfarm_error_string(e));
	} else if ((e = db_end(diag)) != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1002949, "db_end failed: %s",
		    gfarm_error_string(e));
	}
	return (e);
}

/* PREREQUISITE: giant_lock */
void
mdhost_set_self_as_default_master(void)
{
	gfarm_error_t e;
	struct mdhost *self = mdhost_lookup_self();
	static const char diag[] = "mdhost_set_self_as_default_master";

	gfarm_metadb_server_set_is_default_master(&self->ms, 1);
	if ((e = mdhost_db_modify_default_master(self, &self->ms, diag)) !=
	    GFARM_ERR_NO_ERROR)
		;
	else if ((e = mdhost_updated()) != GFARM_ERR_NO_ERROR)
		gflog_error(GFARM_MSG_1002950, "%s: mdhost_updated: %s",
		    diag, gfarm_error_string(e));
}

static gfarm_error_t
metadb_server_check_write_access(struct peer *peer, int from_client,
	const char *diag)
{
	gfarm_error_t e;
	struct user *user = peer_get_user(peer);

	if (!from_client || user == NULL || !user_is_admin(user)) {
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		gflog_debug(GFARM_MSG_1002951,
		    "%s: %s", diag, gfarm_error_string(e));
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_server_metadb_server_set(struct peer *peer, int from_client, int skip)
{
	gfarm_error_t e;
	struct gfarm_metadb_server ms;
	struct mdhost *mh;
	int isdm;
	static const char diag[] = "GFM_PROTO_METADB_SERVER_SET";

	memset(&ms, 0, sizeof(ms));
	if ((e = metadb_server_recv(peer, &ms)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002952,
		    "metadb_server_recv failure: %s",
		    gfarm_error_string(e));
		return (e);
	}
	if (skip) {
		gfarm_metadb_server_free(&ms);
		return (GFARM_ERR_NO_ERROR);
	}
	if (!gfarm_get_metadb_replication_enabled()) {
		gfarm_metadb_server_free(&ms);
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}
	isdm = gfarm_metadb_server_is_default_master(&ms);

	giant_lock();
	if ((e = metadb_server_check_write_access(peer, from_client, diag))
	    != GFARM_ERR_NO_ERROR) {
		/* nothing to do */
	} else if (mdhost_lookup(ms.name)) {
		gflog_debug(GFARM_MSG_1002953,
		    "mdhost already exists");
		e = GFARM_ERR_ALREADY_EXISTS;
	} else if ((e = metadb_server_verify(&ms, diag))
	    != GFARM_ERR_NO_ERROR) {
		/* nothing to do */
	} else if ((e = mdhost_enter(&ms, &mh)) != GFARM_ERR_NO_ERROR) {
		/* nothing to do */
	} else if (isdm) {
		if ((e = db_begin(diag)) != GFARM_ERR_NO_ERROR) {
			gflog_error(GFARM_MSG_1002954,
			    "db_begin failed: %s",
			    gfarm_error_string(e));
		} else if ((e = db_mdhost_add(&ms)) != GFARM_ERR_NO_ERROR) {
			gflog_error(GFARM_MSG_1002955,
			    "db_mdhost_add failed: %s",
			    gfarm_error_string(e));
		} else if ((e = mdhost_fix_default_master(mh, diag))
		    != GFARM_ERR_NO_ERROR) {
			gflog_error(GFARM_MSG_1002956,
			    "db_mdhost_fix_default_master failed: %s",
			    gfarm_error_string(e));
		} else if ((e = db_end(diag)) != GFARM_ERR_NO_ERROR) {
			gflog_error(GFARM_MSG_1002957,
			    "db_end failed: %s",
			    gfarm_error_string(e));
		}
		if (e != GFARM_ERR_NO_ERROR) {
			mdhost_remove_in_cache(ms.name);
			/* do not free after enter */
			ms.name = ms.clustername = NULL;
		}
	} else if ((e = db_mdhost_add(&ms)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002958,
		    "db_mdhost_add failed: %s",
		    gfarm_error_string(e));
		mdhost_remove_in_cache(ms.name);
		/* do not free after enter */
		ms.name = ms.clustername = NULL;
	}

	if (e == GFARM_ERR_NO_ERROR)
		mdhost_updated();
	else {
		gflog_debug(GFARM_MSG_1002959,
		    "error occurred during process: %s",
		    gfarm_error_string(e));
		gfarm_metadb_server_free(&ms);
	}
	giant_unlock();
	return (gfm_server_put_reply(peer, diag, e, ""));
}

gfarm_error_t
gfm_server_metadb_server_modify(struct peer *peer, int from_client, int skip)
{
	gfarm_error_t e;
	struct gfarm_metadb_server ms;
	struct mdhost *mh;
	int isdm;
	static const char diag[] = "GFM_PROTO_METADB_SERVER_MODIFY";

	memset(&ms, 0, sizeof(ms));
	if ((e = metadb_server_recv(peer, &ms)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002960,
		    "metadb_server_recv failure: %s",
		    gfarm_error_string(e));
		return (e);
	}
	if (skip) {
		gfarm_metadb_server_free(&ms);
		return (GFARM_ERR_NO_ERROR);
	}
	if (!gfarm_get_metadb_replication_enabled()) {
		gfarm_metadb_server_free(&ms);
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}
	isdm = gfarm_metadb_server_is_default_master(&ms);

	giant_lock();
	if ((e = metadb_server_check_write_access(peer, from_client, diag))
	    != GFARM_ERR_NO_ERROR) {
		/* nothing to do */
	} else if ((mh = mdhost_lookup(ms.name)) == NULL) {
		gflog_debug(GFARM_MSG_1002961,
		    "mdhost not found: %s", ms.name);
		e = GFARM_ERR_NO_SUCH_OBJECT;
	} else if (mdhost_is_default_master(mh) &&
	    !gfarm_metadb_server_is_default_master(&ms)) {
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		gflog_debug(GFARM_MSG_1002962,
		    "cannot toggle off default master flag directly: %s",
		    ms.name);
	} else
	    e = metadb_server_verify(&ms, diag);
	if (e != GFARM_ERR_NO_ERROR)
		goto unlock;
	mdhost_modify_in_cache(mh, &ms);
	if (isdm) {
		e = mdhost_db_modify_default_master(mh, &ms, diag);
	} else if ((e = db_mdhost_modify(&ms, 0)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002963,
		    "db_mdhost_modify failed: %s",
		    gfarm_error_string(e));
	}
unlock:
	if (e == GFARM_ERR_NO_ERROR)
		mdhost_updated();
	else {
		gflog_debug(GFARM_MSG_1002964,
		    "error occurred during process: %s",
		    gfarm_error_string(e));
	}
	giant_unlock();
	gfarm_metadb_server_free(&ms);
	return (gfm_server_put_reply(peer, diag, e, ""));
}

gfarm_error_t
gfm_server_metadb_server_remove(struct peer *peer, int from_client, int skip)
{
	gfarm_error_t e;
	char *name;
	struct mdhost *mh;
	static const char diag[] = "GFM_PROTO_METADB_SERVER_REMOVE";

	if ((e = gfm_server_get_request(peer, diag, "s", &name))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002965,
		    "get_request failure: %s",
		    gfarm_error_string(e));
	}
	if (skip) {
		free(name);
		return (GFARM_ERR_NO_ERROR);
	}
	if (!gfarm_get_metadb_replication_enabled()) {
		free(name);
		return (GFARM_ERR_OPERATION_NOT_PERMITTED);
	}

	giant_lock();
	if ((e = metadb_server_check_write_access(peer, from_client, diag))
	    != GFARM_ERR_NO_ERROR) {
		/* nothing to do */
	} else if ((mh = mdhost_lookup(name)) == NULL) {
		e = GFARM_ERR_NO_SUCH_OBJECT;
		gflog_debug(GFARM_MSG_1002966,
		    "%s: %s: %s", diag, gfarm_error_string(e), name);
	} else if (mh == mdhost_lookup_self()) {
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		gflog_debug(GFARM_MSG_1002967,
		    "%s: cannot remove self host", diag);
	} else if ((e = mdhost_remove_in_cache(name)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002968,
		    "%s: mdhost_remove_in_cache(%s) failed: %s",
		    diag, name, gfarm_error_string(e));
	} else if ((e = db_mdhost_remove(name)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002969,
		    "%s: db_mdhost_remove(%s) failed: %s",
		    diag, name, gfarm_error_string(e));
	}
	giant_unlock();

	free(name);
	return (gfm_server_put_reply(peer, diag, e, ""));
}

void
mdhost_init(void)
{
	struct mdhost *self;
	struct gfarm_metadb_server ms;
	gfarm_error_t e;
	struct mdhost *mh;
	struct gfarm_hash_iterator it;
	static const char *diag = "mdhost_init";

	if (gfarm_get_metadb_replication_enabled())
		mdcluster_init();

	mdhost_hashtab =
	    gfarm_hash_table_alloc(MDHOST_HASHTAB_SIZE,
		gfarm_hash_strptr, gfarm_hash_key_equal_strptr);
	if (mdhost_hashtab == NULL)
		gflog_fatal(GFARM_MSG_1002970,
		    "%s", gfarm_error_string(GFARM_ERR_NO_MEMORY));

	if (gfarm_get_metadb_replication_enabled()) {
		e = db_mdhost_load(NULL, mdhost_add_one);
		if (e != GFARM_ERR_NO_ERROR && e != GFARM_ERR_NO_SUCH_OBJECT)
			gflog_fatal(GFARM_MSG_1002971,
			    "%s", gfarm_error_string(e));
	}
	if ((self = mdhost_lookup(gfarm_metadb_server_name)) == NULL) {
		ms.name = strdup_ck(gfarm_metadb_server_name, diag);
		ms.port = gfarm_metadb_server_port;
		ms.clustername = strdup_ck("", diag);
		ms.flags = 0;
		ms.tflags = 0;
		gfarm_metadb_server_set_is_self(&ms, 1);
		gfarm_metadb_server_set_is_master(&ms, 1);
		gfarm_metadb_server_set_is_master_candidate(&ms, 1);
		gfarm_metadb_server_set_is_default_master(&ms, 1);
		if ((e = mdhost_enter(&ms, &self)) != GFARM_ERR_NO_ERROR)
			gflog_fatal(GFARM_MSG_1002972,
			    "Failed to add self mdhost");
		else if (gfarm_get_metadb_replication_enabled()) {
			gflog_info(GFARM_MSG_1002973,
			    "mdhost '%s' not found, creating...",
			    gfarm_metadb_server_name);
			if ((e = db_mdhost_add(&ms)) != GFARM_ERR_NO_ERROR)
				gflog_fatal(GFARM_MSG_1002974,
				    "Failed to add self mdhost");
		}
	}
	mdhost_self = self;
	if (gfarm_get_metadb_replication_enabled()) {
		FOREACH_MDHOST(it) {
			mh = mdhost_iterator_access(&it);
			if (!mdhost_is_valid(mh))
				continue;
			if (mdhost_is_default_master(mh)) {
				mdhost_set_is_master(mh, 1);
				break;
			}
		}
		if (!mdhost_is_master(self))
			localhost_is_readonly = 1;
		db_journal_set_fail_store_op(mdhost_self_change_to_readonly);
		if ((e = mdhost_updated()) != GFARM_ERR_NO_ERROR)
			gflog_fatal(GFARM_MSG_1002975,
			    "Failed to update mdhost: %s",
			    gfarm_error_string(e));
	}
	mdhost_activate(self);
}
