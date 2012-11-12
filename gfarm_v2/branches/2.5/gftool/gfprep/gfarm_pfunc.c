/*
 * $Id$
 */

#include <stdio.h>
#include <stdarg.h> /* gfprep.h: va_list*/
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>

#include <gfarm/gfarm.h>

#include "nanosec.h"
#include "thrsubr.h"

#include "config.h"
#include "queue.h" /* for gfs_pio.h */
#include <openssl/evp.h> /* for gfs_pio.h */
#include "gfs_pio.h"

#include "gfprep.h"
#include "gfarm_parallel.h"
#include "gfarm_fifo.h"
#include "gfarm_pfunc.h"
#include "gfarm_dirtree.h"

struct gfarm_pfunc {
	gfpara_t *gfpara_handle;
	gfarm_fifo_t *fifo_handle;
	void (*cb_start)(void *data);
	void (*cb_end)(int success, void *data);
	gfarm_int64_t simulate_KBs;
	char *copy_buf;
	int copy_bufsize;
	int is_end;
	pthread_mutex_t is_end_mutex;
};

struct gfarm_pfunc_cmd {
	int command;
	char *src_url;
	char *dst_url;
	const char *src_host;
	const char *dst_host;
	int src_port;
	int dst_port;
	gfarm_off_t src_size;
	int check_disk_avail;
	void *cb_data;
};

enum pfunc_cmdnum {
	PFUNC_CMD_REPLICATE,
	PFUNC_CMD_REPLICATE_MIGRATE,
	PFUNC_CMD_REPLICATE_SIMULATE,
	PFUNC_CMD_COPY,
	PFUNC_CMD_MOVE,
	PFUNC_CMD_REMOVE_REPLICA,
	PFUNC_CMD_TERMINATE
};

enum pfunc_result {
	PFUNC_RESULT_OK,
	PFUNC_RESULT_NG,
	PFUNC_RESULT_WAIT_OK,
	PFUNC_RESULT_END,
	PFUNC_RESULT_FATAL
};

enum pfunc_mode {
	PFUNC_MODE_NORMAL,
	PFUNC_MODE_MIGRATE
};

static void
pfunc_fifo_set(void *ents, int index, void *entp)
{
	gfarm_pfunc_cmd_t *entries = ents;
	gfarm_pfunc_cmd_t *entryp = entp;
	entries[index] = *entryp; /* copy */
}

static void
pfunc_fifo_get(void *ents, int index, void *entp)
{
	gfarm_pfunc_cmd_t *entries = ents;
	gfarm_pfunc_cmd_t *entryp = entp;
	*entryp = entries[index]; /* copy */
}

static void
pfunc_simulate(const char *url, gfarm_uint64_t KBs)
{
	gfarm_uint64_t size = 0;

	if (gfprep_url_is_local(url)) {
		int retv;
		char *path = (char *) url;
		struct stat st;
		path += GFPREP_FILE_URL_PREFIX_LENGTH;
		retv = lstat(path, &st);
		if (retv == 0)
			size = st.st_size;
	} else {
		gfarm_error_t e;
		struct gfs_stat st;
		e = gfs_lstat(url, &st);
		if  (e == GFARM_ERR_NO_ERROR) {
			size = st.st_size;
			gfs_stat_free(&st);
		}
	}
	/* simulate: "size / (KBs * 1000)" seconds per a file */
	gfarm_nanosleep((size * (GFARM_SECOND_BY_NANOSEC / 1000)) / KBs);
}

static gfarm_error_t
pfunc_check_disk_avail(
	const char *url, char *hostname, int port, gfarm_off_t filesize)
{
	gfarm_error_t e;
	gfarm_int32_t bsize;
	gfarm_off_t blocks, bfree, bavail, files;
	gfarm_off_t ffree, favail, avail;

	e = gfs_statfsnode_by_path(
		url, hostname, port, &bsize, &blocks, &bfree, &bavail,
		&files, &ffree, &favail);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	avail = bavail * bsize;
	if (avail >= filesize &&
	    avail >= gfarm_get_minimum_free_disk_space())
		return (GFARM_ERR_NO_ERROR);
	return (GFARM_ERR_NO_SPACE);
}

