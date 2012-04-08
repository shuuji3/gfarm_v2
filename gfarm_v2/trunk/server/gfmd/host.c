/*
 * $Id$
 */

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

/* for host_addr_lookup() */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <pthread.h>

#include <gfarm/gfarm.h>

#include "internal_host_info.h"

#include "gfutil.h"
#include "hash.h"
#include "thrsubr.h"

#include "metadb_common.h"	/* gfarm_host_info_free_except_hostname() */
#include "gfp_xdr.h"
#include "gfm_proto.h" /* GFM_PROTO_SCHED_FLAG_* */
#include "gfs_proto.h" /* GFS_PROTOCOL_VERSION */
#include "auth.h"
#include "config.h"

#include "callout.h"
#include "subr.h"
#include "rpcsubr.h"
#include "db_access.h"
#include "host.h"
#include "mdhost.h"
#include "user.h"
#include "peer.h"
#include "inode.h"
#include "abstract_host.h"
#include "abstract_host_impl.h"
#include "netsendq.h"
#include "dead_file_copy.h"
#include "file_replication.h"
#include "back_channel.h"
#include "relay.h"

#define HOST_HASHTAB_SIZE	3079	/* prime number */

static pthread_mutex_t total_disk_mutex = PTHREAD_MUTEX_INITIALIZER;
static gfarm_off_t total_disk_used, total_disk_avail;
static const char total_disk_diag[] = "total_disk";

/* in-core gfarm_host_info */
struct host {
	/* abstract_host is common data between
	 * struct host and struct mdhost */
	struct abstract_host ah; /* must be the first member of this struct */

	/*
	 * resources which are protected by the giant_lock()
	 */
	struct gfarm_host_info hi;

	/*
	 * This should be included in the struct gfarm_host_info as a
	 * member, but with several reasons (mainly for ABI backward
	 * compatibilities), it is here.
	 */
	char *fsngroupname;

	pthread_mutex_t back_channel_mutex;

#ifdef COMPAT_GFARM_2_3
	/* used by synchronous protocol (i.e. until gfarm-2.3.0) only */
	result_callback_t back_channel_result;
	disconnect_callback_t back_channel_disconnect;
	struct peer *back_channel_callback_peer;
	void *back_channel_callback_closure;
#endif

	int status_reply_waiting;
	gfarm_int32_t report_flags;
	struct host_status status;
	struct callout *status_callout;
	gfarm_time_t last_report;
	int status_callout_retry;
};

/* referenced from fsngroup.c, it needs host_hashtab. */
struct gfarm_hash_table *host_hashtab = NULL;
static struct gfarm_hash_table *hostalias_hashtab = NULL;

static struct host *host_new(struct gfarm_host_info *, struct callout *);
static void host_free(struct host *);

/* NOTE: each entry should be checked by host_is_valid(h) too */
#define FOR_ALL_HOSTS(it) \
	for (gfarm_hash_iterator_begin(host_hashtab, (it)); \
	    !gfarm_hash_iterator_is_end(it); \
	     gfarm_hash_iterator_next(it))

static const char BACK_CHANNEL_DIAG[] = "back_channel";

struct host *
host_hashtab_lookup(struct gfarm_hash_table *hashtab, const char *hostname)
{
	struct gfarm_hash_entry *entry;

	entry = gfarm_hash_lookup(hashtab, &hostname, sizeof(hostname));
	if (entry == NULL)
		return (NULL);
	return (*(struct host **)gfarm_hash_entry_data(entry));
}

struct host *
host_iterator_access(struct gfarm_hash_iterator *it)
{
	struct host **hp =
	    gfarm_hash_entry_data(gfarm_hash_iterator_access(it));

	return (*hp);
}

static void
host_invalidate(struct host *h)
{
	abstract_host_invalidate(&h->ah);
}

static void
host_validate(struct host *h)
{
	abstract_host_validate(&h->ah);
}

static int
host_is_invalid_unlocked(struct host *h)
{
	return (abstract_host_is_invalid_unlocked(&h->ah));
}

int
host_is_valid(struct host *h)
{
	return (abstract_host_is_valid(&h->ah, BACK_CHANNEL_DIAG));
}

static struct host *
host_lookup_including_invalid(const char *hostname)
{
	return (host_hashtab_lookup(host_hashtab, hostname));
}

struct host *
host_lookup(const char *hostname)
{
	struct host *h = host_lookup_including_invalid(hostname);

	return ((h == NULL || host_is_invalid_unlocked(h)) ? NULL : h);
}

struct host *
host_addr_lookup(const char *hostname, struct sockaddr *addr)
{
	struct host *h = host_lookup(hostname);
#if 0
	struct gfarm_hash_iterator it;
	struct sockaddr_in *addr_in;
	struct hostent *hp;
	int i;
#endif

	if (h != NULL)
		return (h);
	if (addr->sa_family != AF_INET)
		return (NULL);

#if 0
	/*
	 * skip the following case since it is extraordinarily slow
	 * when there are some nodes that cannot be resolved.
	 */
	addr_in = (struct sockaddr_in *)addr;

	/* XXX FIXME - this is too damn slow */

	FOR_ALL_HOSTS(&it) {
		h = host_iterator_access(&it);
		if (!host_is_valid(h))
			continue;
		hp = gethostbyname(h->hi.hostname);
		if (hp == NULL || hp->h_addrtype != AF_INET)
			continue;
		for (i = 0; hp->h_addr_list[i] != NULL; i++) {
			if (memcmp(hp->h_addr_list[i], &addr_in->sin_addr,
			    sizeof(addr_in->sin_addr)) == 0)
				return (h);
		}
	}
#endif
	return (NULL);
}

struct host *
host_namealiases_lookup(const char *hostname)
{
	struct host *h = host_lookup(hostname);

	if (h != NULL)
		return (h);
	h = host_hashtab_lookup(hostalias_hashtab, hostname);
	return ((h == NULL || host_is_invalid_unlocked(h)) ? NULL : h);
}

