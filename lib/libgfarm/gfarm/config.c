/*
 * $Id$
 */

#include <pthread.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <assert.h>

#include <sys/socket.h>
#include <netinet/in.h> /* ntohs */
#include <netdb.h>

#include <time.h>
#include <pwd.h>

#include <gfarm/gfarm.h>

#include "gfutil.h"
#include "hash.h"
#include "lru_cache.h"

#include "context.h"
#include "liberror.h"
#include "patmatch.h"
#include "hostspec.h"
#if 0 /* not yet in gfarm v2 */
#include "param.h"
#endif
#include "sockopt.h"
#include "host.h" /* XXX address_use is disabled for now */
#include "auth.h"
#include "gfpath.h"
#include "config.h"
#include "gfm_proto.h" /* GFMD_DEFAULT_PORT */
#include "gfs_proto.h" /* GFSD_DEFAULT_PORT */
#include "gfs_profile.h"
#include "gfm_client.h"
#include "lookup.h"
#include "metadb_server.h"
#include "filesystem.h"
#include "conn_hash.h"
#include "conn_cache.h"

#define staticp	(gfarm_ctxp->config_static)

struct gfarm_config_static {
	char *config_file;

	/* xattr cache handling */
	gfarm_stringlist xattr_cache_list;

	/* Gfarm username handling */
	struct gfarm_hash_table *local_ug_maps_tab;

	/* client side variables */
	char *local_username;
	char *local_homedir;

	/* static configuration variables */
	int log_message_verbose;
	gfarm_int64_t minimum_free_disk_space;
	int profile;
};

gfarm_error_t
gfarm_config_static_init(struct gfarm_context *ctxp)
{
	struct gfarm_config_static *s;

	GFARM_MALLOC(s);
	if (s == NULL)
		return (GFARM_ERR_NO_MEMORY);

	s->config_file = GFARM_CONFIG;
	/* xattr_cache_list is initialized in gfarm_init_config() */
	s->local_ug_maps_tab = NULL;
	s->local_username = NULL;
	s->local_homedir = NULL;
	s->log_message_verbose = GFARM_CONFIG_MISC_DEFAULT;
	s->minimum_free_disk_space = GFARM_CONFIG_MISC_DEFAULT;
	s->profile = GFARM_CONFIG_MISC_DEFAULT;

	ctxp->config_static = s;
	return (GFARM_ERR_NO_ERROR);
}

void
gfarm_config_set_filename(char *filename)
{
	staticp->config_file = filename;
}

char *
gfarm_config_get_filename(void)
{
	return (staticp->config_file);
}

/* XXX move actual function definition here */
static gfarm_error_t gfarm_strtoken(char **, char **);

/*
 * NOTE:
 * client host should call gfs_stat_cache_clear() after
 * calling this gfarm_xattr_caching_pattern_add() function,
 * otherwise unexpected GFARM_ERR_NO_SUCH_OBJECT may happen.
 *
 * The reason we don't call gfs_stat_cache_clear() automatically is
 * because it's not appropriate for gfmd.
 */
gfarm_error_t
gfarm_xattr_caching_pattern_add(const char *attr_pattern)
{
	gfarm_error_t e;
	char *pat = strdup(attr_pattern);

	if (pat == NULL) {
		gflog_debug(GFARM_MSG_1002446,
		    "failed to allocate an attr_pattern \"%s\": no memory",
		    attr_pattern);
		return (GFARM_ERR_NO_MEMORY);
	}
	e = gfarm_stringlist_add(&staticp->xattr_cache_list, pat);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002447,
		    "failed to allocate record an attr_pattern \"%s\": "
		    "no memory", attr_pattern);
		free(pat);
	}
	return (e);
}

int
gfarm_xattr_caching(const char *attrname)
{
	gfarm_stringlist cache_list = staticp->xattr_cache_list;
	int i, n = gfarm_stringlist_length(&cache_list);
	const char *pattern;

	for (i = 0; i < n; i++) {
		pattern = gfarm_stringlist_elem(&cache_list, i);
		if (gfarm_pattern_match(pattern, attrname, 0))
			return (1);
	}
	return (0);
}

int
gfarm_xattr_caching_patterns_number(void)
{
	return (gfarm_stringlist_length(&staticp->xattr_cache_list));
}

char **
gfarm_xattr_caching_patterns(void)
{
	return (GFARM_STRINGLIST_STRARRAY(staticp->xattr_cache_list));
}

/*
 * GFarm username handling
 */

struct gfarm_local_ug_maps_id {
	char *hostname;
	int port;
};

struct gfarm_local_ug_maps {
	gfarm_stringlist local_user_map_file_list;
	gfarm_stringlist local_group_map_file_list;
};

#define LOCAL_UG_MAP_FILE_HASHTAB_SIZE 31

static int
local_ug_maps_hash_index(const void *key, int keylen)
{
	const struct gfarm_local_ug_maps_id *id = key;

	return (gfarm_hash_casefold(id->hostname, strlen(id->hostname)) +
	    id->port * 3);
}

int
local_ug_maps_hash_equal(const void *key1, int key1len,
	const void *key2, int key2len)
{
	const struct gfarm_local_ug_maps_id *id1 = key1, *id2 = key2;

	return (strcasecmp(id1->hostname, id2->hostname) == 0 &&
	    id1->port == id2->port);
}

#define DEFAULT_HOSTNAME_KEY	"."
#define DEFAULT_PORT_KEY	(-1)

static gfarm_error_t
local_ug_maps_enter(const char *hostname, int port, int is_user,
	const char *map_file)
{
	gfarm_error_t e;
	struct gfarm_hash_entry *entry;
	struct gfarm_local_ug_maps *ugm;
	struct gfarm_local_ug_maps_id id, *idp = NULL;
	char *s = NULL;
	int created;

	if (staticp->local_ug_maps_tab == NULL) {
		staticp->local_ug_maps_tab = gfarm_hash_table_alloc(
		    LOCAL_UG_MAP_FILE_HASHTAB_SIZE,
		    local_ug_maps_hash_index, local_ug_maps_hash_equal);
		if (staticp->local_ug_maps_tab == NULL) {
			gflog_debug(GFARM_MSG_1002524,
			    "allocation of hashtable failed: %s",
			    gfarm_error_string(GFARM_ERR_NO_MEMORY));
			return (GFARM_ERR_NO_MEMORY);
		}
	}

	if (hostname == NULL) {
		hostname = DEFAULT_HOSTNAME_KEY;
		port = DEFAULT_PORT_KEY;
	}
	id.hostname = (char *)hostname; /* UNCONST */
	id.port = port;
	entry = gfarm_hash_enter(staticp->local_ug_maps_tab, &id, sizeof(id),
	    sizeof(*ugm), &created);
	if (entry == NULL) {
		gflog_debug(GFARM_MSG_1002525,
		    "insertion to hashtable failed: %s",
		    gfarm_error_string(GFARM_ERR_NO_MEMORY));
		return (GFARM_ERR_NO_MEMORY);
	}
	if (created) {
		idp = gfarm_hash_entry_key(entry);
		idp->hostname = strdup(hostname);
		if (idp->hostname == NULL) {
			e = GFARM_ERR_NO_MEMORY;
			gflog_debug(GFARM_MSG_1002526,
			    "strdup failed: %s",
			    gfarm_error_string(e));
			goto error;
		}
		ugm = gfarm_hash_entry_data(entry);
		gfarm_stringlist_init(&ugm->local_user_map_file_list);
		gfarm_stringlist_init(&ugm->local_group_map_file_list);
	} else {
		ugm = gfarm_hash_entry_data(entry);
	}
	s = strdup(map_file);
	if (s == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		gflog_debug(GFARM_MSG_1002527,
		    "strdup failed: %s",
		    gfarm_error_string(e));
		goto error;
	}
	if (is_user) {
		if ((e = gfarm_stringlist_add(&ugm->local_user_map_file_list,
		    s)) != GFARM_ERR_NO_ERROR) {
			e = GFARM_ERR_NO_MEMORY;
			gflog_debug(GFARM_MSG_1002528,
			    "gfarm_stringlist_add failed: %s",
			    gfarm_error_string(e));
			goto error;
		}
	} else {
		if ((e = gfarm_stringlist_add(&ugm->local_group_map_file_list,
		    s)) != GFARM_ERR_NO_ERROR) {
			e = GFARM_ERR_NO_MEMORY;
			gflog_debug(GFARM_MSG_1002529,
			    "gfarm_stringlist_add failed: %s",
			    gfarm_error_string(e));
			goto error;
		}
	}

	return (GFARM_ERR_NO_ERROR);
error:
	if (created) {
		if (idp)
			free(idp->hostname);
		gfarm_hash_purge(staticp->local_ug_maps_tab, &id, sizeof(id));
	}
	free(s);
	return (e);
}

static void
local_ug_maps_tab_free()
{
	struct gfarm_hash_iterator it;
	struct gfarm_hash_entry *entry;
	struct gfarm_local_ug_maps *ugm;

	if (staticp->local_ug_maps_tab == NULL)
		return;

	for (gfarm_hash_iterator_begin(staticp->local_ug_maps_tab, &it);
	     !gfarm_hash_iterator_is_end(&it);) {
		entry = gfarm_hash_iterator_access(&it);
		ugm = gfarm_hash_entry_data(entry);
		gfarm_stringlist_free_deeply(&ugm->local_user_map_file_list);
		gfarm_stringlist_free_deeply(&ugm->local_group_map_file_list);
		gfarm_hash_iterator_purge(&it);
	}
}

static struct gfarm_local_ug_maps *
local_ug_maps_lookup(const char *hostname, int port)
{
	struct gfarm_hash_entry *entry = NULL;
	struct gfarm_local_ug_maps_id id;
	struct gfarm_hash_table *map = staticp->local_ug_maps_tab;

	if (map == NULL)
		return (NULL);
	if (hostname && port >= 0) {
		id.hostname = (char *)hostname; /* UNCONST */
		id.port = port;
		entry = gfarm_hash_lookup(map, &id, sizeof(id));
	}
	if (entry == NULL) {
		id.hostname = DEFAULT_HOSTNAME_KEY;
		id.port = DEFAULT_PORT_KEY;
		entry = gfarm_hash_lookup(map, &id, sizeof(id));
		if (entry == NULL)
			return (NULL);
	}
	return ((struct gfarm_local_ug_maps *)gfarm_hash_entry_data(entry));
}

#define LOCAL_USER_MAP_FILE_LIST(ugm, hostname, port) \
	((ugm = local_ug_maps_lookup((hostname), (port))) ? \
	&ugm->local_user_map_file_list : NULL)

#define LOCAL_GROUP_MAP_FILE_LIST(ugm, hostname, port) \
	((ugm = local_ug_maps_lookup((hostname), (port))) ? \
	&ugm->local_group_map_file_list : NULL)