static void
pfunc_replicate_main(gfarm_pfunc_t *handle, int pfunc_mode,
		     FILE *from_parent, FILE *to_parent)
{
	gfarm_error_t e;
	char *src_url, *src_host, *dst_host;
	int src_port, dst_port;
	gfarm_off_t src_size;
	int check_disk_avail;

	gfpara_recv_string(from_parent, &src_url);
	gfpara_recv_int64(from_parent, &src_size);
	gfpara_recv_string(from_parent, &src_host);
	gfpara_recv_int(from_parent, &src_port);
	gfpara_recv_string(from_parent, &dst_host);
	gfpara_recv_int(from_parent, &dst_port);
	gfpara_recv_int(from_parent, &check_disk_avail);

	if (handle->simulate_KBs > 0) {
		pfunc_simulate(src_url, handle->simulate_KBs);
		goto end;
	}
	if (check_disk_avail && dst_port > 0) { /* dst is gfarm */
		e = pfunc_check_disk_avail(
		    src_url, dst_host, dst_port, src_size);
		if (e != GFARM_ERR_NO_ERROR) {
			fprintf(stderr,
				"ERROR: cannot replicate: "
				"checking disk_avail: %s (%s:%d, %s:%d): %s\n",
				src_url, src_host, src_port,
				dst_host, dst_port, gfarm_error_string(e));
			gfpara_send_int(to_parent, PFUNC_RESULT_NG);
			goto free_mem;
		}
	}
	e = gfs_replicate_from_to(src_url, src_host, src_port,
				  dst_host, dst_port);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr,
			"ERROR: cannot replicate: %s (%s:%d, %s:%d): %s\n",
			src_url, src_host, src_port, dst_host, dst_port,
			gfarm_error_string(e));
		gfpara_send_int(to_parent, PFUNC_RESULT_NG);
		goto free_mem;
	}
	if (pfunc_mode == PFUNC_MODE_MIGRATE) {
		e = gfs_replica_remove_by_file(src_url, src_host);
		if (e != GFARM_ERR_NO_ERROR) {
			fprintf(stderr,
				"ERROR: cannot migrate: %s (%s:%d): %s\n",
				src_url, src_host, src_port,
				gfarm_error_string(e));
			e = gfs_replica_remove_by_file(src_url, dst_host);
			if (e != GFARM_ERR_NO_ERROR)
				fprintf(stderr,
					"ERROR: cannot remove useless "
					"replica: %s (%s:%d): %s\n",
					src_url, dst_host, dst_port,
					gfarm_error_string(e));
		}
	}
end:
	gfpara_send_int(to_parent, PFUNC_RESULT_OK);
free_mem:
	free(src_url);
	free(src_host);
	free(dst_host);
}

struct pfunc_file {
	int fd;
	GFS_File gfarm;
};

static gfarm_error_t
pfunc_open(const char *url, int flags, int mode, struct pfunc_file *fp)
{
	if (gfprep_url_is_local(url)) {
		int fd;
		char *path = (char *) url;
		path += GFPREP_FILE_URL_PREFIX_LENGTH;
		fd = open(path, flags, mode);
		if (fd == -1)
			return (gfarm_errno_to_error(errno));
		fp->fd = fd;
		fp->gfarm = NULL;
	} else if (gfprep_url_is_gfarm(url)) {
		gfarm_error_t e;
		GFS_File gf;
		int gflags = 0;
		if (flags & O_RDONLY)
			gflags |= GFARM_FILE_RDONLY;
		if (flags & O_WRONLY)
			gflags |= GFARM_FILE_WRONLY;
		if (flags & O_TRUNC)
			gflags |= GFARM_FILE_TRUNC;
		if (flags & O_CREAT)
			e = gfs_pio_create(url, gflags, mode, &gf);
		else
			e = gfs_pio_open(url, gflags, &gf);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
		fp->fd = 0;
		fp->gfarm = gf;
	} else
		return (GFARM_ERR_INVALID_ARGUMENT);
	return (GFARM_ERR_NO_ERROR);
}

struct pfunc_stat {
	gfarm_int32_t mode;
	gfarm_int64_t size;
	gfarm_int64_t atime_sec;
	gfarm_int32_t atime_nsec;
	gfarm_int64_t mtime_sec;
	gfarm_int32_t mtime_nsec;
};