/* XXX FIXME missing hostaliases */
gfarm_error_t
host_enter(struct gfarm_host_info *hi, struct host **hpp)
{
	struct gfarm_hash_entry *entry;
	int created;
	struct host *h;
	struct callout *callout;
	static const char diag[] = "host_enter";

	h = host_lookup_including_invalid(hi->hostname);
	if (h != NULL) {
		if (host_is_invalid_unlocked(h)) {
			host_validate(h);
			if (hpp != NULL)
				*hpp = h;

			/*
			 * copy host info but keeping address of hostname
			 */
			free(hi->hostname);
			hi->hostname = h->hi.hostname;

			/* see the comment in host_name() */
			gfarm_host_info_free_except_hostname(&h->hi);

			h->hi = *hi;
			return (GFARM_ERR_NO_ERROR);
		} else
			return (GFARM_ERR_ALREADY_EXISTS);
	}

	callout = callout_new();
	if (callout == NULL) {
		gflog_debug(GFARM_MSG_1002212, "%s: no memory for host %s",
		    diag, hi->hostname);
		return (GFARM_ERR_NO_MEMORY);
	}
	h = host_new(hi, callout);
	if (h == NULL) {
		gflog_debug(GFARM_MSG_1001546,
			"allocation of host failed");
		callout_free(callout);
		return (GFARM_ERR_NO_MEMORY);
	}

	entry = gfarm_hash_enter(host_hashtab,
	    &h->hi.hostname, sizeof(h->hi.hostname), sizeof(struct host *),
	    &created);
	if (entry == NULL) {
		gflog_debug(GFARM_MSG_1001547,
			"gfarm_hash_enter() failed");
		host_free(h);
		return (GFARM_ERR_NO_MEMORY);
	}
	if (!created) {
		gflog_debug(GFARM_MSG_1001548,
			"create entry failed");
		host_free(h);
		return (GFARM_ERR_ALREADY_EXISTS);
	}
	*(struct host **)gfarm_hash_entry_data(entry) = h;
	host_validate(h);
	if (hpp != NULL)
		*hpp = h;
	return (GFARM_ERR_NO_ERROR);
}