/* the return value of the following function should be free(3)ed */
static gfarm_error_t
map_user(gfarm_stringlist *map_file_list, const char *from, char **to_p,
	const char *(*mapping)(const char *, const char *, const char *),
	gfarm_error_t error_redefined)
{
	gfarm_error_t e = GFARM_ERR_NO_ERROR;
	FILE *map = NULL;
	char *mapfile = NULL;
	int i, list_len, mapfile_mapped_index;
	char buffer[1024], *g_user, *l_user, *tmp;
	const char *mapped;
	int lineno = 0;

	*to_p = NULL;
	if (map_file_list == NULL)
		goto search_end;
	list_len = gfarm_stringlist_length(map_file_list);
	mapfile_mapped_index = list_len;
	for (i = 0; i < list_len; i++) {
		mapfile = gfarm_stringlist_elem(map_file_list, i);
		if ((map = fopen(mapfile, "r")) == NULL) {
			gflog_error(GFARM_MSG_1000009,
			    "%s: cannot open: %s", mapfile, strerror(errno));
			return (GFARM_ERR_CANT_OPEN);
		}
		lineno = 0;
		while (fgets(buffer, sizeof buffer, map) != NULL) {
			char *bp = buffer;

			lineno++;
			e = gfarm_strtoken(&bp, &g_user);
			if (e != GFARM_ERR_NO_ERROR)
				goto finish;
			if (g_user == NULL) /* blank or comment line */
				continue;
			e = gfarm_strtoken(&bp, &l_user);
			if (e != GFARM_ERR_NO_ERROR)
				goto finish;
			if (l_user == NULL) {
				e = GFARM_ERRMSG_MISSING_LOCAL_USER;
				goto finish;
			}
			mapped = (*mapping)(from, g_user, l_user);
			if (mapped != NULL) {
				if (*to_p != NULL &&
				    strcmp(mapped, *to_p) != 0 &&
				    i == mapfile_mapped_index) {
					e = error_redefined;
					goto finish;
				}
				if (*to_p == NULL) {
					*to_p = strdup(mapped);
					if (*to_p == NULL) {
						e = GFARM_ERR_NO_MEMORY;
						goto finish;
					}
				}
				mapfile_mapped_index = i;
			}
			e = gfarm_strtoken(&bp, &tmp);
			if (e != GFARM_ERR_NO_ERROR)
				goto finish;
			if (tmp != NULL) {
				e = GFARM_ERRMSG_TOO_MANY_ARGUMENTS;
				goto finish;
			}
		}
		fclose(map);
		map = NULL;
	}
search_end:
	if (*to_p == NULL) { /* not found */
	 	*to_p = strdup(from);
		if (*to_p == NULL)
			e = GFARM_ERR_NO_MEMORY;
	}	
finish:	
	if (map != NULL)
		fclose(map);
	if (e != GFARM_ERR_NO_ERROR) {
		if (*to_p != NULL)	 
			free(*to_p);
		gflog_error(GFARM_MSG_1000010,
		    "%s line %d: %s", mapfile, lineno,
		    gfarm_error_string(e));
	}
	return (e);
}

static const char *
map_global_to_local(const char *from, const char *global_user,
	const char *local_user)
{
	if (strcmp(from, global_user) == 0)
		return (local_user);
	return (NULL);
}

/* the return value of the following function should be free(3)ed */
gfarm_error_t
gfarm_global_to_local_username_by_host(const char *hostname, int port,
	const char *global_user, char **local_user_p)
{
	struct gfarm_local_ug_maps *ugm;
	return (map_user(LOCAL_USER_MAP_FILE_LIST(ugm, hostname, port),
	    global_user, local_user_p, map_global_to_local,
	    GFARM_ERRMSG_GLOBAL_USER_REDEFIEND));
}

gfarm_error_t
gfarm_global_to_local_username_by_url(const char *url,
	const char *global_user, char **local_user_p)
{
	gfarm_error_t e;
	char *hostname;
	int port;

	if ((e = gfarm_get_hostname_by_url(url, &hostname, &port))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002530,
		    "gfarm_get_hostname_by_url(%s) failed: %s",
		    url, gfarm_error_string(e));
		return (e);
	}
	if ((e = gfarm_global_to_local_username_by_host(hostname, port,
	    global_user, local_user_p)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002531,
		    "gfarm_global_to_local_username_by_host(%s,%d) (%s)"
		    " failed: %s",
		    hostname, port, url, gfarm_error_string(e));
	}
	free(hostname);
	return (e);
}

static const char *
map_local_to_global(const char *from, const char *global_user,
	const char *local_user)
{
	if (strcmp(from, local_user) == 0)
		return (global_user);
	return (NULL);
}

/* the return value of the following function should be free(3)ed */
gfarm_error_t
gfarm_local_to_global_username_by_host(const char *hostname, int port,
	const char *local_user, char **global_user_p)
{
	struct gfarm_local_ug_maps *ugm;
	return (map_user(LOCAL_USER_MAP_FILE_LIST(ugm, hostname, port),
	    local_user, global_user_p, map_local_to_global,
	    GFARM_ERRMSG_LOCAL_USER_REDEFIEND));
}

gfarm_error_t
gfarm_local_to_global_username_by_url(const char *url,
	const char *local_user, char **global_user_p)
{
	gfarm_error_t e;
	char *hostname;
	int port;

	if ((e = gfarm_get_hostname_by_url(url, &hostname, &port))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002532,
		    "gfarm_get_hostname_by_url(%s) failed: %s",
		    url, gfarm_error_string(e));
		return (e);
	}
	if ((e = gfarm_local_to_global_username_by_host(hostname, port,
	    local_user, global_user_p)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002533,
		    "gfarm_local_to_global_username_by_host(%s,%d) (%s)"
		    " failed: %s",
		    hostname, port, url, gfarm_error_string(e));
	}
	free(hostname);
	return (e);
}

/* the return value of the following function should be free(3)ed */
gfarm_error_t
gfarm_global_to_local_groupname_by_host(const char *hostname, int port,
	const char *global_group, char **local_group_p)
{
	struct gfarm_local_ug_maps *ugm;
	return (map_user(LOCAL_GROUP_MAP_FILE_LIST(ugm, hostname, port),
	    global_group, local_group_p, map_global_to_local,
	    GFARM_ERRMSG_GLOBAL_GROUP_REDEFIEND));
}

gfarm_error_t
gfarm_global_to_local_groupname_by_url(const char *url,
	const char *global_group, char **local_group_p)
{
	gfarm_error_t e;
	char *hostname;
	int port;

	if ((e = gfarm_get_hostname_by_url(url, &hostname, &port))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002534,
		    "gfarm_get_hostname_by_url(%s) failed: %s",
		    url, gfarm_error_string(e));
		return (e);
	}
	if ((e = gfarm_global_to_local_groupname_by_host(hostname, port,
	    global_group, local_group_p)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002535,
		    "gfarm_global_to_local_groupname_by_host(%s,%d) (%s)"
		    " failed: %s",
		    hostname, port, url, gfarm_error_string(e));
	}
	free(hostname);
	return (e);
}

/* the return value of the following function should be free(3)ed */
gfarm_error_t
gfarm_local_to_global_groupname_by_host(const char *hostname, int port,
	const char *local_group, char **global_group_p)
{
	struct gfarm_local_ug_maps *ugm;
	return (map_user(LOCAL_GROUP_MAP_FILE_LIST(ugm, hostname, port),
	    local_group, global_group_p, map_local_to_global,
	    GFARM_ERRMSG_LOCAL_GROUP_REDEFIEND));
}

gfarm_error_t
gfarm_local_to_global_groupname_by_url(const char *url,
	const char *local_group, char **global_group_p)
{
	gfarm_error_t e;
	char *hostname;
	int port;

	if ((e = gfarm_get_hostname_by_url(url, &hostname, &port))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002536,
		    "gfarm_get_hostname_by_url(%s) failed: %s",
		    url, gfarm_error_string(e));
		return (e);
	}
	if ((e = gfarm_local_to_global_groupname_by_host(hostname, port,
	    local_group, global_group_p)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002537,
		    "gfarm_local_to_global_groupname_by_host(%s,%d) (%s)"
		    " failed: %s",
		    hostname, port, url, gfarm_error_string(e));
	}
	free(hostname);
	return (e);
}