static gfarm_error_t
pfunc_fstat(struct pfunc_file *fp, struct pfunc_stat *stp)
{
#ifdef __GNUC__ /* to shut up gcc warning "may be used uninitialized" */
	stp->mode = stp->size = 0;
	stp->mtime_sec = stp->atime_sec = 0;
	stp->mtime_nsec = stp->atime_nsec = 0;
#endif
	if (fp->gfarm) {
		struct gfs_stat gst;
		gfarm_error_t e = gfs_pio_stat(fp->gfarm, &gst);

		if (e != GFARM_ERR_NO_ERROR)
			return (e);
		stp->mode = gst.st_mode;
		stp->size = gst.st_size;
		stp->atime_sec = gst.st_atimespec.tv_sec;
		stp->atime_nsec = gst.st_atimespec.tv_nsec;
		stp->mtime_sec = gst.st_mtimespec.tv_sec;
		stp->mtime_nsec = gst.st_mtimespec.tv_nsec;
		gfs_stat_free(&gst);
	} else {
		int retv;
		struct stat st;

		retv = fstat(fp->fd, &st);
		if (retv == -1)
			return (gfarm_errno_to_error(errno));
		stp->mode = st.st_mode;
		stp->size = st.st_size;
		stp->atime_sec = st.st_atime;
		stp->atime_nsec = gfarm_stat_atime_nsec(&st);
		stp->mtime_sec = st.st_mtime;
		stp->mtime_nsec = gfarm_stat_mtime_nsec(&st);
	}
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
pfunc_read(struct pfunc_file *fp, void *buf, int bufsize, int *rsize)
{
	int len;
	char *b = buf;

	if (fp->gfarm)
		return (gfs_pio_read(fp->gfarm, buf, bufsize, rsize));
	*rsize = 0;
	while ((len = read(fp->fd, b, bufsize)) > 0) {
		if (len == bufsize)
			break;
		b += len;
		bufsize -= len;
		*rsize += len;
	}
	if (len == -1)
		return (gfarm_errno_to_error(errno));
	*rsize += len;
	/* *rsize == 0: EOF */
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
pfunc_write(struct pfunc_file *fp, void *buf, int bufsize, int *wsize)
{
	int len;
	char *b = buf;

	if (fp->gfarm)
		return (gfs_pio_write(fp->gfarm, buf, bufsize, wsize));
	*wsize = 0;
	while ((len = write(fp->fd, b, bufsize)) > 0) {
		if (len == bufsize)
			break;
		b += len;
		bufsize -= len;
		*wsize += len;
	}
	if (len == -1)
		return (gfarm_errno_to_error(errno));
	*wsize += len;
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
pfunc_close(struct pfunc_file *fp)
{
	int retv;

	if (fp->gfarm)
		return (gfs_pio_close(fp->gfarm));
	retv = close(fp->fd);
	if (retv == -1)
		return (gfarm_errno_to_error(errno));
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
pfunc_utimens(const char *url, struct pfunc_stat *stp)
{
	if (gfprep_url_is_local(url)) {
		int retv;
		const char *path = url + GFPREP_FILE_URL_PREFIX_LENGTH;
		struct timeval tv[2];

		tv[0].tv_sec = stp->atime_sec;
		tv[0].tv_usec = stp->atime_nsec / GFARM_MICROSEC_BY_NANOSEC;
		tv[1].tv_sec = stp->mtime_sec;
		tv[1].tv_usec = stp->mtime_nsec / GFARM_MICROSEC_BY_NANOSEC;
		/* XXX support utimensat() */
		retv = utimes(path, tv);
		if (retv == -1)
			return (gfarm_errno_to_error(errno));
	} else if (gfprep_url_is_gfarm(url)) {
		gfarm_error_t e;
		struct gfarm_timespec gt[2];

		gt[0].tv_sec = stp->atime_sec;
		gt[0].tv_nsec = stp->atime_nsec;
		gt[1].tv_sec = stp->mtime_sec;
		gt[1].tv_nsec = stp->mtime_nsec;
		e = gfs_utimes(url, gt);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
	} else
		return (GFARM_ERR_INVALID_ARGUMENT);
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
pfunc_rename(const char *old_url, const char *new_url)
{
	if (gfprep_url_is_local(old_url) && gfprep_url_is_local(new_url)) {
		int retv;
		const char *old_path = old_url;
		const char *new_path = new_url;
		old_path += GFPREP_FILE_URL_PREFIX_LENGTH;
		new_path += GFPREP_FILE_URL_PREFIX_LENGTH;
		retv = rename(old_path, new_path);
		if (retv == -1)
			return (gfarm_errno_to_error(errno));
	} else if (gfprep_url_is_gfarm(old_url) &&
		   gfprep_url_is_gfarm(new_url)) {
		gfarm_error_t e;
		e = gfs_rename(old_url, new_url);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
	} else
		return (GFARM_ERR_INVALID_ARGUMENT);
	return (GFARM_ERR_NO_ERROR);
}

static gfarm_error_t
pfunc_unlink(const char *url)
{
	if (gfprep_url_is_local(url)) {
		int retv;
		const char *path = url;
		path += GFPREP_FILE_URL_PREFIX_LENGTH;
		retv = unlink(path);
		if (retv == -1)
			return (gfarm_errno_to_error(errno));
	} else if (gfprep_url_is_gfarm(url)) {
		gfarm_error_t e;
		e = gfs_unlink(url);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
	} else
		return (GFARM_ERR_INVALID_ARGUMENT);
	return (GFARM_ERR_NO_ERROR);
}

static const char tmp_url_suffix[] = "__tmp_gfpcopy__";

static void
pfunc_copy_main(gfarm_pfunc_t *handle, int pfunc_mode,
		FILE *from_parent, FILE *to_parent)
{
	gfarm_error_t e;
	int ng = 0, retv;
	char *tmp_url, *src_url, *dst_url, *src_host, *dst_host;
	int src_port, dst_port;
	int rsize, wsize;
	struct pfunc_file src_fp, dst_fp;
	struct pfunc_stat st;
	gfarm_off_t src_size;
	int check_disk_avail;

	gfpara_recv_string(from_parent, &src_url);
	gfpara_recv_int64(from_parent, &src_size);
	gfpara_recv_string(from_parent, &src_host);
	gfpara_recv_int(from_parent, &src_port);
	gfpara_recv_string(from_parent, &dst_url);
	gfpara_recv_string(from_parent, &dst_host);
	gfpara_recv_int(from_parent, &dst_port);
	gfpara_recv_int(from_parent, &check_disk_avail);

	retv = gfprep_asprintf(&tmp_url, "%s%s", dst_url, tmp_url_suffix);
	if (retv == -1) {
		fprintf(stderr, "ERROR: copy failed (no memory): %s\n",
			src_url);
		tmp_url = NULL;
		goto end;
	}
	if (handle->simulate_KBs > 0) {
		pfunc_simulate(src_url, handle->simulate_KBs);
		goto end;
	}
	if (check_disk_avail && dst_port > 0) { /* dst is gfarm */
		e = pfunc_check_disk_avail(
		    dst_url, dst_host, dst_port, src_size);
		if (e != GFARM_ERR_NO_ERROR) {
			fprintf(stderr,
				"ERROR: copy failed: checking disk_avail: "
				"%s (%s:%d, %s:%d): %s\n",
				src_url, src_host, src_port,
				dst_host, dst_port, gfarm_error_string(e));
			ng = 1;
			goto end;
		}
	}
	e = pfunc_open(src_url, O_RDONLY, 0, &src_fp);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "ERROR: copy failed: open(%s): %s\n",
			src_url, gfarm_error_string(e));
		ng = 1;
		goto end;
	}
	e = pfunc_fstat(&src_fp, &st);
	if (e != GFARM_ERR_NO_ERROR) {
		(void) pfunc_close(&src_fp);
		fprintf(stderr, "ERROR: copy failed: lstat(%s): %s\n",
			src_url, gfarm_error_string(e));
		ng = 1;
		goto end;
	}
	e = pfunc_open(tmp_url, O_CREAT | O_WRONLY | O_TRUNC,
		       st.mode & 07777, &dst_fp);
	if (e != GFARM_ERR_NO_ERROR) {
		(void) pfunc_close(&src_fp);
		fprintf(stderr, "ERROR: copy failed: open(%s): %s\n",
			tmp_url, gfarm_error_string(e));
		ng = 1;
		goto end;
	}
	if (st.size > 0 && src_fp.gfarm && strcmp(src_host, "") != 0) {
		/* XXX FIXME: INTERNAL FUNCTION SHOULD NOT BE USED */
		e = gfs_pio_internal_set_view_section(src_fp.gfarm, src_host);
		if (e != GFARM_ERR_NO_ERROR) {
			fprintf(stderr,
				"ERROR: copy failed: set_view(%s, %s): %s\n",
				src_url, src_host, gfarm_error_string(e));
			ng = 1;
			goto close;
		}
	}
	if (st.size > 0 && dst_fp.gfarm && strcmp(dst_host, "") != 0) {
		/* XXX FIXME: INTERNAL FUNCTION SHOULD NOT BE USED */
		e = gfs_pio_internal_set_view_section(dst_fp.gfarm, dst_host);
		if (e != GFARM_ERR_NO_ERROR) {
			fprintf(stderr,
				"ERROR: copy failed: set_view(%s, %s): %s\n",
				tmp_url, dst_host, gfarm_error_string(e));
			ng = 1;
			goto close;
		}
	}
	while ((e = pfunc_read(&src_fp, handle->copy_buf,
			       handle->copy_bufsize, &rsize))
	       == GFARM_ERR_NO_ERROR) {
		if (rsize == 0) /* EOF */
			break;
		e = pfunc_write(&dst_fp, handle->copy_buf, rsize, &wsize);
		if (e != GFARM_ERR_NO_ERROR) {
			fprintf(stderr, "ERROR: copy failed: write(%s): %s\n",
				tmp_url, gfarm_error_string(e));
			ng = 1;
			goto close;
		}
		if (rsize != wsize) {
			fprintf(stderr,
				"ERROR: copy failed: write(%s): "
				"rsize!=wsize\n", tmp_url);
			ng = 1;
			goto close;
		}
	}
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "ERROR: copy failed: read(%s): %s\n",
			src_url, gfarm_error_string(e));
		ng = 1;
		goto close;
	}
close:
	e = pfunc_close(&src_fp);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "ERROR: copy failed: close(%s): %s\n",
			tmp_url, gfarm_error_string(e));
		ng = 1;
		goto end;
	}
	e = pfunc_close(&dst_fp);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "ERROR: copy failed: close(%s): %s\n",
			tmp_url, gfarm_error_string(e));
		ng = 1;
		goto end;
	}
	e = pfunc_utimens(tmp_url, &st);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "ERROR: copy failed: utimes(%s): %s\n",
			tmp_url, gfarm_error_string(e));
		ng = 1;
		goto end;
	}
	e = pfunc_rename(tmp_url, dst_url);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "ERROR: copy failed: rename(%s -> %s): %s\n",
			tmp_url, dst_url, gfarm_error_string(e));
		ng = 1;
		goto end;
	}
	/* XXX pfunc_mode == PFUNC_MODE_MIGRATE : unlink src_url */