/* XXX FIXME missing hostaliases */
static gfarm_error_t
host_remove_internal(const char *hostname, int update_netsendq)
{
	struct host *h = host_lookup(hostname);

	if (h == NULL) {
		gflog_debug(GFARM_MSG_1001549,
		    "host_remove(%s): not exist", hostname);
		return (GFARM_ERR_NO_SUCH_OBJECT);
	}
	/*
	 * do not purge the hash entry.  Instead, invalidate it so
	 * that it can be activated later.
	 */
	host_invalidate(h);

	if (update_netsendq) {
		dead_file_copy_host_removed(h);
		netsendq_host_remove(
		    abstract_host_get_sendq(host_to_abstract_host(h)));
	}

	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
host_remove(const char *hostname)
{
	return (host_remove_internal(hostname, 1));
}

gfarm_error_t
host_remove_in_cache(const char *hostname)
{
	return (host_remove_internal(hostname, 0));
}

struct abstract_host *
host_to_abstract_host(struct host *h)
{
	return (&h->ah);
}

static struct host *
host_downcast_to_host(struct abstract_host *h)
{
	return ((struct host *)h);
}

static struct mdhost *
host_downcast_to_mdhost(struct abstract_host *h)
{
	gflog_fatal(GFARM_MSG_1002761, "downcasting host %p to mdhost", h);
	return (NULL);
}

static const char *
host_name0(struct abstract_host *h)
{
	return (host_name(abstract_host_to_host(h)));
}

/*
 * PREREQUISITE: nothing
 * LOCKS: nothing
 *
 * host_name() is usable without any mutex.
 * See hash_enter(), it's using gfarm_host_info_free_except_hostname()
 * to make h->hi.hostname always available.
 * It's implemented that way because host_name() is useful especially
 * for error logging, and acquiring giant_lock just for error logging is
 * not what we want to do.
 */
char *
host_name(struct host *h)
{
	return (h->hi.hostname);
}

static int
host_port0(struct abstract_host *h)
{
	return (host_port(abstract_host_to_host(h)));
}

int
host_port(struct host *h)
{
	return (h->hi.port);
}

char *
host_architecture(struct host *h)
{
	return (h->hi.architecture);
}

int
host_ncpu(struct host *h)
{
	return (h->hi.ncpu);
}

int
host_flags(struct host *h)
{
	return (h->hi.flags);
}

char *
host_fsngroup(struct host *h)
{
	return ((h->fsngroupname != NULL) ? h->fsngroupname : "");
}

struct netsendq *
host_sendq(struct host *h)
{
	return (h->ah.sendq);
}

int
host_supports_async_protocols(struct host *h)
{
	return (abstract_host_get_protocol_version(&h->ah)
		>= GFS_PROTOCOL_VERSION_V2_4);
}

static void
back_channel_mutex_lock(struct host *h, const char *diag)
{
	gfarm_mutex_lock(&h->back_channel_mutex, diag, BACK_CHANNEL_DIAG);
}

static void
back_channel_mutex_unlock(struct host *h, const char *diag)
{
	gfarm_mutex_unlock(&h->back_channel_mutex, diag, BACK_CHANNEL_DIAG);
}

#ifdef COMPAT_GFARM_2_3
void
host_set_callback(struct abstract_host *ah, struct peer *peer,
	result_callback_t result_callback,
	disconnect_callback_t disconnect_callback,
	void *closure)
{
	struct host *h = abstract_host_to_host(ah);
	static const char diag[] = "host_set_callback";

	/* XXX FIXME sanity check? */
	back_channel_mutex_lock(h, diag);
	h->back_channel_result = result_callback;
	h->back_channel_disconnect = disconnect_callback;
	h->back_channel_callback_peer = peer;
	h->back_channel_callback_closure = closure;
	back_channel_mutex_unlock(h, diag);
}

int
host_get_result_callback(struct host *h, struct peer *peer,
	result_callback_t *callbackp, void **closurep)
{
	int ok;
	static const char diag[] = "host_get_result_callback";

	back_channel_mutex_lock(h, diag);

	/* peer comparison works only if at least one is a local peer */
	if (h->back_channel_result == NULL ||
	    h->back_channel_callback_peer != peer) {
		ok = 0;
	} else {
		*callbackp = h->back_channel_result;
		*closurep = h->back_channel_callback_closure;
		h->back_channel_result = NULL;
		h->back_channel_disconnect = NULL;
		h->back_channel_callback_peer = NULL;
		h->back_channel_callback_closure = NULL;
		ok = 1;
	}

	back_channel_mutex_unlock(h, diag);
	return (ok);
}

int
host_get_disconnect_callback(struct host *h,
	disconnect_callback_t *callbackp,
	struct peer **peerp, void **closurep)
{
	int ok;
	static const char diag[] = "host_get_disconnect_callback";

	back_channel_mutex_lock(h, diag);

	if (h->back_channel_disconnect == NULL) {
		ok = 0;
	} else {
		*callbackp = h->back_channel_disconnect;
		*peerp = h->back_channel_callback_peer;
		*closurep = h->back_channel_callback_closure;
		h->back_channel_result = NULL;
		h->back_channel_disconnect = NULL;
		h->back_channel_callback_peer = NULL;
		h->back_channel_callback_closure = NULL;
		ok = 1;
	}

	back_channel_mutex_unlock(h, diag);
	return (ok);
}

#endif

/*
 * PREREQUISITE: host::back_channel_mutex
 * LOCKS: nothing
 * SLEEPS: no
 */
static int
host_is_up_unlocked(struct host *h)
{
	return (abstract_host_is_up_unlocked(&h->ah));
}

/*
 * PREREQUISITE: nothing
 * LOCKS: host::back_channel_mutex
 * SLEEPS: no
 */
int
host_is_up(struct host *h)
{
	return (abstract_host_is_up(&h->ah));
}

int
host_is_disk_available(struct host *h, gfarm_off_t size)
{
	gfarm_off_t avail, minfree = gfarm_get_minimum_free_disk_space();
	static const char diag[] = "host_get_disk_avail";

	back_channel_mutex_lock(h, diag);

	if (host_is_up_unlocked(h))
		avail = h->status.disk_avail * 1024;
	else
		avail = 0;
	back_channel_mutex_unlock(h, diag);

	if (minfree < size)
		minfree = size;
	return (avail >= minfree);
}

struct callout *
host_status_callout(struct host *h)
{
	return (h->status_callout);
}

struct peer *
host_get_peer(struct host *h)
{
	return (abstract_host_get_peer(&h->ah, BACK_CHANNEL_DIAG));
}

void
host_status_reply_waiting(struct host *host)
{
	static const char diag[] = "host_status_reply_waiting";

	back_channel_mutex_lock(host, diag);
	host->status_reply_waiting = 1;
	back_channel_mutex_unlock(host, diag);
}

int
host_status_reply_is_waiting(struct host *host)
{
	int waiting;
	static const char diag[] = "host_status_reply_waiting";

	back_channel_mutex_lock(host, diag);
	waiting = host->status_reply_waiting;
	back_channel_mutex_unlock(host, diag);

	return (waiting);
}

/*
 * PREREQUISITE: nothing
 * LOCKS: total_disk_mutex
 * SLEEPS: no
 */
static void
host_total_disk_update(
	gfarm_uint64_t old_used, gfarm_uint64_t old_avail,
	gfarm_uint64_t new_used, gfarm_uint64_t new_avail)
{
	static const char diag[] = "host_total_disk_update";

	gfarm_mutex_lock(&total_disk_mutex, diag, total_disk_diag);
	total_disk_used += new_used - old_used;
	total_disk_avail += new_avail - old_avail;
	gfarm_mutex_unlock(&total_disk_mutex, diag, total_disk_diag);
}

void
host_status_update(struct host *host, struct host_status *status)
{
	gfarm_uint64_t saved_used = 0, saved_avail = 0;
	const char diag[] = "status_update";

	back_channel_mutex_lock(host, diag);

	host->status_reply_waiting = 0;
	host->status_callout_retry = 0;

	if (host->report_flags & GFM_PROTO_SCHED_FLAG_LOADAVG_AVAIL) {
		saved_used = host->status.disk_used;
		saved_avail = host->status.disk_avail;
	}

	host->last_report = time(NULL);
	host->report_flags =
		GFM_PROTO_SCHED_FLAG_HOST_AVAIL |
		GFM_PROTO_SCHED_FLAG_LOADAVG_AVAIL;
	host->status = *status;

	back_channel_mutex_unlock(host, diag);

	host_total_disk_update(saved_used, saved_avail,
	    status->disk_used, status->disk_avail);
}

/*
 * PREREQUISITE: host::back_channel_mutex
 * LOCKS: nothing
 * SLEEPS: no
 */
static void
host_status_disable_unlocked(struct host *host,
	gfarm_uint64_t *saved_usedp, gfarm_uint64_t *saved_availp)
{
	if (host->report_flags & GFM_PROTO_SCHED_FLAG_LOADAVG_AVAIL) {
		*saved_usedp = host->status.disk_used;
		*saved_availp = host->status.disk_avail;
	} else {
		*saved_usedp = 0;
		*saved_availp = 0;
	}

	host->report_flags = 0;
}

/*
 * PREREQUISITE: giant_lock
 * LOCKS: host::back_channel_mutex, dfc_allq.mutex, removal_pendingq.mutex
 * SLEEPS: maybe (see the comment of dead_file_copy_host_becomes_up())
 *	but host::back_channel_mutex, dfc_allq.mutex and removal_pendingq.mutex
 *	won't be blocked while sleeping.
 */
static void
host_set_peer_locked(struct abstract_host *ah, struct peer *p)
{
	struct host *h = abstract_host_to_host(ah);

#ifdef COMPAT_GFARM_2_3
	h->back_channel_result = NULL;
	h->back_channel_disconnect = NULL;
	h->back_channel_callback_peer = NULL;
	h->back_channel_callback_closure = NULL;
#endif
	h->status_reply_waiting = 0;
	h->status_callout_retry = 0;
}

static void
host_set_peer_unlocked(struct abstract_host *ah, struct peer *p)
{
	struct host *host = abstract_host_to_host(ah);

	dead_file_copy_host_becomes_up(host);
	netsendq_host_becomes_up(abstract_host_get_sendq(ah));
}

/*
 * PREREQUISITE: host::back_channel_mutex
 * LOCKS: removal_pendingq.mutex, host_busyq.mutex
 * SLEEPS: no
 */
static void
host_unset_peer(struct abstract_host *ah, struct peer *peer)
{
	callout_stop(abstract_host_to_host(ah)->status_callout);
}

struct host_disable_closure {
	gfarm_uint64_t saved_used, saved_avail;
};

static gfarm_error_t
host_disable(struct abstract_host *ah, void **closurep)
{
	struct host *h = abstract_host_to_host(ah);
	struct host_disable_closure *c;

	GFARM_MALLOC(c);
	if (c == NULL) {
		gflog_error(GFARM_MSG_1002762,
		    "%s", gfarm_error_string(GFARM_ERR_NO_MEMORY));
		return (GFARM_ERR_NO_MEMORY);
	}
	*closurep = c;
	host_status_disable_unlocked(h, &c->saved_used, &c->saved_avail);
	return (GFARM_ERR_NO_ERROR);
}

static void
host_disabled(struct abstract_host *ah, struct peer *peer, void *closure)
{
	struct host_disable_closure *c = closure;

	host_total_disk_update(c->saved_used, c->saved_avail, 0, 0);
	free(closure);

	netsendq_host_becomes_down(abstract_host_get_sendq(ah));
}

/* giant_lock should be held before calling this */
void
host_disconnect(struct host *h, struct peer *peer)
{
	return (abstract_host_disconnect(&h->ah, peer,
		BACK_CHANNEL_DIAG));
}

struct abstract_host_ops host_ops = {
	host_downcast_to_host,
	host_downcast_to_mdhost,
	host_name0,
	host_port0,
	host_set_peer_locked,
	host_set_peer_unlocked,
	host_unset_peer,
	host_disable,
	host_disabled,
};

struct netsendq_manager *back_channel_send_manager;

static struct host *
host_new(struct gfarm_host_info *hi, struct callout *callout)
{
	gfarm_error_t e;
	struct host *h;
	static const char diag[] = "host_new";

	GFARM_MALLOC(h);
	if (h == NULL) {
		gflog_error(GFARM_MSG_UNFIXED,
		    "host %s: no memory", hi->hostname);
		return (NULL);
	}
	e = abstract_host_init(&h->ah, &host_ops, back_channel_send_manager,
	    diag);
	if (e != GFARM_ERR_NO_ERROR) {
		free(h);
		gflog_error(GFARM_MSG_UNFIXED,
		    "host %s: %s", hi->hostname, gfarm_error_string(e));
		return (NULL);
	}
	h->hi = *hi;
	h->fsngroupname = NULL;
	gfarm_mutex_init(&h->back_channel_mutex, diag, BACK_CHANNEL_DIAG);
#ifdef COMPAT_GFARM_2_3
	h->back_channel_result = NULL;
	h->back_channel_disconnect = NULL;
	h->back_channel_callback_peer = NULL;
	h->back_channel_callback_closure = NULL;
#endif
	h->status_reply_waiting = 0;
	h->report_flags = 0;
	h->status.loadavg_1min =
	h->status.loadavg_5min =
	h->status.loadavg_15min = 0.0;
	h->status.disk_used =
	h->status.disk_avail = 0;
	h->status_callout = callout;
	h->status_callout_retry = 0;
	h->last_report = 0;
	return (h);
}

static void
host_free(struct host *h)
{
	free(h->status_callout);
	free(h);
}

static int
host_order(const void *a, const void *b)
{
	const struct host *const *h1 = a, *const *h2 = b;

	if (*h1 < *h2)
		return (-1);
	else if (*h1 > *h2)
		return (1);
	else
		return (0);
}

static void
host_sort(int nhosts, struct host **hosts)
{
	if (nhosts <= 0) /* 2nd parameter of qsort(3) is unsigned */
		return;

	qsort(hosts, nhosts, sizeof(*hosts), host_order);
}

/*
 * remove duplicated hosts.
 * NOTE: this function assumes that hosts[] is sorted by host_order()
 */
static int
host_unique(int nhosts, struct host **hosts)
{
	int l, r;

	if (nhosts <= 0)
		return (0);
	l = 0;
	r = l + 1;
	for (;;) {
		for (;;) {
			if (r >= nhosts)
				return (l + 1);
			if (hosts[l] != hosts[r])
				break;
			r++;
		}
		++l;
		hosts[l] = hosts[r]; /* maybe l == r here */
		++r;
	}
}

int
host_unique_sort(int nhosts, struct host **hosts)
{
	host_sort(nhosts, hosts);
	return (host_unique(nhosts, hosts));
}

/* NOTE: both hosts[] and excludings[] must be host_unique_sort()ed */
static gfarm_error_t
host_exclude(int *nhostsp, struct host **hosts,
	int n_excludings, struct host **excludings,
	int (*filter)(struct host *, void *), void *closure)
{
	int cmp, i, j, nhosts = *nhostsp;
	unsigned char *candidates;

	GFARM_MALLOC_ARRAY(candidates, nhosts > 0 ? nhosts : 1);
	if (candidates == NULL)
		return (GFARM_ERR_NO_MEMORY);

	memset(candidates, 1, nhosts);

	if (n_excludings > 0) {
		/* exclude excludings[] from hosts[] */
		i = j = 0;
		while (i < nhosts && j < n_excludings) {
			cmp = host_order(&hosts[i], &excludings[j]);
			if (cmp < 0) {
				i++;
			} else if (cmp == 0) {
				candidates[i++] = 0;
			} else if (cmp > 0) {
				j++;
			}
		}
	}

	if (filter != NULL) {
		for (i = 0; i < nhosts; i++) {
			if (!candidates[i])
				continue;
			if (!filter(hosts[i], closure))
				candidates[i] = 0;
		}
	}

	/* compaction */
	j = 0;
	for (i = 0; i < nhosts; i++) {
		if (candidates[i])
			hosts[j++] = hosts[i];
	}
	free(candidates);

	*nhostsp = j;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
host_is_disk_available_filter(struct host *host, void *closure)
{
	gfarm_off_t *sizep = closure;

	return (host_is_disk_available(host, *sizep));
}

static gfarm_error_t
host_array_alloc(int *nhostsp, struct host ***hostsp)
{
	int i, nhosts;
	struct host **hosts;
	struct gfarm_hash_iterator it;

	nhosts = 0;
	FOR_ALL_HOSTS(&it) {
		host_iterator_access(&it);
		++nhosts;
	}

	GFARM_MALLOC_ARRAY(hosts, nhosts > 0 ? nhosts : 1);
	if (hosts == NULL)
		return (GFARM_ERR_NO_MEMORY);

	i = 0;
	FOR_ALL_HOSTS(&it) {
		if (i >= nhosts) /* always false due to giant_lock */
			break;
		hosts[i++] = host_iterator_access(&it);
	}
	*nhostsp = i;
	*hostsp = hosts;
	return (GFARM_ERR_NO_ERROR);
}

/*
 * just select randomly		XXX FIXME: needs to improve
 */
static void
select_hosts(int nhosts, struct host **hosts,
	int nresults, struct host **results)
{
	int i, j;

	assert(nhosts > nresults);
	for (i = 0; i < nresults; i++) {
		j = gfarm_random() % nhosts;
		results[i] = hosts[j];
		hosts[j] = hosts[--nhosts];
	}
}

/*
 * this function sorts excludings[] as a side effect.
 * but caller shouldn't depend on the side effect.
 */
gfarm_error_t
host_schedule_all_except(int n_excludings, struct host **excludings,
	int (*filter)(struct host *, void *), void *closure,
	gfarm_int32_t *nhostsp, struct host ***hostsp)
{
	gfarm_error_t e;
	int nhosts;
	struct host **hosts;

	e = host_array_alloc(&nhosts, &hosts);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	n_excludings = host_unique_sort(n_excludings, excludings);
	nhosts = host_unique_sort(nhosts, hosts);

	e = host_exclude(&nhosts, hosts, n_excludings, excludings,
	    filter, closure);
	if (e == GFARM_ERR_NO_ERROR) {
		*nhostsp = nhosts;
		*hostsp = hosts;
	} else {
		free(hosts);
	}
	return (e);
}

/*
 * this function sorts excludings[] as a side effect.
 * but caller shouldn't depend on the side effect.
 */
gfarm_error_t
host_schedule_except(int n_excludings, struct host **excludings,
	int (*filter)(struct host *, void *), void *closure,
	int n_shortage, int *n_new_targetsp, struct host **new_targets)
{
	gfarm_error_t e;
	struct host **hosts;
	int nhosts;
	int i;

	e = host_schedule_all_except(n_excludings, excludings,
	    filter, closure, &nhosts, &hosts);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	if (nhosts <= n_shortage) {
		for (i = 0; i < nhosts; i++)
			new_targets[i] = hosts[i];
		*n_new_targetsp = nhosts;
	} else {
		select_hosts(nhosts, hosts, n_shortage, new_targets);
		*n_new_targetsp = n_shortage;
	}

	free(hosts);
	return (GFARM_ERR_NO_ERROR);
}

/* give the top priority to the local host */
int
host_schedule_one_except(struct peer *peer,
	int n_excludings, struct host **excludings,
	int (*filter)(struct host *, void *), void *closure,
	gfarm_int32_t *np, struct host ***hostsp, gfarm_error_t *errorp)
{
	struct host **hosts, *h = peer_get_host(peer);
	int i;

	if (h != NULL && (*filter)(h, closure)) {

		/* is the host excluded? */
		for (i = 0; i < n_excludings; i++) {
			if (h == excludings[i])
				return (0); /* not scheduled */
		}

		GFARM_MALLOC_ARRAY(hosts, 1);
		if (hosts == NULL) {
			*errorp = GFARM_ERR_NO_MEMORY;
			return (1); /* scheduled, sort of */
		}
		hosts[0] = h;
		*np = 1;
		*hostsp = hosts;
		*errorp = GFARM_ERR_NO_ERROR;
		return (1); /* scheduled */
	}
	return (0); /* not scheduled */
}

#ifdef NOT_USED
/*
 * save all to text file
 */

static FILE *host_fp;

gfarm_error_t
host_info_open_for_seq_write(void)
{
	host_fp = fopen("host", "w");
	if (host_fp == NULL)
		return (gfarm_errno_to_error(errno));
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
host_info_write_next(struct gfarm_host_info *hi)
{
	int i;

	fprintf(host_fp, "%s %d %d %d %s", hi->hostname, hi->port,
	    hi->ncpu, hi->flags, hi->architecture);
	for (i = 0; i < hi->nhostaliases; i++)
		fprintf(host_fp, " %s", hi->hostaliases[i]);
	fprintf(host_fp, "\n");
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
host_info_close_for_seq_write(void)
{
	fclose(host_fp);
	return (GFARM_ERR_NO_ERROR);
}
#endif /* NOT_USED */

/* The memory owner of `*hi' is changed to host.c */
void
host_add_one(void *closure,
	struct gfarm_internal_host_info *hi)
{
	struct host *h = NULL;
	gfarm_error_t e = host_enter(&(hi->hi), &h);

	if (e != GFARM_ERR_NO_ERROR)
		gflog_warning(GFARM_MSG_1000266,
		    "host_add_one: %s", gfarm_error_string(e));

	if (h != NULL) {
		if (hi->fsngroupname != NULL)
			/*
			 * Not strdup() but just change the ownership.
			 */
			h->fsngroupname = hi->fsngroupname;
		else
			h->fsngroupname = NULL;
	}
}

const struct netsendq_type *
	back_channel_queue_types[NETSENDQ_TYPE_GFS_PROTO_NUM_TYPES] = {
	&gfs_proto_status_queue,
#ifdef not_def_REPLY_QUEUE
	&gfm_async_server_reply_to_gfsd_queue,
#endif
	&gfs_proto_fhremove_queue,
	&gfs_proto_replication_request_queue,
};

void
host_init(void)
{
	gfarm_error_t e;

	host_hashtab =
	    gfarm_hash_table_alloc(HOST_HASHTAB_SIZE,
		gfarm_hash_casefold_strptr,
		gfarm_hash_key_equal_casefold_strptr);
	if (host_hashtab == NULL)
		gflog_fatal(GFARM_MSG_1000267, "no memory for host hashtab");
	hostalias_hashtab =
	    gfarm_hash_table_alloc(HOST_HASHTAB_SIZE,
		gfarm_hash_casefold_strptr,
		gfarm_hash_key_equal_casefold_strptr);
	if (hostalias_hashtab == NULL) {
		gfarm_hash_table_free(host_hashtab);
		gflog_fatal(GFARM_MSG_1000268,
		    "no memory for hostalias hashtab");
	}

	back_channel_send_manager = netsendq_manager_new(
	    NETSENDQ_TYPE_GFS_PROTO_NUM_TYPES, back_channel_queue_types,
	    /* XXX FIXME: use different config parameter */
	    gfarm_metadb_thread_pool_size, gfarm_metadb_job_queue_length,
	    "send queue to gfsd");

	e = db_host_load(NULL, host_add_one);
	if (e != GFARM_ERR_NO_ERROR)
		gflog_error(GFARM_MSG_1000269,
		    "loading hosts: %s", gfarm_error_string(e));
}

#ifndef TEST
/*
 * protocol handler
 */

/*
 * PREREQUISITE: giant_lock
 * LOCKS: host::back_channel_mutex
 * SLEEPS: maybe
 *	but host::back_channel_mutex won't be blocked while sleeping.
 */
static gfarm_error_t
gfm_server_host_generic_get(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	gfarm_error_t (*reply)(struct host *, struct peer *, const char *),
	int (*filter)(struct host *, void *), void *closure,
	int no_match_is_ok, const char *diag)
{
	gfarm_error_t e, e2;
	gfarm_int32_t nhosts, nmatch, i, answered;
	struct gfarm_hash_iterator it;
	struct host *h;
	char *match;

	nhosts = 0;
	FOR_ALL_HOSTS(&it) {
		h = host_iterator_access(&it);
		++nhosts;
	}

	/*
	 * remember the matching result to return consistent answer.
	 * note that the result of host_is_valid() may vary at each call.
	 */
	GFARM_MALLOC_ARRAY(match, nhosts > 0 ? nhosts : 1);
	nmatch = 0;
	if (match == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_debug(GFARM_MSG_1002216,
		    "%s: no memory for %d hosts", diag, nhosts);
	} else {
		i = 0;
		FOR_ALL_HOSTS(&it) {
			if (i >= nhosts) /* always false due to giant_lock */
				break;
			h = host_iterator_access(&it);
			if (host_is_valid(h) &&
			    (filter == NULL || (*filter)(h, closure))) {
				match[i] = 1;
				++nmatch;
			} else {
				match[i] = 0;
			}
			++i;
		}
		if (no_match_is_ok || nmatch > 0) {
			e = GFARM_ERR_NO_ERROR;
		} else {
			e = GFARM_ERR_NO_SUCH_OBJECT;
			gflog_debug(GFARM_MSG_1002217,
			    "%s: no matching host", diag);
		}
	}
	/* XXXRELAY FIXME, reply size is not correct */
	e2 = gfm_server_put_reply(peer, xid, sizep, diag, e, "i", nmatch);
	if (e2 != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002218,
		    "gfm_server_put_reply(%s) failed: %s",
		    diag, gfarm_error_string(e2));
	} else if (e == GFARM_ERR_NO_ERROR) {
		i = answered = 0;
		FOR_ALL_HOSTS(&it) {
			if (i >= nhosts || answered >= nmatch)
				break;
			h = host_iterator_access(&it);
			if (match[i]) {
				/* XXXRELAY FIXME */
				e2 = (*reply)(h, peer, diag);
				if (e2 != GFARM_ERR_NO_ERROR) {
					gflog_debug(GFARM_MSG_1002219,
					    "%s: host_info_send(): %s",
					    diag, gfarm_error_string(e));
					break;
				}
				++answered;
			}
			i++;
		}
	}
	if (match != NULL)
		free(match);

	return (e2);
}

/* this interface is exported for a use from a private extension */
gfarm_error_t
host_info_send(struct gfp_xdr *client, struct host *h)
{
	struct gfarm_host_info *hi;

	hi = &h->hi;
	return (gfp_xdr_send(client, "ssiiii",
	    hi->hostname, hi->architecture,
	    hi->ncpu, hi->port, hi->flags, hi->nhostaliases));
}

static gfarm_error_t
host_info_reply(struct host *h, struct peer *peer, const char *diag)
{
	return (host_info_send(peer_get_conn(peer), h));
}

gfarm_error_t
gfm_server_host_info_get_common(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int (*filter)(struct host *, void *), void *closure, const char *diag)
{
	gfarm_error_t e;

	/* XXX FIXME too long giant lock */
	giant_lock();

	e = gfm_server_host_generic_get(peer, xid, sizep,
	    host_info_reply, filter, closure,
	    filter == NULL, diag);

	giant_unlock();

	return (e);
}

gfarm_error_t
gfm_server_host_info_get_all(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip)
{
	static const char diag[] = "GFM_PROTO_HOST_INFO_GET_ALL";

	if (skip)
		return (GFARM_ERR_NO_ERROR);

	return (gfm_server_host_info_get_common(peer, xid, sizep,
	    NULL, NULL, diag));
}

static int
arch_filter(struct host *h, void *closure)
{
	char *architecture = closure;

	return (strcmp(h->hi.architecture, architecture) == 0);
}

gfarm_error_t
gfm_server_host_info_get_by_architecture(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip)
{
	gfarm_error_t e;
	char *architecture;
	struct relayed_request *relay;
	static const char diag[] = "GFM_PROTO_HOST_INFO_GET_BY_ARCHITECTURE";

	e = gfm_server_get_request_with_relay(peer, sizep, skip, &relay, diag,
	    GFM_PROTO_HOST_INFO_GET_BY_ARCHITECTURE, "s", &architecture);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (skip) {
		free(architecture);
		return (GFARM_ERR_NO_ERROR);
	}

	if (relay == NULL) {
		/* do not relay RPC to master gfmd */
		e = gfm_server_host_info_get_common(peer, xid, sizep,
		    arch_filter, architecture, diag);
	}

	return (gfm_server_put_reply_with_relay(peer, xid, sizep, relay, diag,
	    &e, "s", &architecture));
}

gfarm_error_t
gfm_server_host_info_get_by_names_common(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip,
	struct host *(*lookup)(const char *), const char *diag)
{
	struct gfp_xdr *client = peer_get_conn(peer);
	gfarm_error_t e;
	gfarm_int32_t nhosts;
	char *host, **hosts;
	int i, j, eof, no_memory = 0;
	struct host *h;

	e = gfm_server_get_request(peer, sizep, diag, "i", &nhosts);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001558,
			"gfm_server_get_request() failed: %s",
			gfarm_error_string(e));
		return (e);
	}
	if (!mdhost_self_is_master()) {
		return (GFARM_ERR_FUNCTION_NOT_IMPLEMENTED); /* XXX RELAY */
	}
	if (skip)
		return (GFARM_ERR_NO_ERROR);
	GFARM_MALLOC_ARRAY(hosts, nhosts);
	if (hosts == NULL)
		no_memory = 1;
	for (i = 0; i < nhosts; i++) {
		e = gfp_xdr_recv(client, 0, &eof, "s", &host);
		if (e != GFARM_ERR_NO_ERROR || eof) {
			gflog_debug(GFARM_MSG_1001559,
				"gfp_xdr_recv(host) failed: %s",
				gfarm_error_string(e));
			if (e == GFARM_ERR_NO_ERROR) /* i.e. eof */
				e = GFARM_ERR_PROTOCOL;
			if (hosts != NULL) {
				for (j = 0; j < i; j++) {
					if (hosts[j] != NULL)
						free(hosts[j]);
				}
				free(hosts);
			}
			return (e);
		}
		if (hosts == NULL) {
			free(host);
		} else {
			hosts[i] = host;
		}
	}
	if (no_memory)
		e = gfm_server_put_reply(peer, xid, sizep,
		    diag, GFARM_ERR_NO_MEMORY, "");
	else /* XXXRELAY FIXME, reply size is not correct */
		e = gfm_server_put_reply(peer, xid, sizep,
		    diag, GFARM_ERR_NO_ERROR, "");
	if (no_memory || e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1001560,
			"gfp_xdr_recv(host) failed: %s",
			gfarm_error_string(e));
		if (hosts != NULL) {
			for (i = 0; i < nhosts; i++) {
				if (hosts[i] != NULL)
					free(hosts[i]);
			}
			free(hosts);
		}
		return (e);
	}
	/* XXX FIXME too long giant lock */
	giant_lock();
	for (i = 0; i < nhosts; i++) {
		h = (*lookup)(hosts[i]);
		if (h == NULL) {
			if (debug_mode)
				gflog_info(GFARM_MSG_1000270,
				    "host lookup <%s>: failed",
				    hosts[i]);
			/* XXXRELAY FIXME: should use gfp_xdr_send() */
			e = gfm_server_put_reply(peer, 0, NULL, diag,
			    GFARM_ERR_UNKNOWN_HOST, "");
		} else {
			if (debug_mode)
				gflog_info(GFARM_MSG_1000271,
				    "host lookup <%s>: ok", hosts[i]);
			/* XXXRELAY FIXME: should use gfp_xdr_send() */
			e = gfm_server_put_reply(peer, 0, NULL, diag,
			    GFARM_ERR_NO_ERROR, "");
			/* XXXRELAY FIXME */
			if (e == GFARM_ERR_NO_ERROR)
				e = host_info_send(client, h);
		}
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001561,
				"error occurred during process: %s",
				gfarm_error_string(e));
			break;
		}
	}
	for (i = 0; i < nhosts; i++)
		free(hosts[i]);
	free(hosts);
	giant_unlock();
	return (e);
}

