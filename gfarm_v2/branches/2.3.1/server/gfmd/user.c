#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h> /* fd_set for "filetab.h" */

#include <gfarm/gfarm.h>

#include "gfutil.h"
#include "hash.h"

#include "config.h"	/* gfarm_metadb_admin_user */
#include "auth.h"
#include "gfp_xdr.h"

#include "subr.h"
#include "db_access.h"
#include "user.h"
#include "group.h"
#include "peer.h"
#include "quota.h"

#define USER_HASHTAB_SIZE	3079	/* prime number */
#define USER_DN_HASHTAB_SIZE	3079	/* prime number */

/* in-core gfarm_user_info */
struct user {
	struct gfarm_user_info ui;
	struct group_assignment groups;
	struct quota q;
	int invalid;	/* set when deleted */
};

char ADMIN_USER_NAME[] = "gfarmadm";
char REMOVED_USER_NAME[] = "gfarm-removed-user";

static struct gfarm_hash_table *user_hashtab = NULL;
static struct gfarm_hash_table *user_dn_hashtab = NULL;

/* subroutine of grpassign_add(), shouldn't be called from elsewhere */
void
grpassign_add_group(struct group_assignment *ga)
{
	struct user *u = ga->u;

	ga->group_next = &u->groups;
	ga->group_prev = u->groups.group_prev;
	u->groups.group_prev->group_next = ga;
	u->groups.group_prev = ga;
}

static void
user_invalidate(struct user *u)
{
	u->invalid = 1;
}

static void
user_activate(struct user *u)
{
	u->invalid = 0;
}

int
user_is_invalidated(struct user *u)
{
	return (u->invalid == 1);
}

int
user_is_active(struct user *u)
{
	return (u != NULL && !user_is_invalidated(u));
}

struct user *
user_lookup(const char *username)
{
	struct gfarm_hash_entry *entry;

	entry = gfarm_hash_lookup(user_hashtab, &username, sizeof(username));
	if (entry == NULL)
		return (NULL);
	return (*(struct user **)gfarm_hash_entry_data(entry));
}

struct user *
user_lookup_gsi_dn(const char *gsi_dn)
{
	struct gfarm_hash_entry *entry;

	if (gsi_dn == NULL || gsi_dn[0] == '\0')
		return (NULL);

	entry = gfarm_hash_lookup(user_dn_hashtab, &gsi_dn, sizeof(gsi_dn));
	if (entry == NULL)
		return (NULL);
	return (*(struct user **)gfarm_hash_entry_data(entry));
}