end:
	if (ng) {
		gfpara_send_int(to_parent, PFUNC_RESULT_NG);
		e = pfunc_unlink(tmp_url);
		if (e != GFARM_ERR_NO_ERROR)
			fprintf(stderr,
				"ERROR: cannot remove tmp-file: %s: %s\n",
			tmp_url, gfarm_error_string(e));
	} else
		gfpara_send_int(to_parent, PFUNC_RESULT_OK);
	free(tmp_url);
	free(src_url);
	free(dst_url);
	free(src_host);
	free(dst_host);
}

static void
pfunc_remove_replica_main(gfarm_pfunc_t *handle,
			  FILE *from_parent, FILE *to_parent)
{
	gfarm_error_t e;
	char *src_url, *src_host;
	int src_port; /* XXX unused */

	gfpara_recv_string(from_parent, &src_url);
	gfpara_recv_string(from_parent, &src_host);
	gfpara_recv_int(from_parent, &src_port);

	if (handle->simulate_KBs > 0) {
		gfpara_send_int(to_parent, PFUNC_RESULT_OK);
		goto end;
	}

	e = gfs_replica_remove_by_file(src_url, src_host);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr,
			"ERROR: cannot remove replica: %s (%s:%d): %s\n",
			src_url, src_host, src_port,
			gfarm_error_string(e));
		gfpara_send_int(to_parent, PFUNC_RESULT_NG);
	} else
		gfpara_send_int(to_parent, PFUNC_RESULT_OK);