static gfarm_error_t
set_string(char **var, char *value)
{
	if (*var != NULL)
		free(*var);
	*var = strdup(value);
	if (*var == NULL) {
		gflog_debug(GFARM_MSG_1000918,
			"allocation of memory failed: %s",
			gfarm_error_string(GFARM_ERR_NO_MEMORY));
		return (GFARM_ERR_NO_MEMORY);
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfarm_get_global_username_by_url(const char *url, char **userp)
{
	gfarm_error_t e;
	char *hostname;
	int port;

	if ((e = gfarm_get_hostname_by_url(url, &hostname, &port))
	    != GFARM_ERR_NO_ERROR)
		return (e);
	e = gfarm_get_global_username_by_host(hostname, port, userp);
	free(hostname);
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfarm_get_global_username_by_host(const char *hostname, int port, char **userp)
{
	char *local_user = gfarm_get_local_username();
	return (gfarm_local_to_global_username_by_host(
	    hostname, port, local_user, userp));
}

gfarm_error_t
gfarm_set_local_username(char *local_username)
{
	return (set_string(&staticp->local_username, local_username));
}

char *
gfarm_get_local_username(void)
{
	return (staticp->local_username);
}

gfarm_error_t
gfarm_set_local_homedir(char *local_homedir)
{
	return (set_string(&staticp->local_homedir, local_homedir));
}

char *
gfarm_get_local_homedir(void)
{
	return (staticp->local_homedir);
}

/*
 * We should not trust gfarm_get_*() values as a result of this function
 * (because it may be forged).
 */
gfarm_error_t
gfarm_set_local_user_for_this_local_account(void)
{
	gfarm_error_t error;
	struct passwd pwbuf, *pwd;
	char *buf;

	GFARM_MALLOC_ARRAY(buf, gfarm_ctxp->getpw_r_bufsz);
	if (buf == NULL) {
		error = GFARM_ERR_NO_MEMORY;
		gflog_error(GFARM_MSG_1000011, "gfarm_set_local_user: %s",
			gfarm_error_string(error));
		return (error);
	}
	if (getpwuid_r(geteuid(), &pwbuf, buf, gfarm_ctxp->getpw_r_bufsz,
	    &pwd) != 0) {
		gflog_error(GFARM_MSG_1000012, "local account doesn't exist");
		error = GFARM_ERR_NO_SUCH_OBJECT;
		goto error;
	}
	error = gfarm_set_local_username(pwd->pw_name);
	if (error == GFARM_ERR_NO_ERROR)
		error = gfarm_set_local_homedir(pwd->pw_dir);
 error:
	free(buf);
	return (error);
}

/*
 * GFarm Configurations.
 *
 * Initial string values should be NULL, otherwise the value incorrectly
 * free(3)ed in the gfarm_config_clear() function below.
 * If you would like to provide default value other than NULL, set the
 * value at gfarm_config_set_default*().
 */
/* GFS dependent */
char *gfarm_spool_server_listen_address = NULL;
char *gfarm_spool_root = NULL;

/* GFM dependent */
enum gfarm_backend_db_type gfarm_backend_db_type =
	GFARM_BACKEND_DB_TYPE_UNKNOWN;

/* LDAP dependent */
char *gfarm_ldap_server_name = NULL;
char *gfarm_ldap_server_port = NULL;
char *gfarm_ldap_base_dn = NULL;
char *gfarm_ldap_bind_dn = NULL;
char *gfarm_ldap_bind_password = NULL;
char *gfarm_ldap_tls = NULL;
char *gfarm_ldap_tls_cipher_suite = NULL;
char *gfarm_ldap_tls_certificate_key_file = NULL;
char *gfarm_ldap_tls_certificate_file = NULL;

/* PostgreSQL dependent */
char *gfarm_postgresql_server_name = NULL;
char *gfarm_postgresql_server_port = NULL;
char *gfarm_postgresql_dbname = NULL;
char *gfarm_postgresql_user = NULL;
char *gfarm_postgresql_password = NULL;
char *gfarm_postgresql_conninfo = NULL;

/* LocalFS dependent */
char *gfarm_localfs_datadir = NULL;

/* miscellaneous */
#define GFARM_LOG_MESSAGE_VERBOSE_DEFAULT	0
#define GFARM_NO_FILE_SYSTEM_NODE_TIMEOUT_DEFAULT 30 /* 30 seconds */
#define GFARM_GFMD_RECONNECTION_TIMEOUT_DEFAULT 30 /* 30 seconds */
#define GFARM_ATTR_CACHE_LIMIT_DEFAULT		40000 /* 40,000 entries */
#define GFARM_ATTR_CACHE_TIMEOUT_DEFAULT	1000 /* 1,000 milli second */
#define GFARM_SCHEDULE_CACHE_TIMEOUT_DEFAULT 600 /* 10 minutes */
#define GFARM_SCHEDULE_IDLE_LOAD_DEFAULT	0.1F
#define GFARM_SCHEDULE_BUSY_LOAD_DEFAULT	0.5F
#define GFARM_SCHEDULE_VIRTUAL_LOAD_DEFAULT	0.3F
#define GFARM_SCHEDULE_WRITE_LOCAL_PRIORITY_DEFAULT 1 /* enable */
#define GFARM_MINIMUM_FREE_DISK_SPACE_DEFAULT	(128 * 1024 * 1024) /* 128MB */
#define GFARM_SIMULTANEOUS_REPLICATION_RECEIVERS_DEFAULT	20
#define GFARM_GFSD_CONNECTION_CACHE_DEFAULT 16 /* 16 free connections */
#define GFARM_GFMD_CONNECTION_CACHE_DEFAULT  8 /*  8 free connections */
#define GFARM_RECORD_ATIME_DEFAULT 1 /* enable */
#define GFARM_CLIENT_FILE_BUFSIZE_DEFAULT	(1048576 - 8) /* 1MB - 8B */
#define GFARM_PROFILE_DEFAULT 0 /* disable */
#define GFARM_METADB_REPLICATION_ENABLED_DEFAULT	0
#define GFARM_JOURNAL_MAX_SIZE_DEFAULT		(32 * 1024 * 1024) /* 32MB */
#define GFARM_JOURNAL_RECVQ_SIZE_DEFAULT	100000
#define GFARM_JOURNAL_SYNC_FILE_DEFAULT		1
#define GFARM_JOURNAL_SYNC_SLAVE_TIMEOUT_DEFAULT 10 /* 10 second */
#define GFARM_METADB_SERVER_SLAVE_MAX_SIZE_DEFAULT	16
#define GFARM_METADB_SERVER_FORCE_SLAVE_DEFAULT		0
#define GFARM_NETWORK_RECEIVE_TIMEOUT_DEFAULT  20 /* 20 seconds */
#define GFARM_FILE_TRACE_DEFAULT 0 /* disable */
#if 0 /* not yet in gfarm v2 */
static char *schedule_write_target_domain = NULL;
static int schedule_write_local_priority = GFARM_CONFIG_MISC_DEFAULT;
#endif
int gfarm_simultaneous_replication_receivers = GFARM_CONFIG_MISC_DEFAULT;
int gfarm_metadb_stack_size = GFARM_CONFIG_MISC_DEFAULT;
int gfarm_metadb_thread_pool_size = GFARM_CONFIG_MISC_DEFAULT;
int gfarm_metadb_job_queue_length = GFARM_CONFIG_MISC_DEFAULT;
int gfarm_metadb_heartbeat_interval = GFARM_CONFIG_MISC_DEFAULT;
int gfarm_metadb_dbq_size = GFARM_CONFIG_MISC_DEFAULT;
static int metadb_replication_enabled = GFARM_CONFIG_MISC_DEFAULT;
static char *journal_dir = NULL;
static int journal_max_size = GFARM_CONFIG_MISC_DEFAULT;
static int journal_recvq_size = GFARM_CONFIG_MISC_DEFAULT;
static int journal_sync_file = GFARM_CONFIG_MISC_DEFAULT;
static int journal_sync_slave_timeout = GFARM_CONFIG_MISC_DEFAULT;
static int metadb_server_slave_max_size = GFARM_CONFIG_MISC_DEFAULT;
static int metadb_server_force_slave = GFARM_CONFIG_MISC_DEFAULT;

void
gfarm_config_clear(void)
{
	static char **vars[] = {
		&gfarm_spool_server_listen_address,
		&gfarm_spool_root,
		&gfarm_ldap_server_name,
		&gfarm_ldap_server_port,
		&gfarm_ldap_base_dn,
		&gfarm_ldap_bind_dn,
		&gfarm_ldap_bind_password,
		&gfarm_ldap_tls,
		&gfarm_ldap_tls_cipher_suite,
		&gfarm_ldap_tls_certificate_key_file,
		&gfarm_ldap_tls_certificate_file,
		&gfarm_postgresql_server_name,
		&gfarm_postgresql_server_port,
		&gfarm_postgresql_dbname,
		&gfarm_postgresql_user,
		&gfarm_postgresql_password,
		&gfarm_postgresql_conninfo,
		&gfarm_localfs_datadir,
#if 0 /* not yet in gfarm v2 */
		&schedule_write_target_domain,
#endif
		&journal_dir,
	};
	int i;

	for (i = 0; i < GFARM_ARRAY_LENGTH(vars); i++) {
		if (*vars[i] != NULL) {
			free(*vars[i]);
			*vars[i] = NULL;
		}
	}

#if 0 /* XXX */
	config_read = gfarm_config_not_read;
#endif
}

static gfarm_error_t
set_backend_db_type(enum gfarm_backend_db_type set)
{
	if (gfarm_backend_db_type == set)
		return (GFARM_ERR_NO_ERROR);
	switch (gfarm_backend_db_type) {
	case GFARM_BACKEND_DB_TYPE_UNKNOWN:
		gfarm_backend_db_type = set;
		return (GFARM_ERR_NO_ERROR);
	case GFARM_BACKEND_DB_TYPE_LDAP:
		return (GFARM_ERRMSG_BACKEND_ALREADY_LDAP);
	case GFARM_BACKEND_DB_TYPE_POSTGRESQL:
		return (GFARM_ERRMSG_BACKEND_ALREADY_POSTGRESQL);
	case GFARM_BACKEND_DB_TYPE_LOCALFS:
		return (GFARM_ERRMSG_BACKEND_ALREADY_LOCALFS);
	default:
		assert(0);
		return (GFARM_ERR_UNKNOWN); /* workaround compiler warning */
	}
}

static gfarm_error_t
set_backend_db_type_ldap(void)
{
	return (set_backend_db_type(GFARM_BACKEND_DB_TYPE_LDAP));
}

static gfarm_error_t
set_backend_db_type_postgresql(void)
{
	return (set_backend_db_type(GFARM_BACKEND_DB_TYPE_POSTGRESQL));
}

static gfarm_error_t
set_backend_db_type_localfs(void)
{
	return (set_backend_db_type(GFARM_BACKEND_DB_TYPE_LOCALFS));
}

#if 0 /* not yet in gfarm v2 */
int
gfarm_schedule_write_local_priority(void)
{
	return (schedule_write_local_priority);
}

char *
gfarm_schedule_write_target_domain(void)
{
	return (schedule_write_target_domain);
}
#endif

gfarm_off_t
gfarm_get_minimum_free_disk_space(void)
{
	return (staticp->minimum_free_disk_space);
}

void
gfarm_set_record_atime(int boolean)
{
	gfarm_ctxp->record_atime = boolean;
}

int
gfarm_get_metadb_replication_enabled(void)
{
	return (metadb_replication_enabled);
}

void
gfarm_set_metadb_replication_enabled(int enable)
{
	metadb_replication_enabled = enable;
}

const char *
gfarm_get_journal_dir(void)
{
	return (journal_dir);
}

int
gfarm_get_journal_max_size(void)
{
	return (journal_max_size);
}

int
gfarm_get_journal_recvq_size(void)
{
	return (journal_recvq_size);
}

int
gfarm_get_journal_sync_file(void)
{
	return (journal_sync_file);
}

int
gfarm_get_journal_sync_slave_timeout(void)
{
	return (journal_sync_slave_timeout);
}

int
gfarm_get_metadb_server_slave_max_size(void)
{
	return (metadb_server_slave_max_size);
}

int
gfarm_get_metadb_server_force_slave(void)
{
	return (metadb_server_force_slave);
}

void
gfarm_set_metadb_server_force_slave(int slave)
{
	metadb_server_force_slave = slave;
}

/*
 * get (almost) shell style token.
 * e.g.
 *	string...
 *	'string...' (do not interpret escape character `\')
 *	"string..." (interpret escape character `\')
 *	# comment
 * difference from shell token:
 *	don't allow newline in "..." and '...".
 *
 * return value:
 *	string
 *   OR
 *	NULL	- if error or end-of-line.
 * output parameter:
 *	*cursorp:
 *		next character to read
 *	*errorp:
 *		NULL (if success or end-of-line)
 *	    OR
 *		error message
 */

gfarm_error_t
gfarm_strtoken(char **cursorp, char **tokenp)
{
	unsigned char *top, *p, *s = *(unsigned char **)cursorp;

	while (*s != '\n' && isspace(*s))
		s++;
	if (*s == '\0' || *s == '\n' || *s == '#') {
		/* end of line */
		*cursorp = (char *)s;
		*tokenp = NULL;
		return (GFARM_ERR_NO_ERROR);
	}
	top = s;
	p = s;
	for (;;) {
		switch (*s) {
		case '\'':
			s++;
			for (;;) {
				if (*s == '\'')
					break;
				if (*s == '\0' || *s == '\n') {
					gflog_debug(GFARM_MSG_1000919,
						"Unterminated single quote "
						"found in string");
					return (GFARM_ERRMSG_UNTERMINATED_SINGLE_QUOTE);
				}
				*p++ = *s++;
			}
			s++;
			break;
		case '"':
			s++;
			for (;;) {
				if (*s == '"')
					break;
				if (*s == '\0' || *s == '\n') {
					gflog_debug(GFARM_MSG_1000920,
						"Unterminated double quote "
						"found in string");
					return (GFARM_ERRMSG_UNTERMINATED_DOUBLE_QUOTE);
				}
				if (*s == '\\') {
					if (s[1] == '\0' || s[1] == '\n') {
						gflog_debug(GFARM_MSG_1000921,
							"Unterminated double "
							"quote found in string"
						);
						return (GFARM_ERRMSG_UNTERMINATED_DOUBLE_QUOTE);
					}
					/*
					 * only interpret `\"' and `\\'
					 * in double quotation.
					 */
					if (s[1] == '"' || s[1] == '\\')
						s++;
				}
				*p++ = *s++;
			}
			s++;
			break;
		case '\\':
			s++;
			if (*s == '\0' || *s == '\n') {
				gflog_debug(GFARM_MSG_1000922,
					"Incomplete escape found in string");
				return (GFARM_ERRMSG_INCOMPLETE_ESCAPE);
			}
			*p++ = *s++;
			break;
		case '\n':	
		case '#':
		case '\0':
			*p = '\0';
			*cursorp = (char *)s;
			*tokenp = (char *)top;
			return (GFARM_ERR_NO_ERROR);
		default:
			if (isspace(*s)) {
				*p = '\0';
				*cursorp = (char *)(s + 1);
				*tokenp = (char *)top;
				return (GFARM_ERR_NO_ERROR);
			}
			*p++ = *s++;
			break;
		}
	}
}

static gfarm_error_t
parse_auth_arguments(char *p, char **op)
{
	gfarm_error_t e;
	char *tmp, *command, *auth, *host;
	enum gfarm_auth_method auth_method;
	struct gfarm_hostspec *hostspecp;

	/* assert(strcmp(*op, "auth") == 0); */

	e = gfarm_strtoken(&p, &command);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1000923,
			"parsing of auth command argument (%s) failed: %s",
			p, gfarm_error_string(e));
		return (e);
	}
	if (command == NULL) {
		gflog_debug(GFARM_MSG_1000924,
			"Missing first auth command argument");
		return (GFARM_ERRMSG_MISSING_1ST_AUTH_COMMAND_ARGUMENT);
	}

	e = gfarm_strtoken(&p, &auth);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1000925,
			"parsing of auth method argument (%s) failed: %s",
			p, gfarm_error_string(e));
		return (e);
	}
	if (auth == NULL) {
		gflog_debug(GFARM_MSG_1000926,
			"Missing second auth method argument");
		return (GFARM_ERRMSG_MISSING_2ND_AUTH_METHOD_ARGUMENT);
	}
	if (strcmp(auth, "*") == 0 || strcmp(auth, "ALL") == 0) {
		auth_method = GFARM_AUTH_METHOD_ALL;
	} else {
		e = gfarm_auth_method_parse(auth, &auth_method);
		if (e != GFARM_ERR_NO_ERROR) {
			*op = "2nd(auth-method) argument";
			if (e == GFARM_ERR_NO_SUCH_OBJECT)
				e = GFARM_ERRMSG_UNKNOWN_AUTH_METHOD;
			gflog_debug(GFARM_MSG_1000927,
				"parsing of auth method (%s) failed: %s",
				auth, gfarm_error_string(e));
			return (e);
		}
	}

	e = gfarm_strtoken(&p, &host);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1000928,
			"parsing of auth host argument (%s) failed: %s",
			p, gfarm_error_string(e));
		return (e);
	}
	if (host == NULL) {
		gflog_debug(GFARM_MSG_1000929,
			"Missing third auth host spec argument");
		return (GFARM_ERRMSG_MISSING_3RD_HOST_SPEC_ARGUMENT);
	}
	e = gfarm_strtoken(&p, &tmp);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1000930,
			"parsing of auth arguments (%s) failed: %s",
			p, gfarm_error_string(e));
		return (e);
	}
	if (tmp != NULL) {
		gflog_debug(GFARM_MSG_1000931,
			"Too many auth arguments passed");
		return (GFARM_ERRMSG_TOO_MANY_ARGUMENTS);
	}
	e = gfarm_hostspec_parse(host, &hostspecp);
	if (e != GFARM_ERR_NO_ERROR) {
		/*
		 * we don't return `host' to *op here,
		 * because it may be too long.
		 */
		*op = "3rd(host-spec) argument";
		gflog_debug(GFARM_MSG_1000932,
			"parsing of auth host spec (%s) failed: %s",
			host, gfarm_error_string(e));
		return (e);
	}

	if (strcmp(command, "enable") == 0) {
		e = gfarm_auth_enable(auth_method, hostspecp);
	} else if (strcmp(command, "disable") == 0) {
		e = gfarm_auth_disable(auth_method, hostspecp);
	} else {
		/*
		 * we don't return `command' to *op here,
		 * because it may be too long.
		 */
		*op = "1st(auth-command) argument";
		gfarm_hostspec_free(hostspecp);
		gflog_debug(GFARM_MSG_1000933,
			"Unknown auth subcommand (%s)",
			command);
		return (GFARM_ERRMSG_UNKNOWN_AUTH_SUBCOMMAND);
	}
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1000934,
			"Failed to enable/disable auth (%s)(%s)(%s): (%s)",
			command, auth, host,
			gfarm_error_string(e));
		gfarm_hostspec_free(hostspecp);
	}
	return (e);
}