gfarm_error_t
gfm_server_host_info_get_by_names(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip)
{
	return (gfm_server_host_info_get_by_names_common(
	    peer, xid, sizep, from_client, skip,
	    host_lookup, "host_info_get_by_names"));
}

gfarm_error_t
gfm_server_host_info_get_by_namealiases(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip)
{
	return (gfm_server_host_info_get_by_names_common(
	    peer, xid, sizep, from_client, skip,
	    host_namealiases_lookup, "host_info_get_by_namealiases"));
}

gfarm_error_t
host_info_verify(struct gfarm_host_info *hi, const char *diag)
{
	if (strlen(hi->hostname) > GFARM_HOST_NAME_MAX) {
		gflog_debug(GFARM_MSG_1002421, "%s: too long hostname: %s",
		    diag, hi->hostname);
		return (GFARM_ERR_INVALID_ARGUMENT);
	}
	if (strlen(hi->architecture) > GFARM_HOST_ARCHITECTURE_NAME_MAX) {
		gflog_debug(GFARM_MSG_1002422,
		    "%s: %s: too long architecture: %s",
		    diag, hi->hostname, hi->architecture);
		return (GFARM_ERR_INVALID_ARGUMENT);
	}
	if (hi->ncpu < 0) {
		gflog_debug(GFARM_MSG_1002423,
		    "%s: %s: invalid cpu number: %d",
		    diag, hi->hostname, hi->ncpu);
		return (GFARM_ERR_INVALID_ARGUMENT);
	}
	if (hi->port <= 0 || hi->port >= 65536) {
		gflog_debug(GFARM_MSG_1002424,
		    "%s: %s: invalid port number: %d",
		    diag, hi->hostname, hi->port);
		return (GFARM_ERR_INVALID_ARGUMENT);
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfm_server_host_info_set(struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip)
{
	gfarm_error_t e;
	struct user *user = peer_get_user(peer);
	gfarm_int32_t ncpu, port, flags;
	struct gfarm_host_info hi;
	struct relayed_request *relay;
	static const char diag[] = "GFM_PROTO_HOST_INFO_SET";

	e = gfm_server_get_request_with_relay(peer, sizep, skip, &relay, diag,
	    GFM_PROTO_HOST_INFO_SET, "ssiii",
	    &hi.hostname, &hi.architecture, &ncpu, &port, &flags);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (skip) {
		free(hi.hostname);
		free(hi.architecture);
		return (GFARM_ERR_NO_ERROR);
	}

	if (relay != NULL) {
		free(hi.hostname);
		free(hi.architecture);
	} else {
		/* do not relay RPC to master gfmd */
		hi.ncpu = ncpu;
		hi.port = port;
		hi.flags = flags;
		/* XXX FIXME missing hostaliases */
		hi.nhostaliases = 0;
		hi.hostaliases = NULL;

		giant_lock();
		if (!from_client || user == NULL || !user_is_admin(user)) {
			gflog_debug(GFARM_MSG_1001563,
			    "operation is not permitted");
			e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		} else if (host_lookup(hi.hostname) != NULL) {
			gflog_debug(GFARM_MSG_1001564,
			    "host already exists");
			e = GFARM_ERR_ALREADY_EXISTS;
		} else if ((e = host_info_verify(&hi, diag)) !=
		    GFARM_ERR_NO_ERROR) {
			/* nothing to do */
		} else if ((e = host_enter(&hi, NULL)) != GFARM_ERR_NO_ERROR) {
			/* nothing to do */
		} else if ((e = db_host_add(&hi)) != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001565,
			    "db_host_add() failed: %s",
			    gfarm_error_string(e));
			host_remove(hi.hostname);
			hi.hostname = hi.architecture = NULL;
		}
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001566,
			    "error occurred during process: %s",
			    gfarm_error_string(e));
			if (hi.hostname != NULL)
				free(hi.hostname);
			if (hi.architecture != NULL)
				free(hi.architecture);
		}
		giant_unlock();
	}
	return (gfm_server_put_reply_with_relay(peer, xid, sizep, relay, diag,
	    &e, ""));
}