end:
	free(src_url);
	free(src_host);
}

static int
pfunc_child(void *param, FILE *from_parent, FILE *to_parent)
{
	gfarm_error_t e;
	int command;
	gfarm_pfunc_t *handle = param;

	e = gfarm_initialize(NULL, NULL);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "ERROR: gfarm_initialize: %s\n",
			gfarm_error_string(e));
		gfpara_recv_purge(from_parent);
		gfpara_send_int(to_parent, PFUNC_RESULT_FATAL);
		return (0);
	}
	for (;;) {
		gfpara_recv_int(from_parent, &command);
		switch (command) {
		case PFUNC_CMD_REPLICATE:
			pfunc_replicate_main(handle, PFUNC_MODE_NORMAL,
					     from_parent, to_parent);
			continue;
		case PFUNC_CMD_REPLICATE_MIGRATE:
			pfunc_replicate_main(handle, PFUNC_MODE_MIGRATE,
					     from_parent, to_parent);
			continue;
		case PFUNC_CMD_COPY:
			pfunc_copy_main(handle, PFUNC_MODE_NORMAL,
					from_parent, to_parent);
			continue;
		case PFUNC_CMD_MOVE:
			pfunc_copy_main(handle, PFUNC_MODE_MIGRATE,
					from_parent, to_parent);
			continue;
		case PFUNC_CMD_REMOVE_REPLICA:
			pfunc_remove_replica_main(handle,
						  from_parent, to_parent);
			continue;
		case PFUNC_CMD_TERMINATE:
			gfpara_send_int(to_parent, PFUNC_RESULT_END);
			goto term;
		default:
			fprintf(stderr,
				"ERROR: unexpected command = %d\n", command);
			gfpara_recv_purge(from_parent);
			gfpara_send_int(to_parent, PFUNC_RESULT_FATAL);
			goto term;
		}
	}