#if 0 /* not yet in gfarm v2 */
static gfarm_error_t
parse_netparam_arguments(char *p, char **op)
{
	gfarm_error_t e;
	char *tmp, *option, *host;
	struct gfarm_hostspec *hostspecp;

	/* assert(strcmp(*op, "netparam") == 0); */

	e = gfarm_strtoken(&p, &option);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1000935,
			"parsing of netparam option argument (%s) failed: %s",
			p, gfarm_error_string(e));
		return (e);
	}
	if (option == NULL) {
		gflog_debug(GFARM_MSG_1000936,
			"Missing first netparam option argument");
		return (GFARM_ERRMSG_MISSING_NETPARAM_OPTION_ARGUMENT);
	}

	e = gfarm_strtoken(&p, &host);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1000937,
			"parsing of netparam host argument (%s) failed: %s",
			p, gfarm_error_string(e));
		return (e);
	}
	if (host == NULL) {
		/* if 2nd argument is omitted, it is treated as "*". */
		host = "*";
	} else if ((e = gfarm_strtoken(&p, &tmp)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1000938,
			"parsing of netparam arguments (%s) failed: %s",
			p, gfarm_error_string(e));
		return (e);
	} else if (tmp != NULL) {
		gflog_debug(GFARM_MSG_1000939,
			"Too many netparam arguments passed");
		return (GFARM_ERRMSG_TOO_MANY_ARGUMENTS);
	}
	
	e = gfarm_hostspec_parse(host, &hostspecp);
	if (e != GFARM_ERR_NO_ERROR) {
		/*
		 * we don't return `host' to *op here,
		 * because it may be too long.
		 */
		*op = "2nd(host-spec) argument";
		gflog_debug(GFARM_MSG_1000940,
			"parsing of netparam host spec (%s) failed: %s",
			host, gfarm_error_string(e));
		return (e);
	}

	e = gfarm_netparam_config_add_long(option, hostspecp);
	if (e != GFARM_ERR_NO_ERROR) {
		/*
		 * we don't return `option' to *op here,
		 * because it may be too long.
		 */
		*op = "1st(sockopt-option) argument";
		gfarm_hostspec_free(hostspecp);
		gflog_debug(GFARM_MSG_1000941,
			"add netparam config (%s)(%s) failed: %s",
			host, option, gfarm_error_string(e));
		return (e);
	}
	return (GFARM_ERR_NO_ERROR);
}
#endif

static gfarm_error_t
parse_sockopt_arguments(char *p, char **op)
{
	gfarm_error_t e;
	char *tmp, *option, *host;
	struct gfarm_hostspec *hostspecp;
	int is_listener;

	/* assert(strcmp(*op, "sockopt") == 0); */

	e = gfarm_strtoken(&p, &option);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1000942,
			"parsing of sockopt option argument (%s) failed: %s",
			p, gfarm_error_string(e));
		return (e);
	}
	if (option == NULL) {
		gflog_debug(GFARM_MSG_1000943,
			"Missing sockopt option argument");
		return (GFARM_ERRMSG_MISSING_SOCKOPT_OPTION_ARGUMENT);
	}

	e = gfarm_strtoken(&p, &host);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1000944,
			"parsing of sockopt host argument (%s) failed: %s",
			p, gfarm_error_string(e));
		return (e);
	}
	if (host == NULL) {
		/*
		 * if 2nd argument is omitted, it is treated as:
		 *	"LISTENER" + "*".
		 */
		is_listener = 1;
	} else {
		is_listener = strcmp(host, "LISTENER") == 0;
		if ((e = gfarm_strtoken(&p, &tmp)) != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1000945,
				"parsing of sockopt arguments (%s) failed: %s",
				p, gfarm_error_string(e));
			return (e);
		}
		if (tmp != NULL) {
			gflog_debug(GFARM_MSG_1000946,
				"Too many sockopt arguments passed");
			return (GFARM_ERRMSG_TOO_MANY_ARGUMENTS);
		}
	}
	
	if (is_listener) {
		e = gfarm_sockopt_listener_config_add(option);
		if (e != GFARM_ERR_NO_ERROR) {
			/*
			 * we don't return `option' to *op here,
			 * because it may be too long.
			 */
			*op = "1st(sockopt-option) argument";
			gflog_debug(GFARM_MSG_1000947,
			    "cannot set sockopt %s for listener: %s",
			    option, gfarm_error_string(e));
			return (e);
		}
	}
	if (host == NULL || !is_listener) {
		e = gfarm_hostspec_parse(host != NULL ? host : "*",
		    &hostspecp);
		if (e != GFARM_ERR_NO_ERROR) {
			/*
			 * we don't return `host' to *op here,
			 * because it may be too long.
			 */
			*op = "2nd(host-spec) argument";
			gflog_debug(GFARM_MSG_1000948,
				"parsing of sockopt host (%s) failed: %s",
				host, gfarm_error_string(e));
			return (e);
		}

		e = gfarm_sockopt_config_add(option, hostspecp);
		if (e != GFARM_ERR_NO_ERROR) {
			/*
			 * we don't return `option' to *op here,
			 * because it may be too long.
			 */
			*op = "1st(sockopt-option) argument";
			gfarm_hostspec_free(hostspecp);
			gflog_debug(GFARM_MSG_1000949,
			    "cannot set sockopt %s for host %s: %s",
			    option, host == NULL ? "*" : host,
			    gfarm_error_string(e));
			return (e);
		}
	}
	return (GFARM_ERR_NO_ERROR);
}