void
host_modify(struct host *h, struct gfarm_host_info *hi)
{
	free(h->hi.architecture);
	h->hi.architecture = hi->architecture;
	hi->architecture = NULL;
	h->hi.ncpu = hi->ncpu;
	h->hi.port = hi->port;
	h->hi.flags = hi->flags;
}

void
host_fsngroup_modify(struct host *h, const char *fsngroupname)
{
	if (h->fsngroupname != NULL)
		free(h->fsngroupname);
	if (fsngroupname != NULL)
		h->fsngroupname = strdup_ck(fsngroupname,
					"host_fsngroup_modify");
	else
		h->fsngroupname = NULL;
}

gfarm_error_t
gfm_server_host_info_modify(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip)
{
	gfarm_error_t e;
	struct user *user = peer_get_user(peer);
	gfarm_int32_t ncpu, port, flags;
	struct gfarm_host_info hi;
	struct host *h;
	int needs_free = 0;
	struct relayed_request *relay;
	static const char diag[] = "GFM_PROTO_HOST_INFO_MODIFY";

	e = gfm_server_get_request_with_relay(peer, sizep, skip, &relay, diag,
	    GFM_PROTO_HOST_INFO_MODIFY,
	    "ssiii", &hi.hostname, &hi.architecture, &ncpu, &port, &flags);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (skip) {
		free(hi.hostname);
		free(hi.architecture);
		return (GFARM_ERR_NO_ERROR);
	}

	if (relay != NULL) {
		free(hi.hostname);
		free(hi.architecture);
	} else {
		/* do not relay RPC to master gfmd */
		hi.ncpu = ncpu;
		hi.port = port;
		hi.flags = flags;
		/* XXX FIXME missing hostaliases */

		/* XXX should we disconnect a back channel to the host? */
		giant_lock();
		if (!from_client || user == NULL || !user_is_admin(user)) {
			gflog_debug(GFARM_MSG_1001568,
			    "operation is not permitted");
			e = GFARM_ERR_OPERATION_NOT_PERMITTED;
			needs_free = 1;
		} else if ((h = host_lookup(hi.hostname)) == NULL) {
			gflog_debug(GFARM_MSG_1001569,
			    "host does not exists");
			e = GFARM_ERR_NO_SUCH_OBJECT;
			needs_free = 1;
		} else if ((e = host_info_verify(&hi, diag)) !=
		    GFARM_ERR_NO_ERROR) {
			needs_free = 1;
		} else if ((e = db_host_modify(&hi,
		DB_HOST_MOD_ARCHITECTURE|DB_HOST_MOD_NCPU|DB_HOST_MOD_FLAGS,
		    /* XXX */ 0, NULL, 0, NULL)) != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1001570,
			    "db_host_modify failed: %s",
			    gfarm_error_string(e));
			needs_free = 1;
		} else {
			host_modify(h, &hi);
			free(hi.hostname);
		}
		if (needs_free) {
			free(hi.hostname);
			free(hi.architecture);
		}
		giant_unlock();
	}
	return (gfm_server_put_reply_with_relay(peer, xid, sizep, relay, diag,
	    &e, ""));
}