term:
	e = gfarm_terminate();
	if (e != GFARM_ERR_NO_ERROR)
		fprintf(stderr, "ERROR: gfarm_terminate: %s\n",
			gfarm_error_string(e));

	return (0);
}

static void
pfunc_entry_free(gfarm_pfunc_cmd_t *entp)
{
	if (entp->src_url)
		free(entp->src_url);
	if (entp->dst_url)
		free(entp->dst_url);
	entp->src_url = NULL;
	entp->dst_url = NULL;
	entp->src_host = NULL;
	entp->dst_host = NULL;
}

static int
pfunc_is_end(gfarm_pfunc_t *handle)
{
	static const char diag[] = "pfunc_is_end";
	int res;

	gfarm_mutex_lock(&handle->is_end_mutex, diag, "is_end_mutex");
	res = handle->is_end;
	gfarm_mutex_unlock(&handle->is_end_mutex, diag, "is_end_mutex");
	return (res);
}

static void
pfunc_set_end(gfarm_pfunc_t *handle)
{
	static const char diag[] = "pfunc_set_end";

	gfarm_mutex_lock(&handle->is_end_mutex, diag, "is_end_mutex");
	handle->is_end = 1;
	gfarm_mutex_unlock(&handle->is_end_mutex, diag, "is_end_mutex");
}

static int
pfunc_send(FILE *child_in, gfpara_proc_t *proc, void *param, int interrupt)
{
	gfarm_pfunc_t *handle = param;
	gfarm_error_t e;
	gfarm_pfunc_cmd_t ent;

	if (interrupt || pfunc_is_end(handle)) {
		gfpara_data_set(proc, NULL);
		gfpara_send_int(child_in, PFUNC_CMD_TERMINATE);
		return (GFPARA_NEXT);
	}
	e = gfarm_fifo_delete(handle->fifo_handle, &ent); /* block */
	if (e == GFARM_ERR_NO_SUCH_OBJECT) { /* finish and empty */
		gfpara_data_set(proc, NULL);
		gfpara_send_int(child_in, PFUNC_CMD_TERMINATE);
		pfunc_set_end(handle);
		return (GFPARA_NEXT);
	} else if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "ERROR: fifo: %s\n", gfarm_error_string(e));
		gfpara_data_set(proc, NULL);
		gfpara_send_int(child_in, PFUNC_CMD_TERMINATE);
		pfunc_set_end(handle);
		return (GFPARA_NEXT);
	}
	gfpara_send_int(child_in, ent.command);
	switch (ent.command) {
	case PFUNC_CMD_REPLICATE:
	case PFUNC_CMD_REPLICATE_MIGRATE:
		gfpara_send_string(child_in, "%s", ent.src_url);
		gfpara_send_int64(child_in, ent.src_size);
		gfpara_send_string(child_in, "%s", ent.src_host);
		gfpara_send_int(child_in, ent.src_port);
		gfpara_send_string(child_in, "%s", ent.dst_host);
		gfpara_send_int(child_in, ent.dst_port);
		gfpara_send_int(child_in, ent.check_disk_avail);
		break;
	case PFUNC_CMD_COPY:
	case PFUNC_CMD_MOVE:
		gfpara_send_string(child_in, "%s", ent.src_url);
		gfpara_send_int64(child_in, ent.src_size);
		gfpara_send_string(child_in, "%s", ent.src_host);
		gfpara_send_int(child_in, ent.src_port);
		gfpara_send_string(child_in, "%s", ent.dst_url);
		gfpara_send_string(child_in, "%s", ent.dst_host);
		gfpara_send_int(child_in, ent.dst_port);
		gfpara_send_int(child_in, ent.check_disk_avail);
		break;
	case PFUNC_CMD_REMOVE_REPLICA:
		gfpara_send_string(child_in, "%s", ent.src_url);
		gfpara_send_string(child_in, "%s", ent.src_host);
		gfpara_send_int(child_in, ent.src_port);
		break;
	default:
		fprintf(stderr,
			"ERROR: unexpected command: %d\n", ent.command);
		gfpara_send_int(child_in, PFUNC_CMD_TERMINATE);
	}
	gfpara_data_set(proc, ent.cb_data);
	if (handle->cb_start && ent.cb_data)
		handle->cb_start(ent.cb_data); /* success */
	pfunc_entry_free(&ent);
	return (GFPARA_NEXT);
}

