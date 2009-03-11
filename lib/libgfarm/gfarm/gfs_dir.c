/*
 * $Id$
 */

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <openssl/evp.h>

#if !defined(__GNUC__) && \
	(!defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L)
# define inline
#endif

#define GFARM_INTERNAL_USE
#include <gfarm/gfarm.h>

#include "timer.h"
#include "gfutil.h"

#include "gfs_profile.h"
#include "gfm_client.h"
#include "config.h"
#include "lookup.h"
#include "gfs_io.h"

#if 0 /* not yet in gfarm v2 */

static char *gfarm_current_working_directory;

gfarm_error_t
gfs_chdir_canonical(const char *canonic_dir)
{
	static int cwd_len = 0;
	static char env_name[] = "GFS_PWD=";
	static char *env = NULL;
	static int env_len = 0;
	int len, old_len;
	char *e, *tmp, *old_env;
	struct gfarm_path_info pi;

	e = gfarm_path_info_get(canonic_dir, &pi);
	if (e == NULL) {
		e = gfarm_path_info_access(&pi, X_OK);
		gfarm_path_info_free(&pi);
	}
	if (e != NULL)
		return (e);

	len = 1 + strlen(canonic_dir) + 1;
	if (cwd_len < len) {
		GFARM_REALLOC_ARRAY(tmp, gfarm_current_working_directory, len);
		if (tmp == NULL)
			return (GFARM_ERR_NO_MEMORY);
		gfarm_current_working_directory = tmp;
		cwd_len = len;
	}
	sprintf(gfarm_current_working_directory, "/%s", canonic_dir);

	len += sizeof(env_name) - 1 + GFARM_URL_PREFIX_LENGTH;
	tmp = getenv("GFS_PWD");
	if (tmp == NULL || tmp != env + sizeof(env_name) - 1) {
		/*
		 * changed by an application instead of this function, and
		 * probably it's already free()ed.  In this case, realloc()
		 * does not work well at least using bash.  allocate it again.
		 */
		env = NULL;
		env_len = 0;
	}
	old_env = env;
	old_len = env_len;
	if (env_len < len) {
		/*
		 * We cannot use realloc(env, ...) here, because `env' may be
		 * still pointed by environ[somewhere] (at least with glibc),
		 * and realloc() may break the memory.  So, allocate different
		 * memory.
		 */
		GFARM_MALLOC_ARRAY(tmp, len);
		if (tmp == NULL)
			return (GFARM_ERR_NO_MEMORY);
		env = tmp;
		env_len = len;
	}
	sprintf(env, "%s%s%s",
	    env_name, GFARM_URL_PREFIX, gfarm_current_working_directory);

	if (putenv(env) != 0) {
		if (env != old_env && env != NULL)
			free(env);
		env = old_env;
		env_len = old_len;
		return (gfarm_errno_to_error(errno));
	}
	if (old_env != env && old_env != NULL)
		free(old_env);

	return (NULL);
}

gfarm_error_t
gfs_chdir(const char *dir)
{
	gfarm_error_t e;
	char *canonic_path;
	struct gfs_stat st;

	if ((e = gfs_stat(dir, &st)) != NULL)
		return (e);
	if (!GFARM_S_ISDIR(st.st_mode)) {
		gfs_stat_free(&st);
		return (GFARM_ERR_NOT_A_DIRECTORY);
	}
	gfs_stat_free(&st);

	e = gfarm_canonical_path(gfarm_url_prefix_skip(dir), &canonic_path);
	if (e != NULL)
		return (e);
	e = gfs_chdir_canonical(canonic_path);
	free (canonic_path);
	return (e);
}

gfarm_error_t
gfs_getcwd(char *cwd, int cwdsize)
{
	const char *path;
	char *default_cwd = NULL, *e, *p;
	int len;
	
	if (gfarm_current_working_directory != NULL)
		path = gfarm_current_working_directory;
	else if ((path = getenv("GFS_PWD")) != NULL)
		path = gfarm_url_prefix_skip(path);
	else { /* default case, use user's home directory */
		gfarm_error_t e;

		e = gfarm_path_expand_home("~", &default_cwd);
		if (e != NULL)
			return (e);
		path = default_cwd;
	}

	/* check the existence */
	e = gfarm_canonical_path(path, &p);
	if (e != NULL)
		goto finish;
	free(p);

	len = strlen(path);
	if (len < cwdsize) {
		strcpy(cwd, path);
		e = NULL;
	} else {
		e = GFARM_ERR_NUMERICAL_RESULT_OUT_OF_RANGE;
	}
finish:

	if (default_cwd != NULL)
		free(default_cwd);

	return (e);
}