/* this interface is exported for a use from a private extension */
gfarm_error_t
host_info_remove_default(const char *hostname, const char *diag)
{
	gfarm_error_t e, e2;
	struct host *host;

	if ((host = host_lookup(hostname)) == NULL)
		return (GFARM_ERR_NO_SUCH_OBJECT);

	/* disconnect the back channel */
	gflog_info(GFARM_MSG_1002425,
	    "back_channel(%s): disconnecting: host info removed", hostname);
	host_disconnect(host, NULL);

	if ((e = host_remove(hostname)) == GFARM_ERR_NO_ERROR) {
		e2 = db_host_remove(hostname);
		if (e2 != GFARM_ERR_NO_ERROR)
			gflog_error(GFARM_MSG_1000272,
			    "%s: db_host_remove: %s",
			    diag, gfarm_error_string(e2));
	}
	return (e);
}

/* this interface is made as a hook for a private extension */
gfarm_error_t (*host_info_remove)(const char *, const char *) =
	host_info_remove_default;

gfarm_error_t
gfm_server_host_info_remove(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip)
{
	gfarm_error_t e;
	struct user *user = peer_get_user(peer);
	char *hostname;
	struct relayed_request *relay;
	static const char diag[] = "GFM_PROTO_HOST_INFO_REMOVE";

	e = gfm_server_get_request_with_relay(peer, sizep, skip, &relay, diag,
	    GFM_PROTO_HOST_INFO_REMOVE, "s", &hostname);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (skip) {
		free(hostname);
		return (GFARM_ERR_NO_ERROR);
	}

	if (relay != NULL) {
		free(hostname);
	} else {
		/*
		 * XXX should we remove all file copy entries stored on the
		 * specified host?
		 */
		giant_lock();
		if (!from_client || user == NULL || !user_is_admin(user)) {
			gflog_debug(GFARM_MSG_1001572,
			    "operation is not permitted");
			e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		} else
			e = host_info_remove(hostname, diag);
		free(hostname);
		giant_unlock();
	}
	return (gfm_server_put_reply_with_relay(peer, xid, sizep, relay, diag,
	    &e, ""));
}