gfarm_error_t
user_enter_gsi_dn(const char *gsi_dn, struct user *u)
{
	struct gfarm_hash_entry *entry;
	int created;

	if (gsi_dn == NULL || gsi_dn[0] == '\0')
		return (GFARM_ERR_NO_ERROR);

	entry = gfarm_hash_enter(user_dn_hashtab,
	    &gsi_dn, sizeof(gsi_dn), sizeof(struct user *), &created);
	if (entry == NULL)
		return (GFARM_ERR_NO_MEMORY);
	if (!created)
		return (GFARM_ERR_ALREADY_EXISTS);
	*(struct user **)gfarm_hash_entry_data(entry) = u;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
user_enter(struct gfarm_user_info *ui, struct user **upp)
{
	struct gfarm_hash_entry *entry;
	int created;
	struct user *u;
	gfarm_error_t e;

	u = user_lookup(ui->username);
	if (u != NULL) {
		if (user_is_invalidated(u)) {
			e = user_enter_gsi_dn(ui->gsi_dn, u);
			if (e != GFARM_ERR_NO_ERROR)
				return (e);
			user_activate(u);
			if (upp != NULL)
				*upp = u;
			/* copy user info but keeping address of username */
			free(ui->username);
			ui->username = u->ui.username;
			u->ui.username = NULL; /* prevent to free this area */
			gfarm_user_info_free(&u->ui);
			u->ui = *ui;
			return (GFARM_ERR_NO_ERROR);
		}
		else
			return (GFARM_ERR_ALREADY_EXISTS);
	}

	GFARM_MALLOC(u);
	if (u == NULL)
		return (GFARM_ERR_NO_MEMORY);
	u->ui = *ui;

	entry = gfarm_hash_enter(user_hashtab,
	    &u->ui.username, sizeof(u->ui.username), sizeof(struct user *),
	    &created);
	if (entry == NULL) {
		free(u);
		return (GFARM_ERR_NO_MEMORY);
	}
	if (!created) {
		free(u);
		return (GFARM_ERR_ALREADY_EXISTS);
	}
	e = user_enter_gsi_dn(u->ui.gsi_dn, u);
	if (e != GFARM_ERR_NO_ERROR) {
		gfarm_hash_purge(user_hashtab,
		    &u->ui.username, sizeof(u->ui.username));
		free(u);
		return (e);
	}

	quota_data_init(&u->q);
	u->groups.group_prev = u->groups.group_next = &u->groups;
	*(struct user **)gfarm_hash_entry_data(entry) = u;
	user_activate(u);
	if (upp != NULL)
		*upp = u;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
user_remove(const char *username)
{
	struct gfarm_hash_entry *entry;
	struct user *u;

	entry = gfarm_hash_lookup(user_hashtab, &username, sizeof(username));
	if (entry == NULL)
		return (GFARM_ERR_NO_SUCH_USER);
	u = *(struct user **)gfarm_hash_entry_data(entry);
	if (user_is_invalidated(u))
		return (GFARM_ERR_NO_SUCH_USER);

	if (u->ui.gsi_dn != NULL && u->ui.gsi_dn[0] != '\0')
		gfarm_hash_purge(user_dn_hashtab,
		    &u->ui.gsi_dn, sizeof(u->ui.gsi_dn));
	quota_user_remove(u);
	/*
	 * do not purge the hash entry.  Instead, invalidate it so
	 * that it can be activated later.
	 */
	user_invalidate(u);

	return (GFARM_ERR_NO_ERROR);
}

char *
user_name(struct user *u)
{
	return (user_is_active(u) ? u->ui.username : REMOVED_USER_NAME);
}

char *
user_gsi_dn(struct user *u)
{
	return (user_is_active(u) ? u->ui.gsi_dn : REMOVED_USER_NAME);
}

struct quota *
user_quota(struct user *u)
{
	return (&u->q);
}

void
user_all(void *closure, void (*callback)(void *, struct user *),
	 int active_only)
{
	struct gfarm_hash_iterator it;
	struct user **u;

	for (gfarm_hash_iterator_begin(user_hashtab, &it);
	     !gfarm_hash_iterator_is_end(&it);
	     gfarm_hash_iterator_next(&it)) {
		u = gfarm_hash_entry_data(gfarm_hash_iterator_access(&it));
		if (!active_only || user_is_active(*u))
			callback(closure, *u);
	}
}

int
user_in_group(struct user *user, struct group *group)
{
	struct group_assignment *ga;

	if (user_is_invalidated(user))
		return (0);
	if (group_is_invalidated(group))
		return (0);

	for (ga = user->groups.group_next; ga != &user->groups;
	    ga = ga->group_next) {
		if (ga->g == group)
			return (1);
	}
	return (0);
}

int
user_is_admin(struct user *user)
{
	static struct group *admin = NULL;

	if (admin == NULL)
		admin = group_lookup(ADMIN_GROUP_NAME);
	return (user_in_group(user, admin));
}

int
user_is_root(struct user *user)
{
	static struct group *root = NULL;

	if (root == NULL)
		root = group_lookup(ROOT_GROUP_NAME);
	return (user_in_group(user, root));
}

/* The memory owner of `*ui' is changed to user.c */
void
user_add_one(void *closure, struct gfarm_user_info *ui)
{
	gfarm_error_t e = user_enter(ui, NULL);

	if (e != GFARM_ERR_NO_ERROR)
		gflog_warning(GFARM_MSG_1000233,
		    "user_add_one: %s", gfarm_error_string(e));
}

void
create_user(const char *username, const char *gsi_dn)
{
	gfarm_error_t e;
	struct gfarm_user_info ui;

	gflog_info(GFARM_MSG_1000234,
	    "user '%s' not found, creating...", username);

	ui.username = strdup(username);
	ui.realname = strdup("Gfarm administrator");
	ui.homedir = strdup("/");
	ui.gsi_dn = strdup(gsi_dn == NULL ? "" : gsi_dn);
	user_add_one(NULL, &ui);
	e = db_user_add(&ui);
	if (e != GFARM_ERR_NO_ERROR)
		gflog_error(GFARM_MSG_1000235,
		    "failed to store user '%s' to storage: %s",
		    username, gfarm_error_string(e));
}

void
user_init(void)
{
	gfarm_error_t e;

	user_hashtab =
	    gfarm_hash_table_alloc(USER_HASHTAB_SIZE,
		gfarm_hash_strptr, gfarm_hash_key_equal_strptr);
	user_dn_hashtab =
	    gfarm_hash_table_alloc(USER_DN_HASHTAB_SIZE,
		gfarm_hash_strptr, gfarm_hash_key_equal_strptr);
	if (user_hashtab == NULL || user_dn_hashtab == NULL)
		gflog_fatal(GFARM_MSG_1000236, "no memory for user hashtab");

	e = db_user_load(NULL, user_add_one);
	if (e != GFARM_ERR_NO_ERROR)
		gflog_error(GFARM_MSG_1000237,
		    "loading users: %s", gfarm_error_string(e));

	/*
	 * there is no removed (invalidated) user since the hash is
	 * just created.
	 */
	if (user_lookup(ADMIN_USER_NAME) == NULL)
		create_user(ADMIN_USER_NAME, NULL);
	if (gfarm_metadb_admin_user != NULL &&
	    user_lookup(gfarm_metadb_admin_user) == NULL)
		create_user(gfarm_metadb_admin_user,
		    gfarm_metadb_admin_user_gsi_dn);
}

#ifndef TEST
/*
 * protocol handler
 */

gfarm_error_t
user_info_send(struct gfp_xdr *client, struct gfarm_user_info *ui)
{
	return (gfp_xdr_send(client, "ssss",
	    ui->username, ui->realname, ui->homedir, ui->gsi_dn));
}

gfarm_error_t
gfm_server_user_info_get_all(struct peer *peer, int from_client, int skip)
{
	gfarm_error_t e;
	struct gfp_xdr *client = peer_get_conn(peer);
	struct gfarm_hash_iterator it;
	gfarm_int32_t nusers;
	struct user **u;
	const char msg[] = "protocol USER_INFO_GET_ALL";

	if (skip)
		return (GFARM_ERR_NO_ERROR);

	/* XXX FIXME too long giant lock */
	giant_lock();

	nusers = 0;
	for (gfarm_hash_iterator_begin(user_hashtab, &it);
	     !gfarm_hash_iterator_is_end(&it);
	     gfarm_hash_iterator_next(&it)) {
		u = gfarm_hash_entry_data(gfarm_hash_iterator_access(&it));
		if (user_is_active(*u))
			++nusers;
	}
	e = gfm_server_put_reply(peer, msg,
	    GFARM_ERR_NO_ERROR, "i", nusers);
	if (e != GFARM_ERR_NO_ERROR) {
		giant_unlock();
		return (e);
	}
	for (gfarm_hash_iterator_begin(user_hashtab, &it);
	     !gfarm_hash_iterator_is_end(&it);
	     gfarm_hash_iterator_next(&it)) {
		u = gfarm_hash_entry_data(gfarm_hash_iterator_access(&it));
		if (user_is_active(*u)) {
			e = user_info_send(client, &(*u)->ui);
			if (e != GFARM_ERR_NO_ERROR) {
				giant_unlock();
				return (e);
			}
		}
	}

	giant_unlock();
	return (GFARM_ERR_NO_ERROR);
}

/*
 * We need to allow gfsd use this operation
 * to implement gfarm_metadb_verify_username()
 */
gfarm_error_t
gfm_server_user_info_get_by_names(struct peer *peer, int from_client, int skip)
{
	struct gfp_xdr *client = peer_get_conn(peer);
	gfarm_error_t e;
	gfarm_int32_t nusers;
	char *user, **users;
	int i, j, eof, no_memory = 0;
	struct user *u;
	const char msg[] = "protocol USER_INFO_GET_BY_NAMES";

	e = gfm_server_get_request(peer, msg, "i", &nusers);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	GFARM_MALLOC_ARRAY(users, nusers);
	if (users == NULL)
		no_memory = 1;
	for (i = 0; i < nusers; i++) {
		e = gfp_xdr_recv(client, 0, &eof, "s", &user);
		if (e != GFARM_ERR_NO_ERROR || eof) {
			if (e == GFARM_ERR_NO_ERROR) /* i.e. eof */
				e = GFARM_ERR_PROTOCOL;
			if (users != NULL) {
				for (j = 0; j < i; j++) {
					if (users[j] != NULL)
						free(users[j]);
				}
				free(users);
			}
			return (e);
		}
		if (users == NULL) {
			free(user);
		} else {
			if (user == NULL)
				no_memory = 1;
			users[i] = user;
		}
	}
	if (skip) {
		e = GFARM_ERR_NO_ERROR;
		goto free_user;
	}

	e = gfm_server_put_reply(peer, "user_info_get_by_names",
		no_memory ? GFARM_ERR_NO_MEMORY : e, "");
	if (no_memory || e != GFARM_ERR_NO_ERROR)
		goto free_user;

	/* XXX FIXME too long giant lock */
	giant_lock();
	for (i = 0; i < nusers; i++) {
		u = user_lookup(users[i]);
		if (u == NULL || user_is_invalidated(u)) {
			if (debug_mode)
				gflog_info(GFARM_MSG_1000238,
				    "user lookup <%s>: failed",
				    users[i]);
			e = gfm_server_put_reply(peer, msg,
			    GFARM_ERR_NO_SUCH_USER, "");
		} else {
			if (debug_mode)
				gflog_info(GFARM_MSG_1000239,
				    "user lookup <%s>: ok", users[i]);
			e = gfm_server_put_reply(peer, msg,
			    GFARM_ERR_NO_ERROR, "");
			if (e == GFARM_ERR_NO_ERROR)
				e = user_info_send(client, &u->ui);
		}
		if (peer_had_protocol_error(peer))
			break;
	}
	giant_unlock();

free_user:
	if (users != NULL) {
		for (i = 0; i < nusers; i++) {
			if (users[i] != NULL)
				free(users[i]);
		}
		free(users);
	}
	return (no_memory ? GFARM_ERR_NO_MEMORY : e);
}

gfarm_error_t
gfm_server_user_info_get_by_gsi_dn(
	struct peer *peer, int from_client, int skip)
{
	gfarm_error_t e;
	char *gsi_dn;
	struct user *u;
	struct gfarm_user_info *ui;
	const char msg[] = "protocol USER_INFO_GET_BY_GSI_DN";

	e = gfm_server_get_request(peer, msg, "s", &gsi_dn);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (skip) {
		free(gsi_dn);
		return (GFARM_ERR_NO_ERROR);
	}

	/* XXX FIXME too long giant lock */
	giant_lock();
	u = user_lookup_gsi_dn(gsi_dn);
	if (u == NULL || user_is_invalidated(u))
		e = gfm_server_put_reply(peer, msg,
			GFARM_ERR_NO_SUCH_USER, "");
	else {
		ui = &u->ui;
		e = gfm_server_put_reply(peer, msg, e,
			"ssss", ui->username, ui->realname, ui->homedir,
			ui->gsi_dn);
	}
	giant_unlock();
	free(gsi_dn);
	return (e);
}

gfarm_error_t
gfm_server_user_info_set(struct peer *peer, int from_client, int skip)
{
	struct gfarm_user_info ui;
	gfarm_error_t e;
	struct user *user = peer_get_user(peer);
	int do_not_free = 0;
	const char msg[] = "protocol USER_INFO_SET";

	e = gfm_server_get_request(peer, msg,
	    "ssss", &ui.username, &ui.realname, &ui.homedir, &ui.gsi_dn);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (skip) {
		gfarm_user_info_free(&ui);
		return (GFARM_ERR_NO_ERROR);
	}
	giant_lock();
	if (!from_client || user == NULL || !user_is_admin(user)) {
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
	} else if (user_is_active(user_lookup(ui.username))) {
		e = GFARM_ERR_ALREADY_EXISTS;
	} else {
		e = user_enter(&ui, NULL);
		if (e == GFARM_ERR_NO_ERROR) {
			e = db_user_add(&ui);
			if (e != GFARM_ERR_NO_ERROR) {
				user_remove(ui.username);
				/* do not free since ui still used in hash */
				do_not_free = 1;
			}
		}
	}
	if (e != GFARM_ERR_NO_ERROR && !do_not_free)
		gfarm_user_info_free(&ui);
	giant_unlock();
	return (gfm_server_put_reply(peer, msg, e, ""));
}

gfarm_error_t
gfm_server_user_info_modify(struct peer *peer, int from_client, int skip)
{
	struct gfarm_user_info ui;
	struct user *u, *user = peer_get_user(peer);
	gfarm_error_t e;
	int needs_free = 0;
	const char msg[] = "protocol USER_INFO_MODIFY";

	e = gfm_server_get_request(peer, msg,
	    "ssss", &ui.username, &ui.realname, &ui.homedir, &ui.gsi_dn);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (skip) {
		gfarm_user_info_free(&ui);
		return (GFARM_ERR_NO_ERROR);
	}
	giant_lock();
	if (!from_client || user == NULL || !user_is_admin(user)) {
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		needs_free = 1;
	} else if ((u = user_lookup(ui.username)) == NULL ||
		   user_is_invalidated(u)) {
		e = GFARM_ERR_NO_SUCH_USER;
		needs_free = 1;
	} else if ((e = db_user_modify(&ui,
	    DB_USER_MOD_REALNAME|DB_USER_MOD_HOMEDIR|DB_USER_MOD_GSI_DN)) !=
	    GFARM_ERR_NO_ERROR) {
		needs_free = 1;
	} else {
		free(u->ui.realname);
		free(u->ui.homedir);
		free(u->ui.gsi_dn);
		u->ui.realname = ui.realname;
		u->ui.homedir = ui.homedir;
		u->ui.gsi_dn = ui.gsi_dn;
		free(ui.username);
	}
	if (needs_free)
		gfarm_user_info_free(&ui);
	giant_unlock();
	return (gfm_server_put_reply(peer, msg, e, ""));
}

gfarm_error_t
user_info_remove_default(const char *username, const char *diag)
{
	gfarm_int32_t e, e2;

	if ((e = user_remove(username)) == GFARM_ERR_NO_ERROR) {
		e2 = db_user_remove(username);
		if (e2 != GFARM_ERR_NO_ERROR)
			gflog_error(GFARM_MSG_1000240,
			    "%s: db_user_remove: %s",
			    diag, gfarm_error_string(e2));
	}
	return (e);
}

gfarm_error_t (*user_info_remove)(const char *, const char *) =
	user_info_remove_default;

gfarm_error_t
gfm_server_user_info_remove(struct peer *peer, int from_client, int skip)
{
	char *username;
	gfarm_int32_t e;
	struct user *user = peer_get_user(peer);
	const char msg[] = "protocol USER_INFO_REMOVE";

	e = gfm_server_get_request(peer, msg,
	    "s", &username);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (skip) {
		free(username);
		return (GFARM_ERR_NO_ERROR);
	}
	giant_lock();
	if (!from_client || user == NULL || !user_is_admin(user)) {
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
	} else
		e = user_info_remove(username, msg);
	free(username);
	giant_unlock();
	return (gfm_server_put_reply(peer, msg, e, ""));
}
#endif /* TEST */