#if 0 /* XXX address_use is disabled for now */
static gfarm_error_t
parse_address_use_arguments(char *p, char **op)
{
	gfarm_error_t e;
	char *tmp, *address;
	struct gfarm_hostspec *hostspecp;

	/* assert(strcmp(*op, "address_use") == 0); */

	e = gfarm_strtoken(&p, &address);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (address == NULL)
		return (GFARM_ERRMSG_MISSING_ADDRESS_ARGUMENT);
	e = gfarm_strtoken(&p, &tmp);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (tmp != NULL)
		return (GFARM_ERRMSG_TOO_MANY_ARGUMENTS);

	e = gfarm_hostspec_parse(address, &hostspecp);
	if (e != GFARM_ERR_NO_ERROR) {
		/*
		 * we don't return `host' to *op here,
		 * because it may be too long.
		 */
		*op = "1st(address) argument";
		return (e);
	}

	e = gfarm_host_address_use(hostspecp);
	if (e != GFARM_ERR_NO_ERROR) {
		/*
		 * we don't return `option' to *op here,
		 * because it may be too long.
		 */
		*op = "1st(address) argument";
		gfarm_hostspec_free(hostspecp);
		return (e);
	}
	return (GFARM_ERR_NO_ERROR);
}
#endif

static gfarm_error_t
parse_known_network_arguments(char *p, char **op)
{
	gfarm_error_t e;
	char *tmp, *address;
	struct gfarm_hostspec *hostspecp;

	/* assert(strcmp(*op, "known_network") == 0); */

	e = gfarm_strtoken(&p, &address);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (address == NULL)
		return (GFARM_ERRMSG_MISSING_ADDRESS_ARGUMENT);
	e = gfarm_strtoken(&p, &tmp);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (tmp != NULL)
		return (GFARM_ERRMSG_TOO_MANY_ARGUMENTS);

	e = gfarm_hostspec_parse(address, &hostspecp);
	if (e != GFARM_ERR_NO_ERROR) {
		/*
		 * we don't return `host' to *op here,
		 * because it may be too long.
		 */
		*op = "1st(address) argument";
		return (e);
	}

	e = gfarm_known_network_list_add(hostspecp);
	if (e != GFARM_ERR_NO_ERROR) {
		/*
		 * we don't return `option' to *op here,
		 * because it may be too long.
		 */
		*op = "1st(address) argument";
		gfarm_hostspec_free(hostspecp);
		return (e);
	}
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
parse_stringlist(char *p, char **op,
	gfarm_stringlist *list, const char *listname)
{
	gfarm_error_t e;
	char *tmp, *arg;

	e = gfarm_strtoken(&p, &arg);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002448,
		    "parsing argument %s of %s failed: %s",
		    p, listname, gfarm_error_string(e));
		return (e);
	}
	if (arg == NULL) {
		gflog_debug(GFARM_MSG_1002449,
		    "missing argument for %s", listname);
		return (GFARM_ERRMSG_MISSING_USER_MAP_FILE_ARGUMENT);
	}
	e = gfarm_strtoken(&p, &tmp);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002450,
		    "parsing argument %s of %s failed: %s",
		    p, listname, gfarm_error_string(e));
		return (e);
	}
	if (tmp != NULL) {
		gflog_debug(GFARM_MSG_1002451,
		    "Too many arguments for %s", listname);
		return (GFARM_ERRMSG_TOO_MANY_ARGUMENTS);
	}
	arg = strdup(arg);
	if (arg == NULL) {
		gflog_debug(GFARM_MSG_1002452,
		    "failed to allocate an argument of %s: no memory",
		    listname);
		return (GFARM_ERR_NO_MEMORY);
	}
	e = gfarm_stringlist_add(list, arg);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002453,
		    "failed to allocate a %s entry for \"%s\": no memory",
		    listname, arg);
		free(arg);
	}
	return (e);
}

#if 0 /* XXX NOTYET */
static gfarm_error_t
parse_client_architecture(char *p, char **op)
{
	gfarm_error_t e;
	char *architecture, *host, *junk;
	struct gfarm_hostspec *hostspecp;

	e = gfarm_strtoken(&p, &architecture);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (architecture == NULL)
		return (GFARM_ERRMSG_MISSING_1ST_ARCHITECTURE_ARGUMENT);
	e = gfarm_strtoken(&p, &host);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (host == NULL)
		return (GFARM_ERRMSG_MISSING_2ND_HOST_SPEC_ARGUMENT);
	e = gfarm_strtoken(&p, &junk);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (junk != NULL)
		return (GFARM_ERRMSG_TOO_MANY_ARGUMENTS);
	e = gfarm_hostspec_parse(host, &hostspecp);
	if (e != GFARM_ERR_NO_ERROR) {
		/*
		 * we don't return `host' to *op here,
		 * because it may be too long.
		 */
		*op = "2nd(host-spec) argument";
		return (e);
	}
	e = gfarm_set_client_architecture(architecture, hostspecp);
	if (e != GFARM_ERR_NO_ERROR)
		gfarm_hostspec_free(hostspecp);
	return (e);
}
#endif /* XXX NOTYET */

static gfarm_error_t
get_one_argument(char *p, char **rv)
{
	gfarm_error_t e;
	char *tmp, *s;

	e = gfarm_strtoken(&p, &s);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1000955,
			"parsing of one argument (%s) failed: %s",
			p, gfarm_error_string(e));
		return (e);
	}
	if (s == NULL) {
		gflog_debug(GFARM_MSG_1000956,
			"Missing argument");
		return (GFARM_ERRMSG_MISSING_ARGUMENT);
	}
	e = gfarm_strtoken(&p, &tmp);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1000957,
			"parsing of arguments (%s) failed: %s",
			p, gfarm_error_string(e));
		return (e);
	}
	if (tmp != NULL) {
		gflog_debug(GFARM_MSG_1000958,
			"Too many arguments passed");
		return (GFARM_ERRMSG_TOO_MANY_ARGUMENTS);
	}

	*rv = s;
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
parse_set_var(char *p, char **rv)
{
	gfarm_error_t e;
	char *s;

	e = get_one_argument(p, &s);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1000959,
			"get_one_argument failed "
			"when parsing var (%s): %s",
			p, gfarm_error_string(e));
		return (e);
	}

	if (*rv != NULL) /* first line has precedence */
		return (GFARM_ERR_NO_ERROR);
	s = strdup(s);
	if (s == NULL) {
		gflog_debug(GFARM_MSG_1000960,
			"allocation of argument failed when parsing set var: %s",
			gfarm_error_string(GFARM_ERR_NO_MEMORY));
		return (GFARM_ERR_NO_MEMORY);
	}
	*rv = s;
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
parse_set_misc_int(char *p, int *vp)
{
	gfarm_error_t e;
	char *ep, *s;
	int v;

	e = get_one_argument(p, &s);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1000961,
			"get_one_argument failed "
			"when parsing misc integer (%s): %s",
			p, gfarm_error_string(e));
		return (e);
	}

	if (*vp != GFARM_CONFIG_MISC_DEFAULT) /* first line has precedence */
		return (GFARM_ERR_NO_ERROR);
	errno = 0;
	v = strtol(s, &ep, 10);
	if (errno != 0) {
		int save_errno = errno;
		gflog_debug(GFARM_MSG_1000962,
			"conversion to integer failed "
			"when parsing misc integer (%s): %s",
			p, strerror(save_errno));
		return (gfarm_errno_to_error(save_errno));
	}
	if (ep == s) {
		gflog_debug(GFARM_MSG_1000963,
			"Integer expected when parsing misc integer but (%s)",
			s);
		return (GFARM_ERRMSG_INTEGER_EXPECTED);
	}
	if (*ep != '\0') {
		gflog_debug(GFARM_MSG_1000964,
			"Invalid character found "
			"when parsing misc integer (%s)",
			s);
		return (GFARM_ERRMSG_INVALID_CHARACTER);
	}
	*vp = v;
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
parse_set_misc_float(char *p, float *vp)
{
	gfarm_error_t e;
	char *ep, *s;
	double v;

	e = get_one_argument(p, &s);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	if (*vp != GFARM_CONFIG_MISC_DEFAULT) /* first line has precedence */
		return (GFARM_ERR_NO_ERROR);
	errno = 0;
	v = strtod(s, &ep);	/* strtof is not ANSI C standard */
	if (errno != 0)
		return (gfarm_errno_to_error(errno));
	if (ep == s)
		return (GFARM_ERRMSG_FLOATING_POINT_NUMBER_EXPECTED);
	if (*ep != '\0')
		return (GFARM_ERRMSG_INVALID_CHARACTER);
	*vp = (float)v;
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
parse_set_misc_offset(char *p, gfarm_off_t *vp)
{
	gfarm_error_t e;
	char *ep, *s;
	gfarm_off_t v;

	e = get_one_argument(p, &s);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1000965,
			"get_one_argument failed "
			"when parsing misc offset (%s): %s",
			p, gfarm_error_string(e));
		return (e);
	}

	if (*vp != GFARM_CONFIG_MISC_DEFAULT) /* first line has precedence */
		return (GFARM_ERR_NO_ERROR);
	errno = 0;
	v = gfarm_strtoi64(s, &ep);
	if (errno != 0) {
		int save_errno = errno;
		gflog_debug(GFARM_MSG_1000966,
			"conversion to int64 failed "
			"when parsing misc offset (%s): %s",
			p, strerror(save_errno));
		return (gfarm_errno_to_error(save_errno));
	}
	if (ep == s) {
		gflog_debug(GFARM_MSG_1000967,
			"Integer expected when parsing misc offset but (%s)",
			s);
		return (GFARM_ERRMSG_INTEGER_EXPECTED);
	}
	if (*ep != '\0') {
		switch (*ep) {
		case 'k': case 'K': ep++; v *= 1024; break;
		case 'm': case 'M': ep++; v *= 1024 * 1024; break;
		case 'g': case 'G': ep++; v *= 1024 * 1024 * 1024; break;
		case 't': case 'T': ep++; v *=1024*1024; v *=1024*1024; break;
		}
		if (*ep != '\0') {
			gflog_debug(GFARM_MSG_1000968,
				"Invalid character found "
				"when parsing misc offset (%s)",
				s);
			return (GFARM_ERRMSG_INVALID_CHARACTER);
		}
	}
	*vp = v;
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
parse_set_misc_enabled(char *p, int *vp)
{
	gfarm_error_t e;
	char *s;
	int v;

	e = get_one_argument(p, &s);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1000969,
			"get_one_argument failed "
			"when parsing misc enabled (%s): %s",
			p, gfarm_error_string(e));
		return (e);
	}

	if (*vp != GFARM_CONFIG_MISC_DEFAULT) /* first line has precedence */
		return (GFARM_ERR_NO_ERROR);
	if (strcmp(s, "enable") == 0)
		v = 1;
	else if (strcmp(s, "disable") == 0)
		v = 0;
	else {
		gflog_debug(GFARM_MSG_1000970,
			"'enable' or 'disable' expected "
			"when parsing misc enabled but (%s)",
			s);
		return (GFARM_ERRMSG_ENABLED_OR_DISABLED_EXPECTED);
	}
	*vp = v;
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
parse_metadb_server_port(char *p, char **op)
{
	char *s;
	const char *listname = *op;
	struct servent *sp;
	int port;
	gfarm_error_t e;

	e = get_one_argument(p, &s);
	if (e != GFARM_ERR_NO_ERROR) {
		*op = "port argument";
		gflog_debug(GFARM_MSG_UNFIXED, "%s %s: %s: %s",
		    listname, *op, p, gfarm_error_string(e));
		return (e);
	}
	if (gfarm_ctxp->metadb_server_port != GFARM_CONFIG_MISC_DEFAULT)
		return (GFARM_ERR_NO_ERROR);

	sp = getservbyname(s, "tcp");
	if (sp != NULL)
		gfarm_ctxp->metadb_server_port = ntohs(sp->s_port);
	else if ((port = strtol(s, NULL, 0)) != 0 && port > 0 && port < 65536)
		gfarm_ctxp->metadb_server_port = port;
	else {
		*op = "port argument";
		e = GFARM_ERR_INVALID_ARGUMENT;
		gflog_debug(GFARM_MSG_UNFIXED, "%s %s: %s: %s",
		    listname, *op, s, gfarm_error_string(e));
		return (e);
	}
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
parse_cred_config(char *p, char *service,
	gfarm_error_t (*set)(char *, char *))
{
	gfarm_error_t e;
	char *s;

	e = get_one_argument(p, &s);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1000971,
			"get_one_argument failed "
			"when parsing cred config (%s): %s",
			p, gfarm_error_string(e));
		return (e);
	}

	return ((*set)(service, s));
}