/* called from fs.c:gfm_server_schedule_file() as well */
gfarm_error_t
host_schedule_reply(struct host *h, struct peer *peer, const char *diag)
{
	struct host_status status;
	gfarm_time_t last_report;
	gfarm_int32_t report_flags;

	back_channel_mutex_lock(h, diag);
	status = h->status;
	last_report = h->last_report;
	report_flags = h->report_flags;
	back_channel_mutex_unlock(h, diag);
	return (gfp_xdr_send(peer_get_conn(peer), "siiillllii",
	    h->hi.hostname, h->hi.port, h->hi.ncpu,
	    (gfarm_int32_t)(status.loadavg_1min * GFM_PROTO_LOADAVG_FSCALE),
	    last_report,
	    status.disk_used, status.disk_avail,
	    (gfarm_int64_t)0 /* rtt_cache_time */,
	    (gfarm_int32_t)0 /* rtt_usec */,
	    report_flags));
}

gfarm_error_t
host_schedule_reply_all(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int (*filter)(struct host *, void *), void *closure, const char *diag)
{
	return (gfm_server_host_generic_get(peer, xid, sizep,
	    host_schedule_reply, filter, closure, 1, diag));
}

gfarm_error_t
gfm_server_hostname_set(struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip)
{
	gfarm_int32_t e;
	char *hostname;
	struct relayed_request *relay;
	static const char diag[] = "GFM_PROTO_HOSTNAME_SET";

	e = gfm_server_get_request_with_relay(peer, sizep, skip, &relay, diag,
	    GFM_PROTO_HOSTNAME_SET, "s", &hostname);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (skip) {
		free(hostname);
		return (GFARM_ERR_NO_ERROR);
	}

	if (relay == NULL) {
		/* do not relay RPC to master gfmd */
		giant_lock();
		if (from_client) {
			gflog_debug(GFARM_MSG_1001578,
			    "operation is not permitted for from_client");
			e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		} else
			e = peer_set_host(peer, hostname);
		giant_unlock();
	}

	free(hostname);

	return (gfm_server_put_reply_with_relay(peer, xid, sizep, relay, diag,
	    &e, ""));
}