static int
pfunc_recv(FILE *child_out, gfpara_proc_t *proc, void *param)
{
	int status;
	gfarm_pfunc_t *handle = param;
	void *data = gfpara_data_get(proc);

	gfpara_recv_int(child_out, &status);
	switch (status) {
	case PFUNC_RESULT_OK:
	case PFUNC_RESULT_NG:
		if (handle->cb_end && data)
			handle->cb_end(status == PFUNC_RESULT_OK, data);
		return (GFPARA_NEXT);
	case PFUNC_RESULT_WAIT_OK:
		return (GFPARA_NEXT);
	case PFUNC_RESULT_END:
		return (GFPARA_END);
	case PFUNC_RESULT_FATAL:
	default:
		gfpara_recv_purge(child_out);
		return (GFPARA_FATAL);
	}
}

/* Do not call this function after gfarm_initialize() */
gfarm_error_t
gfarm_pfunc_start(gfarm_pfunc_t **handlep, int n_parallel, int queue_size,
		  gfarm_int64_t simulate_KBs, int copy_bufsize,
		  void (*cb_start)(void *), void (*cb_end)(int, void *))
{
	gfarm_error_t e;
	gfarm_pfunc_t *handle;
	char *buf;

	GFARM_MALLOC(handle);
	GFARM_MALLOC_ARRAY(buf, copy_bufsize);
	if (handle == NULL || buf == NULL)
		return (GFARM_ERR_NO_MEMORY);
	handle->simulate_KBs = simulate_KBs;
	handle->copy_buf = buf;
	handle->copy_bufsize = copy_bufsize;
	handle->cb_start = cb_start;
	handle->cb_end = cb_end;
	handle->is_end = 0;
	gfarm_mutex_init(&handle->is_end_mutex, "gfarm_pfunc_start",
			 "is_end_mutex");
	e = gfpara_init(&handle->gfpara_handle, n_parallel,
			pfunc_child, handle,
			pfunc_send, handle, pfunc_recv, handle, NULL, NULL);
	if (e != GFARM_ERR_NO_ERROR) {
		free(handle);
		return (e);
	}
	e = gfarm_fifo_init(&handle->fifo_handle,
			    queue_size, sizeof(gfarm_pfunc_cmd_t),
			    pfunc_fifo_set, pfunc_fifo_get);
	if (e != GFARM_ERR_NO_ERROR) {
		gfpara_join(handle->gfpara_handle);
		free(handle);
		return (e);
	}
	e = gfpara_start(handle->gfpara_handle);
	if (e != GFARM_ERR_NO_ERROR) {
		gfarm_fifo_wait_to_finish(handle->fifo_handle);
		gfpara_join(handle->gfpara_handle);
		free(handle);
		return (e);
	}
	*handlep = handle;
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfarm_pfunc_cmd_add(gfarm_pfunc_t *handle, gfarm_pfunc_cmd_t *cmd)
{
	return (gfarm_fifo_enter(handle->fifo_handle, cmd));
}

gfarm_error_t
gfarm_pfunc_interrupt(gfarm_pfunc_t *handle)
{
	return (gfpara_interrupt(handle->gfpara_handle, 5000));
}

gfarm_error_t
gfarm_pfunc_join(gfarm_pfunc_t *handle)
{
	gfarm_error_t e, e2;

	e = gfarm_fifo_wait_to_finish(handle->fifo_handle);
	e2 = gfpara_join(handle->gfpara_handle);
	gfarm_fifo_free(handle->fifo_handle);
	free(handle->copy_buf);
	free(handle);
	if (e2 == GFARM_ERR_NO_ERROR)
		return (e);
	else
		return (e2);
}

/* NOTE: src_host and dst_host is not free()ed (see pfunc_entry_free()) */
gfarm_error_t
gfarm_pfunc_replicate(
	gfarm_pfunc_t *pfunc_handle, const char *path,
	const char *src_host, int src_port, gfarm_off_t src_size,
	const char *dst_host, int dst_port,
	void *cb_data, int migrate, int check_disk_avail)
{
	gfarm_pfunc_cmd_t cmd;

	assert(src_host && dst_host);
	if (migrate)
		cmd.command = PFUNC_CMD_REPLICATE_MIGRATE;
	else
		cmd.command = PFUNC_CMD_REPLICATE;
	cmd.src_url = strdup(path);
	if (cmd.src_url == NULL)
		return (GFARM_ERR_NO_MEMORY);
	cmd.dst_url = NULL;
	cmd.src_host = src_host;
	cmd.dst_host = dst_host;
	cmd.src_port = src_port;
	cmd.dst_port = dst_port;
	cmd.src_size = src_size;
	cmd.check_disk_avail = check_disk_avail;
	cmd.cb_data = cb_data;
	gfarm_pfunc_cmd_add(pfunc_handle, &cmd);
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfarm_pfunc_remove_replica(
	gfarm_pfunc_t *pfunc_handle, const char *path,
	const char *host, int port, gfarm_off_t src_size, void *cb_data)
{
	gfarm_pfunc_cmd_t cmd;

	assert(host);
	cmd.command = PFUNC_CMD_REMOVE_REPLICA;
	cmd.src_url = strdup(path);
	if (cmd.src_url == NULL)
		return (GFARM_ERR_NO_MEMORY);
	cmd.dst_url = NULL;
	cmd.src_host = host;
	cmd.dst_host = NULL;
	cmd.src_port = port;
	cmd.dst_port = 0;
	cmd.src_size = src_size;
	cmd.cb_data = cb_data;
	gfarm_pfunc_cmd_add(pfunc_handle, &cmd);
	return (GFARM_ERR_NO_ERROR);
}

static const char no_host[] = "";

/* NOTE: src_host and dst_host is not free()ed (see pfunc_entry_free()) */
gfarm_error_t
gfarm_pfunc_copy(
	gfarm_pfunc_t *pfunc_handle,
	const char *src_url, const char *src_host, int src_port,
	gfarm_off_t src_size,
	const char *dst_url, const char *dst_host, int dst_port,
	void *cb_data, int is_move, int check_disk_avail)
{
	gfarm_pfunc_cmd_t cmd;

	if (is_move) { /* not supported yet */
		/* cmd.command = PFUNC_CMD_MOVE; */
		return (GFARM_ERR_OPERATION_NOT_SUPPORTED);
	} else
		cmd.command = PFUNC_CMD_COPY;
	cmd.src_url = strdup(src_url);
	cmd.dst_url = strdup(dst_url);
	if (cmd.src_url == NULL || cmd.dst_url == NULL) {
		free(cmd.src_url);
		free(cmd.dst_url);
		return (GFARM_ERR_NO_MEMORY);
	}
	cmd.src_host = src_host ? src_host : no_host;
	cmd.dst_host = dst_host ? dst_host : no_host;
	cmd.src_port = src_host ? src_port : -1;
	cmd.dst_port = dst_host ? dst_port : -1;
	cmd.src_size = src_size;
	cmd.check_disk_avail = check_disk_avail;
	cmd.cb_data = cb_data;
	gfarm_pfunc_cmd_add(pfunc_handle, &cmd);
	return (GFARM_ERR_NO_ERROR);
}
