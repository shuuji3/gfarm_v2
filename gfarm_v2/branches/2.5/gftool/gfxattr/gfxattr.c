/*
 * Copyright (c) 2009 National Institute of Informatics in Japan.
 * All rights reserved.
 */

/*
 * $Id$
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <libgen.h>
#include <strings.h>
#include <string.h>
#include <errno.h>

#include <gfarm/gfarm.h>
#include "gfutil.h"

#include "gfarm_path.h"
#include "lookup.h"
#include "gfm_client.h"

#define DEFAULT_ALLOC_SIZE (64 * 1024)

static gfarm_error_t
alloc_buf(const char *filename, char **bufp, size_t *szp)
{
	const size_t count = 65536;
	ssize_t sz;
	size_t buf_sz, msg_sz = 0;
	char *buf = NULL, *tbuf;
	int fd, save_errno;
	int overflow;

#ifdef __GNUC__ /* workaround gcc warning: might be used uninitialized */
	tbuf = NULL;
#endif
	if (filename != NULL) {
		if ((fd = open(filename, O_RDONLY)) == -1) {
			save_errno = errno;
			fprintf(stderr, "%s: %s\n", filename,
			    strerror(save_errno));
			return (gfarm_errno_to_error(save_errno));
		}
	} else
		fd = STDIN_FILENO;

	buf_sz = count;
	overflow = 0;
	buf_sz = gfarm_size_add(&overflow, buf_sz, 1);
	if (!overflow)
		buf = malloc(buf_sz);
	if (buf == NULL)
		return (GFARM_ERR_NO_MEMORY);

	while ((sz = read(fd, buf + msg_sz, count)) > 0) {
		msg_sz += sz;
		buf_sz = gfarm_size_add(&overflow, buf_sz, count);
		if (!overflow)
			tbuf = realloc(buf, buf_sz);
		if (overflow || (tbuf == NULL)) {
			free(buf);
			return (GFARM_ERR_NO_MEMORY);
		}
		buf = tbuf;
	}
	buf[msg_sz] = '\0';
	if (filename != NULL)
		close(fd);

	if (bufp != NULL)
		*bufp = buf;
	if (szp != NULL)
		*szp = msg_sz;
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
set_xattr(int xmlMode, int nofollow, char *path, char *xattrname,
	char *filename, int flags)
{
	size_t msg_sz;
	char *buf;
	gfarm_error_t e;

	e = alloc_buf(filename, &buf, &msg_sz);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (xmlMode)
		e = (nofollow ? gfs_lsetxmlattr : gfs_setxmlattr)
			(path, xattrname, buf, msg_sz + 1, flags);
	else
		e = (nofollow ? gfs_lsetxattr : gfs_setxattr)
			(path, xattrname, buf, msg_sz, flags);
	free(buf);
	return (e);
}

static gfarm_error_t
set_xattr_by_inode(int xmlMode, gfarm_ino_t inum, gfarm_uint64_t gen,
	char *xattrname, char *filename, int flags)
{
	size_t msg_sz;
	char *buf;
	struct gfm_connection *gfm_server;
	gfarm_error_t e;

	if ((e = gfm_client_connection_and_process_acquire_by_path(
	    GFARM_PATH_ROOT, &gfm_server)) != GFARM_ERR_NO_ERROR)
		return (e);
	if ((e = alloc_buf(filename, &buf, &msg_sz)) != GFARM_ERR_NO_ERROR)
		return (e);
	if (xmlMode)
		++msg_sz; /* for the last additional '\0' */
	e = gfm_client_setxattr_by_inode(gfm_server, xmlMode, inum, gen,
		xattrname, buf, msg_sz, flags);
	gfm_client_connection_free(gfm_server);
	free(buf);
	return (e);
}

static gfarm_error_t
get_xattr_alloc(int xmlMode, int nofollow, char *path, char *xattrname,
		void **valuep, size_t *size)
{
	gfarm_error_t e;
	void *value;

	value = malloc(*size);
	if (value == NULL)
		return (GFARM_ERR_NO_MEMORY);

	if (xmlMode)
		e = (nofollow ? gfs_lgetxmlattr : gfs_getxmlattr)
			(path, xattrname, value, size);
	else
		e = (nofollow ? gfs_lgetxattr : gfs_getxattr)
			(path, xattrname, value, size);

	if (e == GFARM_ERR_NO_ERROR)
		*valuep = value;
	else
		free(value);
	return (e);
}