#endif

/*
 * gfs_opendir()/readdir()/closedir()
 */

#define DIRENTS_BUFCOUNT	256

struct gfs_dir {
	int fd;
	struct gfs_dirent buffer[DIRENTS_BUFCOUNT];
	int n, index;
};

static gfarm_error_t
gfs_dir_alloc(gfarm_int32_t fd, GFS_Dir *dirp)
{
	GFS_Dir dir;

	GFARM_MALLOC(dir);
	if (dir == NULL)
		return (GFARM_ERR_NO_MEMORY);

	dir->fd = fd;
	dir->n = 0;
	dir->index = 0;

	*dirp = dir;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfs_opendir(const char *path, GFS_Dir *dirp)
{
	gfarm_error_t e;
	int fd, type;

	if ((e = gfm_open_fd(path, GFARM_FILE_RDONLY, &fd, &type))
	    != GFARM_ERR_NO_ERROR)
 		;
	else if (type != GFS_DT_DIR) {
		(void)gfm_close_fd(fd); /* ignore this result */
		e = GFARM_ERR_NOT_A_DIRECTORY;
	} else if ((e = gfs_dir_alloc(fd, dirp)) != GFARM_ERR_NO_ERROR)
		(void)gfm_close_fd(fd); /* ignore this result */

	return (e);
}

gfarm_error_t
gfs_readdir(GFS_Dir dir, struct gfs_dirent **entry)
{
	gfarm_error_t e;

	if (dir->index >= dir->n) {
		if ((e = gfm_client_compound_begin_request(
		    gfarm_metadb_server)) != GFARM_ERR_NO_ERROR)
			gflog_warning("compound_begin request: %s",
			    gfarm_error_string(e));
		else if ((e = gfm_client_put_fd_request(gfarm_metadb_server,
		    dir->fd)) != GFARM_ERR_NO_ERROR)
			gflog_warning("put_fd request: %s",
			    gfarm_error_string(e));
		else if ((e = gfm_client_getdirents_request(
		    gfarm_metadb_server, DIRENTS_BUFCOUNT))
		    != GFARM_ERR_NO_ERROR)
			gflog_warning("get_dirents request: %s",
			    gfarm_error_string(e));
		else if ((e = gfm_client_compound_end_request(
		    gfarm_metadb_server)) != GFARM_ERR_NO_ERROR)
			gflog_warning("compound_end request: %s",
			    gfarm_error_string(e));

		else if ((e = gfm_client_compound_begin_result(
		    gfarm_metadb_server)) != GFARM_ERR_NO_ERROR)
			gflog_warning("compound_begin result: %s",
			    gfarm_error_string(e));
		else if ((e = gfm_client_put_fd_result(gfarm_metadb_server))
		    != GFARM_ERR_NO_ERROR)
			gflog_warning("put_fd result: %s",
			    gfarm_error_string(e));
		else if ((e = gfm_client_getdirents_result(
		    gfarm_metadb_server, &dir->n, dir->buffer))
		    != GFARM_ERR_NO_ERROR)
			gflog_warning("get_dirents result: %s",
			    gfarm_error_string(e));
		else if ((e = gfm_client_compound_end_result(
		    gfarm_metadb_server)) != GFARM_ERR_NO_ERROR)
			gflog_warning("compound_end result: %s",
			    gfarm_error_string(e));

		if (e != GFARM_ERR_NO_ERROR)
			return (e);
		if (dir->n == 0) {
			*entry = NULL;
			return (GFARM_ERR_NO_ERROR);
		}
		dir->index = 0;
	}
	*entry = &dir->buffer[dir->index++];
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfs_closedir(GFS_Dir dir)
{
	gfarm_error_t e = gfm_close_fd(dir->fd);

	free(dir);
	return (e);
}

gfarm_error_t
gfs_seekdir(GFS_Dir dir, gfarm_off_t off)
{
	return (GFARM_ERR_FUNCTION_NOT_IMPLEMENTED); /* XXX FIXME */
}

gfarm_error_t
gfs_telldir(GFS_Dir dir, gfarm_off_t *offp)
{
	return (GFARM_ERR_FUNCTION_NOT_IMPLEMENTED); /* XXX FIXME */
}