static int
up_and_domain_filter(struct host *h, void *d)
{
	const char *domain = d;

	return (host_is_up(h) &&
	    gfarm_host_is_in_domain(host_name(h), domain));
}

gfarm_error_t
gfm_server_schedule_host_domain(
	struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip)
{
	gfarm_int32_t e;
	char *domain;
	struct relayed_request *relay;
	static const char diag[] = "GFM_PROTO_SCHEDULE_HOST_DOMAIN";

	e = gfm_server_get_request_with_relay(peer, sizep, skip, &relay, diag,
	      GFM_PROTO_SCHEDULE_HOST_DOMAIN, "s", &domain);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (skip) {
		free(domain);
		return (GFARM_ERR_NO_ERROR);
	}
	if (relay != NULL)
		return (GFARM_ERR_FUNCTION_NOT_IMPLEMENTED); /* XXX RELAY */

	/* XXX FIXME too long giant lock */
	giant_lock();
	/* XXXRELAY FIXME, reply size is not correct */
	e = host_schedule_reply_all(peer, xid, sizep,
	    up_and_domain_filter, domain, diag);
	giant_unlock();
	free(domain);

	return (e);
}

gfarm_error_t
gfm_server_statfs(struct peer *peer, gfp_xdr_xid_t xid, size_t *sizep,
	int from_client, int skip)
{
	gfarm_error_t e;
	gfarm_uint64_t used = 0, avail = 0, files = 0;
	struct relayed_request *relay;
	static const char diag[] = "GFM_PROTO_STATFS";

	e = gfm_server_get_request_with_relay(peer, sizep, skip, &relay, diag,
	      GFM_PROTO_STATFS, "");
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (skip)
		return (GFARM_ERR_NO_ERROR);

	if (relay == NULL) {
		/* do not relay RPC to master gfmd */
		files = inode_total_num();
		gfarm_mutex_lock(&total_disk_mutex, diag, total_disk_diag);
		used = total_disk_used;
		avail = total_disk_avail;
		gfarm_mutex_unlock(&total_disk_mutex, diag, total_disk_diag);
	}

	return (gfm_server_put_reply_with_relay(peer, xid, sizep, relay, diag,
	    &e, "lll", &used, &avail, &files));
}

#endif /* TEST */