static gfarm_error_t
write_buf(const char *filename, void *value, size_t size)
{
	FILE *f;
	int save_errno;
	size_t wsize;

	if (filename != NULL) {
		f = fopen(filename, "w");
		if (f == NULL) {
			save_errno = errno;
			fprintf(stderr, "%s: %s\n", filename,
				strerror(save_errno));
			return (gfarm_errno_to_error(save_errno));
		}
	} else
		f = stdout;

	wsize = fwrite(value, 1, size, f);
	if (wsize != size)
		perror("fwrite");
	fflush(f);
	if (filename != NULL)
		fclose(f);
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
get_xattr(int xmlMode, int nofollow, char *path, char *xattrname,
	char *filename)
{
	gfarm_error_t e;
	void *value = NULL;
	size_t size;

	size = DEFAULT_ALLOC_SIZE;
	e = get_xattr_alloc(xmlMode, nofollow, path, xattrname, &value, &size);
	if (e == GFARM_ERR_RESULT_OUT_OF_RANGE)
		e = get_xattr_alloc(xmlMode, nofollow, path, xattrname, &value,
			&size);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	e = write_buf(filename, value, size);
	free(value);
	return (e);
}

static gfarm_error_t
get_xattr_by_inode(int xmlMode, gfarm_ino_t inum, gfarm_uint64_t gen,
	char *xattrname, char *filename)
{
	struct gfm_connection *gfm_server;
	void *value;
	size_t size;
	gfarm_error_t e;

	if ((e = gfm_client_connection_and_process_acquire_by_path(
	    GFARM_PATH_ROOT, &gfm_server)) != GFARM_ERR_NO_ERROR)
		return (e);
	e = gfm_client_getxattr_by_inode(gfm_server, xmlMode, inum, gen,
	    xattrname, &value, &size);
	gfm_client_connection_free(gfm_server);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	e = write_buf(filename, value, size);
	free(value);
	return (e);
}

static gfarm_error_t
remove_xattr(int xmlMode, int nofollow, char *path, char *xattrname)
{
	gfarm_error_t e;

	if (xmlMode) {
		e = (nofollow ? gfs_lremovexmlattr : gfs_removexmlattr)
			(path, xattrname);
	} else {
		e = (nofollow ? gfs_lremovexattr : gfs_removexattr)
			(path, xattrname);
	}
	return (e);
}

static gfarm_error_t
remove_xattr_by_inode(int xmlMode, gfarm_ino_t inum, gfarm_uint64_t gen,
	char *xattrname)
{
	struct gfm_connection *gfm_server;
	gfarm_error_t e;

	if ((e = gfm_client_connection_and_process_acquire_by_path(
	    GFARM_PATH_ROOT, &gfm_server)) != GFARM_ERR_NO_ERROR)
		return (e);
	e = gfm_client_removexattr_by_inode(gfm_server, xmlMode, inum, gen,
	    xattrname);
	gfm_client_connection_free(gfm_server);
	return (e);
}

static gfarm_error_t
list_xattr_alloc(int xmlMode, int nofollow, char *path, char **listp,
	size_t *size)
{
	gfarm_error_t e;
	char *list;

	list = malloc(*size);
	if (list == NULL)
		return (GFARM_ERR_NO_MEMORY);

	if (xmlMode)
		e = (nofollow ? gfs_llistxmlattr : gfs_listxmlattr)
			(path, list, size);
	else
		e = (nofollow ? gfs_llistxattr : gfs_listxattr)
			(path, list, size);

	if (e == GFARM_ERR_NO_ERROR)
		*listp = list;
	else
		free(list);
	return (e);
}

static gfarm_error_t
list_xattr(int xmlMode, int nofollow, char *path)
{
	gfarm_error_t e;
	char *list = NULL, *base, *p, *last;
	size_t size;

	size = DEFAULT_ALLOC_SIZE;
	e = list_xattr_alloc(xmlMode, nofollow, path, &list, &size);
	if (e == GFARM_ERR_RESULT_OUT_OF_RANGE) {
		e = list_xattr_alloc(xmlMode, nofollow, path, &list, &size);
	}
	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	base = list;
	last = base + size;
	while ((base < last) && ((p = strchr(base, '\0')) != NULL)) {
		printf("%s\n", base);
		base = p + 1;
	}
	free(list);
	return (e);
}

void
usage(char *prog_name)
{
	fprintf(stderr, "Usage: %s [ -s | -g | -r | -l ]"
#ifdef ENABLE_XMLATTR
		" [ -x ]"
#endif
		" [ -c | -m ]"
		" [ -f xattrfile ] [ -h ] file [xattrname]\n", prog_name);
	fprintf(stderr, "       %s [ -s | -g | -r | -l ]"
#ifdef ENABLE_XMLATTR
		" [ -x ]"
#endif
		" [ -c | -m ] [ -G gen ]"
		" [ -f xattrfile ] -I inum [xattrname]\n", prog_name);
	fprintf(stderr, "\t-s\tset extended attribute\n");
	fprintf(stderr, "\t-g\tget extended attribute\n");
	fprintf(stderr, "\t-r\tremove extended attribute\n");
	fprintf(stderr, "\t-l\tlist extended attribute\n");
#ifdef ENABLE_XMLATTR
	fprintf(stderr, "\t-x\thandle XML extended attribute\n");
#endif
	fprintf(stderr, "\t-c\tfail if xattrname already exists "
		"(use with -s)\n");
	fprintf(stderr, "\t-m\tfail if xattrname does not exist "
		"(use with -s)\n");
	fprintf(stderr, "\t-h\tprocess symbolic link instead of "
		"any referenced file\n");
	exit(2);
}

/*
 *
 */

int
main(int argc, char *argv[])
{
	char *prog_name = basename(argv[0]);
	char *filename = NULL, *c_path, *c_realpath = NULL, *xattrname = NULL;
	enum { NONE, SET_MODE, GET_MODE, REMOVE_MODE, LIST_MODE } mode = NONE;
	enum { REGULAR, INUM } file_mode = REGULAR;
	int c, xmlMode = 0, nofollow = 0, flags = 0;
	gfarm_ino_t inum = 0;
	gfarm_uint64_t gen = 0;
	gfarm_error_t e;
	const char *opts = "f:G:gIsrlcmh?"
#ifdef ENABLE_XMLATTR
		"x"
#endif
		;
#ifdef __GNUC__ /* workaround gcc warning: might be used uninitialized */
	c_path = NULL;
#endif

	while ((c = getopt(argc, argv, opts)) != -1) {
		switch (c) {
		case 'f':
			filename = optarg;
			break;
#ifdef ENABLE_XMLATTR
		case 'x':
			xmlMode = 1;
			break;
#endif
		case 'g':
			mode = GET_MODE;
			break;
		case 's':
			mode = SET_MODE;
			break;
		case 'r':
			mode = REMOVE_MODE;
			break;
		case 'l':
			mode = LIST_MODE;
			break;
		case 'c':
			if (flags == 0)
				flags = GFS_XATTR_CREATE;
			else
				usage(prog_name);
			break;
		case 'm':
			if (flags == 0)
				flags = GFS_XATTR_REPLACE;
			else
				usage(prog_name);
			break;
		case 'h':
			nofollow = 1;
			break;
		case 'G':
			gen = atoll(optarg);
			break;
		case 'I':
			file_mode = INUM;
			break;
		case '?':
		default:
			usage(prog_name);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1 || mode == NONE)
		usage(prog_name);

	e = gfarm_initialize(&argc, &argv);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "%s: %s\n", prog_name, gfarm_error_string(e));
		exit(1);
	}

	if (file_mode == REGULAR) {
		e = gfarm_realpath_by_gfarm2fs(argv[0], &c_realpath);
		if (e == GFARM_ERR_NO_ERROR)
			c_path = c_realpath;
		else
			c_path = argv[0];
	} else
		inum = atoll(argv[0]);
	if (argc > 1)
		xattrname = argv[1];

	switch (mode) {
	case SET_MODE:
		if (argc != 2)
			usage(prog_name);
		if (file_mode == REGULAR)
			e = set_xattr(xmlMode, nofollow, c_path, xattrname,
			    filename, flags);
		else
			e = set_xattr_by_inode(xmlMode, inum, gen, xattrname,
			    filename, flags);
		break;
	case GET_MODE:
		if (argc != 2)
			usage(prog_name);
		if (file_mode == REGULAR)
			e = get_xattr(xmlMode, nofollow, c_path, xattrname,
			    filename);
		else
			e = get_xattr_by_inode(xmlMode, inum, gen, xattrname,
			    filename);
		break;
	case REMOVE_MODE:
		if (argc != 2)
			usage(prog_name);
		if (file_mode == REGULAR)
			e = remove_xattr(xmlMode, nofollow, c_path, xattrname);
		else
			e = remove_xattr_by_inode(xmlMode, inum, gen,
			    xattrname);
		break;
	case LIST_MODE:
		if (argc != 1)
			usage(prog_name);
		if (file_mode == REGULAR)
			e = list_xattr(xmlMode, nofollow, c_path);
		else
			e = GFARM_ERR_OPERATION_NOT_SUPPORTED;
		break;
	default:
		usage(prog_name);
		break;
	}
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "%s\n", gfarm_error_string(e));
		exit(1);
	}
	free(c_realpath);

	e = gfarm_terminate();
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "%s: %s\n", prog_name, gfarm_error_string(e));
		exit(1);
	}
	exit(0);
}