static gfarm_error_t
parse_log_level(char *p, int *vp)
{
	gfarm_error_t e;
	char *s;
	int v;

	e = get_one_argument(p, &s);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1000972,
			"get_one_argument failed "
			"when parsing log level (%s): %s",
			p, gfarm_error_string(e));
		return (e);
	}

	if (*vp != GFARM_CONFIG_MISC_DEFAULT) /* first line has precedence */
		return (GFARM_ERR_NO_ERROR);
	v = gflog_syslog_name_to_priority(s);
	if (v == -1) {
		gflog_debug(GFARM_MSG_1000973,
			"Invalid syslog priority level (%s)",
			s);
		return (GFARM_ERRMSG_INVALID_SYSLOG_PRIORITY_LEVEL);
	}
	*vp = v;
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
parse_hostname_and_port(char *host_and_port, const char *listname,
	char **hostname, int *port)
{
	gfarm_error_t e;
	char c, *p, *sport;
	long lport;
	size_t n;

	p = host_and_port;
	n = strcspn(p, ":");
	if (n == 0) {
		e = GFARM_ERR_INVALID_ARGUMENT;
		gflog_debug(GFARM_MSG_1002538,
		    "parsing of %s host argument failed: %s",
		    listname, gfarm_error_string(e));
		if (*p)
			p++;
		*hostname = NULL;
		return (e);
	}
	*hostname = p;
	p += n;
	c = *p;
	*p = 0;
	if (c != ':')
		return (GFARM_ERR_NO_ERROR);
	p++;
	sport = p;
	errno = 0;
	lport = strtol(sport, NULL, 10);
	if (errno != 0 || lport <= 0 || lport > 0xFFFF) {
		e = GFARM_ERR_INVALID_ARGUMENT;
		gflog_debug(GFARM_MSG_1002539,
		    "parsing of %s port argument (%s) failed: %s",
		    listname, sport, gfarm_error_string(e));
		return (e);
	}
	*port = (int)lport;
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
parse_local_usergroup_map_arguments(char *p, char **op, int is_user)
{
	gfarm_error_t e;
	char *tmp, *filepath, *host_and_port, *host = NULL;
	const char *listname;
	int port = GFMD_DEFAULT_PORT;

	listname = *op;
	if ((e = gfarm_strtoken(&p, &filepath)) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002540,
		    "parsing of %s file path argument (%s) failed: %s",
		    listname, p, gfarm_error_string(e));
		return (e);
	}
	if (filepath == NULL) {
		*op = "1st (file path) argument";
		gflog_debug(GFARM_MSG_1002541,
		    "Missing %s file path argument", listname);
		return (GFARM_ERRMSG_MISSING_USER_MAP_FILE_ARGUMENT);
	}
	if ((e = gfarm_strtoken(&p, &host_and_port))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002542,
		    "parsing of %s file path argument (%s) failed: %s",
		    listname, p, gfarm_error_string(e));
		return (e);
	}
	if (host_and_port) {
		if ((e = parse_hostname_and_port(host_and_port, listname,
		    &host, &port)) != GFARM_ERR_NO_ERROR) {
			*op = "2nd (hostname:port) argument";
			gflog_debug(GFARM_MSG_1002543,
			    "parsing of %s arguments (%s) failed: %s",
			    listname, p, gfarm_error_string(e));
			return (e);
		}
		if ((e = gfarm_strtoken(&p, &tmp)) != GFARM_ERR_NO_ERROR) {
			gflog_debug(GFARM_MSG_1002544,
			    "parsing of %s arguments (%s) failed: %s",
			    listname, p, gfarm_error_string(e));
			return (e);
		}
		if (tmp) {
			gflog_debug(GFARM_MSG_1002545,
			    "Too many local_%s_map arguments passed",
			    is_user ? "user" : "group");
			return (GFARM_ERRMSG_TOO_MANY_ARGUMENTS);
		}
	}
	if ((e = local_ug_maps_enter(host, (int)port, is_user, filepath))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002546,
		    "local_ug_maps_enter for %s failed: %s",
		    listname, gfarm_error_string(e));
		return (e);
	}

	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
parse_profile(char *p, int *vp)
{
	gfarm_error_t e = parse_set_misc_enabled(p, vp);

	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (*vp == 1)
		gfs_profile_set();
	else
		gfs_profile_unset();
	return (e);
}

static gfarm_error_t
parse_metadb_server_list_arguments(char *p, char **op)
{
#define METADB_SERVER_NUM_MAX 1024
	gfarm_error_t e;
	int i, port;
	char *host_and_port, *host = NULL;
	const char *listname = *op;
	struct gfarm_metadb_server *m;
	int n = 0;
	struct gfarm_filesystem *fs;
	struct gfarm_metadb_server *ms[METADB_SERVER_NUM_MAX];

	/* XXX - consider to allow to specify several server lists */
	if (gfarm_filesystem_is_initialized())
		return (GFARM_ERR_NO_ERROR);

	for (;;) {
		if ((e = gfarm_strtoken(&p, &host_and_port))
		    != GFARM_ERR_NO_ERROR) {
			*op = "hostname:port argument";
			gflog_debug(GFARM_MSG_1002547,
			    "parsing of %s (%s) failed: %s",
			    listname, p, gfarm_error_string(e));
			goto error;
		}
		if (host_and_port == NULL)
			break;
		if (n >= METADB_SERVER_NUM_MAX) {
			e = GFARM_ERR_INVALID_ARGUMENT;
			gflog_debug(GFARM_MSG_1002548,
			    "Too many arguments passed to %s", listname);
			goto error;
		}
		port = -1;
		if ((e = parse_hostname_and_port(host_and_port, listname,
		    &host, &port)) != GFARM_ERR_NO_ERROR) {
			*op = "hostname:port argument";
			gflog_debug(GFARM_MSG_1002549,
			    "parsing of %s arguments (%s) failed: %s",
			    listname, p, gfarm_error_string(e));
			return (e);
		}
		host = strdup(host);
		if (host == NULL) {
			e = GFARM_ERR_NO_MEMORY;
			gflog_debug(GFARM_MSG_1002550,
			    "%s", gfarm_error_string(e));
			goto error;
		}
		if (port < 0)
			port = GFMD_DEFAULT_PORT;
		if ((e = gfarm_metadb_server_new(&m, host, port))
		    != GFARM_ERR_NO_ERROR) {
			free(host);
			goto error;
		}
		ms[n++] = m;
	}
	if (n == 0) {
		*op = "1st (hostname:port) argument";
		gflog_debug(GFARM_MSG_1002551,
		    "Too few arguments passed to %s", listname);
		return (GFARM_ERR_INVALID_ARGUMENT);
	}
	gfarm_metadb_server_set_is_master(ms[0], 1);
	if ((e = gfarm_filesystem_init()) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002552,
		    "%s", gfarm_error_string(e));
		goto error;
	}
	fs = gfarm_filesystem_get_default();
	if ((e = gfarm_filesystem_set_metadb_server_list(fs, ms, n))
	    != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002553,
		    "%s", gfarm_error_string(e));
		goto error;
	}
	return (GFARM_ERR_NO_ERROR);
error:
	for (i = 0; i < n; ++i)
		gfarm_metadb_server_free(ms[i]);
	return (e);
}

static gfarm_error_t
parse_one_line(char *s, char *p, char **op)
{
	gfarm_error_t e;
	char *o;

	if (strcmp(s, o = "spool") == 0) {
		e = parse_set_var(p, &gfarm_spool_root);
	} else if (strcmp(s, o = "spool_server_listen_address") == 0) {
		e = parse_set_var(p, &gfarm_spool_server_listen_address);
	} else if (strcmp(s, o = "spool_server_cred_type") == 0) {
		e = parse_cred_config(p, GFS_SERVICE_TAG,
		    gfarm_auth_server_cred_type_set_by_string);
	} else if (strcmp(s, o = "spool_server_cred_service") == 0) {
		e = parse_cred_config(p, GFS_SERVICE_TAG,
		    gfarm_auth_server_cred_service_set);
	} else if (strcmp(s, o = "spool_server_cred_name") == 0) {
		e = parse_cred_config(p, GFS_SERVICE_TAG,
		    gfarm_auth_server_cred_name_set);

	} else if (strcmp(s, o = "metadb_server_host") == 0) {
		e = parse_set_var(p, &gfarm_ctxp->metadb_server_name);
	} else if (strcmp(s, o = "metadb_server_port") == 0) {
		e = parse_metadb_server_port(p, &o);
	} else if (strcmp(s, o = "metadb_server_list") == 0) {
		e = parse_metadb_server_list_arguments(p, &o);
	} else if (strcmp(s, o = "admin_user") == 0) {
		e = parse_set_var(p, &gfarm_ctxp->metadb_admin_user);
	} else if (strcmp(s, o = "admin_user_gsi_dn") == 0) {
		e = parse_set_var(p, &gfarm_ctxp->metadb_admin_user_gsi_dn);
	} else if (strcmp(s, o = "metadb_server_cred_type") == 0) {
		e = parse_cred_config(p, GFM_SERVICE_TAG,
		    gfarm_auth_server_cred_type_set_by_string);
	} else if (strcmp(s, o = "metadb_server_cred_service") == 0) {
		e = parse_cred_config(p, GFM_SERVICE_TAG,
		    gfarm_auth_server_cred_service_set);
	} else if (strcmp(s, o = "metadb_server_cred_name") == 0) {
		e = parse_cred_config(p, GFM_SERVICE_TAG,
		    gfarm_auth_server_cred_name_set);

	} else if (strcmp(s, o = "ldap_server_host") == 0) {
		e = parse_set_var(p, &gfarm_ldap_server_name);
		if (e == GFARM_ERR_NO_ERROR)
			e = set_backend_db_type_ldap();
	} else if (strcmp(s, o = "ldap_server_port") == 0) {
		e = parse_set_var(p, &gfarm_ldap_server_port);
		if (e == GFARM_ERR_NO_ERROR)
			e = set_backend_db_type_ldap();
	} else if (strcmp(s, o = "ldap_base_dn") == 0) {
		e = parse_set_var(p, &gfarm_ldap_base_dn);
		if (e == GFARM_ERR_NO_ERROR)
			e = set_backend_db_type_ldap();
	} else if (strcmp(s, o = "ldap_bind_dn") == 0) {
		e = parse_set_var(p, &gfarm_ldap_bind_dn);
		if (e == GFARM_ERR_NO_ERROR)
			e = set_backend_db_type_ldap();
	} else if (strcmp(s, o = "ldap_bind_password") == 0) {
		e = parse_set_var(p, &gfarm_ldap_bind_password);
		if (e == GFARM_ERR_NO_ERROR)
			e = set_backend_db_type_ldap();
	} else if (strcmp(s, o = "ldap_tls") == 0) {
		e = parse_set_var(p, &gfarm_ldap_tls);
		if (e == GFARM_ERR_NO_ERROR)
			e = set_backend_db_type_ldap();
	} else if (strcmp(s, o = "ldap_tls_cipher_suite") == 0) {
		e = parse_set_var(p, &gfarm_ldap_tls_cipher_suite);
		if (e == GFARM_ERR_NO_ERROR)
			e = set_backend_db_type_ldap();
	} else if (strcmp(s, o = "ldap_tls_certificate_key_file") == 0) {
		e = parse_set_var(p, &gfarm_ldap_tls_certificate_key_file);
		if (e == GFARM_ERR_NO_ERROR)
			e = set_backend_db_type_ldap();
	} else if (strcmp(s, o = "ldap_tls_certificate_file") == 0) {
		e = parse_set_var(p, &gfarm_ldap_tls_certificate_file);
		if (e == GFARM_ERR_NO_ERROR)
			e = set_backend_db_type_ldap();

	} else if (strcmp(s, o = "postgresql_server_host") == 0) {
		e = parse_set_var(p, &gfarm_postgresql_server_name);
		if (e == GFARM_ERR_NO_ERROR)
			e = set_backend_db_type_postgresql();
	} else if (strcmp(s, o = "postgresql_server_port") == 0) {
		e = parse_set_var(p, &gfarm_postgresql_server_port);
		if (e == GFARM_ERR_NO_ERROR)
			e = set_backend_db_type_postgresql();
	} else if (strcmp(s, o = "postgresql_dbname") == 0) {
		e = parse_set_var(p, &gfarm_postgresql_dbname);
		if (e == GFARM_ERR_NO_ERROR)
			e = set_backend_db_type_postgresql();
	} else if (strcmp(s, o = "postgresql_user") == 0) {
		e = parse_set_var(p, &gfarm_postgresql_user);
		if (e == GFARM_ERR_NO_ERROR)
			e = set_backend_db_type_postgresql();
	} else if (strcmp(s, o = "postgresql_password") == 0) {
		e = parse_set_var(p, &gfarm_postgresql_password);
		if (e == GFARM_ERR_NO_ERROR)
			e = set_backend_db_type_postgresql();
	} else if (strcmp(s, o = "postgresql_conninfo") == 0) {
		e = parse_set_var(p, &gfarm_postgresql_conninfo);
		if (e == GFARM_ERR_NO_ERROR)
			e = set_backend_db_type_postgresql();

	} else if (strcmp(s, o = "localfs_datadir") == 0) {
		e = parse_set_var(p, &gfarm_localfs_datadir);
		if (e == GFARM_ERR_NO_ERROR)
			e = set_backend_db_type_localfs();

	} else if (strcmp(s, o = "auth") == 0) {
		e = parse_auth_arguments(p, &o);
#if 0 /* not yet in gfarm v2 */
	} else if (strcmp(s, o = "netparam") == 0) {
		e = parse_netparam_arguments(p, &o);
#endif
	} else if (strcmp(s, o = "sockopt") == 0) {
		e = parse_sockopt_arguments(p, &o);
#if 0 /* XXX address_use is disabled for now */
	} else if (strcmp(s, o = "address_use") == 0) {
		e = parse_address_use_arguments(p, &o);
#endif
	} else if (strcmp(s, o = "known_network") == 0) {
		e = parse_known_network_arguments(p, &o);
	} else if (strcmp(s, o = "xattr_cache") == 0) {
		e = parse_stringlist(p, &o,
		    &staticp->xattr_cache_list, "xattr cache");
	} else if (strcmp(s, o = "local_user_map") == 0) {
		e = parse_local_usergroup_map_arguments(p, &o, 1);
	} else if (strcmp(s, o = "local_group_map") == 0) {
		e = parse_local_usergroup_map_arguments(p, &o, 0);
#if 0 /* XXX NOTYET */
	} else if (strcmp(s, o = "client_architecture") == 0) {
		e = parse_client_architecture(p, &o);
#endif

	} else if (strcmp(s, o = "log_level") == 0) {
		e = parse_log_level(p, &gfarm_ctxp->log_level);
	} else if (strcmp(s, o = "log_message_verbose_level") == 0) {
		e = parse_set_misc_int(p, &staticp->log_message_verbose);
		if (e == GFARM_ERR_NO_ERROR)
			gflog_set_message_verbose(staticp->log_message_verbose);
	} else if (strcmp(s, o = "log_auth_verbose") == 0) {
		int tmp;
		e = parse_set_misc_enabled(p, &tmp);
		if (e == GFARM_ERR_NO_ERROR)
			gflog_auth_set_verbose(tmp);
	} else if (strcmp(s, o = "no_file_system_node_timeout") == 0) {
		e = parse_set_misc_int(
		    p, &gfarm_ctxp->no_file_system_node_timeout);
	} else if (strcmp(s, o = "gfmd_reconnection_timeout") == 0) {
		e = parse_set_misc_int(
		    p, &gfarm_ctxp->gfmd_reconnection_timeout);
	} else if (strcmp(s, o = "attr_cache_limit") == 0) {
		e = parse_set_misc_int(p, &gfarm_ctxp->attr_cache_limit);
	} else if (strcmp(s, o = "attr_cache_timeout") == 0) {
		e = parse_set_misc_int(p, &gfarm_ctxp->attr_cache_timeout);
	} else if (strcmp(s, o = "schedule_cache_timeout") == 0) {
		e = parse_set_misc_int(p, &gfarm_ctxp->schedule_cache_timeout);
	} else if (strcmp(s, o = "schedule_idle_load_thresh") == 0) {
		e = parse_set_misc_float(p, &gfarm_ctxp->schedule_idle_load);
	} else if (strcmp(s, o = "schedule_busy_load_thresh") == 0) {
		e = parse_set_misc_float(p, &gfarm_ctxp->schedule_busy_load);
	} else if (strcmp(s, o = "schedule_virtual_load") == 0) {
		e = parse_set_misc_float(p, &gfarm_ctxp->schedule_virtual_load);
#if 0 /* not yet in gfarm v2 */
	} else if (strcmp(s, o = "write_local_priority") == 0) {
		e = parse_set_misc_enabled(p, &schedule_write_local_priority);
	} else if (strcmp(s, o = "write_target_domain") == 0) {
		e = parse_set_var(p, &schedule_write_target_domain);
#endif
	} else if (strcmp(s, o = "minimum_free_disk_space") == 0) {
		e = parse_set_misc_offset(p, &staticp->minimum_free_disk_space);
	} else if (strcmp(s, o = "simultaneous_replication_receivers") == 0) {
		e = parse_set_misc_int(p,
		    &gfarm_simultaneous_replication_receivers);
	} else if (strcmp(s, o = "gfsd_connection_cache") == 0) {
		e = parse_set_misc_int(p, &gfarm_ctxp->gfsd_connection_cache);
	} else if (strcmp(s, o = "gfmd_connection_cache") == 0) {
		e = parse_set_misc_int(p, &gfarm_ctxp->gfmd_connection_cache);
	} else if (strcmp(s, o = "metadb_server_stack_size") == 0) {
		e = parse_set_misc_int(p, &gfarm_metadb_stack_size);
	} else if (strcmp(s, o = "metadb_server_thread_pool_size") == 0) {
		e = parse_set_misc_int(p, &gfarm_metadb_thread_pool_size);
	} else if (strcmp(s, o = "metadb_server_job_queue_length") == 0) {
		e = parse_set_misc_int(p, &gfarm_metadb_job_queue_length);
	} else if (strcmp(s, o = "metadb_server_heartbeat_interval") == 0) {
		e = parse_set_misc_int(p, &gfarm_metadb_heartbeat_interval);
	} else if (strcmp(s, o = "metadb_server_dbq_size") == 0) {
		e = parse_set_misc_int(p, &gfarm_metadb_dbq_size);
	} else if (strcmp(s, o = "record_atime") == 0) {
		e = parse_set_misc_enabled(p, &gfarm_ctxp->record_atime);
	} else if (strcmp(s, o = "client_file_bufsize") == 0) {
		e = parse_set_misc_int(p, &gfarm_ctxp->client_file_bufsize);
	} else if (strcmp(s, o = "profile") == 0) {
		e = parse_profile(p, &staticp->profile);

	} else if (strcmp(s, o = "metadb_replication") == 0) {
		e = parse_set_misc_enabled(p, &metadb_replication_enabled);
	} else if (strcmp(s, o = "metadb_journal_dir") == 0) {
		e = parse_set_var(p, &journal_dir);
	} else if (strcmp(s, o = "metadb_journal_max_size") == 0) {
		e = parse_set_misc_int(p, &journal_max_size);
	} else if (strcmp(s, o = "metadb_journal_recvq_size") == 0) {
		e = parse_set_misc_int(p, &journal_recvq_size);
	} else if (strcmp(s, o = "synchronous_journaling") == 0) {
		e = parse_set_misc_enabled(p, &journal_sync_file);
	} else if (strcmp(s, o = "synchronous_replication_timeout") == 0) {
		e = parse_set_misc_int(p, &journal_sync_slave_timeout);
	} else if (strcmp(s, o = "metadb_server_slave_max_size") == 0) {
		e = parse_set_misc_int(p, &metadb_server_slave_max_size);
	} else if (strcmp(s, o = "metadb_server_force_slave") == 0) {
		e = parse_set_misc_enabled(p, &metadb_server_force_slave);
	} else if (strcmp(s, o = "network_receive_timeout") == 0) {
		e = parse_set_misc_int(p, &gfarm_ctxp->network_receive_timeout);
	} else if (strcmp(s, o = "file_trace") == 0) {
		e = parse_set_misc_enabled(p, &gfarm_ctxp->file_trace);
	} else {
		o = s;
		gflog_debug(GFARM_MSG_1000974,
			"Unknown keyword encountered "
			"when parsing one line (%s)",
			s);
		e = GFARM_ERRMSG_UNKNOWN_KEYWORD;
	}
	*op = o;
	return (e);
}

gfarm_error_t
gfarm_init_config(void)
{
	gfarm_stringlist_init(&staticp->xattr_cache_list);
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfarm_free_config(void)
{
	gfarm_stringlist_free_deeply(&staticp->xattr_cache_list);
	local_ug_maps_tab_free();
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfarm_config_read_file(FILE *config, int *lineno_p)
{
	gfarm_error_t e;
	int lineno = 0;
	char *s, *p, *o = NULL, buffer[1024];

	while (fgets(buffer, sizeof buffer, config) != NULL) {
		lineno++;
		p = buffer;
		e = gfarm_strtoken(&p, &s);

		if (e == GFARM_ERR_NO_ERROR) {
			if (s == NULL) /* blank or comment line */
				continue;
			e = parse_one_line(s, p, &o);
		}
		if (e != GFARM_ERR_NO_ERROR) {
			fclose(config);
			*lineno_p = lineno;
			gflog_debug(GFARM_MSG_1000975,
			    "line %d: %s: %s: %s", lineno, o == NULL ? "" : o,
			    p, gfarm_error_string(e));
			return (e);
		}
	}
	fclose(config);
	return (GFARM_ERR_NO_ERROR);
}

/*
 * set default value of configurations.
 */
void
gfarm_config_set_default_ports(void)
{
	if (gfarm_ctxp->metadb_server_name == NULL)
		gflog_fatal(GFARM_MSG_1000013,
		    "metadb_serverhost isn't specified in "
		    GFARM_CONFIG " file");

	if (gfarm_ctxp->metadb_server_port == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_ctxp->metadb_server_port = GFMD_DEFAULT_PORT;
}

static gfarm_error_t
gfarm_config_set_default_filesystem(void)
{
	gfarm_error_t e;
	struct gfarm_filesystem *fs;
	int n;

	/* gfarm_metadb_server_name is checked in
	 * gfarm_config_set_default_ports */
	assert(gfarm_ctxp->metadb_server_name != NULL);

	if ((e = gfarm_filesystem_init()) != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002554,
		    "%s", gfarm_error_string(e));
		return (e);
	}
	fs = gfarm_filesystem_get(
		gfarm_ctxp->metadb_server_name, gfarm_ctxp->metadb_server_port);
	if (fs == NULL) {
		fs = gfarm_filesystem_get_default();
		if (gfarm_filesystem_get_metadb_server_list(fs, &n) != NULL)
			/* XXX - for now, this is assumed */
			gflog_fatal(GFARM_MSG_1002555, "configuration error: "
			    "%s:%d is not included in the metadb_server_list",
			    gfarm_ctxp->metadb_server_name,
			    gfarm_ctxp->metadb_server_port);
	}
	return (GFARM_ERR_NO_ERROR);
}

void
gfarm_config_set_default_misc(void)
{
	if (gfarm_ctxp->log_level == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_ctxp->log_level = GFARM_DEFAULT_PRIORITY_LEVEL_TO_LOG;
	gflog_set_priority_level(gfarm_ctxp->log_level);
	if (staticp->log_message_verbose == GFARM_CONFIG_MISC_DEFAULT)
		staticp->log_message_verbose =
		    GFARM_LOG_MESSAGE_VERBOSE_DEFAULT;
	gflog_set_message_verbose(staticp->log_message_verbose);

	if (gfarm_ctxp->no_file_system_node_timeout ==
	    GFARM_CONFIG_MISC_DEFAULT)
		gfarm_ctxp->no_file_system_node_timeout =
		    GFARM_NO_FILE_SYSTEM_NODE_TIMEOUT_DEFAULT;
	if (gfarm_ctxp->gfmd_reconnection_timeout == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_ctxp->gfmd_reconnection_timeout =
		    GFARM_GFMD_RECONNECTION_TIMEOUT_DEFAULT;
	if (gfarm_ctxp->attr_cache_limit == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_ctxp->attr_cache_limit = GFARM_ATTR_CACHE_LIMIT_DEFAULT;
	if (gfarm_ctxp->attr_cache_timeout == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_ctxp->attr_cache_timeout =
		    GFARM_ATTR_CACHE_TIMEOUT_DEFAULT;
	if (gfarm_ctxp->schedule_cache_timeout == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_ctxp->schedule_cache_timeout =
		    GFARM_SCHEDULE_CACHE_TIMEOUT_DEFAULT;
	if (gfarm_ctxp->schedule_idle_load == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_ctxp->schedule_idle_load =
		    GFARM_SCHEDULE_IDLE_LOAD_DEFAULT;
	if (gfarm_ctxp->schedule_busy_load == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_ctxp->schedule_busy_load =
		    GFARM_SCHEDULE_BUSY_LOAD_DEFAULT;
	if (gfarm_ctxp->schedule_virtual_load == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_ctxp->schedule_virtual_load =
		    GFARM_SCHEDULE_VIRTUAL_LOAD_DEFAULT;
#if 0 /* not yet in gfarm v2 */
	if (schedule_write_local_priority == GFARM_CONFIG_MISC_DEFAULT)
		schedule_write_local_priority =
		    GFARM_SCHEDULE_WRITE_LOCAL_PRIORITY_DEFAULT;
#endif
	if (staticp->minimum_free_disk_space == GFARM_CONFIG_MISC_DEFAULT)
		staticp->minimum_free_disk_space =
		    GFARM_MINIMUM_FREE_DISK_SPACE_DEFAULT;
	if (gfarm_simultaneous_replication_receivers ==
	    GFARM_CONFIG_MISC_DEFAULT)
		 gfarm_simultaneous_replication_receivers =
		    GFARM_SIMULTANEOUS_REPLICATION_RECEIVERS_DEFAULT;
	if (gfarm_ctxp->gfsd_connection_cache == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_ctxp->gfsd_connection_cache =
		    GFARM_GFSD_CONNECTION_CACHE_DEFAULT;
	if (gfarm_ctxp->gfmd_connection_cache == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_ctxp->gfmd_connection_cache =
		    GFARM_GFMD_CONNECTION_CACHE_DEFAULT;
	if (gfarm_metadb_stack_size == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_metadb_stack_size = GFARM_METADB_STACK_SIZE_DEFAULT;
	if (gfarm_metadb_thread_pool_size == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_metadb_thread_pool_size =
		    GFARM_METADB_THREAD_POOL_SIZE_DEFAULT;
	if (gfarm_metadb_job_queue_length == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_metadb_job_queue_length =
		    GFARM_METADB_JOB_QUEUE_LENGTH_DEFAULT;
	if (gfarm_metadb_heartbeat_interval == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_metadb_heartbeat_interval =
		    GFARM_METADB_HEARTBEAT_INTERVAL_DEFAULT;
	if (gfarm_metadb_dbq_size == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_metadb_dbq_size = GFARM_METADB_DBQ_SIZE_DEFAULT;
	if (gfarm_ctxp->record_atime == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_ctxp->record_atime = GFARM_RECORD_ATIME_DEFAULT;
	if (gfarm_ctxp->client_file_bufsize == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_ctxp->client_file_bufsize =
		    GFARM_CLIENT_FILE_BUFSIZE_DEFAULT;
	if (staticp->profile == GFARM_CONFIG_MISC_DEFAULT)
		staticp->profile = GFARM_PROFILE_DEFAULT;
	if (metadb_replication_enabled == GFARM_CONFIG_MISC_DEFAULT)
		metadb_replication_enabled =
		    GFARM_METADB_REPLICATION_ENABLED_DEFAULT;
	if (journal_max_size == GFARM_CONFIG_MISC_DEFAULT)
		journal_max_size = GFARM_JOURNAL_MAX_SIZE_DEFAULT;
	if (journal_recvq_size == GFARM_CONFIG_MISC_DEFAULT)
		journal_recvq_size = GFARM_JOURNAL_RECVQ_SIZE_DEFAULT;
	if (journal_sync_file == GFARM_CONFIG_MISC_DEFAULT)
		journal_sync_file = GFARM_JOURNAL_SYNC_FILE_DEFAULT;
	if (journal_sync_slave_timeout == GFARM_CONFIG_MISC_DEFAULT)
		journal_sync_slave_timeout =
		    GFARM_JOURNAL_SYNC_SLAVE_TIMEOUT_DEFAULT;
	if (metadb_server_slave_max_size == GFARM_CONFIG_MISC_DEFAULT)
		metadb_server_slave_max_size =
		    GFARM_METADB_SERVER_SLAVE_MAX_SIZE_DEFAULT;
	if (metadb_server_force_slave == GFARM_CONFIG_MISC_DEFAULT)
		metadb_server_force_slave =
		    GFARM_METADB_SERVER_FORCE_SLAVE_DEFAULT;
	if (gfarm_ctxp->network_receive_timeout == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_ctxp->network_receive_timeout =
		    GFARM_NETWORK_RECEIVE_TIMEOUT_DEFAULT;
	if (gfarm_ctxp->file_trace == GFARM_CONFIG_MISC_DEFAULT)
		gfarm_ctxp->file_trace = GFARM_FILE_TRACE_DEFAULT;

	gfarm_config_set_default_filesystem();
}

void
gfs_display_timers(void)
{
	gfs_pio_display_timers();
	gfs_pio_section_display_timers();
	gfs_stat_display_timers();
	gfs_unlink_display_timers();
	gfs_xattr_display_timers();
}

#ifdef STRTOKEN_TEST
main()
{
	char buffer[1024];
	char *cursor, *token, *error;

	while (fgets(buffer, sizeof buffer, stdin) != NULL) {
		cursor = buffer;
		while ((token = strtoken(&cursor, &error)) != NULL)
			printf("token: <%s>\n", token);
		if (error == NULL)
			printf("newline\n");
		else
			printf("error: %s\n", error);
	}
}
#endif
