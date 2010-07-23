/*
 * $Id$
 */

#define _POSIX_PII_SOCKET /* to use struct msghdr on Tru64 */
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/resource.h>
#include <netdb.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <syslog.h>
#include <stdarg.h>
#include <setjmp.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <pwd.h>
#include <grp.h>
#include <libgen.h>

#if defined(SCM_RIGHTS) && \
		(!defined(sun) || (!defined(__svr4__) && !defined(__SVR4)))
#define HAVE_MSG_CONTROL 1
#endif

#if !defined(WCOREDUMP) && defined(_AIX)
#define WCOREDUMP(status)	((status) & 0x80)
#endif

#ifndef SHUT_WR		/* some really old OS doesn't define this symbol. */
#define SHUT_WR	1
#endif

#include <openssl/evp.h>

#include <gfarm/gfarm_config.h>

#ifdef HAVE_SYS_LOADAVG_H
#include <sys/loadavg.h>	/* getloadavg() on Solaris */
#endif

#define GFLOG_USE_STDARG
#include <gfarm/gflog.h>
#include <gfarm/error.h>
#include <gfarm/gfarm_misc.h>
#include <gfarm/gfs.h>
#include <gfarm/host_info.h>

#include "gfutil.h"
#include "gfnetdb.h"
#include "hash.h"

#include "iobuffer.h"
#include "gfp_xdr.h"
#include "io_fd.h"
#include "param.h"
#include "sockopt.h"
#include "hostspec.h"
#include "host.h"
#include "conn_hash.h"
#include "auth.h"
#include "config.h"
#include "gfs_proto.h"
#include "gfs_client.h"
#include "gfm_proto.h"
#include "gfm_client.h"

#include "gfsd_subr.h"

#define COMPAT_OLD_GFS_PROTOCOL

#ifndef DEFAULT_PATH
#define DEFAULT_PATH "PATH=/usr/local/bin:/usr/bin:/bin:/usr/ucb:/usr/X11R6/bin:/usr/openwin/bin:/usr/pkg/bin"
#endif

#define GFARM_DEFAULT_PATH	DEFAULT_PATH ":" GFARM_DEFAULT_BINDIR

#ifndef PATH_BSHELL
#define PATH_BSHELL "/bin/sh"
#endif

#define LOCAL_SOCKDIR_MODE	0755
#define LOCAL_SOCKET_MODE	0777
#define PERMISSION_MASK		0777

/* need to be accessed as an executable (in future, e.g. after chmod) */
#define	DATA_FILE_MASK		0711
#define	DATA_DIR_MASK		0700

#ifdef SOMAXCONN
#define LISTEN_BACKLOG	SOMAXCONN
#else
#define LISTEN_BACKLOG	5
#endif

/* limit maximum open files per client, when system limit is very high */
#ifndef FILE_TABLE_LIMIT
#define FILE_TABLE_LIMIT	2048
#endif

#define HOST_HASHTAB_SIZE	3079	/* prime number */

#define fatal(msg_no, ...) \
	fatal_full(msg_no, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define fatal_errno(msg_no, ...) \
	fatal_errno_full(msg_no, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define accepting_fatal(msg_no, ...) \
	accepting_fatal_full(msg_no, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define accepting_fatal_errno(msg_no, ...) \
	accepting_fatal_errno_full(msg_no, __FILE__, __LINE__, __func__,\
				   __VA_ARGS__)

static const char READONLY_CONFIG_FILE[] = ".readonly";

const char *program_name = "gfsd";

int debug_mode = 0;
pid_t master_gfsd_pid;
pid_t back_channel_gfsd_pid;

int restrict_user = 0;
uid_t restricted_user = 0;

uid_t gfsd_uid = -1;
mode_t command_umask;

struct gfm_connection *gfm_server;
char *canonical_self_name;
char *username; /* gfarm global user name */

struct gfp_xdr *credential_exported = NULL;

long file_read_size;
#if 0 /* not yet in gfarm v2 */
long rate_limit;
#endif

static char *listen_addrname = NULL;

struct local_socket {
	int sock;
	char *dir, *name;
};

struct accepting_sockets {
	int local_socks_count, udp_socks_count;
	int tcp_sock, *udp_socks;
	struct local_socket *local_socks;
} accepting;

/* this routine should be called before the accepting server calls exit(). */
void
cleanup_accepting(int sighandler)
{
	int i;

	for (i = 0; i < accepting.local_socks_count; i++) {
		if (unlink(accepting.local_socks[i].name) == -1 && !sighandler)
			gflog_warning(GFARM_MSG_1002378,
			    "unlink(%s)", accepting.local_socks[i].name);
		if (rmdir(accepting.local_socks[i].dir) == -1 && !sighandler)
			gflog_warning(GFARM_MSG_1002379,
			    "rmdir(%s)", accepting.local_socks[i].dir);
	}
}

static void close_all_fd(void);

/* this routine should be called before calling exit(). */
static void
cleanup(int sighandler)
{
	static int cleanup_started = 0;
	pid_t pid = getpid();

	if (!cleanup_started) {
		cleanup_started = 1;

		if (pid != master_gfsd_pid && pid != back_channel_gfsd_pid &&
		    !sighandler)
			close_all_fd(); /* may recursivelly call cleanup() */
	}

	if (pid == master_gfsd_pid) {
		cleanup_accepting(sighandler);
		/* send terminate signal to a back channel process */
		if (kill(back_channel_gfsd_pid, SIGTERM) == -1 && !sighandler)
			gflog_warning_errno(GFARM_MSG_1002377,
			    "kill(%d)", back_channel_gfsd_pid);
	}

	if (credential_exported != NULL)
		gfp_xdr_delete_credential(credential_exported, sighandler);
	credential_exported = NULL;

	if (!sighandler) {
		/* It's not safe to do the following operation */
		gflog_notice(GFARM_MSG_1000451, "disconnected");
	}
}

static void
cleanup_handler(int signo)
{
	cleanup(1);
	_exit(2);
}

static void fatal_full(int, const char *, int, const char *,
		const char *, ...) GFLOG_PRINTF_ARG(5, 6);
static void
fatal_full(int msg_no, const char *file, int line_no, const char *func,
		const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	gflog_vmessage(msg_no, LOG_ERR, file, line_no, func, format, ap);
	va_end(ap);

	cleanup(0);
	if (getpid() == back_channel_gfsd_pid) {
		/*
		 * send terminate signal to the master process.
		 * this should be done at the end of fatal(),
		 * because both the master process and the back channel process
		 * try to kill each other.
		 */
		kill(master_gfsd_pid, SIGTERM);
	}
	exit(2);
}

static void fatal_errno_full(int, const char *, int, const char*,
		const char *, ...) GFLOG_PRINTF_ARG(5, 6);
static void
fatal_errno_full(int msg_no, const char *file, int line_no, const char *func,
		const char *format, ...)
{
	char buffer[2048];
	va_list ap;

	va_start(ap, format);
	vsnprintf(buffer, sizeof buffer, format, ap);
	va_end(ap);
	fatal_full(msg_no, file, line_no, func, "%s: %s",
			buffer, strerror(errno));
}

void
fatal_metadb_proto_full(int msg_no,
	const char *file, int line_no, const char *func,
	const char *diag, const char *proto, gfarm_error_t e)
{
	fatal_full(msg_no, file, line_no, func,
	    "gfmd protocol: %s error on %s: %s", proto, diag,
	    gfarm_error_string(e));
}

static void accepting_fatal_full(int, const char *, int, const char *,
		const char *, ...) GFLOG_PRINTF_ARG(5, 6);
static void
accepting_fatal_full(int msg_no, const char *file, int line_no,
		const char *func, const char *format, ...)
{
	va_list ap;

	cleanup_accepting(0);
	va_start(ap, format);
	gflog_vmessage(msg_no, LOG_ERR, file, line_no, func, format, ap);
	va_end(ap);
	exit(2);
}

static void accepting_fatal_errno_full(int, const char *, int, const char *,
		const char *, ...) GFLOG_PRINTF_ARG(5, 6);
static void
accepting_fatal_errno_full(int msg_no, const char *file, int line_no,
		const char *func, const char *format, ...)
{
	int save_errno = errno;
	char buffer[2048];

	va_list ap;

	va_start(ap, format);
	vsnprintf(buffer, sizeof buffer, format, ap);
	va_end(ap);
	accepting_fatal_full(msg_no, file, line_no, func, "%s: %s", buffer,
			strerror(save_errno));
}


static int
fd_send_message(int fd, void *buf, size_t size, int fdc, int *fdv)
{
	char *buffer = buf;
	int i, rv;
	struct iovec iov[1];
	struct msghdr msg;
#ifdef HAVE_MSG_CONTROL /* 4.3BSD Reno or later */
	struct {
		struct cmsghdr hdr;
		char data[CMSG_SPACE(sizeof(*fdv) * GFSD_MAX_PASSING_FD)
			  - sizeof(struct cmsghdr)];
	} cmsg;

	if (fdc > GFSD_MAX_PASSING_FD) {
		fatal(GFARM_MSG_1000453,
		    "gfsd: fd_send_message(): fd count %d > %d",
		    fdc, GFSD_MAX_PASSING_FD);
		return (EINVAL);
	}
#endif

	while (size > 0) {
		iov[0].iov_base = buffer;
		iov[0].iov_len = size;
		msg.msg_iov = iov;
		msg.msg_iovlen = 1;
		msg.msg_name = NULL;
		msg.msg_namelen = 0;
#ifndef HAVE_MSG_CONTROL
		if (fdc > 0) {
			msg.msg_accrights = (caddr_t)fdv;
			msg.msg_accrightslen = sizeof(*fdv) * fdc;
		} else {
			msg.msg_accrights = NULL;
			msg.msg_accrightslen = 0;
		}
#else /* 4.3BSD Reno or later */
		if (fdc > 0) {
			msg.msg_control = (caddr_t)&cmsg.hdr;
			msg.msg_controllen = CMSG_SPACE(sizeof(*fdv) * fdc);
			cmsg.hdr.cmsg_len = CMSG_LEN(sizeof(*fdv) * fdc);
			cmsg.hdr.cmsg_level = SOL_SOCKET;
			cmsg.hdr.cmsg_type = SCM_RIGHTS;
			for (i = 0; i < fdc; i++)
				((int *)CMSG_DATA(&cmsg.hdr))[i] = fdv[i];
		} else {
			msg.msg_control = NULL;
			msg.msg_controllen = 0;
		}
#endif
		rv = sendmsg(fd, &msg, 0);
		if (rv == -1) {
			if (errno == EINTR)
				continue;
			return (errno); /* failure */
		}
		fdc = 0; fdv = NULL;
		buffer += rv;
		size -= rv;
	}
	return (0); /* success */
}

void
gfs_server_get_request(struct gfp_xdr *client, const char *diag,
	const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;

	va_start(ap, format);
	e = gfp_xdr_vrecv_request_parameters(client, 0, NULL, format, &ap);
	va_end(ap);

	if (e != GFARM_ERR_NO_ERROR)
		fatal(GFARM_MSG_1000455, "%s get request: %s",
		    diag, gfarm_error_string(e));
}

void
gfs_server_put_reply_common(struct gfp_xdr *client, const char *diag,
	gfp_xdr_xid_t xid,
	gfarm_int32_t ecode, const char *format, va_list *app)
{
	gfarm_error_t e;

	if (debug_mode)
		gflog_debug(GFARM_MSG_1000458, "reply: %s: %d (%s)",
		    diag, (int)ecode, gfarm_error_string(ecode));

	e = gfp_xdr_vsend_result(client, ecode, format, app);
	if (e == GFARM_ERR_NO_ERROR)
		e = gfp_xdr_flush(client);
	if (e != GFARM_ERR_NO_ERROR)
		fatal(GFARM_MSG_1000459, "%s put reply: %s",
		    diag, gfarm_error_string(e));
}

void
gfs_server_put_reply_with_errno_common(struct gfp_xdr *client, const char *diag,
	gfp_xdr_xid_t xid,
	int eno, const char *format, va_list *app)
{
	gfarm_int32_t ecode = gfarm_errno_to_error(eno);

	if (ecode == GFARM_ERR_UNKNOWN)
		gflog_warning(GFARM_MSG_1000461, "%s: %s", diag, strerror(eno));
	gfs_server_put_reply_common(client, diag, xid, ecode, format, app);
}

void
gfs_server_put_reply(struct gfp_xdr *client, const char *diag,
	int ecode, char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	gfs_server_put_reply_common(client, diag, -1, ecode, format, &ap);
	va_end(ap);
}

void
gfs_server_put_reply_with_errno(struct gfp_xdr *client, const char *diag,
	int eno, char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	gfs_server_put_reply_with_errno_common(client, diag, -1, eno,
	    format, &ap);
	va_end(ap);
}

gfarm_error_t
gfs_async_server_get_request(struct gfp_xdr *client, size_t size,
	const char *diag, const char *format, ...)
{
	va_list ap;
	gfarm_error_t e;

	va_start(ap, format);
	e = gfp_xdr_vrecv_request_parameters(client, 0, &size, format, &ap);
	va_end(ap);
	if (e != GFARM_ERR_NO_ERROR)
		gflog_error(GFARM_MSG_1002380, "%s get request: %s",
		    diag, gfarm_error_string(e));
	return (e);
}

gfarm_error_t
gfs_async_server_put_reply_common(struct gfp_xdr *client, gfp_xdr_xid_t xid,
	const char *diag, gfarm_error_t ecode, char *format, va_list *app)
{
	gfarm_error_t e;

	if (debug_mode)
		gflog_debug(GFARM_MSG_1002381, "async_reply: %s: %d (%s)",
		    diag, (int)ecode, gfarm_error_string(ecode));

	e = gfp_xdr_vsend_async_result(client, xid, ecode, format, app);

	if (e == GFARM_ERR_NO_ERROR)
		e = gfp_xdr_flush(client);
	if (e != GFARM_ERR_NO_ERROR)
		gflog_error(GFARM_MSG_1002382, "%s put reply: %s",
		    diag, gfarm_error_string(e));
	return (e);
}

gfarm_error_t
gfs_async_server_put_reply(struct gfp_xdr *client, gfp_xdr_xid_t xid,
	const char *diag, gfarm_error_t ecode, char *format, ...)
{
	va_list ap;
	gfarm_error_t e;

	va_start(ap, format);
	e = gfs_async_server_put_reply_common(client, xid, diag, ecode,
	    format, &ap);
	va_end(ap);
	return (e);
}

gfarm_error_t
gfs_async_server_put_reply_with_errno(struct gfp_xdr *client,
	gfp_xdr_xid_t xid, const char *diag, int eno, char *format, ...)
{
	va_list ap;
	gfarm_int32_t ecode = gfarm_errno_to_error(eno);
	gfarm_error_t e;

	if (ecode == GFARM_ERR_UNKNOWN)
		gflog_warning(GFARM_MSG_1002383, "%s: %s", diag, strerror(eno));

	va_start(ap, format);
	e = gfs_async_server_put_reply_common(client, xid, diag, ecode,
	    format, &ap);
	va_end(ap);
	return (e);
}

gfarm_error_t
gfm_async_client_send_request(struct gfp_xdr *bc_conn,
	gfp_xdr_async_peer_t async, const char *diag,
	gfarm_int32_t (*result_callback)(void *, void *, size_t),
	void (*disconnect_callback)(void *, void *),
	void *closure,
	gfarm_int32_t command, const char *format, ...)
{
	gfarm_error_t e;
	va_list ap;

	va_start(ap, format);
	e = gfp_xdr_vsend_async_request(bc_conn, async,
	    result_callback, disconnect_callback, closure,
	    command, format, &ap);
	va_end(ap);
	if (e != GFARM_ERR_NO_ERROR)
		gflog_error(GFARM_MSG_1002164,
		    "gfm_async_client_send_request %s: %s",
		    diag, gfarm_error_string(e));
	return (e);
}

gfarm_error_t
gfm_async_client_recv_reply(struct gfp_xdr *bc_conn, const char *diag,
	size_t size, const char *format, ...)
{
	gfarm_error_t e;
	gfarm_int32_t errcode;
	va_list ap;

	va_start(ap, format);
	e = gfp_xdr_vrpc_result_sized(bc_conn, 0, &size,
	    &errcode, &format, &ap);
	va_end(ap);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1002165,
		    "gfm_async_client_recv_reply %s: %s",
		    diag, gfarm_error_string(e));
	} else if (size != 0) {
		gflog_error(GFARM_MSG_1002166,
		    "gfm_async_client_recv_reply %s: protocol residual %d",
		    diag, (int)size);
		if ((e = gfp_xdr_purge(bc_conn, 0, size))
		    != GFARM_ERR_NO_ERROR)
			gflog_warning(GFARM_MSG_1002167,
			    "gfm_async_client_recv_reply %s: skipping: %s",
			    diag, gfarm_error_string(e));
		e = GFARM_ERR_PROTOCOL;
	} else {
		e = errcode;
	}
	return (e);
}


void
gfs_server_process_set(struct gfp_xdr *client)
{
	gfarm_int32_t e;
	gfarm_pid_t pid;
	gfarm_int32_t keytype;
	size_t keylen;
	char sharedkey[GFM_PROTO_PROCESS_KEY_LEN_SHAREDSECRET];

	gfs_server_get_request(client, "process_set",
	    "ibl", &keytype, sizeof(sharedkey), &keylen, sharedkey, &pid);

	e = gfm_client_process_set(gfm_server,
	    keytype, sharedkey, keylen, pid);

	gfs_server_put_reply(client, "process_set", e, "");
}

int file_table_size = 0;

struct file_entry {
	off_t size;
	time_t mtime, atime; /* XXX FIXME tv_nsec */
	gfarm_ino_t ino;
	int flags, local_fd;
#define FILE_FLAG_LOCAL		0x01
#define FILE_FLAG_CREATED	0x02
#define FILE_FLAG_WRITABLE	0x04
#define FILE_FLAG_WRITTEN	0x08
#define FILE_FLAG_READ		0x10
} *file_table;

static void
file_entry_set_atime(struct file_entry *fe,
	gfarm_time_t sec, gfarm_int32_t nsec)
{
	fe->flags |= FILE_FLAG_READ;
	fe->atime = sec;
	/* XXX FIXME st_atimespec.tv_nsec */
}

static void
file_entry_set_mtime(struct file_entry *fe,
	gfarm_time_t sec, gfarm_int32_t nsec)
{
	fe->flags |= FILE_FLAG_WRITTEN;
	fe->mtime = sec;
	/* XXX FIXME st_mtimespec.tv_nsec */
}

static void
file_entry_set_size(struct file_entry *fe, gfarm_off_t size)
{
	fe->flags |= FILE_FLAG_WRITTEN;
	fe->size = size;
}

void
file_table_init(int table_size)
{
	int i;

	GFARM_MALLOC_ARRAY(file_table, table_size);
	if (file_table == NULL) {
		errno = ENOMEM; fatal_errno(GFARM_MSG_1000462, "file table");
	}
	for (i = 0; i < table_size; i++)
		file_table[i].local_fd = -1;
	file_table_size = table_size;
}

int
file_table_is_available(gfarm_int32_t net_fd)
{
	if (0 <= net_fd && net_fd < file_table_size)
		return (file_table[net_fd].local_fd == -1);
	else
		return (0);
}

void
file_table_add(gfarm_int32_t net_fd, int local_fd, int flags, gfarm_ino_t ino)
{
	struct file_entry *fe;
	struct stat st;

	if (fstat(local_fd, &st) < 0)
		fatal_errno(GFARM_MSG_1000463, "file_table_add: fstat failed");
	fe = &file_table[net_fd];
	fe->local_fd = local_fd;
	fe->flags = 0;
	fe->ino = ino;
	if (flags & O_CREAT)
		fe->flags |= FILE_FLAG_CREATED;
	if (flags & O_TRUNC)
		fe->flags |= FILE_FLAG_WRITTEN;
	if ((flags & O_ACCMODE) != O_RDONLY)
		fe->flags |= FILE_FLAG_WRITABLE;
	fe->atime = st.st_atime;
	/* XXX FIXME st_atimespec.tv_nsec */
	fe->mtime = st.st_mtime;
	/* XXX FIXME st_mtimespec.tv_nsec */
	fe->size = st.st_size;
}

gfarm_error_t
file_table_close(gfarm_int32_t net_fd)
{
	gfarm_error_t e;

	if (net_fd < 0 || net_fd >= file_table_size ||
	    file_table[net_fd].local_fd == -1) {
		gflog_debug(GFARM_MSG_1002168,
			"bad file descriptor");
		return (GFARM_ERR_BAD_FILE_DESCRIPTOR);
	}
	if (close(file_table[net_fd].local_fd) < 0)
		e = gfarm_errno_to_error(errno);
	else
		e = GFARM_ERR_NO_ERROR;
	file_table[net_fd].local_fd = -1;
	return (e);
}

int
file_table_get(gfarm_int32_t net_fd)
{
	if (0 <= net_fd && net_fd < file_table_size)
		return (file_table[net_fd].local_fd);
	else
		return (-1);
}

struct file_entry *
file_table_entry(gfarm_int32_t net_fd)
{
	if (0 <= net_fd && net_fd < file_table_size)
		return (&file_table[net_fd]);
	else
		return (NULL);
}

static void
file_table_set_flag(gfarm_int32_t net_fd, int flags)
{
	struct file_entry *fe = file_table_entry(net_fd);

	if (fe != NULL)
		fe->flags |= flags;
}

static void
file_table_set_read(gfarm_int32_t net_fd)
{
	struct file_entry *fe = file_table_entry(net_fd);
	struct timeval now;

	if (fe == NULL)
		return;

	gettimeofday(&now, NULL);
	file_entry_set_atime(fe, now.tv_sec, 0);
}

static void
file_table_set_written(gfarm_int32_t net_fd)
{
	struct file_entry *fe = file_table_entry(net_fd);
	struct timeval now;

	if (fe == NULL)
		return;

	gettimeofday(&now, NULL);
	file_entry_set_mtime(fe, now.tv_sec, 0);
}

static void
file_table_for_each(void (*callback)(void *, gfarm_int32_t), void *closure)
{
	gfarm_int32_t net_fd;

	if (file_table == NULL)
		return;

	for (net_fd = 0; net_fd < file_table_size; net_fd++) {
		if (file_table[net_fd].local_fd != -1)
			(*callback)(closure, net_fd);
	}
}

int
gfs_open_flags_localize(int open_flags)
{
	int local_flags;

	switch (open_flags & GFARM_FILE_ACCMODE) {
	case GFARM_FILE_RDONLY:	local_flags = O_RDONLY; break;
	case GFARM_FILE_WRONLY:	local_flags = O_WRONLY; break;
	case GFARM_FILE_RDWR:	local_flags = O_RDWR; break;
	default: return (-1);
	}

#if 0
	if ((open_flags & GFARM_FILE_CREATE) != 0)
		local_flags |= O_CREAT;
#endif
	if ((open_flags & GFARM_FILE_TRUNC) != 0)
		local_flags |= O_TRUNC;
#if 0 /* not yet in gfarm v2 */
	if ((open_flags & GFARM_FILE_APPEND) != 0)
		local_flags |= O_APPEND;
	if ((open_flags & GFARM_FILE_EXCLUSIVE) != 0)
		local_flags |= O_EXCL;
#endif /* not yet in gfarm v2 */
	return (local_flags);
}

/*
 * if inum == 0x0011223344556677, and gen == 0X8899AABBCCDDEEFF, then
 * local_path = gfarm_spool_root + "data/00112233/44/55/66/778899AABBCCDDEEFF".
 *
 * If the metadata server uses inum > 0x700000000000,
 * We need a modern filesystem which satisfies follows:
 * - can create more than 32765 (= 32767 - 1 (for current) - 1 (for parent))
 *   subdirectories.
 *   32767 comes from platforms which st_nlink is 16bit signed integer.
 *   ext2/ext3fs can create only 32000 subdirectories at maximum.
 * - uses B-tree or similar mechanism to search directory entries
 *   to avoid overhead of linear search.
 */

void
local_path(gfarm_ino_t inum, gfarm_uint64_t gen, const char *diag,
	char **pathp)
{
	char *p;
	static int length = 0;
	static char template[] = "/data/00112233/44/55/66/778899AABBCCDDEEFF";
#define DIRLEVEL 5 /* there are 5 levels of directories in template[] */

	if (length == 0)
		length = strlen(gfarm_spool_root) + sizeof(template);

	GFARM_MALLOC_ARRAY(p, length);
	if (p == NULL) {
		fatal(GFARM_MSG_1000464, "%s: no memory for %d bytes",
			diag, length);
	}
	snprintf(p, length, "%s/data/%08X/%02X/%02X/%02X/%02X%08X%08X",
	    gfarm_spool_root,
	    (unsigned int)((inum >> 32) & 0xffffffff),
	    (unsigned int)((inum >> 24) & 0xff),
	    (unsigned int)((inum >> 16) & 0xff),
	    (unsigned int)((inum >>  8) & 0xff),
	    (unsigned int)( inum        & 0xff),
	    (unsigned int)((gen  >> 32) & 0xffffffff),
	    (unsigned int)( gen         & 0xffffffff));
	*pathp = p;
}

int
open_data(char *path, int flags)
{
	int i, j, tail, slashpos[DIRLEVEL];
	int fd = open(path, flags, DATA_FILE_MASK);
	struct stat st;

	if (fd >= 0)
		return (fd);
	if ((flags & O_CREAT) == 0 || errno != ENOENT)
		return (-1); /* with errno */

	/* errno == ENOENT, so, maybe we don't have an ancestor directory */
	tail = strlen(path);
	for (i = 0; i < DIRLEVEL; i++) {
		for (--tail; tail > 0 && path[tail] != '/'; --tail)
			;
		if (tail <= 0) {
			gflog_warning(GFARM_MSG_1000465,
			    "something wrong in local_path(): %s\n", path);
			errno = ENOENT;
			return (-1);
		}
		assert(path[tail] == '/');
		slashpos[i] = tail;
		path[tail] = '\0';

		if (stat(path, &st) == 0) {
			/* maybe race? */
		} else if (errno != ENOENT) {
			gflog_warning(GFARM_MSG_1000466,
			    "stat(`%s') failed: %s", path, strerror(errno));
			errno = ENOENT;
			return (-1);
		} else if (mkdir(path, DATA_DIR_MASK) < 0) {
			if (errno == ENOENT)
				continue;
			if (errno == EEXIST) {
				/* maybe race */
			} else {
				gflog_warning(GFARM_MSG_1000467,
				    "mkdir(`%s') failed: %s", path,
				    strerror(errno));
				errno = ENOENT;
				return (-1);
			}
		}
		/* Now, we have the ancestor directory */
		for (j = i;; --j) {
			path[slashpos[j]] = '/';
			if (j <= 0)
				break;
			if (mkdir(path, DATA_DIR_MASK) < 0) {
				if (errno == EEXIST) /* maybe race */
					continue;
				gflog_warning(GFARM_MSG_1000468,
				    "unexpected mkdir(`%s') failure: %s",
				    path, strerror(errno));
				errno = ENOENT;
				return (-1);
			}
		}
		return (open(path, flags, DATA_FILE_MASK)); /* with errno */
	}
	gflog_warning(GFARM_MSG_1000469,
	    "gfsd spool_root doesn't exist?: %s\n", path);
	errno = ENOENT;
	return (-1);
}

static void
gfm_client_compound_put_fd_request(gfarm_int32_t net_fd, const char *diag)
{
	gfarm_error_t e;

	if ((e = gfm_client_compound_begin_request(gfm_server))
	    != GFARM_ERR_NO_ERROR)
		fatal_metadb_proto(GFARM_MSG_1002291,
		    "compound_begin request", diag, e);
	else if ((e = gfm_client_put_fd_request(gfm_server, net_fd))
	    != GFARM_ERR_NO_ERROR)
		fatal_metadb_proto(GFARM_MSG_1002292,
		    "put_fd request", diag, e);
}

static void
gfm_client_compound_put_fd_result(const char *diag)
{
	gfarm_error_t e;

	if ((e = gfm_client_compound_end_request(gfm_server))
	    != GFARM_ERR_NO_ERROR)
		fatal_metadb_proto(GFARM_MSG_1002293,
		    "compound_end request", diag, e);

	else if ((e = gfm_client_compound_begin_result(gfm_server))
	    != GFARM_ERR_NO_ERROR)
		fatal_metadb_proto(GFARM_MSG_1002294,
		    "compound_begin result", diag, e);
	else if ((e = gfm_client_put_fd_result(gfm_server))
	    != GFARM_ERR_NO_ERROR)
		fatal_metadb_proto(GFARM_MSG_1002295,
		    "put_fd result", diag, e);
}

static int
gfm_client_compound_end(const char *diag)
{
	gfarm_error_t e;

	if ((e = gfm_client_compound_end_result(gfm_server))
	    != GFARM_ERR_NO_ERROR)
		fatal_metadb_proto(GFARM_MSG_1002296,
		    "compound_end result", diag, e);
	return (1);
}

static gfarm_error_t
gfs_server_reopen(char *diag, gfarm_int32_t net_fd, char **pathp, int *flagsp,
	gfarm_ino_t *inop, gfarm_uint64_t *genp)
{
	gfarm_error_t e;
	gfarm_ino_t ino;
	gfarm_uint64_t gen;
	gfarm_int32_t mode, net_flags, to_create;
	char *path;
	int local_flags;

	gfm_client_compound_put_fd_request(net_fd, diag);
	if ((e = gfm_client_reopen_request(gfm_server))
	    != GFARM_ERR_NO_ERROR)
		fatal_metadb_proto(GFARM_MSG_1000472,
		    "reopen request", diag, e);
	gfm_client_compound_put_fd_result(diag);
	if ((e = gfm_client_reopen_result(gfm_server,
	    &ino, &gen, &mode, &net_flags, &to_create))
	    != GFARM_ERR_NO_ERROR) {
		if (debug_mode)
			gflog_info(GFARM_MSG_1000476,
			    "reopen(%s) result: %s", diag,
			    gfarm_error_string(e));
	} else if (!gfm_client_compound_end(diag))
		/*NOTREACHED*/;
	else if (!GFARM_S_ISREG(mode)) {
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		gflog_debug(GFARM_MSG_1002169,
			"mode:operation is not permitted");
	} else if ((local_flags = gfs_open_flags_localize(net_flags)) == -1) {
		e = GFARM_ERR_OPERATION_NOT_PERMITTED;
		gflog_debug(GFARM_MSG_1002170,
			"local_flags:operation is not permitted");
	} else {
		local_path(ino, gen, diag, &path);
		if (to_create)
			local_flags |= O_CREAT;
		*pathp = path;
		*flagsp = local_flags;
		*inop = ino;
		*genp = gen;
	}
	return (e);
}

gfarm_error_t
replica_remove(gfarm_ino_t ino, gfarm_uint64_t gen)
{
	gfarm_error_t e;
	static const char diag[] = "GFM_PROTO_REPLICA_REMOVE";

	if ((e = gfm_client_replica_remove_request(gfm_server, ino, gen))
	     != GFARM_ERR_NO_ERROR)
		fatal_metadb_proto(GFARM_MSG_1000478,
		    "replica_remove request", diag, e);
	else if ((e = gfm_client_replica_remove_result(gfm_server))
	     != GFARM_ERR_NO_ERROR && e != GFARM_ERR_NO_SUCH_OBJECT)
		if (debug_mode)
			gflog_info(GFARM_MSG_1000479,
			    "replica_remove(%s) result: %s", diag,
			    gfarm_error_string(e));
	return (e);
}

gfarm_error_t
gfs_server_open_common(struct gfp_xdr *client, char *diag,
	gfarm_int32_t *net_fdp, int *local_fdp)
{
	gfarm_error_t e;
	char *path = NULL;
	gfarm_ino_t ino = 0;
	gfarm_uint64_t gen = 0;
	int net_fd, local_fd, save_errno, local_flags = 0;

	gfs_server_get_request(client, diag, "i", &net_fd);

	if (!file_table_is_available(net_fd)) {
		e = GFARM_ERR_BAD_FILE_DESCRIPTOR;
		gflog_debug(GFARM_MSG_1002171,
			"bad file descriptor");
	} else {
		for (;;) {
			if ((e = gfs_server_reopen(diag, net_fd,
			    &path, &local_flags, &ino, &gen)) !=
			    GFARM_ERR_NO_ERROR) {
				gflog_debug(GFARM_MSG_1002172,
					"gfs_server_reopen() failed: %s",
					gfarm_error_string(e));
				break;
			}
			local_fd = open_data(path, local_flags);
			save_errno = errno;
			free(path);
			if (local_fd >= 0) {
				file_table_add(net_fd, local_fd, local_flags,
				    ino);
				*net_fdp = net_fd;
				*local_fdp = local_fd;
				break;
			}

			gfm_client_compound_put_fd_request(net_fd, diag);
			if ((e = gfm_client_close_request(gfm_server)) !=
			    GFARM_ERR_NO_ERROR)
				fatal(GFARM_MSG_1002297,
				    "%s: close(%d) request: %s",
				    diag, net_fd, gfarm_error_string(e));
			gfm_client_compound_put_fd_result(diag);
			if ((e = gfm_client_close_result(gfm_server)) !=
			    GFARM_ERR_NO_ERROR)
				gflog_info(GFARM_MSG_1002298,
				    "%s: close(%d): %s",
				    diag, net_fd, gfarm_error_string(e));
			else
				gfm_client_compound_end(diag);

			if (save_errno == ENOENT) {
				e = replica_remove(ino, gen);
				if (e == GFARM_ERR_NO_SUCH_OBJECT) {
					gflog_debug(GFARM_MSG_1002299,
					    "possible race between "
					    "rename & reopen: "
					    "ino %lld, gen %lld",
					    (long long)ino, (long long)gen);
					continue;
				}
				if (e == GFARM_ERR_NO_ERROR)
					gflog_info(GFARM_MSG_1000480,
					    "invalid metadata deleted: "
					    "ino %lld, gen %lld",
					    (long long)ino, (long long)gen);
				else
					gflog_warning(GFARM_MSG_1000481,
					    "fails to delete invalid metadata"
					    ": ino %lld, gen %lld: %s",
					    (long long)ino, (long long)gen,
					    gfarm_error_string(e));
			}
			e = gfarm_errno_to_error(save_errno);
			break;
		}
	}

	gfs_server_put_reply(client, diag, e, "");
	return (e);
}

void
gfs_server_open(struct gfp_xdr *client)
{
	gfarm_int32_t net_fd;
	int local_fd;

	gfs_server_open_common(client, "open", &net_fd, &local_fd);
}

void
gfs_server_open_local(struct gfp_xdr *client)
{
	gfarm_error_t e;
	gfarm_int32_t net_fd;
	int local_fd, rv;
	gfarm_int8_t dummy = 0; /* needs at least 1 byte */

	if (gfs_server_open_common(client, "open_local", &net_fd, &local_fd) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002173,
			"gfs_server_open_common() failed");
		return;
	}

	/* need to flush iobuffer before sending data w/o iobuffer */
	e = gfp_xdr_flush(client);
	if (e != GFARM_ERR_NO_ERROR)
		gflog_warning(GFARM_MSG_1000482, "open_local: flush: %s",
		    gfarm_error_string(e));

	/* layering violation, but... */
	rv = fd_send_message(gfp_xdr_fd(client),
	    &dummy, sizeof(dummy), 1, &local_fd);
	if (rv != 0)
		gflog_warning(GFARM_MSG_1000483,
		    "open_local: send_message: %s", strerror(rv));

	file_table_set_flag(net_fd, FILE_FLAG_LOCAL);
}

gfarm_error_t
close_request(struct file_entry *fe)
{
	if (fe->flags & FILE_FLAG_WRITTEN) {
		return (gfm_client_close_write_v2_4_request(gfm_server,
		    fe->size,
		    (gfarm_int64_t)fe->atime, (gfarm_int32_t)0,
		    (gfarm_int64_t)fe->mtime, (gfarm_int32_t)0));
		/* XXX FIXME st_atimespec.tv_nsec */
		/* XXX FIXME st_mtimespec.tv_nsec */
	} else if (fe->flags & FILE_FLAG_READ) {
		return (gfm_client_close_read_request(gfm_server,
		    (gfarm_int64_t)fe->atime, (gfarm_int32_t)0));
		/* XXX FIXME st_atimespec.tv_nsec */
	} else {
		return (gfm_client_close_request(gfm_server));
	}
}

gfarm_error_t
close_result(struct file_entry *fe, gfarm_int32_t *gen_update_result_p)
{
	if (fe->flags & FILE_FLAG_WRITTEN) {
		gfarm_error_t e;
		gfarm_int32_t flags;
		gfarm_int64_t old_gen, new_gen;
		char *old, *new;

		e = gfm_client_close_write_v2_4_result(gfm_server,
		    &flags, &old_gen, &new_gen);
		if (e == GFARM_ERR_NO_ERROR &&
		    (flags & GFM_PROTO_CLOSE_WRITE_GENERATION_UPDATE_NEEDED)) {
			local_path(fe->ino, old_gen, "close_write: old", &old);
			local_path(fe->ino, new_gen, "close_write: new", &new);
			*gen_update_result_p =
			    rename(old, new) == -1 ? errno : 0;
			if (*gen_update_result_p != 0) {
				gflog_error(GFARM_MSG_1002300,
				    "close_write: new generation: "
				    "%llu -> %llu: %s",
				    (unsigned long long)old_gen,
				    (unsigned long long)new_gen,
				    strerror(*gen_update_result_p));
			}
			free(old);
			free(new);
		} else
			*gen_update_result_p = -1;
		return (e);
	} else if (fe->flags & FILE_FLAG_READ) {
		*gen_update_result_p = -1;
		return (gfm_client_close_read_result(gfm_server));
	} else {
		*gen_update_result_p = -1;
		return (gfm_client_close_result(gfm_server));
	}
}

gfarm_error_t
close_fd(gfarm_int32_t fd, const char *diag)
{
	gfarm_error_t e, e2;
	int stat_is_done = 0;
	struct file_entry *fe;
	struct stat st;
	gfarm_int32_t gen_update_result = -1;

	if ((fe = file_table_entry(fd)) == NULL) {
		e = GFARM_ERR_BAD_FILE_DESCRIPTOR;
		gflog_debug(GFARM_MSG_1002174,
			"bad file descriptor");
	} else {
		if ((fe->flags & FILE_FLAG_LOCAL) == 0) { /* remote? */
			;
		} else if (fstat(fe->local_fd, &st) == -1) {
			gflog_warning(GFARM_MSG_1000484,
			    "fd %d: stat failed at close: %s",
			    fd, strerror(errno));
		} else {
			stat_is_done = 1;
			/* XXX FIXME st_atimespec.tv_nsec */
			if (st.st_atime != fe->atime)
				file_entry_set_atime(fe, st.st_atime, 0);
			/* another process might write this file */
			if ((fe->flags & FILE_FLAG_WRITABLE) != 0) {
				/* XXX FIXME st_mtimespec.tv_nsec */
				if (st.st_mtime != fe->mtime)
					file_entry_set_mtime(fe,
					    st.st_mtime, 0);
				if (st.st_size != fe->size)
					file_entry_set_size(fe, st.st_size);
				/* XXX FIXME this may be caused by others */
			}
		}
		if ((fe->flags & FILE_FLAG_WRITTEN) != 0 && !stat_is_done) {
			if (fstat(fe->local_fd, &st) == -1)
				gflog_warning(GFARM_MSG_1000485,
				    "fd %d: stat failed at close: %s",
				    fd, strerror(errno));
			else
				fe->size = st.st_size;
		}

		gfm_client_compound_put_fd_request(fd, diag);
		if ((e = close_request(fe)) != GFARM_ERR_NO_ERROR)
			fatal_metadb_proto(GFARM_MSG_1000488,
			    "close request", diag, e);
		gfm_client_compound_put_fd_result(diag);
		if ((e = close_result(fe, &gen_update_result))
		    != GFARM_ERR_NO_ERROR) {
			if (debug_mode)
				gflog_info(GFARM_MSG_1000492,
				    "close(%s) result: %s", diag,
				    gfarm_error_string(e));
		} else
			gfm_client_compound_end(diag);

		if (gen_update_result != -1) {
			gfm_client_compound_put_fd_request(fd, diag);
			if ((e2 = gfm_client_generation_updated_request(
			    gfm_server, gen_update_result))
			    != GFARM_ERR_NO_ERROR)
				fatal_metadb_proto(GFARM_MSG_1002301,
				    "generation_updated request",
				    diag, e2);
			gfm_client_compound_put_fd_result(diag);
			if ((e2 = gfm_client_generation_updated_result(
			    gfm_server)) != GFARM_ERR_NO_ERROR)
				gflog_error(GFARM_MSG_1002302,
				    "generation_updated result: %s", 
				    gfarm_error_string(e2));
			else
				gfm_client_compound_end(diag);
			if (e == GFARM_ERR_NO_ERROR)
				e = e2;
		}

		e2 = file_table_close(fd);
		if (e == GFARM_ERR_NO_ERROR)
			e = e2;
	}
	return (e);
}

static void
close_fd_adapter(void *closure, gfarm_int32_t fd)
{
	close_fd(fd, closure);
}

static void
close_all_fd(void)
{
	file_table_for_each(close_fd_adapter, "closing all descriptor");
}

void
gfs_server_close(struct gfp_xdr *client)
{
	gfarm_error_t e;
	gfarm_int32_t fd;
	static const char diag[] = "GFS_PROTO_CLOSE";

	gfs_server_get_request(client, diag, "i", &fd);
	e = close_fd(fd, diag);
	gfs_server_put_reply(client, diag, e, "");
}

void
gfs_server_pread(struct gfp_xdr *client)
{
	gfarm_int32_t fd, size;
	gfarm_int64_t offset;
	ssize_t rv;
	int save_errno = 0;
	char buffer[GFS_PROTO_MAX_IOSIZE];

	gfs_server_get_request(client, "pread", "iil", &fd, &size, &offset);

	/* We truncatef i/o size bigger than GFS_PROTO_MAX_IOSIZE. */
	if (size > GFS_PROTO_MAX_IOSIZE)
		size = GFS_PROTO_MAX_IOSIZE;
#if 0 /* XXX FIXME: pwrite(2) on NetBSD-3.0_BETA is broken */
	if ((rv = pread(file_table_get(fd), buffer, size, offset)) == -1)
#else
	rv = 0;
	if (lseek(file_table_get(fd), offset, SEEK_SET) == -1)
		save_errno = errno;
	else if ((rv = read(file_table_get(fd), buffer, size)) == -1)
#endif
		save_errno = errno;
	else
		file_table_set_read(fd);

	gfs_server_put_reply_with_errno(client, "pread", save_errno,
	    "b", rv, buffer);
}

void
gfs_server_pwrite(struct gfp_xdr *client)
{
	gfarm_int32_t fd;
	size_t size;
	gfarm_int64_t offset;
	ssize_t rv;
	int save_errno = 0;
	char buffer[GFS_PROTO_MAX_IOSIZE];

	gfs_server_get_request(client, "pwrite", "ibl",
	    &fd, sizeof(buffer), &size, buffer, &offset);

	/*
	 * We truncate i/o size bigger than GFS_PROTO_MAX_IOSIZE.
	 * This is inefficient because passed extra data are just
	 * abandoned. So client should avoid such situation.
	 */
	if (size > GFS_PROTO_MAX_IOSIZE)
		size = GFS_PROTO_MAX_IOSIZE;
#if 0 /* XXX FIXME: pwrite(2) on NetBSD-3.0_BETA is broken */
	if ((rv = pwrite(file_table_get(fd), buffer, size, offset)) == -1)
#else
	rv = 0;
	if (lseek(file_table_get(fd), offset, SEEK_SET) == -1)
		save_errno = errno;
	else if ((rv = write(file_table_get(fd), buffer, size)) == -1)
#endif
		save_errno = errno;
	else
		file_table_set_written(fd);

	gfs_server_put_reply_with_errno(client, "pwrite", save_errno,
	    "i", (gfarm_int32_t)rv);
}

void
gfs_server_ftruncate(struct gfp_xdr *client)
{
	int fd;
	gfarm_int64_t length;
	int save_errno = 0;

	gfs_server_get_request(client, "ftruncate", "il", &fd, &length);

	if (ftruncate(file_table_get(fd), (off_t)length) == -1)
		save_errno = errno;
	else
		file_table_set_written(fd);

	gfs_server_put_reply_with_errno(client, "ftruncate", save_errno, "");
}

void
gfs_server_fsync(struct gfp_xdr *client)
{
	int fd;
	int operation;
	int save_errno = 0;
	char *msg = "fsync";

	gfs_server_get_request(client, msg, "ii", &fd, &operation);

	switch (operation) {
	case GFS_PROTO_FSYNC_WITHOUT_METADATA:      
#ifdef HAVE_FDATASYNC
		if (fdatasync(file_table_get(fd)) == -1)
			save_errno = errno;
		break;
#else
		/*FALLTHROUGH*/
#endif
	case GFS_PROTO_FSYNC_WITH_METADATA:
		if (fsync(file_table_get(fd)) == -1)
			save_errno = errno;
		break;
	default:
		save_errno = EINVAL;
		break;
	}

	gfs_server_put_reply_with_errno(client, "fsync", save_errno, "");
}

void
gfs_server_fstat(struct gfp_xdr *client)
{
	struct stat st;
	gfarm_int32_t fd;
	gfarm_off_t size = 0;
	gfarm_int64_t atime_sec = 0, mtime_sec = 0;
	gfarm_int32_t atime_nsec = 0, mtime_nsec = 0;
	int save_errno = 0;

	gfs_server_get_request(client, "fstat", "i", &fd);

	if (fstat(file_table_get(fd), &st) == -1)
		save_errno = errno;
	else {
		size = st.st_size;
		atime_sec = st.st_atime;
		/* XXX FIXME st_atimespec.tv_nsec */
		mtime_sec = st.st_mtime;
		/* XXX FIXME st_mtimespec.tv_nsec */
	}

	gfs_server_put_reply_with_errno(client, "fstat", save_errno,
	    "llili", size, atime_sec, atime_nsec, mtime_sec, mtime_nsec);
}

void
gfs_server_cksum_set(struct gfp_xdr *client)
{
	gfarm_error_t e;
	int fd;
	gfarm_int32_t cksum_len;
	char *cksum_type;
	char cksum[GFM_PROTO_CKSUM_MAXLEN];
	struct file_entry *fe;
	int was_written;
	time_t mtime;
	struct stat st;
	static const char diag[] = "GFS_PROTO_CKSUM_SET";

	gfs_server_get_request(client, diag, "isb", &fd,
	    &cksum_type, sizeof(cksum), &cksum_len, cksum);

	if ((fe = file_table_entry(fd)) == NULL) {
		e = GFARM_ERR_BAD_FILE_DESCRIPTOR;
		gflog_debug(GFARM_MSG_1002175,
			"bad file descriptor");
	} else {
		/* NOTE: local client could use remote operation as well */
		was_written = (fe->flags & FILE_FLAG_WRITTEN) != 0;
		mtime = fe->mtime;
		if ((fe->flags & FILE_FLAG_LOCAL) == 0) { /* remote? */
			;
		} else if (fstat(fe->local_fd, &st) == -1) {
			gflog_warning(GFARM_MSG_1000494,
			    "fd %d: stat failed at cksum_set: %s",
			    fd, strerror(errno));
		} else {
			if (st.st_mtime != fe->mtime) {
				mtime = st.st_mtime;
				was_written = 1;
			}
			/* XXX FIXME st_mtimespec.tv_nsec */
		}

		gfm_client_compound_put_fd_request(fd, diag);
		if ((e = gfm_client_cksum_set_request(gfm_server,
		    cksum_type, cksum_len, cksum,
		    was_written, (gfarm_int64_t)mtime, (gfarm_int32_t)0)) !=
		    GFARM_ERR_NO_ERROR)
			fatal_metadb_proto(GFARM_MSG_1000497,
			    "cksum_set request", diag, e);
		gfm_client_compound_put_fd_result(diag);
		if ((e = gfm_client_cksum_set_result(gfm_server)) !=
		    GFARM_ERR_NO_ERROR) {
			if (debug_mode)
				gflog_info(GFARM_MSG_1000501,
				    "cksum_set(%s) result: %s", diag,
				    gfarm_error_string(e));
		} else
			gfm_client_compound_end(diag);
	}

	gfs_server_put_reply(client, diag, e, "");
}

static int
is_readonly_mode(void)
{
	struct stat st;
	int length;
	static char *p = NULL;
	static const char diag[] = "is_readonly_mode";

	if (p == NULL) {
		length = strlen(gfarm_spool_root) + 1 +
			sizeof(READONLY_CONFIG_FILE);
		GFARM_MALLOC_ARRAY(p, length);
		if (p == NULL)
			fatal(GFARM_MSG_1000503, "%s: no memory for %d bytes",
			    diag, length);
		snprintf(p, length, "%s/%s", gfarm_spool_root,
			 READONLY_CONFIG_FILE);
	}		
	return (stat(p, &st) == 0);
}

void
gfs_server_statfs(struct gfp_xdr *client)
{
	char *dir;
	int save_errno = 0;
	gfarm_int32_t bsize;
	gfarm_off_t blocks, bfree, bavail, files, ffree, favail;

	/*
	 * do not use dir since there is no way to know gfarm_spool_root.
	 * this code is kept for backward compatibility reason.
	 */
	gfs_server_get_request(client, "statfs", "s", &dir);

	save_errno = gfsd_statfs(gfarm_spool_root, &bsize,
	    &blocks, &bfree, &bavail,
	    &files, &ffree, &favail);
	free(dir);

	if (save_errno == 0 && is_readonly_mode()) {
		/* pretend to be disk full, to make this gfsd read-only */
		bavail -= bfree;
		bfree = 0;
	}

	gfs_server_put_reply_with_errno(client, "statfs", save_errno,
	    "illllll", bsize, blocks, bfree, bavail, files, ffree, favail);
}

static gfarm_error_t
replica_adding(gfarm_int32_t net_fd, char *src_host,
	gfarm_ino_t *inop, gfarm_uint64_t *genp,
	gfarm_int64_t *mtime_secp, gfarm_int32_t *mtime_nsecp,
	const char *request)
{
	gfarm_error_t e;
	gfarm_ino_t ino;
	gfarm_uint64_t gen;
	gfarm_int64_t mtime_sec;
	gfarm_int32_t mtime_nsec;
	static const char diag[] = "GFM_PROTO_REPLICA_ADDING";

	gfm_client_compound_put_fd_request(net_fd, diag);
	if ((e = gfm_client_replica_adding_request(gfm_server, src_host))
	    != GFARM_ERR_NO_ERROR)
		fatal_metadb_proto(GFARM_MSG_1000506,
		    request, diag, e);
	gfm_client_compound_put_fd_result(diag);
	if ((e = gfm_client_replica_adding_result(gfm_server,
	    &ino, &gen, &mtime_sec, &mtime_nsec))
	    != GFARM_ERR_NO_ERROR) {
		if (debug_mode)
			gflog_info(GFARM_MSG_1000510,
			    "%s result error on %s: %s", diag, request,
			    gfarm_error_string(e));
	} else if (!gfm_client_compound_end(diag))
		/*NOTREACHED*/;
	else {
		*inop = ino;
		*genp = gen;
		*mtime_secp = mtime_sec;
		*mtime_nsecp = mtime_nsec;
	}
	return (e);
}

static gfarm_error_t
replica_added(gfarm_int32_t net_fd,
    gfarm_int32_t flags, gfarm_int64_t mtime_sec, gfarm_int32_t mtime_nsec,
    gfarm_off_t size, const char *request)
{
	gfarm_error_t e;
	static const char diag[] = "GFM_PROTO_REPLICA_ADDED2";

	gfm_client_compound_put_fd_request(net_fd, diag);
	if ((e = gfm_client_replica_added2_request(gfm_server,
	    flags, mtime_sec, mtime_nsec, size))
	    != GFARM_ERR_NO_ERROR)
		fatal_metadb_proto(GFARM_MSG_1000514, request, diag, e);
	gfm_client_compound_put_fd_result(diag);
	if ((e = gfm_client_replica_added_result(gfm_server))
	    != GFARM_ERR_NO_ERROR) {
		if (debug_mode)
			gflog_info(GFARM_MSG_1000518,
			    "%s result on %s: %s", diag, request,
			    gfarm_error_string(e));
	} else
		gfm_client_compound_end(diag);

	return (e);
}

void
gfs_server_replica_add_from(struct gfp_xdr *client)
{
	gfarm_int32_t net_fd, local_fd, port, mtime_nsec = 0;
	gfarm_int64_t mtime_sec = 0;
	gfarm_ino_t ino = 0;
	gfarm_uint64_t gen = 0;
	gfarm_error_t e, e2;
	char *host, *path;
	struct gfs_connection *server;
	int flags = 0; /* XXX - for now */
	struct stat sb;
	static const char diag[] = "GFS_PROTO_REPLICA_ADD_FROM";

	sb.st_size = -1;
	gfs_server_get_request(client, diag, "sii", &host, &port, &net_fd);

	e = replica_adding(net_fd, host, &ino, &gen, &mtime_sec, &mtime_nsec,
	    diag);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002176,
			"replica_adding() failed: %s",
			gfarm_error_string(e));
		goto free_host;
	}

	local_path(ino, gen, diag, &path);
	local_fd = open_data(path, O_WRONLY|O_CREAT|O_TRUNC);
	free(path);
	if (local_fd < 0) {
		e = gfarm_errno_to_error(errno);
		/* invalidate the creating file replica */
		mtime_sec = mtime_nsec = 0;
		goto adding_cancel;
	}

	e = gfs_client_connection_acquire_by_host(gfm_server, host, port,
	    &server, listen_addrname);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002177,
			"gfs_client_connection_acquire_by_host() failed: %s",
			gfarm_error_string(e));
		mtime_sec = mtime_nsec = 0; /* invalidate */
		goto close;
	}
	e = gfs_client_replica_recv(server, ino, gen, local_fd);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_debug(GFARM_MSG_1002178,
			"gfs_client_replica_recv() failed: %s",
			gfarm_error_string(e));
		mtime_sec = mtime_nsec = 0; /* invalidate */
		goto free_server;
	}
	if (fstat(local_fd, &sb) == -1) {
		e = gfarm_errno_to_error(errno);
		mtime_sec = mtime_nsec = 0; /* invalidate */
	}
 free_server:
	gfs_client_connection_free(server);
 close:
	close(local_fd);
 adding_cancel:
	e2 = replica_added(net_fd, flags, mtime_sec, mtime_nsec, sb.st_size,
	    diag);
	if (e == GFARM_ERR_NO_ERROR)
		e = e2;
 free_host:
	free(host);
	gfs_server_put_reply(client, diag, e, "");
	return;
}

void
gfs_server_replica_recv(struct gfp_xdr *client,
	enum gfarm_auth_id_type peer_type)
{
	gfarm_error_t e, error = GFARM_ERR_NO_ERROR;
	gfarm_ino_t ino;
	gfarm_uint64_t gen;
	ssize_t rv;
	char buffer[GFS_PROTO_MAX_IOSIZE];
#if 0 /* not yet in gfarm v2 */
	struct gfs_client_rep_rate_info *rinfo = NULL;
#endif
	char *path;
	int local_fd;
	static const char diag[] = "GFS_PROTO_REPLICA_RECV";

	gfs_server_get_request(client, diag, "ll", &ino, &gen);
	/* from gfsd only */
	if (peer_type != GFARM_AUTH_ID_TYPE_SPOOL_HOST) {
		error = GFARM_ERR_OPERATION_NOT_PERMITTED;
		gflog_debug(GFARM_MSG_1002179,
			"operation is not permitted(peer_type)");
		goto send_eof;
	}

	local_path(ino, gen, diag, &path);
	local_fd = open_data(path, O_RDONLY);
	free(path);
	if (local_fd < 0) {
		error = gfarm_errno_to_error(errno);
		goto send_eof;
	}

	/* data transfer */
	if (file_read_size >= sizeof(buffer))
		file_read_size = sizeof(buffer);
#if 0 /* not yet in gfarm v2 */
	if (rate_limit != 0) {
		rinfo = gfs_client_rep_rate_info_alloc(rate_limit);
		if (rinfo == NULL)
			fatal("%s:rate_info_alloc: %s", diag,
			    gfarm_error_string(GFARM_ERR_NO_MEMORY));
	}
#endif
	do {
		rv = read(local_fd, buffer, file_read_size);
		if (rv <= 0) {
			if (rv == -1)
				error = gfarm_errno_to_error(errno);
			break;
		}
		e = gfp_xdr_send(client, "b", rv, buffer);
		if (e != GFARM_ERR_NO_ERROR) {
			error = e;
			gflog_debug(GFARM_MSG_1002180,
				"gfp_xdr_send() failed: %s",
				gfarm_error_string(e));
			break;
		}
		if (file_read_size < GFS_PROTO_MAX_IOSIZE) {
			e = gfp_xdr_flush(client);
			if (e != GFARM_ERR_NO_ERROR) {
				error = e;
				gflog_debug(GFARM_MSG_1002181,
					"gfp_xdr_send() failed: %s",
					gfarm_error_string(e));
				break;
			}
		}
#if 0 /* not yet in gfarm v2 */
		if (rate_limit != 0)
			gfs_client_rep_rate_control(rinfo, rv);
#endif
	} while (rv > 0);

#if 0 /* not yet in gfarm v2 */
	if (rinfo != NULL)
		gfs_client_rep_rate_info_free(rinfo);
#endif
	e = close(local_fd);
	if (error == GFARM_ERR_NO_ERROR)
		error = e;
 send_eof:
	/* send EOF mark */
	e = gfp_xdr_send(client, "b", 0, buffer);
	if (error == GFARM_ERR_NO_ERROR)
		error = e;

	gfs_server_put_reply(client, diag, error, "");
}

/* from gfmd */

gfarm_error_t
gfs_async_server_fhstat(struct gfp_xdr *conn, gfp_xdr_xid_t xid, size_t size)
{
	gfarm_error_t e;
	struct stat st;
	gfarm_ino_t ino;
	gfarm_uint64_t gen;
	gfarm_off_t filesize = 0;
	gfarm_int64_t atime_sec = 0, mtime_sec = 0;
	gfarm_int32_t atime_nsec = 0, mtime_nsec = 0;
	int save_errno = 0;
	char *path;

	e = gfs_async_server_get_request(conn, size, "fhstat",
	    "ll", &ino, &gen);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	local_path(ino, gen, "fhstat", &path);
	if (stat(path, &st) == -1)
		save_errno = errno;
	else {
		filesize = st.st_size;
		atime_sec = st.st_atime;
		/* XXX FIXME st_atimespec.tv_nsec */
		mtime_sec = st.st_mtime;
		/* XXX FIXME st_mtimespec.tv_nsec */
	}
	free(path);

	return (gfs_async_server_put_reply_with_errno(conn, xid,
	    "fhstat", save_errno,
	    "llili", filesize, atime_sec, atime_nsec, mtime_sec, mtime_nsec));
}

gfarm_error_t
gfs_async_server_fhremove(struct gfp_xdr *conn, gfp_xdr_xid_t xid, size_t size)
{
	gfarm_error_t e;
	gfarm_ino_t ino;
	gfarm_uint64_t gen;
	int save_errno = 0;
	char *path;

	e = gfs_async_server_get_request(conn, size, "fhremove",
	    "ll", &ino, &gen);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	local_path(ino, gen, "fhremove", &path);
	if (unlink(path) == -1)
		save_errno = errno;
	free(path);

	return (gfs_async_server_put_reply_with_errno(conn, xid,
	    "fhremove", save_errno, ""));
}

gfarm_error_t
gfs_async_server_status(struct gfp_xdr *conn, gfp_xdr_xid_t xid, size_t size)
{
	gfarm_error_t e;
	int save_errno = 0;
	double loadavg[3];
	gfarm_int32_t bsize;
	gfarm_off_t blocks, bfree, bavail, files, ffree, favail;
	gfarm_off_t used = 0, avail = 0;

	/* just check that size == 0 */
	e = gfs_async_server_get_request(conn, size, "status", "");
	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	if (getloadavg(loadavg, GFARM_ARRAY_LENGTH(loadavg)) == -1) {
		save_errno = EPERM; /* XXX */
		gflog_warning(GFARM_MSG_1000520,
		    "gfs_server_status: cannot get load average");
	} else {
		save_errno = gfsd_statfs(gfarm_spool_root, &bsize,
			&blocks, &bfree, &bavail, &files, &ffree, &favail);

		/* pretend to be disk full, to make this gfsd read-only */
		if (save_errno == 0 && is_readonly_mode()) {
			bavail -= bfree;
			bfree = 0;
		}
		if (save_errno == 0) {
			used = (blocks - bfree) * bsize / 1024;
			avail = bavail * bsize / 1024;
		}
	}
	return (gfs_async_server_put_reply_with_errno(conn, xid,
	    "status", save_errno,
	    "fffll", loadavg[0], loadavg[1], loadavg[2], used, avail));
}

static struct gfarm_hash_table *replication_queue_set = NULL;

/* per source-host queue */
struct replication_queue_data {
	struct replication_request *head;
	struct replication_request **tail;
};

gfarm_error_t
replication_queue_lookup(const char *hostname, int port,
	struct gfarm_hash_entry **qp)
{
	gfarm_error_t e;
	int created;
	struct gfarm_hash_entry *q;
	struct replication_queue_data *qd;

	e = gfp_conn_hash_enter(&replication_queue_set, HOST_HASHTAB_SIZE,
	    sizeof(*qd), hostname, port, gfarm_get_global_username(),
	    &q, &created);
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_UNFIXED,
		    "creating replication queue for %s:%d: %s",
		    hostname, port, gfarm_error_string(e));
		return (e);
	}
	qd = gfarm_hash_entry_data(q);
	if (created) {
		qd->head = NULL;
		qd->tail = &qd->head;
	}
	*qp = q;
	return (GFARM_ERR_NO_ERROR);
}

struct replication_request {
	/* only used when actual replication is ongoing */
	struct replication_request *ongoing_next, *ongoing_prev;

	struct replication_request *q_next;
	struct gfarm_hash_entry *q;

	gfp_xdr_xid_t xid;
	gfarm_ino_t ino;
	gfarm_int64_t gen;

	/* the followings are only used when actual replication is ongoing */
	struct gfs_connection *src_gfsd;
	int file_fd, pipe_fd;
	pid_t pid;

};

/* dummy header of doubly linked circular list */
struct replication_request ongoing_replications = 
	{ &ongoing_replications, &ongoing_replications };

struct replication_errcodes {
	gfarm_int32_t src_errcode;
	gfarm_int32_t dst_errcode;
};

gfarm_error_t
try_replication(struct gfp_xdr *conn, struct gfarm_hash_entry *q,
	gfarm_error_t *resultp)
{
	gfarm_error_t e;
	struct replication_queue_data *qd = gfarm_hash_entry_data(q);
	struct replication_request *rep = qd->head;
	char *path;
	struct gfs_connection *src_gfsd;
	int fds[2];
	pid_t pid = -1;
	struct replication_errcodes errcodes;
	int local_fd, rv;
	static const char diag[] = "GFS_PROTO_REPLICATION_REQUEST";

	local_path(rep->ino, rep->gen, diag, &path);
	local_fd = open_data(path, O_WRONLY|O_CREAT|O_TRUNC);
	free(path);
	if (local_fd < 0) {
		e = gfarm_errno_to_error(errno);
		gflog_error(GFARM_MSG_1002182,
		    "%s: cannot open local file for %lld:%lld: %s", diag,
		    (long long)rep->ino, (long long)rep->gen, strerror(errno));
	} else if ((e = gfs_client_connection_acquire_by_host(gfm_server,
	    gfp_conn_hash_hostname(q), gfp_conn_hash_port(q),
	    &src_gfsd, listen_addrname)) != GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_1002184, "%s: connecting to %s:%d: %s",
		    diag,
		    gfp_conn_hash_hostname(q), gfp_conn_hash_port(q),
		    gfarm_error_string(e));
		close(local_fd);
	} else if (pipe(fds) == -1) {
		e = gfarm_errno_to_error(errno);
		gflog_error(GFARM_MSG_1002185, "%s: cannot create pipe: %s",
		    diag, strerror(errno));
		gfs_client_connection_free(src_gfsd);
		close(local_fd);
	} else if (fds[0] > FD_SETSIZE) { /* XXX select FD_SETSIZE */
		e = GFARM_ERR_TOO_MANY_OPEN_FILES;
		gflog_error(GFARM_MSG_1002186, "%s: cannot select %d: %s",
		    diag, fds[0], gfarm_error_string(e));
		close(fds[0]);
		close(fds[1]);
		gfs_client_connection_free(src_gfsd);
		close(local_fd);
	} else if ((pid = fork()) == 0) { /* child */
		close(fds[0]);
		e = gfs_client_replica_recv(src_gfsd, rep->ino, rep->gen,
		    local_fd);
		if (e != GFARM_ERR_NO_ERROR) {
			gflog_error(GFARM_MSG_1002187, "%s: replica_recv: %s",
			    diag, gfarm_error_string(e));
		}
		/*
		 * XXX FIXME
		 * modify gfs_client_replica_recv() interface to return
		 * the error codes for both source and destination side.
		 */
		if (IS_CONNECTION_ERROR(e)) {
			errcodes.src_errcode = e;
			errcodes.dst_errcode = GFARM_ERR_NO_ERROR;
		} else {
			errcodes.src_errcode = GFARM_ERR_NO_ERROR;
			errcodes.dst_errcode = e;
		}
		if ((rv = write(fds[1], &errcodes, sizeof(errcodes))) == -1)
			gflog_error(GFARM_MSG_1002188, "%s: write pipe: %s",
			    diag, strerror(errno));
		else if (rv != sizeof(errcodes))
			gflog_error(GFARM_MSG_1002189, "%s: partial write: "
			    "%d < %d", diag, rv, (int)sizeof(e));
		close(fds[1]);
		exit(e == GFARM_ERR_NO_ERROR ? 0 : 1);
	} else { /* parent */
		if (pid == -1) {
			e = gfarm_errno_to_error(errno);
			gflog_error(GFARM_MSG_1002190,
			    "%s: cannot child process: %s",
			    diag, strerror(errno));
			close(fds[0]);
			gfs_client_connection_free(src_gfsd);
			close(local_fd);
		} else {
			rep->src_gfsd = src_gfsd;
			rep->file_fd = local_fd;
			rep->pipe_fd = fds[0];
			rep->pid = pid;
			rep->ongoing_next = &ongoing_replications;
			rep->ongoing_prev = ongoing_replications.ongoing_prev;
			ongoing_replications.ongoing_prev->ongoing_next = rep;
			ongoing_replications.ongoing_prev = rep;
		}
		close(fds[1]);
	}

	*resultp = e;
	return (gfs_async_server_put_reply(conn, rep->xid, diag, e,
	    "l", (gfarm_int64_t)pid));
}

gfarm_error_t
start_replication(struct gfp_xdr *conn, struct gfarm_hash_entry *q)
{
	gfarm_error_t e, e2;
	struct replication_queue_data *qd = gfarm_hash_entry_data(q);
	struct replication_request *rep;

	for (;;) {
		e2 = try_replication(conn, q, &e);
		if (e == GFARM_ERR_NO_ERROR || e2 != GFARM_ERR_NO_ERROR)
			return (e2);

		/* we don't have to touch rep->ongoing_{next,prev} here */

		rep = qd->head->q_next;
		free(qd->head);

		qd->head = rep;
		if (rep == NULL) {
			qd->tail = &qd->head;
			return (e2);
		} else {
			qd->tail = &rep->q_next;
		}
	}
}

gfarm_error_t
gfs_async_server_replication_request(struct gfp_xdr *conn,
	gfp_xdr_xid_t xid, size_t size)
{
	gfarm_error_t e;
	char *host;
	gfarm_int32_t port;
	gfarm_ino_t ino;
	gfarm_uint64_t gen;
	struct gfarm_hash_entry *q;
	struct replication_queue_data *qd;
	struct replication_request *rep;
	static const char diag[] = "GFS_PROTO_REPLICATION_REQUEST";

	e = gfs_async_server_get_request(conn, size, diag,
	    "sill", &host, &port, &ino, &gen);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	if ((e = replication_queue_lookup(host, port, &q)) !=
	    GFARM_ERR_NO_ERROR) {
		gflog_error(GFARM_MSG_UNFIXED,
		    "cannot allocate replication queue for %s:%d: %s",
		    host, port, gfarm_error_string(e));
	} else {
		GFARM_MALLOC(rep);
		if (rep == NULL) {
			e = GFARM_ERR_NO_MEMORY;
			gflog_error(GFARM_MSG_UNFIXED,
			    "cannot allocate replication record for "
			    "%s:%d %lld:%lld: no memory",
			    host, port, (long long)ino, (long long)gen);
		} else {
			free(host);

			rep->xid = xid;
			rep->ino = ino;
			rep->gen = gen;

			/* not set yet, will be set in try_replication() */
			rep->src_gfsd = NULL;
			rep->file_fd = -1;
			rep->pipe_fd = -1;
			rep->pid = 0;
			rep->ongoing_next = rep->ongoing_prev = rep;

			rep->q = q;
			rep->q_next = NULL;

			qd = gfarm_hash_entry_data(q);
			*qd->tail = rep;
			qd->tail = &rep->q_next;
			if (qd->head == rep) { /* this host is idle */
				return (start_replication(conn, q));
			} else { /* the replication is postponed */
				return (GFARM_ERR_NO_ERROR);
			}
		}
	}
	free(host);

	/* only used in an error case */
	return (gfs_async_server_put_reply(conn, xid, diag, e, ""));
}

#if 0 /* not yet in gfarm v2 */

void
gfs_server_striping_read(struct gfp_xdr *client)
{
	gfarm_error_t e;
	gfarm_int32_t fd, interleave_factor;
	gfarm_off_t offset, size, full_stripe_size;
	gfarm_off_t chunk_size;
	ssize_t rv;
	gfarm_error_t error = GFARM_ERR_NO_ERROR;
	char buffer[GFS_PROTO_MAX_IOSIZE];
	struct gfs_client_rep_rate_info *rinfo = NULL;

	gfs_server_get_request(client, "striping_read", "iooio", &fd,
	    &offset, &size, &interleave_factor, &full_stripe_size);

	if (file_read_size >= sizeof(buffer))
		file_read_size = sizeof(buffer);
	if (rate_limit != 0) {
		rinfo = gfs_client_rep_rate_info_alloc(rate_limit);
		if (rinfo == NULL)
			fatal("striping_read:rate_info_alloc: %s",
			    gfarm_error_string(GFARM_ERR_NO_MEMORY));
	}

	fd = file_table_get(fd);
	if (lseek(fd, (off_t)offset, SEEK_SET) == -1) {
		error = gfarm_errno_to_error(errno);
		goto finish;
	}
	for (;;) {
		chunk_size = interleave_factor == 0 || size < interleave_factor
		    ? size : interleave_factor;
		for (; chunk_size > 0; chunk_size -= rv, size -= rv) {
			rv = read(fd, buffer, chunk_size < file_read_size ?
			    chunk_size : file_read_size);
			if (rv <= 0) {
				if (rv == -1)
					error =
					    gfarm_errno_to_error(errno);
				goto finish;
			}
			e = gfp_xdr_send(client, "b", rv, buffer);
			if (e != GFARM_ERR_NO_ERROR) {
				error = e;
				goto finish;
			}
			if (file_read_size < GFS_PROTO_MAX_IOSIZE) {
				e = gfp_xdr_flush(client);
				if (e != GFARM_ERR_NO_ERROR) {
					error = e;
					goto finish;
				}
			}
			if (rate_limit != 0)
				gfs_client_rep_rate_control(rinfo, rv);
		}
		if (size <= 0)
			break;
		offset += full_stripe_size;
		if (lseek(fd, (off_t)offset, SEEK_SET) == -1) {
			error = gfarm_errno_to_error(errno);
			break;
		}
	}
 finish:
	if (rinfo != NULL)
		gfs_client_rep_rate_info_free(rinfo);
	/* send EOF mark */
	e = gfp_xdr_send(client, "b", 0, buffer);
	if (e != GFARM_ERR_NO_ERROR && error == GFARM_ERR_NO_ERROR)
		error = e;

	gfs_server_put_reply(client, "striping_read", error, "");
}

void
gfs_server_replicate_file_sequential_common(struct gfp_xdr *client,
	char *file, gfarm_int32_t mode,
	char *src_canonical_hostname, char *src_if_hostname)
{
	gfarm_error_t e;
	char *path;
	struct gfs_connection *src_conn;
	int fd, src_fd;
	long file_sync_rate;
	gfarm_error_t error = GFARM_ERR_NO_ERROR;
	struct hostent *hp;
	struct sockaddr_in peer_addr;

	hp = gethostbyname(src_if_hostname);
	free(src_if_hostname);
	if (hp == NULL || hp->h_addrtype != AF_INET) {
		e = GFARM_ERR_UNKNOWN_HOST;
	} else {
		memset(&peer_addr, 0, sizeof(peer_addr));
		memcpy(&peer_addr.sin_addr, hp->h_addr,
		       sizeof(peer_addr.sin_addr));
		peer_addr.sin_family = hp->h_addrtype;
		peer_addr.sin_port = htons(gfarm_spool_server_port);

		e = gfarm_netparam_config_get_long(
		    &gfarm_netparam_file_sync_rate,
		    src_canonical_hostname, (struct sockaddr *)&peer_addr,
		    &file_sync_rate);
		if (e != GFARM_ERR_NO_ERROR) /* shouldn't happen */
			gflog_warning(GFARM_MSG_1000521, "file_sync_rate: %s",
			    gfarm_error_string(e));

		/*
		 * the following gfs_client_connect() accesses user & home
		 * information which was set in gfarm_authorize()
		 * with switch_to==1.
		 */
		e = gfs_client_connect(
		    src_canonical_hostname, (struct sockaddr *)&peer_addr,
		    &src_conn);
	}
	free(src_canonical_hostname);
	if (e != GFARM_ERR_NO_ERROR) {
		error = e;
		gflog_warning(GFARM_MSG_1000522,
		    "replicate_file_seq:remote_connect: %s",
		    gfarm_error_string(e));
	} else {
		e = gfs_client_open(src_conn, file, GFARM_FILE_RDONLY, 0,
				    &src_fd);
		if (e != GFARM_ERR_NO_ERROR) {
			error = e;
			gflog_warning(GFARM_MSG_1000523,
			    "replicate_file_seq:remote_open: %s",
			    gfarm_error_string(e));
		} else {
			e = gfarm_path_localize(file, &path);
			if (e != GFARM_ERR_NO_ERROR)
				fatal(GFARM_MSG_1000524,
				    "replicate_file_seq:path: %s",
				    gfarm_error_string(e));
			fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, mode);
			if (fd < 0) {
				error = gfarm_errno_to_error(errno);
				gflog_warning_errno(GFARM_MSG_1000525,
				    "replicate_file_seq:local_open");
			} else {
				e = gfs_client_copyin(src_conn, src_fd, fd,
				    file_sync_rate);
				if (e != GFARM_ERR_NO_ERROR) {
					error = e;
					gflog_warning(GFARM_MSG_1000526,
					    "replicate_file_seq:copyin: %s",
					    gfarm_error_string(e));
				}
				close(fd);
			}
			e = gfs_client_close(src_conn, src_fd);
			free(path);
		}
		gfs_client_disconnect(src_conn);
	}
	free(file);

	gfs_server_put_reply(client, "replicate_file_seq", error, "");
}

/* obsolete interafce, keeped for backward compatibility */
void
gfs_server_replicate_file_sequential_old(struct gfp_xdr *client)
{
	char *file, *src_canonical_hostname, *src_if_hostname;
	gfarm_int32_t mode;

	gfs_server_get_request(client, "replicate_file_seq_old",
	    "sis", &file, &mode, &src_if_hostname);

	src_canonical_hostname = strdup(src_if_hostname);
	if (src_canonical_hostname == NULL) {
		gfs_server_put_reply(client, "replicate_file_seq_old",
		    GFARM_ERR_NO_MEMORY, "");
		return;
	}
	gfs_server_replicate_file_sequential_common(client, file, mode,
	    src_canonical_hostname, src_if_hostname);
}

void
gfs_server_replicate_file_sequential(struct gfp_xdr *client)
{
	char *file, *src_canonical_hostname, *src_if_hostname;
	gfarm_int32_t mode;

	gfs_server_get_request(client, "replicate_file_seq",
	    "siss", &file, &mode, &src_canonical_hostname, &src_if_hostname);

	gfs_server_replicate_file_sequential_common(client, file, mode,
	    src_canonical_hostname, src_if_hostname);
}

int iosize_alignment = 4096;
int iosize_minimum_division = 65536;

struct parallel_stream {
	struct gfs_connection *src_conn;
	int src_fd;
	enum { GSRFP_COPYING, GSRFP_FINISH } state;
};

gfarm_error_t
simple_division(int ofd, struct parallel_stream *divisions,
	off_t file_size, int n)
{
	gfarm_error_t e, e_save = GFARM_ERR_NO_ERROR;
	gfarm_off_t offset = 0, residual = file_size;
	gfarm_off_t size_per_division = file_size / n;
	int i;

	if ((size_per_division / iosize_alignment) *
	    iosize_alignment != size_per_division) {
		size_per_division =
		    ((size_per_division / iosize_alignment) + 1) *
		    iosize_alignment;
	}

	for (i = 0; i < n; i++) {
		gfarm_off_t size;

		if (residual <= 0 || e_save != GFARM_ERR_NO_ERROR) {
			divisions[i].state = GSRFP_FINISH;
			continue;
		}
		size = residual <= size_per_division ?
		    residual : size_per_division;
		e = gfs_client_striping_copyin_request(
		    divisions[i].src_conn, divisions[i].src_fd, ofd,
		    offset, size, 0, 0);
		offset += size_per_division;
		residual -= size;
		if (e != GFARM_ERR_NO_ERROR) {
			if (e_save == GFARM_ERR_NO_ERROR)
				e_save = e;
			gflog_warning(GFARM_MSG_1000527,
			    "replicate_file_division:copyin: %s",
			    gfarm_error_string(e));
			divisions[i].state = GSRFP_FINISH;
			continue;
		}
		divisions[i].state = GSRFP_COPYING;
	}
	return (e_save);
}

gfarm_error_t
striping(int ofd, struct parallel_stream *divisions,
	off_t file_size, int n, int interleave_factor)
{
	gfarm_error_t e, e_save = GFARM_ERR_NO_ERROR;
	gfarm_off_t full_stripe_size = (gfarm_off_t)interleave_factor * n;
	gfarm_off_t stripe_number = file_size / full_stripe_size;
	gfarm_off_t size_per_division = interleave_factor * stripe_number;
	gfarm_off_t residual = file_size - full_stripe_size * stripe_number;
	gfarm_off_t chunk_number_on_last_stripe;
	gfarm_off_t last_chunk_size;
	gfarm_off_t offset = 0;
	int i;

	if (residual == 0) {
		chunk_number_on_last_stripe = 0;
		last_chunk_size = 0;
	} else {
		chunk_number_on_last_stripe = residual / interleave_factor;
		last_chunk_size = residual - 
		    interleave_factor * chunk_number_on_last_stripe;
	}

	for (i = 0; i < n; i++) {
		gfarm_off_t size = size_per_division;

		if (i < chunk_number_on_last_stripe)
			size += interleave_factor;
		else if (i == chunk_number_on_last_stripe)
			size += last_chunk_size;
		if (size <= 0 || e_save != GFARM_ERR_NO_ERROR) {
			divisions[i].state = GSRFP_FINISH;
			continue;
		}
		e = gfs_client_striping_copyin_request(
		    divisions[i].src_conn, divisions[i].src_fd, ofd,
		    offset, size, interleave_factor, full_stripe_size);
		offset += interleave_factor;
		if (e != GFARM_ERR_NO_ERROR) {
			if (e_save == GFARM_ERR_NO_ERROR)
				e_save = e;
			gflog_warning(GFARM_MSG_1000528,
			    "replicate_file_stripe:copyin: %s",
			    gfarm_error_string(e));
			divisions[i].state = GSRFP_FINISH;
			continue;
		}
		divisions[i].state = GSRFP_COPYING;
	}
	return (e_save);
}

void
limit_division(int *ndivisionsp, gfarm_off_t file_size)
{
	int ndivisions = *ndivisionsp;

	/* do not divide too much */
	if (ndivisions > file_size / iosize_minimum_division) {
		ndivisions = file_size / iosize_minimum_division;
		if (ndivisions == 0)
			ndivisions = 1;
	}
	*ndivisionsp = ndivisions;
}

void
gfs_server_replicate_file_parallel_common(struct gfp_xdr *client,
	char *file, gfarm_int32_t mode, gfarm_off_t file_size,
	gfarm_int32_t ndivisions, gfarm_int32_t interleave_factor,
	char *src_canonical_hostname, char *src_if_hostname)
{
	struct parallel_stream *divisions;
	gfarm_error_t e, e_save = GFARM_ERR_NO_ERROR;
	char *path;
	long file_sync_rate, written;
	int i, j, n, ofd;
	gfarm_error_t error = GFARM_ERR_NO_ERROR;
	struct hostent *hp;
	struct sockaddr_in peer_addr;

	e = gfarm_path_localize(file, &path);
	if (e != GFARM_ERR_NO_ERROR)
		fatal(GFARM_MSG_1000529, "replicate_file_par: %s",
		    gfarm_error_string(e));
	ofd = open(path, O_WRONLY|O_CREAT|O_TRUNC, mode);
	free(path);
	if (ofd == -1) {
		error = gfarm_errno_to_error(errno);
		gflog_warning_errno(GFARM_MSG_1000530,
		    "replicate_file_par:local_open");
		goto finish;
	}

	limit_division(&ndivisions, file_size);

	GFARM_MALLOC_ARRAY(divisions, ndivisions);
	if (divisions == NULL) {
		error = GFARM_ERR_NO_MEMORY;
		goto finish_ofd;
	}

	hp = gethostbyname(src_if_hostname);
	if (hp == NULL || hp->h_addrtype != AF_INET) {
		error = GFARM_ERR_CONNECTION_REFUSED;
		goto finish_free_divisions;
	}
	memset(&peer_addr, 0, sizeof(peer_addr));
	memcpy(&peer_addr.sin_addr, hp->h_addr, sizeof(peer_addr.sin_addr));
	peer_addr.sin_family = hp->h_addrtype;
	peer_addr.sin_port = htons(gfarm_spool_server_port);

	e = gfarm_netparam_config_get_long(&gfarm_netparam_file_sync_rate,
	    src_canonical_hostname, (struct sockaddr *)&peer_addr,
	    &file_sync_rate);
	if (e != GFARM_ERR_NO_ERROR) /* shouldn't happen */
		gflog_warning(GFARM_MSG_1000531, "file_sync_rate: %s",
		    gfarm_error_string(e));

	/* XXX - this should be done in parallel rather than sequential */
	for (i = 0; i < ndivisions; i++) {

		e = gfs_client_connect(
		    src_canonical_hostname, (struct sockaddr *)&peer_addr,
		    &divisions[i].src_conn);
		if (e != GFARM_ERR_NO_ERROR) {
			if (e_save == GFARM_ERR_NO_ERROR)
				e_save = e;
			gflog_warning(GFARM_MSG_1000532,
			    "replicate_file_par:remote_connect: %s",
			    gfarm_error_string(e));
			break;
		}
	}
	n = i;
	if (n == 0) {
		error = e;
		goto finish_free_divisions;
	}
	e_save = GFARM_ERR_NO_ERROR; /* not fatal */

	/* XXX - this should be done in parallel rather than sequential */
	for (i = 0; i < n; i++) {
		e = gfs_client_open(divisions[i].src_conn, file,
		    GFARM_FILE_RDONLY, 0, &divisions[i].src_fd);
		if (e != GFARM_ERR_NO_ERROR) {
			if (e_save == GFARM_ERR_NO_ERROR)
				e_save = e;
			gflog_warning(GFARM_MSG_1000533,
			    "replicate_file_par:remote_open: %s",
			    gfarm_error_string(e));

			/*
			 * XXX - this should be done in parallel
			 * rather than sequential
			 */
			for (j = i; j < n; j++)
				gfs_client_disconnect(divisions[j].src_conn);
			n = i;
			break;
		}
	}
	if (n == 0) {
		error = e_save;
		goto finish_free_divisions;
	}
	e_save = GFARM_ERR_NO_ERROR; /* not fatal */

	if (interleave_factor == 0) {
		e = simple_division(ofd, divisions, file_size, n);
	} else {
		e = striping(ofd, divisions, file_size, n, interleave_factor);
	}
	e_save = e;

	written = 0;
	/*
	 * XXX - we cannot stop here, even if e_save != GFARM_ERR_NO_ERROR,
	 * because currently there is no way to cancel
	 * striping_copyin request.
	 */
	for (;;) {
		int max_fd, fd, nfound, rv;
		fd_set readable;

		FD_ZERO(&readable);
		max_fd = -1;
		for (i = 0; i < n; i++) {
			if (divisions[i].state != GSRFP_COPYING)
				continue;
			fd = gfs_client_connection_fd(divisions[i].src_conn);
			/* XXX - prevent this happens */
			if (fd >= FD_SETSIZE) {
				fatal(GFARM_MSG_1000534, "replicate_file_par: "
				    "too big file descriptor");
			}
			FD_SET(fd, &readable);
			if (max_fd < fd)
				max_fd = fd;
		}
		if (max_fd == -1)
			break;
		nfound = select(max_fd + 1, &readable, NULL, NULL, NULL);
		if (nfound <= 0) {
			if (nfound == -1 && errno != EINTR && errno != EAGAIN)
				gflog_warning_errno(GFARM_MSG_1000535,
				    "replicate_file_par:select");
			continue;
		}
		for (i = 0; i < n; i++) {
			if (divisions[i].state != GSRFP_COPYING)
				continue;
			fd = gfs_client_connection_fd(divisions[i].src_conn);
			if (!FD_ISSET(fd, &readable))
				continue;
			e = gfs_client_striping_copyin_partial(
			    divisions[i].src_conn, &rv);
			if (e != GFARM_ERR_NO_ERROR) {
				if (e_save == GFARM_ERR_NO_ERROR)
					e_save = e;
				divisions[i].state = GSRFP_FINISH; /* XXX */
			} else if (rv == 0) {
				divisions[i].state = GSRFP_FINISH;
				e = gfs_client_striping_copyin_result(
				    divisions[i].src_conn);
				if (e != GFARM_ERR_NO_ERROR) {
					if (e_save == GFARM_ERR_NO_ERROR)
						e_save = e;
				} else {
					e = gfs_client_close(
					    divisions[i].src_conn,
					    divisions[i].src_fd);
					if (e_save == GFARM_ERR_NO_ERROR)
						e_save = e;
				}
			} else if (file_sync_rate != 0) {
				written += rv;
				if (written >= file_sync_rate) {
					written -= file_sync_rate;
#ifdef HAVE_FDATASYNC
					fdatasync(ofd);
#else
					fsync(ofd);
#endif
				}
			}
			if (--nfound <= 0)
				break;
		}
	}

	/* XXX - this should be done in parallel rather than sequential */
	for (i = 0; i < n; i++) {
		e = gfs_client_disconnect(divisions[i].src_conn);
		if (e_save == GFARM_ERR_NO_ERROR)
			e_save = e;
	}
	if (e_save != GFARM_ERR_NO_ERROR)
		error = e_save;

finish_free_divisions:
	free(divisions);
finish_ofd:
	close(ofd);
finish:
	free(file);
	free(src_canonical_hostname);
	free(src_if_hostname);
	gfs_server_put_reply(client, "replicate_file_par", error, "");
}

/* obsolete interafce, keeped for backward compatibility */
void
gfs_server_replicate_file_parallel_old(struct gfp_xdr *client)
{
	char *file, *src_canonical_hostname, *src_if_hostname;
	gfarm_int32_t mode;
	gfarm_int32_t ndivisions; /* parallel_streams */
	gfarm_int32_t interleave_factor; /* stripe_unit_size, chuck size */
	gfarm_off_t file_size;

	gfs_server_get_request(client, "replicate_file_par_old", "sioiis",
	    &file, &mode, &file_size, &ndivisions, &interleave_factor,
	    &src_if_hostname);

	src_canonical_hostname = strdup(src_if_hostname);
	if (src_canonical_hostname == NULL) {
		gfs_server_put_reply(client, "replicate_file_par_old",
		    GFARM_ERR_NO_MEMORY, "");
		return;
	}
	gfs_server_replicate_file_parallel_common(client,
	    file, mode, file_size, ndivisions, interleave_factor,
	    src_canonical_hostname, src_if_hostname);
}

void
gfs_server_replicate_file_parallel(struct gfp_xdr *client)
{
	char *file, *src_canonical_hostname, *src_if_hostname;
	gfarm_int32_t mode;
	gfarm_int32_t ndivisions; /* parallel_streams */
	gfarm_int32_t interleave_factor; /* stripe_unit_size, chuck size */
	gfarm_off_t file_size;

	gfs_server_get_request(client, "replicate_file_par", "sioiiss",
	    &file, &mode, &file_size, &ndivisions, &interleave_factor,
	    &src_canonical_hostname, &src_if_hostname);

	gfs_server_replicate_file_parallel_common(client,
	    file, mode, file_size, ndivisions, interleave_factor,
	    src_canonical_hostname, src_if_hostname);
}

void
gfs_server_chdir(struct gfp_xdr *client)
{
	char *gpath, *path;
	int save_errno = 0;
	char *msg = "chdir";

	gfs_server_get_request(client, msg, "s", &gpath);

	local_path(gpath, &path, msg);
	if (chdir(path) == -1)
		save_errno = errno;
	free(path);

	gfs_server_put_reply_with_errno(client, msg, save_errno, "");
	check_input_output_error(msg, save_errno);
}

struct gfs_server_command_context {
	struct gfarm_iobuffer *iobuffer[NFDESC];

	enum { GFS_COMMAND_SERVER_STATE_NEUTRAL,
		       GFS_COMMAND_SERVER_STATE_OUTPUT,
		       GFS_COMMAND_SERVER_STATE_EXITED }
		server_state;
	int server_output_fd;
	int server_output_residual;
	enum { GFS_COMMAND_CLIENT_STATE_NEUTRAL,
		       GFS_COMMAND_CLIENT_STATE_OUTPUT }
		client_state;
	int client_output_residual;

	int pid;
	int exited_pid;
	int status;
} server_command_context;

#define COMMAND_IS_RUNNING()	(server_command_context.exited_pid == 0)

volatile sig_atomic_t sigchld_jmp_needed = 0;
sigjmp_buf sigchld_jmp_buf;

#endif /* not yet in gfarm v2 */

void
sigchld_handler(int sig)
{
	int pid, status, save_errno = errno;

	for (;;) {
		pid = waitpid(-1, &status, WNOHANG);
		if (pid == -1 || pid == 0)
			break;
#if 0 /* not yet in gfarm v2 */
		server_command_context.exited_pid = pid;
		server_command_context.status = status;
#endif /* not yet in gfarm v2 */
	}
	errno = save_errno;
#if 0 /* not yet in gfarm v2 */
	if (sigchld_jmp_needed) {
		sigchld_jmp_needed = 0;
		siglongjmp(sigchld_jmp_buf, 1);
	}
#endif /* not yet in gfarm v2 */
}

#if 0 /* not yet in gfarm v2 */

void fatal_command(const char *, ...) GFLOG_PRINTF_ARG(1, 2);
void
fatal_command(const char *format, ...)
{
	va_list ap;
	struct gfs_server_command_context *cc = &server_command_context;

	/* "-" is to send it to the process group */
	kill(-cc->pid, SIGTERM);

	va_start(ap, format);
	vfatal(format, ap);
	va_end(ap);
}

char *
gfs_server_command_fd_set(struct gfp_xdr *client,
			  fd_set *readable, fd_set *writable, int *max_fdp)
{
	struct gfs_server_command_context *cc = &server_command_context;
	int conn_fd = gfp_xdr_fd(client);
	int i, fd;

	/*
	 * The following test condition should just match with
	 * the i/o condition in gfs_server_command_io_fd_set(),
	 * otherwise unneeded busy wait happens.
	 */

	if (cc->client_state == GFS_COMMAND_CLIENT_STATE_NEUTRAL ||
	    (cc->client_state == GFS_COMMAND_CLIENT_STATE_OUTPUT &&
	     gfarm_iobuffer_is_readable(cc->iobuffer[FDESC_STDIN]))) {
		FD_SET(conn_fd, readable);
		if (*max_fdp < conn_fd)
			*max_fdp = conn_fd;
	}
	if ((cc->server_state == GFS_COMMAND_SERVER_STATE_NEUTRAL &&
	     (gfarm_iobuffer_is_writable(cc->iobuffer[FDESC_STDERR]) ||
	      gfarm_iobuffer_is_writable(cc->iobuffer[FDESC_STDOUT]) ||
	      !COMMAND_IS_RUNNING())) ||
	    cc->server_state == GFS_COMMAND_SERVER_STATE_OUTPUT) {
		FD_SET(conn_fd, writable);
		if (*max_fdp < conn_fd)
			*max_fdp = conn_fd;
	}

	if (COMMAND_IS_RUNNING() &&
	    gfarm_iobuffer_is_writable(cc->iobuffer[FDESC_STDIN])) {
		fd = gfarm_iobuffer_get_write_fd(cc->iobuffer[FDESC_STDIN]);
		FD_SET(fd, writable);
		if (*max_fdp < fd)
			*max_fdp = fd;
	}

	for (i = FDESC_STDOUT; i <= FDESC_STDERR; i++) {
		if (gfarm_iobuffer_is_readable(cc->iobuffer[i])) {
			fd = gfarm_iobuffer_get_read_fd(cc->iobuffer[i]);
			FD_SET(fd, readable);
			if (*max_fdp < fd)
				*max_fdp = fd;
		}
	}
	return (NULL);
}

gfarm_error_t
gfs_server_command_io_fd_set(struct gfp_xdr *client,
			     fd_set *readable, fd_set *writable)
{
	gfarm_error_t e;
	struct gfs_server_command_context *cc = &server_command_context;
	int i, fd, conn_fd = gfp_xdr_fd(client);

	fd = gfarm_iobuffer_get_write_fd(cc->iobuffer[FDESC_STDIN]);
	if (FD_ISSET(fd, writable)) {
		assert(gfarm_iobuffer_is_writable(cc->iobuffer[FDESC_STDIN]));
		gfarm_iobuffer_write(cc->iobuffer[FDESC_STDIN], NULL);
		e = gfarm_iobuffer_get_error(cc->iobuffer[FDESC_STDIN]);
		if (e != GFARM_ERR_NO_ERROR) {
			/* just purge the content */
			gfarm_iobuffer_purge(cc->iobuffer[FDESC_STDIN], NULL);
			gflog_warning(GFARM_MSG_UNUSED,
			    "command: abandon stdin: %s",
			    gfarm_error_string(e));
			gfarm_iobuffer_set_error(cc->iobuffer[FDESC_STDIN],
			    GFARM_ERR_NO_ERROR);
		}
		if (gfarm_iobuffer_is_eof(cc->iobuffer[FDESC_STDIN])) {
			/*
			 * We need to use shutdown(2) instead of close(2) here,
			 * to make bash happy...
			 * At least on Solaris 9, getpeername(2) returns EINVAL
			 * if the opposite side of the socketpair is closed,
			 * and bash doesn't read ~/.bashrc in such case.
			 * Read the comment about socketpair(2) in
			 * gfs_server_command() too.
			 */
			shutdown(fd, SHUT_WR);
		}
	}

	for (i = FDESC_STDOUT; i <= FDESC_STDERR; i++) {
		fd = gfarm_iobuffer_get_read_fd(cc->iobuffer[i]);
		if (!FD_ISSET(fd, readable))
			continue;
		gfarm_iobuffer_read(cc->iobuffer[i], NULL);
		e = gfarm_iobuffer_get_error(cc->iobuffer[i]);
		if (e == GFARM_ERR_NO_ERROR)
			continue;
		/* treat this as eof */
		gfarm_iobuffer_set_read_eof(cc->iobuffer[i]);
		gflog_warning(GFARM_MSG_UNUSED, "%s: %s", i == FDESC_STDOUT ?
		    "command: reading stdout" :
		    "command: reading stderr",
		     gfarm_error_string(e));
		gfarm_iobuffer_set_error(cc->iobuffer[i], GFARM_ERR_NO_ERROR);
	}

	if (FD_ISSET(conn_fd, readable) &&
	    cc->server_state != GFS_COMMAND_SERVER_STATE_EXITED) {
		if (cc->client_state == GFS_COMMAND_CLIENT_STATE_NEUTRAL) {
			gfarm_int32_t cmd, fd, len, sig;
			int eof;

			e = gfp_xdr_recv(client, 1, &eof, "i", &cmd);
			if (e != GFARM_ERR_NO_ERROR)
				fatal_command("command:client subcommand");
			if (eof)
				fatal_command("command:client subcommand: "
				    "eof");
			switch (cmd) {
			case GFS_PROTO_COMMAND_EXIT_STATUS:
				fatal_command("command:client subcommand: "
				    "unexpected exit_status");
				break;
			case GFS_PROTO_COMMAND_SEND_SIGNAL:
				e = gfp_xdr_recv(client, 1, &eof, "i", &sig);
				if (e != GFARM_ERR_NO_ERROR)
					fatal_command(
					    "command_send_signal: %s",
					    gfarm_error_string(e));
				if (eof)
					fatal_command(
					    "command_send_signal: eof");
				/* "-" is to send it to the process group */
				kill(-cc->pid, sig);
				break;
			case GFS_PROTO_COMMAND_FD_INPUT:
				e = gfp_xdr_recv(client, 1, &eof,
						   "ii", &fd, &len);
				if (e != GFARM_ERR_NO_ERROR)
					fatal_command("command_fd_input: %s",
					    gfarm_error_string(e));
				if (eof)
					fatal_command("command_fd_input: eof");
				if (fd != FDESC_STDIN) {
					/* XXX - something wrong */
					fatal_command("command_fd_input: fd");
				}
				if (len <= 0) {
					/* notify closed */
					gfarm_iobuffer_set_read_eof(
					    cc->iobuffer[FDESC_STDIN]);
				} else {
					cc->client_state =
					    GFS_COMMAND_CLIENT_STATE_OUTPUT;
					cc->client_output_residual = len;
				}
				break;
			default:
				/* XXX - something wrong */
				fatal_command("command_io: "
				    "unknown subcommand");
				break;
			}
		} else if (cc->client_state==GFS_COMMAND_CLIENT_STATE_OUTPUT) {
			gfarm_iobuffer_read(cc->iobuffer[FDESC_STDIN],
				&cc->client_output_residual);
			if (cc->client_output_residual == 0)
				cc->client_state =
					GFS_COMMAND_CLIENT_STATE_NEUTRAL;
			e = gfarm_iobuffer_get_error(
			    cc->iobuffer[FDESC_STDIN]);
			if (e != GFARM_ERR_NO_ERROR) {
				/* treat this as eof */
				gfarm_iobuffer_set_read_eof(
				    cc->iobuffer[FDESC_STDIN]);
				gflog_warning(GFARM_MSG_UNUSED,
				    "command: receiving stdin: %s",
				    gfarm_error_string(e));
				gfarm_iobuffer_set_error(
				    cc->iobuffer[FDESC_STDIN],
				    GFARM_ERR_NO_ERROR);
			}
			if (gfarm_iobuffer_is_read_eof(
					cc->iobuffer[FDESC_STDIN])) {
				fatal_command("command_fd_input_content: eof");
			}
		}
	}
	if (FD_ISSET(conn_fd, writable)) {
		if (cc->server_state == GFS_COMMAND_SERVER_STATE_NEUTRAL) {
			if (gfarm_iobuffer_is_writable(
				cc->iobuffer[FDESC_STDERR]) ||
			    gfarm_iobuffer_is_writable(
				cc->iobuffer[FDESC_STDOUT])) {
				if (gfarm_iobuffer_is_writable(
						cc->iobuffer[FDESC_STDERR]))
					cc->server_output_fd = FDESC_STDERR;
				else
					cc->server_output_fd = FDESC_STDOUT;
				/*
				 * cc->server_output_residual may be 0,
				 * if stdout or stderr is closed.
				 */
				cc->server_output_residual =
				    gfarm_iobuffer_avail_length(
					cc->iobuffer[cc->server_output_fd]);
				e = gfp_xdr_send(client, "iii",
					GFS_PROTO_COMMAND_FD_OUTPUT,
					cc->server_output_fd,
					cc->server_output_residual);
				if (e != GFARM_ERR_NO_ERROR ||
				    (e = gfp_xdr_flush(client)) !=
				     GFARM_ERR_NO_ERROR)
					fatal_command("command: fd_output: %s",
					    gfarm_error_string(e));
				cc->server_state =
					GFS_COMMAND_SERVER_STATE_OUTPUT;
			} else if (!COMMAND_IS_RUNNING()) {
				e = gfp_xdr_send(client, "i",
					GFS_PROTO_COMMAND_EXITED);
				if (e != GFARM_ERR_NO_ERROR ||
				    (e = gfp_xdr_flush(client)) !=
				    GFARM_ERR_NO_ERROR)
					fatal(GFARM_MSG_1000536,
					    "command: report exit: %s",
					    gfarm_error_string(e));
				cc->server_state =
					GFS_COMMAND_SERVER_STATE_EXITED;
			}
		} else if (cc->server_state==GFS_COMMAND_SERVER_STATE_OUTPUT) {
			gfarm_iobuffer_write(
				cc->iobuffer[cc->server_output_fd],
				&cc->server_output_residual);
			if (cc->server_output_residual == 0)
				cc->server_state =
					GFS_COMMAND_SERVER_STATE_NEUTRAL;
			e = gfarm_iobuffer_get_error(
			    cc->iobuffer[cc->server_output_fd]);
			if (e != GFARM_ERR_NO_ERROR) {
				fatal_command("command: sending %s: %s",
				    cc->server_output_fd == FDESC_STDOUT ?
				    "stdout" : "stderr",
				    gfarm_error_string(e));
			}
		}
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfs_server_command_io(struct gfp_xdr *client, struct timeval *timeout)
{
	volatile int nfound;
	int max_fd, conn_fd = gfp_xdr_fd(client);
	fd_set readable, writable;
	gfarm_error_t e;

	if (server_command_context.server_state ==
	    GFS_COMMAND_SERVER_STATE_EXITED)
		return (GFARM_ERR_NO_ERROR);

	max_fd = -1;
	FD_ZERO(&readable);
	FD_ZERO(&writable);

	gfs_server_command_fd_set(client, &readable, &writable, &max_fd);

	/*
	 * We wait for either SIGCHLD or select(2) event here,
	 * and use siglongjmp(3) to avoid a race condition about the signal.
	 *
	 * The race condition happens, if the SIGCHLD signal is delivered
	 * after the if-statement which does FD_SET(conn_fd, writable) in
	 * gfs_server_command_fd_set(), and before the select(2) below.
	 * In that condition, the following select(2) may wait that the
	 * `conn_fd' becomes readable, and because it may never happan,
	 * it waits forever (i.e. both gfrcmd and gfsd wait each other
	 * forever), and hangs, unless there is the sigsetjmp()/siglongjmp()
	 * handling.
	 *
	 * If the SIGCHLD is delivered inside the select(2) system call,
	 * the problem doesn't happen, because select(2) will return
	 * with EINTR.
	 *
	 * Also, if the SIGCHLD is delivered before an EOF from either
	 * cc->iobuffer[FDESC_STDOUT] or cc->iobuffer[FDESC_STDERR],
	 * the problem doesn't happen, either. Because the select(2)
	 * will be woken up by the EOF. But actually the SIGCHLD is
	 * delivered after the EOF (of both FDESC_STDOUT and FDESC_STDERR,
	 * and even after the EOFs are reported to a client) at least
	 * on linux-2.4.21-pre4.
	 */
	nfound = -1; errno = EINTR;
	if (sigsetjmp(sigchld_jmp_buf, 1) == 0) {
		sigchld_jmp_needed = 1;
		/*
		 * Here, we have to wait until the `conn_fd' is writable,
		 * if this is !COMMAND_IS_RUNNING() case.
		 */
		if (COMMAND_IS_RUNNING() || FD_ISSET(conn_fd, &writable)) {
			nfound = select(max_fd + 1, &readable, &writable, NULL,
			    timeout);
		}
	}
	sigchld_jmp_needed = 0;

	if (nfound > 0)
		e = gfs_server_command_io_fd_set(client, &readable, &writable);
	else
		e = GFARM_ERR_NO_ERROR;

	return (e);
}

gfarm_error_t
gfs_server_client_command_result(struct gfp_xdr *client)
{
	struct gfs_server_command_context *cc = &server_command_context;
	gfarm_int32_t cmd, fd, len, sig;
	int finish, eof;
	gfarm_error_t e;

	while (cc->server_state != GFS_COMMAND_SERVER_STATE_EXITED)
		gfs_server_command_io(client, NULL);
	/*
	 * Because COMMAND_IS_RUNNING() must be false here,
	 * we don't have to call fatal_command() from now on.
	 */

	/*
	 * Now, we recover the connection file descriptor blocking mode.
	 */
	if (fcntl(gfp_xdr_fd(client), F_SETFL, 0) == -1)
		gflog_warning(GFARM_MSG_1000537, "command-result:block: %s",
		    strerror(errno));

	/* make cc->client_state neutral */
	if (cc->client_state == GFS_COMMAND_CLIENT_STATE_OUTPUT) {
		e = gfp_xdr_purge(client, 0, cc->client_output_residual);
		if (e != GFARM_ERR_NO_ERROR)
			fatal(GFARM_MSG_1000538,
			    "command_fd_input:purge: %s",
			    gfarm_error_string(e));
	}

	for (finish = 0; !finish; ) {
		e = gfp_xdr_recv(client, 0, &eof, "i", &cmd);
		if (e != GFARM_ERR_NO_ERROR)
			fatal(GFARM_MSG_1000539,
			    "command:client subcommand: %s",
			    gfarm_error_string(e));
		if (eof)
			fatal(GFARM_MSG_1000540,
			    "command:client subcommand: eof");
		switch (cmd) {
		case GFS_PROTO_COMMAND_EXIT_STATUS:
			finish = 1;
			break;
		case GFS_PROTO_COMMAND_SEND_SIGNAL:
			e = gfp_xdr_recv(client, 0, &eof, "i", &sig);
			if (e != GFARM_ERR_NO_ERROR)
				fatal(GFARM_MSG_1000541,
				    "command_send_signal: %s",
				    gfarm_error_string(e));
			if (eof)
				fatal(GFARM_MSG_1000542,
				    "command_send_signal: eof");
			break;
		case GFS_PROTO_COMMAND_FD_INPUT:
			e = gfp_xdr_recv(client, 0, &eof, "ii", &fd, &len);
			if (e != GFARM_ERR_NO_ERROR)
				fatal(GFARM_MSG_1000543, "command_fd_input: %s",
				    gfarm_error_string(e));
			if (eof)
				fatal(GFARM_MSG_1000544,
				    "command_fd_input: eof");
			if (fd != FDESC_STDIN) {
				/* XXX - something wrong */
				fatal(GFARM_MSG_1000545,
				    "command_fd_input: fd");
			}
			e = gfp_xdr_purge(client, 0, len);
			if (e != GFARM_ERR_NO_ERROR)
				fatal(GFARM_MSG_1000546,
				    "command_fd_input:purge: %s",
				    gfarm_error_string(e));
			break;
		default:
			/* XXX - something wrong */
			fatal(GFARM_MSG_1000547,
			    "command_io: unknown subcommand %d", (int)cmd);
			break;
		}
	}
	gfs_server_put_reply(client, "command:exit_status",
	    GFARM_ERR_NO_ERROR, "iii",
	    WIFEXITED(cc->status) ? 0 : WTERMSIG(cc->status),
	    WIFEXITED(cc->status) ? WEXITSTATUS(cc->status) : 0,
	    WIFEXITED(cc->status) ? 0 : WCOREDUMP(cc->status));
	return (GFARM_ERR_NO_ERROR);
}

void
gfs_server_command(struct gfp_xdr *client, char *cred_env)
{
	struct gfs_server_command_context *cc = &server_command_context;
	gfarm_int32_t argc, argc_opt, nenv, flags, error;
	char *path, *command, **argv_storage = NULL, **argv = NULL;
	char **envp, *xauth;
	int stdin_pipe[2], stdout_pipe[2], stderr_pipe[2];
	int conn_fd = gfp_xdr_fd(client);
	int i, eof;
	socklen_t siz;
	struct passwd *pw;
	gfarm_error_t e = GFARM_ERR_NO_ERROR;

	char *user, *home, *shell;
	char *user_env, *home_env, *shell_env, *xauth_env; /* cred_end */
	static char user_format[] = "USER=%s";
	static char home_format[] = "HOME=%s";
	static char shell_format[] = "SHELL=%s";
	static char path_env[] = GFARM_DEFAULT_PATH;
#define N_EXTRA_ENV	4	/* user_env, home_env, shell_env, path_env */
	int use_cred_env = cred_env != NULL ? 1 : 0;

	static char xauth_format[] = "XAUTHORITY=%s";
	static char xauth_template[] = "/tmp/.xaXXXXXX";
	static char xauth_filename[sizeof(xauth_template)];
	int use_xauth_env = 0;
	size_t size;
	int overflow = 0;

#ifdef __GNUC__ /* workaround gcc warning: might be used uninitialized */
	envp = NULL;
	user_env = home_env =shell_env = xauth_env = NULL;
#endif
	gfs_server_get_request(client, "command", "siii",
			       &path, &argc, &nenv, &flags);
	argc_opt = flags & GFS_CLIENT_COMMAND_FLAG_SHELL_COMMAND ? 2 : 0;
	/* 2 for "$SHELL" + "-c" */

	size = gfarm_size_add(&overflow, argc, argc_opt + 1);
	if (!overflow)
		GFARM_MALLOC_ARRAY(argv_storage, size);
	if (overflow || argv_storage == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		goto rpc_error;
	}
	argv = argv_storage + argc_opt;
	if ((flags & GFS_CLIENT_COMMAND_FLAG_XAUTHCOPY) != 0)
		use_xauth_env = 1;
	size = gfarm_size_add(&overflow, nenv, 
			N_EXTRA_ENV + use_cred_env + use_xauth_env + 1);
	if (!overflow)
		GFARM_MALLOC_ARRAY(envp, size);
	if (overflow || envp == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		goto free_argv;
	}

	user = gfarm_get_local_username();
	home = gfarm_get_local_homedir();
	GFARM_MALLOC_ARRAY(user_env, sizeof(user_format) + strlen(user));
	if (user_env == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		goto free_envp;
	}
	sprintf(user_env, user_format, user);

	GFARM_MALLOC_ARRAY(home_env, sizeof(home_format) + strlen(home));
	if (home_env == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		goto free_user_env;
	}
	sprintf(home_env, home_format, home);

	pw = getpwnam(user);
	if (pw == NULL)
		fatal(GFARM_MSG_1000548, "%s: user doesn't exist", user);
	shell = pw->pw_shell;
	if (*shell == '\0')
		shell = PATH_BSHELL;

	if ((flags & GFS_CLIENT_COMMAND_FLAG_SHELL_COMMAND) == 0) {
		/*
		 * SECURITY.
		 * disallow anyone who doesn't have a standard shell.
		 */
		char *s;

		while ((s = getusershell()) != NULL)
			if (strcmp(s, shell) == 0)
				break;
		endusershell();
		if (s == NULL) {
			e = GFARM_ERR_OPERATION_NOT_PERMITTED;
			goto free_home_env;
		}
	}

	GFARM_MALLOC_ARRAY(shell_env, sizeof(shell_format) + strlen(shell));
	if (shell_env == NULL) {
		e = GFARM_ERR_NO_MEMORY;
		goto free_home_env;
	}
	sprintf(shell_env, shell_format, shell);

	argv[argc] = envp[nenv + N_EXTRA_ENV + use_cred_env + use_xauth_env] =
	    NULL;
	envp[nenv + 0] = user_env;
	envp[nenv + 1] = home_env;
	envp[nenv + 2] = shell_env;
	envp[nenv + 3] = path_env;

	for (i = 0; i < argc; i++) {
		e = gfp_xdr_recv(client, 0, &eof, "s", &argv[i]);
		if (e != GFARM_ERR_NO_ERROR || eof) {
			while (--i >= 0)
				free(argv[i]);
			goto free_shell_env;
		}
	}
	for (i = 0; i < nenv; i++) {
		e = gfp_xdr_recv(client, 0, &eof, "s", &envp[i]);
		if (e != GFARM_ERR_NO_ERROR || eof) {
			while (--i >= 0)
				free(envp[i]);
			goto free_argv_array;
		}
	}
	if ((flags & GFS_CLIENT_COMMAND_FLAG_SHELL_COMMAND) != 0) {
		argv_storage[0] = shell;
		argv_storage[1] = "-c";
		argv[1] = NULL; /* ignore argv[1 ... argc - 1] in this case. */
		command = shell;
	} else {
		command = path;
	}
	if (use_cred_env)
		envp[nenv + N_EXTRA_ENV] = cred_env;
	if (use_xauth_env) {
		static char xauth_command_format[] =
			"%s %s nmerge - 2>/dev/null";
		char *xauth_command;
		FILE *fp;
		int xauth_fd;

		e = gfp_xdr_recv(client, 0, &eof, "s", &xauth);
		if (e != GFARM_ERR_NO_ERROR || eof)
			goto free_envp_array;

		/*
		 * don't touch $HOME/.Xauthority to avoid lock contention
		 * on NFS home. (Is this really better? XXX)
		 */
		xauth_fd = mkstemp(strcpy(xauth_filename, xauth_template));
		if (xauth_fd == -1)
			goto free_xauth;
		close(xauth_fd);

		GFARM_MALLOC_ARRAY(xauth_env,
		    sizeof(xauth_format) + sizeof(xauth_filename));
		if (xauth_env == NULL)
			goto remove_xauth;
		sprintf(xauth_env, xauth_format, xauth_filename);
		envp[nenv + N_EXTRA_ENV + use_cred_env] = xauth_env;

		GFARM_MALLOC_ARRAY(xauth_command,
				   sizeof(xauth_command_format) +
				   strlen(xauth_env) +
				   strlen(XAUTH_COMMAND));
		if (xauth_command == NULL)
			goto free_xauth_env;
		sprintf(xauth_command, xauth_command_format,
			xauth_env, XAUTH_COMMAND);
		if ((fp = popen(xauth_command, "w")) == NULL)
			goto free_xauth_env;
		fputs(xauth, fp);
		pclose(fp);
		free(xauth_command);
	}
#if 1	/*
	 * The reason why we use socketpair(2) instead of pipe(2) is
	 * to make bash read ~/.bashrc. Because the condition that
	 * bash reads it is as follows:
	 *   1. $SSH_CLIENT/$SSH2_CLIENT is set, or stdin is a socket.
	 * and
	 *   2. $SHLVL < 2
	 * This condition that bash uses is broken, for example, this
	 * doesn't actually work with Sun's variant of OpenSSH on Solaris 9.
	 *
	 * Read the comment about shutdown(2) in gfs_server_command_io_fd_set()
	 * too.
	 * Honestly, people should use zsh instead of bash.
	 */
	if (socketpair(PF_UNIX, SOCK_STREAM, 0, stdin_pipe) == -1)
#else
	if (pipe(stdin_pipe) == -1)
#endif
	{
		e = gfarm_errno_to_error(errno);
		goto free_xauth_env;
	}
	if (pipe(stdout_pipe) == -1) {
		e = gfarm_errno_to_error(errno);
		goto close_stdin_pipe;
	}
	if (pipe(stderr_pipe) == -1) {
		e = gfarm_errno_to_error(errno);
		goto close_stdout_pipe;
	}
	cc->pid = cc->exited_pid = 0;
	if ((cc->pid = fork()) == 0) {
		struct rlimit rlim;

		/*
		 * XXX - Some Linux distributions set coredump size 0
		 *	 by default.
		 */
		if (getrlimit(RLIMIT_CORE, &rlim) != -1) {
			rlim.rlim_cur = rlim.rlim_max;
			setrlimit(RLIMIT_CORE, &rlim);
		}

		/* child */
		dup2(stdin_pipe[0], 0);
		dup2(stdout_pipe[1], 1);
		dup2(stderr_pipe[1], 2);
		close(stderr_pipe[1]);
		close(stderr_pipe[0]);
		close(stdout_pipe[1]);
		close(stdout_pipe[0]);
		close(stdin_pipe[1]);
		close(stdin_pipe[0]);
		/* close client connection, syslog and other sockets */
		for (i = 3; i < stderr_pipe[1]; i++)
			close(i);
		/* re-install default signal handler (see main) */
		if (signal(SIGPIPE, SIG_DFL) == SIG_ERR)
			gflog_error_errno(GFARM_MSG_1002384,
			    "signal(SIGPIPE, SIG_DFL)");
		/*
		 * create a process group
		 * to make it possible to send a signal later
		 */
		setpgid(0, getpid());
		umask(command_umask);
		execve(command, argv_storage, envp);
		fprintf(stderr, "%s: ", gfarm_host_get_self_name());
		perror(path);
		_exit(1);
	} else if (cc->pid == -1) {
		e = gfarm_errno_to_error(errno);
		goto close_stderr_pipe;
	}
	close(stderr_pipe[1]);
	close(stdout_pipe[1]);
	close(stdin_pipe[0]);
	error = GFARM_ERR_NO_ERROR;
	goto rpc_reply;

close_stderr_pipe:
	close(stderr_pipe[0]);
	close(stderr_pipe[1]);
close_stdout_pipe:
	close(stdout_pipe[0]);
	close(stdout_pipe[1]);
close_stdin_pipe:
	close(stdin_pipe[0]);
	close(stdin_pipe[1]);
free_xauth_env:
	if (use_xauth_env)
		free(xauth_env);
remove_xauth:
	if (use_xauth_env)
		unlink(xauth_filename);
free_xauth:
	if (use_xauth_env)
		free(xauth);
free_envp_array:
	for (i = 0; i < nenv; i++)
		free(envp[i]);
free_argv_array:
	for (i = 0; i < argc; i++)
		free(argv[i]);
free_shell_env:
	free(shell_env);
free_home_env:
	free(home_env);
free_user_env:
	free(user_env);
free_envp:
	free(envp);
free_argv:
	free(argv_storage);
rpc_error:
	free(path);
	error = e;
rpc_reply:
	gfs_server_put_reply(client, "command-start", error, "i", cc->pid);
	gfp_xdr_flush(client);
	if (error != GFARM_ERR_NO_ERROR)
		return;

	/*
	 * Now, we set the connection file descriptor non-blocking mode.
	 */
	if (fcntl(conn_fd, F_SETFL, O_NONBLOCK) == -1) /* shouldn't fail */
		gflog_warning(GFARM_MSG_1000549, "command-start:nonblock: %s",
		    strerror(errno));

	siz = sizeof(i);
	if (getsockopt(conn_fd, SOL_SOCKET, SO_RCVBUF, &i, &siz))
		i = GFARM_DEFAULT_COMMAND_IOBUF_SIZE;
	cc->iobuffer[FDESC_STDIN] = gfarm_iobuffer_alloc(i);

	siz = sizeof(i);
	if (getsockopt(conn_fd, SOL_SOCKET, SO_SNDBUF, &i, &siz))
		i = GFARM_DEFAULT_COMMAND_IOBUF_SIZE;
	cc->iobuffer[FDESC_STDOUT] = gfarm_iobuffer_alloc(i);
	cc->iobuffer[FDESC_STDERR] = gfarm_iobuffer_alloc(i);

	/*
	 * It's safe to use gfarm_iobuffer_set_nonblocking_write_fd()
	 * instead of gfarm_iobuffer_set_nonblocking_write_socket() here,
	 * because we always ignore SIGPIPE in gfsd.
	 * cf. gfarm_sigpipe_ignore() in main().
	 */
	gfarm_iobuffer_set_nonblocking_read_xxx(
		cc->iobuffer[FDESC_STDIN], client);
	gfarm_iobuffer_set_nonblocking_write_fd(
		cc->iobuffer[FDESC_STDIN], stdin_pipe[1]);

	gfarm_iobuffer_set_nonblocking_read_fd(
		cc->iobuffer[FDESC_STDOUT], stdout_pipe[0]);
	gfarm_iobuffer_set_nonblocking_write_xxx(
		cc->iobuffer[FDESC_STDOUT], client);

	gfarm_iobuffer_set_nonblocking_read_fd(
		cc->iobuffer[FDESC_STDERR], stderr_pipe[0]);
	gfarm_iobuffer_set_nonblocking_write_xxx(
		cc->iobuffer[FDESC_STDERR], client);

	while (cc->server_state != GFS_COMMAND_SERVER_STATE_EXITED)
		gfs_server_command_io(client, NULL);

	gfs_server_client_command_result(client);

	/*
	 * clean up
	 */

	gfarm_iobuffer_free(cc->iobuffer[FDESC_STDIN]);
	gfarm_iobuffer_free(cc->iobuffer[FDESC_STDOUT]);
	gfarm_iobuffer_free(cc->iobuffer[FDESC_STDERR]);

	close(stderr_pipe[0]);
	close(stdout_pipe[0]);
	close(stdin_pipe[1]);
	if (use_xauth_env) {
		free(xauth_env);
		unlink(xauth_filename);
		free(xauth);
	}
	for (i = 0; i < nenv; i++)
		free(envp[i]);
	for (i = 0; i < argc; i++)
		free(argv[i]);
	free(shell_env);
	free(home_env);
	free(user_env);
	free(envp);
	free(argv_storage);
	free(path);
}
#endif /* not yet in gfarm v2 */

static void
gfm_client_connect_with_reconnection()
{
	gfarm_error_t e;
	unsigned int sleep_interval = 10;	/* 10 sec */
	unsigned int sleep_max_interval = 640;	/* about 10 min */

	e = gfm_client_connect(gfarm_metadb_server_name,
	    gfarm_metadb_server_port, &gfm_server, listen_addrname);
	while (e != GFARM_ERR_NO_ERROR) {
		/* suppress excessive log */
		if (sleep_interval < sleep_max_interval)
			gflog_error(GFARM_MSG_1000550,
			    "connecting to gfmd at %s:%d failed, "
			    "sleep %d sec: %s", gfarm_metadb_server_name,
			    gfarm_metadb_server_port, sleep_interval,
			    gfarm_error_string(e));
		sleep(sleep_interval);
		e = gfm_client_connect(gfarm_metadb_server_name,
		    gfarm_metadb_server_port, &gfm_server, listen_addrname);
		if (sleep_interval < sleep_max_interval)
			sleep_interval *= 2;
	}

	/*
	 * If canonical_self_name is specified (by the command-line
	 * argument), send the hostname to identify myself.  If not
	 * sending the hostname, the canonical name will be decided by
	 * the gfmd using the reverse lookup of the connected IP
	 * address.
	 */
	if (canonical_self_name != NULL &&
	    (e = gfm_client_hostname_set(gfm_server, canonical_self_name))
	    != GFARM_ERR_NO_ERROR)
		fatal(GFARM_MSG_1000551,
		    "cannot set canonical hostname of this node (%s), "
		    "died: %s\n", canonical_self_name, gfarm_error_string(e));
}

static void
gfm_client_reconnect(void)
{
	gfm_client_connection_free(gfm_server);
	gfm_server = NULL;
	gfm_client_connect_with_reconnection();
}

void
server(int client_fd, char *client_name, struct sockaddr *client_addr)
{
	gfarm_error_t e;
	struct gfp_xdr *client;
	int eof;
	gfarm_int32_t request;
	char *aux, addr_string[GFARM_SOCKADDR_STRLEN];
	enum gfarm_auth_id_type peer_type;
	enum gfarm_auth_method auth_method;

	gfm_client_connect_with_reconnection();

	if (client_name == NULL) { /* i.e. not UNIX domain socket case */
		char *s;
		int port;

		e = gfarm_sockaddr_to_name(client_addr, &client_name);
		if (e != GFARM_ERR_NO_ERROR) {
			gfarm_sockaddr_to_string(client_addr,
			    addr_string, GFARM_SOCKADDR_STRLEN);
			gflog_warning(GFARM_MSG_1000552, "%s: %s", addr_string,
			    gfarm_error_string(e));
			client_name = strdup(addr_string);
			if (client_name == NULL)
				fatal(GFARM_MSG_1000553, "%s: no memory",
				    addr_string);
		}
		e = gfm_host_get_canonical_name(gfm_server, client_name,
		    &s, &port);
		if (e == GFARM_ERR_NO_ERROR) {
			free(client_name);
			client_name = s;
		}
	}

#if 0 /* not yet in gfarm v2 */
	e = gfarm_netparam_config_get_long(&gfarm_netparam_file_read_size,
	    client_name, client_addr, &file_read_size);
	if (e != GFARM_ERR_NO_ERROR) /* shouldn't happen */
		fatal("file_read_size: %s", gfarm_error_string(e));

	e = gfarm_netparam_config_get_long(&gfarm_netparam_rate_limit,
	    client_name, client_addr, &rate_limit);
	if (e != GFARM_ERR_NO_ERROR) /* shouldn't happen */
		fatal("rate_limit: %s", gfarm_error_string(e));
#else
	file_read_size = GFS_PROTO_MAX_IOSIZE;
#endif /* not yet in gfarm v2 */

	e = gfp_xdr_new_socket(client_fd, &client);
	if (e != GFARM_ERR_NO_ERROR) {
		close(client_fd);
		fatal(GFARM_MSG_1000554, "%s: gfp_xdr_new: %s",
		    client_name, gfarm_error_string(e));
	}

	e = gfarm_authorize(client, 0, GFS_SERVICE_TAG,
	    client_name, client_addr,
	    gfarm_auth_uid_to_global_username, gfm_server,
	    &peer_type, &username, &auth_method);
	if (e != GFARM_ERR_NO_ERROR)
		fatal(GFARM_MSG_1000555, "%s: gfarm_authorize: %s",
		    client_name, gfarm_error_string(e));
	GFARM_MALLOC_ARRAY(aux, strlen(username)+1 + strlen(client_name)+1);
	if (aux == NULL)
		fatal(GFARM_MSG_1000556, "%s: no memory\n", client_name);
	sprintf(aux, "%s@%s", username, client_name);
	gflog_set_auxiliary_info(aux);	

	/* set file creation mask */
	command_umask = umask(0);

	for (;;) {
		e = gfp_xdr_recv(client, 0, &eof, "i", &request);
		if (e != GFARM_ERR_NO_ERROR)
			fatal(GFARM_MSG_1000557, "request number: %s",
			    gfarm_error_string(e));
		if (eof) {
			/*
			 * XXX FIXME update metadata of all opened
			 * file descriptor before exit.
			 */
			cleanup(0);
			exit(0);
		}
		switch (request) {
		case GFS_PROTO_PROCESS_SET:
			gfs_server_process_set(client); break;
		case GFS_PROTO_OPEN_LOCAL:
			gfs_server_open_local(client); break;
		case GFS_PROTO_OPEN:	gfs_server_open(client); break;
		case GFS_PROTO_CLOSE:	gfs_server_close(client); break;
		case GFS_PROTO_PREAD:	gfs_server_pread(client); break;
		case GFS_PROTO_PWRITE:	gfs_server_pwrite(client); break;
		case GFS_PROTO_FTRUNCATE: gfs_server_ftruncate(client); break;
		case GFS_PROTO_FSYNC:	gfs_server_fsync(client); break;
		case GFS_PROTO_FSTAT:	gfs_server_fstat(client); break;
		case GFS_PROTO_CKSUM_SET: gfs_server_cksum_set(client); break;
		case GFS_PROTO_STATFS:	gfs_server_statfs(client); break;
#if 0 /* not yet in gfarm v2 */
		case GFS_PROTO_COMMAND:
			if (credential_exported == NULL) {
				e = gfp_xdr_export_credential(client);
				if (e == GFARM_ERR_NO_ERROR)
					credential_exported = client;
				else
					gflog_warning(GFARM_MSG_UNUSED,
					    "export delegated credential: %s",
					    gfarm_error_string(e));
			}
			gfs_server_command(client,
			    credential_exported == NULL ? NULL :
			    gfp_xdr_env_for_credential(client));
			break;
#endif /* not yet in gfarm v2 */
		case GFS_PROTO_REPLICA_ADD_FROM:
			gfs_server_replica_add_from(client); break;
		case GFS_PROTO_REPLICA_RECV:
			gfs_server_replica_recv(client, peer_type); break;
		default:
			gflog_warning(GFARM_MSG_1000558, "unknown request %d",
			    (int)request);
			cleanup(0);
			exit(1);
		}
		if (gfm_client_is_connection_error(
		    gfp_xdr_flush(gfm_client_connection_conn(gfm_server))))
			gfm_client_reconnect();
	}
}

void
start_server(int accepting_sock,
	struct sockaddr *client_addr_storage, socklen_t client_addr_size,
	struct sockaddr *client_addr, char *client_name,
	struct accepting_sockets *accepting)
{
	int i, client = accept(accepting_sock,
	   client_addr_storage, &client_addr_size);

	if (client < 0) {
		if (errno == EINTR || errno == ECONNABORTED ||
#ifdef EPROTO
		    errno == EPROTO ||
#endif
		    errno == EAGAIN)
			return;
		fatal_errno(GFARM_MSG_1000559, "accept");
	}
#ifndef GFSD_DEBUG
	switch (fork()) {
	case 0:
#endif
		for (i = 0; i < accepting->local_socks_count; i++)
			close(accepting->local_socks[i].sock);
		close(accepting->tcp_sock);
		for (i = 0; i < accepting->udp_socks_count; i++)
			close(accepting->udp_socks[i]);

		server(client, client_name, client_addr);
		/*NOTREACHED*/
#ifndef GFSD_DEBUG
	case -1:
		gflog_warning_errno(GFARM_MSG_1000560, "fork");
		/*FALLTHROUGH*/
	default:
		close(client);
		break;
	}
#endif
}

/* XXX FIXME: add protocol magic number and transaction ID */
void
datagram_server(int sock)
{
	int rv;
	struct sockaddr_in client_addr;
	socklen_t client_addr_size = sizeof(client_addr);
	double loadavg[3];
#ifndef WORDS_BIGENDIAN
	struct { char c[8]; } nloadavg[3];
#else
#	define nloadavg loadavg
#endif
	char buffer[1024];

	rv = recvfrom(sock, buffer, sizeof(buffer), 0,
	    (struct sockaddr *)&client_addr, &client_addr_size);
	if (rv == -1)
		return;
	rv = getloadavg(loadavg, GFARM_ARRAY_LENGTH(loadavg));
	if (rv == -1) {
		gflog_warning(GFARM_MSG_1000561,
		    "datagram_server: cannot get load average");
		return;
	}
#ifndef WORDS_BIGENDIAN
	swab(&loadavg[0], &nloadavg[0], sizeof(nloadavg[0]));
	swab(&loadavg[1], &nloadavg[1], sizeof(nloadavg[1]));
	swab(&loadavg[2], &nloadavg[2], sizeof(nloadavg[2]));
#endif
	rv = sendto(sock, nloadavg, sizeof(nloadavg), 0,
	    (struct sockaddr *)&client_addr, sizeof(client_addr));
}

gfarm_int32_t
gfm_async_client_replication_result(void *peer, void *arg, size_t size)
{
	struct gfp_xdr *bc_conn = peer;

	return (gfm_async_client_recv_reply(bc_conn,
	    "gfm_async_client_replication_result", size, ""));
}

/*
 * called from gfp_xdr_async_peer_free() via gfp_xdr_async_xid_free(),
 * when disconnected.
 */
void
gfm_async_client_replication_free(void *peer, void *arg)
{
#if 0
	struct gfp_xdr *bc_conn = peer;
#endif
}

gfarm_error_t
replication_result_notify(struct gfp_xdr *bc_conn,
	gfp_xdr_async_peer_t async, struct gfarm_hash_entry *q)
{
	gfarm_error_t e, e2 = GFARM_ERR_NO_ERROR;
	struct replication_queue_data *qd = gfarm_hash_entry_data(q);
	struct replication_request *rep = qd->head;
	struct replication_errcodes errcodes;
	int rv = read(rep->pipe_fd, &errcodes, sizeof(errcodes)), status;
	struct stat st;
	static const char diag[] = "GFM_PROTO_REPLICATION_RESULT";

	if (rv != sizeof(errcodes)) {
		if (rv == -1) {
			gflog_error(GFARM_MSG_1002191,
			    "%s: cannot read child result: %s",
			    diag, strerror(errno));
		} else {
			gflog_error(GFARM_MSG_1002192,
			    "%s: too short child result: %d bytes", diag, rv);
		}
		errcodes.src_errcode = 0;
		errcodes.dst_errcode = GFARM_ERR_UNKNOWN;
	} else if (fstat(rep->file_fd, &st) == -1) {
		gflog_error(GFARM_MSG_1002193,
		    "%s: cannot stat local fd: %s", diag, strerror(errno));
		if (errcodes.dst_errcode == GFARM_ERR_NO_ERROR)
			errcodes.dst_errcode = GFARM_ERR_UNKNOWN;
	}
	e = gfm_async_client_send_request(bc_conn, async, diag,
	    gfm_async_client_replication_result,
	    gfm_async_client_replication_free,
	    /* rep */ NULL,
	    GFM_PROTO_REPLICATION_RESULT, "llliil",
	    rep->ino, rep->gen, (gfarm_int64_t)rep->pid,
	    errcodes.src_errcode, errcodes.dst_errcode,
	    (gfarm_int64_t)st.st_size);
	close(rep->pipe_fd);
	close(rep->file_fd);
	if ((rv = waitpid(rep->pid, &status, 0)) == -1)
		gflog_warning(GFARM_MSG_1002303,
		    "replication(%lld, %lld): child %d: %s",
		    (long long)rep->ino, (long long)rep->gen, (int)rep->pid,
		    strerror(errno));

	gfs_client_connection_free(rep->src_gfsd);

	rep->ongoing_prev->ongoing_next = rep->ongoing_next;
	rep->ongoing_next->ongoing_prev = rep->ongoing_prev;

	rep = rep->q_next;
	free(qd->head);

	qd->head = rep;
	if (rep == NULL) {
		qd->tail = &qd->head;
	} else {
		qd->tail = &rep->q_next;
		e2 = start_replication(bc_conn, q);
	}

	return (e != GFARM_ERR_NO_ERROR ? e : e2);
}

static int
watch_fds(struct gfp_xdr *conn, gfp_xdr_async_peer_t async)
{
	gfarm_error_t e;
	fd_set fds; /* XXX select FD_SETSIZE */
	struct replication_request *rep, *next;
	int nfound, max_fd;
	struct timeval timeout;

	for (;;) {
		FD_ZERO(&fds);
		max_fd = gfp_xdr_fd(conn);
		FD_SET(max_fd, &fds);
		for (rep = ongoing_replications.ongoing_next;
		    rep != &ongoing_replications; rep = rep->ongoing_next) {
			FD_SET(rep->pipe_fd, &fds);
			if (max_fd < rep->pipe_fd)
				max_fd = rep->pipe_fd;
		}

		timeout.tv_sec = gfarm_metadb_heartbeat_interval * 2;
		timeout.tv_usec = 0;

		nfound = select(max_fd + 1, &fds, NULL, NULL, &timeout);
		if (nfound == 0) {
			gflog_error(GFARM_MSG_1002304,
			    "back channel: gfmd is down");
			return (0);
		}
		if (nfound < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			fatal_errno(GFARM_MSG_1002194, "back channel select");
		}

		for (rep = ongoing_replications.ongoing_next;
		    rep != &ongoing_replications; rep = next) {
			if (FD_ISSET(rep->pipe_fd, &fds)) {
				/*
				 * replication_result_notify() may add an entry
				 * at the tail of the ongoing_replications.
				 * accessing and ignoring the new entry at
				 * further iteration of this loop are both ok.
				 */
				next = rep->ongoing_next;

				/*
				 * the following is necessary to make it
				 * possible to access a new entry in this loop.
				 * note that the new entry may use same pipe_fd
				 * with this rep->pipe_fd.
				 */
				FD_CLR(rep->pipe_fd, &fds);

				e = replication_result_notify(conn, async,
				    rep->q);
				if (e != GFARM_ERR_NO_ERROR) {
					gflog_error(GFARM_MSG_1002385,
					    "back channel: "
					    "communication error: %s",
					    gfarm_error_string(e));
					return (0);
				}
			} else {
				next = rep->ongoing_next;
			}
		}
		if (FD_ISSET(gfp_xdr_fd(conn), &fds))
			return (1);
	}
}

static void
kill_pending_replications(void)
{
	struct gfarm_hash_iterator it;
	struct gfarm_hash_entry *q;
	struct replication_queue_data *qd;
	struct replication_request *rep, *next;

	if (replication_queue_set == NULL)
		return;
	for (gfarm_hash_iterator_begin(replication_queue_set, &it);
	     !gfarm_hash_iterator_is_end(&it);
	     gfarm_hash_iterator_next(&it)) {
		q = gfarm_hash_iterator_access(&it);
		qd = gfarm_hash_entry_data(q);
		if (qd->head == NULL)
			continue;
		/* do not free active replication */
		for (rep = qd->head->q_next; rep != NULL; rep = next) {
			next = rep->q_next;
			gflog_debug(GFARM_MSG_UNFIXED,
			    "forget pending replication request "
			    "%s:%d %lld:%lld",
			    gfp_conn_hash_hostname(q), gfp_conn_hash_port(q),
			    (long long)rep->ino, (long long)rep->gen);
			free(rep);
		}
		qd->head->q_next = NULL;
		qd->tail = &qd->head->q_next;		
	}
}

static void
back_channel_server(void)
{
	gfarm_error_t e;
	struct gfm_connection *back_channel;
	struct gfp_xdr *bc_conn;
	gfp_xdr_async_peer_t async;
	enum gfp_xdr_msg_type type;
	gfp_xdr_xid_t xid;
	size_t size;
	gfarm_int32_t gfmd_knows_me, rv, request;

	static int hack_to_make_cookie_not_work = 0; /* XXX FIXME */

	for (;;) {
		e = gfm_client_switch_async_back_channel(gfm_server,
		    GFS_PROTOCOL_VERSION,
		    (gfarm_int64_t)(getpid() + hack_to_make_cookie_not_work++),
		    &gfmd_knows_me);
		if (e != GFARM_ERR_NO_ERROR) {
			/*
			 * gfmd has to be newer than gfsd.
			 * so we won't try GFM_PROTO_SWITCH_BACK_CHANNEL,
			 * if GFM_PROTO_SWITCH_TO_ASYNC_BACK_CHANNEL is
			 * not supported.
			 */
			fatal(GFARM_MSG_1000562,
			    "cannot switch to async back channel: %s",
			    gfarm_error_string(e));
		}
		e = gfp_xdr_async_peer_new(&async);
		if (e != GFARM_ERR_NO_ERROR) {
			fatal(GFARM_MSG_1002195,
			    "cannot allocate resource for async protocol: %s",
			    gfarm_error_string(e));
		}

		back_channel = gfm_server;
		bc_conn = gfm_client_connection_conn(gfm_server);
 
		/* create another gfmd connection for a foreground channel */
		gfm_server = NULL;
		gfm_client_connect_with_reconnection();

		gflog_debug(GFARM_MSG_1000563, "back channel mode");
		for (;;) {
			if (!gfp_xdr_recv_is_ready(bc_conn)) {
				if (!watch_fds(bc_conn, async))
					break;
			}

			e = gfp_xdr_recv_async_header(bc_conn, 0,
			    &type, &xid, &size);
			if (e != GFARM_ERR_NO_ERROR) {
				if (e == GFARM_ERR_UNEXPECTED_EOF) {
					gflog_error(GFARM_MSG_1002386,
					    "back channel disconnected");
				} else {
					gflog_error(GFARM_MSG_1002387,
					    "back channel RPC protocol error, "
					    "reset: %s", gfarm_error_string(e));
				}
				break;
			}
			if (type == GFP_XDR_TYPE_RESULT) {
				e = gfp_xdr_callback_async_result(async,
				    bc_conn, xid, size, &rv);
				if (e != GFARM_ERR_NO_ERROR) {
					gflog_warning(GFARM_MSG_1002196,
					    "(back channel) unknown reply "
					    "xid:%d size:%d",
					    (int)xid, (int)size);
					e = gfp_xdr_purge(bc_conn, 0, size);
					if (e != GFARM_ERR_NO_ERROR) {
						gflog_error(GFARM_MSG_1002197,
						    "skipping %d bytes: %s",
						    (int)size,
						    gfarm_error_string(e));
						break;
					}
				} else if (IS_CONNECTION_ERROR(rv)) {
					gflog_error(GFARM_MSG_1002198,
					    "back channel result: %s",
					    gfarm_error_string(e));
					break;
				}
				continue;
			} else if (type != GFP_XDR_TYPE_REQUEST) {
				fatal(GFARM_MSG_1002199,
				    "async_back_channel_service: type %d",
				    type);
			}
			e = gfp_xdr_recv_request_command(bc_conn, 0, &size,
			    &request);
			if (e != GFARM_ERR_NO_ERROR) {
				if (e == GFARM_ERR_UNEXPECTED_EOF) {
					gflog_error(GFARM_MSG_1000564,
					    "back channel disconnected");
				} else {
					gflog_error(GFARM_MSG_1000565,
					    "(back channel) request error, "
					    "reset: %s", gfarm_error_string(e));
				}
				break;
			}
			switch (request) {
			case GFS_PROTO_FHSTAT:
				e = gfs_async_server_fhstat(
				    bc_conn, xid, size);
				break;
			case GFS_PROTO_FHREMOVE:
				e = gfs_async_server_fhremove(
				    bc_conn, xid, size);
				break;
			case GFS_PROTO_STATUS:
				e = gfs_async_server_status(
				    bc_conn, xid, size);
				break;
			case GFS_PROTO_REPLICATION_REQUEST:
				e = gfs_async_server_replication_request(
				    bc_conn, xid, size);
				break;
			default:
				gflog_error(GFARM_MSG_1000566,
				    "(back channel) unknown request %d "
				    "(xid:%d size:%d), skip",
				    (int)request, (int)xid, (int)size);
				e = gfp_xdr_purge(bc_conn, 0, size);
				if (e != GFARM_ERR_NO_ERROR) {
					gflog_error(GFARM_MSG_1002200,
					    "skipping %d bytes: %s",
					    (int)size, gfarm_error_string(e));
				}
				break;
			}
			if (e != GFARM_ERR_NO_ERROR)
				break;
		}

		kill_pending_replications();

		/* free the foreground channel */
		gfm_client_connection_free(gfm_server);

		gfp_xdr_async_peer_free(async, bc_conn);
		gfm_server = back_channel;
		gfm_client_reconnect();
	}
}

static void
start_back_channel_server(void)
{
	pid_t pid;

	pid = fork();
	switch (pid) {
	case 0:
		back_channel_gfsd_pid = getpid();
		back_channel_server();
		/*NOTREACHED*/
	case -1:
		gflog_warning_errno(GFARM_MSG_1000567, "fork");
		/*FALLTHROUGH*/
	default:
		back_channel_gfsd_pid = pid;
		gfm_client_connection_free(gfm_server);
		gfm_server = NULL;
		break;
	}
}

int
open_accepting_tcp_socket(struct in_addr address, int port)
{
	gfarm_error_t e;
	struct sockaddr_in self_addr;
	socklen_t self_addr_size;
	int sock, sockopt;

	memset(&self_addr, 0, sizeof(self_addr));
	self_addr.sin_family = AF_INET;
	self_addr.sin_addr = address;
	self_addr.sin_port = htons(port);
	self_addr_size = sizeof(self_addr);
	sock = socket(PF_INET, SOCK_STREAM, 0);
	if (sock < 0)
		accepting_fatal_errno(GFARM_MSG_1000568, "accepting socket");
	sockopt = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
	    &sockopt, sizeof(sockopt)) == -1)
		gflog_warning_errno(GFARM_MSG_1000569, "SO_REUSEADDR");
	if (bind(sock, (struct sockaddr *)&self_addr, self_addr_size) < 0)
		accepting_fatal_errno(GFARM_MSG_1000570,
		    "bind accepting socket");
	e = gfarm_sockopt_apply_listener(sock);
	if (e != GFARM_ERR_NO_ERROR)
		gflog_warning(GFARM_MSG_1000571, "setsockopt: %s",
		    gfarm_error_string(e));
	if (listen(sock, LISTEN_BACKLOG) < 0)
		accepting_fatal_errno(GFARM_MSG_1000572, "listen");
	return (sock);
}

void
open_accepting_local_socket(struct in_addr address, int port,
	struct local_socket *result)
{
	struct sockaddr_un self_addr;
	socklen_t self_addr_size;
	int sock, save_errno;
	char *sock_name, *sock_dir, dir_buf[PATH_MAX];
	struct stat st;

	memset(&self_addr, 0, sizeof(self_addr));
	self_addr.sun_family = AF_UNIX;
	snprintf(self_addr.sun_path, sizeof self_addr.sun_path,
	    GFSD_LOCAL_SOCKET_NAME, inet_ntoa(address), port);
	self_addr_size = sizeof(self_addr);

	snprintf(dir_buf, sizeof dir_buf,
	    GFSD_LOCAL_SOCKET_DIR, inet_ntoa(address), port);

	sock_name = strdup(self_addr.sun_path);
	sock_dir = strdup(dir_buf);
	if (sock_name == NULL || sock_dir == NULL)
		accepting_fatal(GFARM_MSG_1000573, "not enough memory");

	/* to make sure */
	if (unlink(sock_name) == 0)
		gflog_info(GFARM_MSG_1002441,
		    "%s: remaining socket found and removed", sock_name);
	else if (errno != ENOENT)
		accepting_fatal_errno(GFARM_MSG_1002442,
		    "%s: failed to remove remaining socket", sock_name);
	if (rmdir(sock_dir) == 0)
		gflog_info(GFARM_MSG_1002443,
		    "%s: remaining socket directory found and removed",
		    sock_dir);
	else if (errno != ENOENT) /* something wrong, but tries to continue */
		gflog_error_errno(GFARM_MSG_1002444,
		    "%s: failed to remove remaining socket directory",
		    sock_dir);

	if (mkdir(sock_dir, LOCAL_SOCKDIR_MODE) == -1) {
		if (errno != EEXIST) {
			accepting_fatal_errno(GFARM_MSG_1000574,
			    "%s: cannot mkdir", sock_dir);
		} else if (stat(sock_dir, &st) != 0) {
			accepting_fatal_errno(GFARM_MSG_1000575, "stat(%s)",
			    sock_dir);
		} else if (st.st_uid != gfsd_uid) {
			accepting_fatal(GFARM_MSG_1000576,
			    "%s: not owned by uid %d", sock_dir, gfsd_uid);
		} else if ((st.st_mode & PERMISSION_MASK) != LOCAL_SOCKDIR_MODE
		    && chmod(sock_dir, LOCAL_SOCKDIR_MODE) != 0) {
			accepting_fatal_errno(GFARM_MSG_1000577,
			    "%s: cannot chmod to 0%o",
			    sock_dir, LOCAL_SOCKDIR_MODE);
		}
	}
	if (chown(sock_dir, gfsd_uid, -1) == -1)
		gflog_warning_errno(GFARM_MSG_1002201, "chown(%s, %d)",
		    sock_dir, (int)gfsd_uid);

	sock = socket(PF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		save_errno = errno;
		if (rmdir(sock_dir) == -1)
			gflog_error_errno(GFARM_MSG_1002388,
			    "rmdir(%s)", sock_dir);
		accepting_fatal(GFARM_MSG_1000578,
		    "creating UNIX domain socket: %s",
		    strerror(save_errno));
	}
	if (bind(sock, (struct sockaddr *)&self_addr, self_addr_size) == -1) {
		save_errno = errno;
		if (rmdir(sock_dir) == -1)
			gflog_error_errno(GFARM_MSG_1002389,
			    "rmdir(%s)", sock_dir);
		accepting_fatal(GFARM_MSG_1000579,
		    "%s: cannot bind UNIX domain socket: %s",
		    sock_name, strerror(save_errno));
	}
	if (chown(sock_name, gfsd_uid, -1) == -1)
		gflog_warning_errno(GFARM_MSG_1002202, "chown(%s, %d)",
		    sock_name, gfsd_uid);
	/* ensure access from all user, Linux at least since 2.4 needs this. */
	if (chmod(sock_name, LOCAL_SOCKET_MODE) == -1)
		gflog_debug_errno(GFARM_MSG_1002390, "chmod(%s, 0%o)",
		    sock_name, (int)LOCAL_SOCKET_MODE);

	if (listen(sock, LISTEN_BACKLOG) == -1) {
		save_errno = errno;
		if (unlink(sock_name) == -1)
			gflog_error_errno(GFARM_MSG_1002391,
			    "unlink(%s)", sock_name);
		if (rmdir(sock_dir) == -1)
			gflog_error_errno(GFARM_MSG_1002392,
			    "rmdir(%s)", sock_dir);
		accepting_fatal(GFARM_MSG_1000580,
		    "listen UNIX domain socket: %s", strerror(save_errno));
	}

	result->sock = sock;
	result->name = sock_name;
	result->dir = sock_dir;
}

void
open_accepting_local_sockets(
	int self_addresses_count, struct in_addr *self_addresses, int port,
	struct accepting_sockets *accepting)
{
	int i;

	GFARM_MALLOC_ARRAY(accepting->local_socks, self_addresses_count);
	if (accepting->local_socks == NULL)
		accepting_fatal(GFARM_MSG_1000581,
		    "not enough memory for UNIX sockets");

	for (i = 0; i < self_addresses_count; i++) {
		open_accepting_local_socket(self_addresses[i], port,
		    &accepting->local_socks[i]);

		/* for cleanup_accepting() */
		accepting->local_socks_count = i + 1;
	}
}

int
open_udp_socket(struct in_addr address, int port)
{
	struct sockaddr_in bind_addr;
	socklen_t bind_addr_size;
	int s;

	memset(&bind_addr, 0, sizeof(bind_addr));
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr = address;
	bind_addr.sin_port = ntohs(port);
	bind_addr_size = sizeof(bind_addr);
	s = socket(PF_INET, SOCK_DGRAM, 0);
	if (s < 0)
		accepting_fatal_errno(GFARM_MSG_1000582, "UDP socket");
	if (bind(s, (struct sockaddr *)&bind_addr, bind_addr_size) < 0)
		accepting_fatal_errno(GFARM_MSG_1000583,
		    "UDP socket bind(%s, %d)",
		    inet_ntoa(address), port);
	return (s);
}

int *
open_datagram_service_sockets(
	int self_addresses_count, struct in_addr *self_addresses, int port)
{
	int i, *sockets;

	GFARM_MALLOC_ARRAY(sockets, self_addresses_count);
	if (sockets == NULL)
		accepting_fatal(GFARM_MSG_1000584,
		    "no memory for %d datagram sockets",
		    self_addresses_count);
	for (i = 0; i < self_addresses_count; i++)
		sockets[i] = open_udp_socket(self_addresses[i], port);
	return (sockets);
}

void
usage(void)
{
	fprintf(stderr, "Usage: %s [option]\n", program_name);
	fprintf(stderr, "option:\n");
	fprintf(stderr, "\t-L <syslog-priority-level>\n");
	fprintf(stderr, "\t-P <pid-file>\n");
	fprintf(stderr, "\t-c\t\t\t\t... check and display invalid files\n");
	fprintf(stderr, "\t-cc\t\t\t\t... check and delete invalid files\n");
	fprintf(stderr, "\t-d\t\t\t\t... debug mode\n");
	fprintf(stderr, "\t-f <gfarm-configuration-file>\n");
	fprintf(stderr, "\t-h <hostname>\n");
	fprintf(stderr, "\t-l <listen_address>\n");
	fprintf(stderr, "\t-r <spool_root>\n");
	fprintf(stderr, "\t-s <syslog-facility>\n");
	fprintf(stderr, "\t-v\t\t\t\t... make authentication log verbose\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	struct sockaddr_in client_addr, *self_sockaddr_array;
	struct sockaddr_un client_local_addr;
	gfarm_error_t e, e2;
	char *config_file = NULL, *pid_file = NULL;
	char *local_gfsd_user;
	struct gfarm_host_info self_info;
	struct passwd *gfsd_pw;
	FILE *pid_fp = NULL;
	int syslog_facility = GFARM_DEFAULT_FACILITY;
	int syslog_level = -1;
	struct in_addr *self_addresses, listen_address;
	int table_size, self_addresses_count, ch, i, nfound, max_fd, p;
	struct sigaction sa;
	fd_set requests;
	struct stat sb;
	int spool_check_level = 0;
	int is_root = geteuid() == 0;

	if (argc >= 1)
		program_name = basename(argv[0]);
	gflog_set_identifier(program_name);

	while ((ch = getopt(argc, argv, "L:P:dcf:h:l:r:s:uv")) != -1) {
		switch (ch) {
		case 'L':
			syslog_level = gflog_syslog_name_to_priority(optarg);
			if (syslog_level == -1)
				gflog_fatal(GFARM_MSG_1000585,
				    "-L %s: invalid syslog priority", optarg);
			break;
		case 'P':
			pid_file = optarg;
			break;
		case 'c':
			++spool_check_level;
			break;
		case 'd':
			debug_mode = 1;
			if (syslog_level == -1)
				syslog_level = LOG_DEBUG;
			break;
		case 'f':
			config_file = optarg;
			break;
		case 'h':
			canonical_self_name = optarg;
			break;
		case 'l':
			listen_addrname = optarg;
			break;
		case 'r':
			gfarm_spool_root = strdup(optarg);
			if (gfarm_spool_root == NULL)
				gflog_fatal(GFARM_MSG_1000586, "%s",
				    gfarm_error_string(GFARM_ERR_NO_MEMORY));
			break;
		case 's':
			syslog_facility =
			    gflog_syslog_name_to_facility(optarg);
			if (syslog_facility == -1)
				gflog_fatal(GFARM_MSG_1000587,
				    "%s: unknown syslog facility", optarg);
			break;
		case 'u':
			restrict_user = 1;
			restricted_user = getuid();
			break;
		case 'v':
			gflog_auth_set_verbose(1);
			break;
		case '?':
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (config_file != NULL)
		gfarm_config_set_filename(config_file);
	e = gfarm_server_initialize();
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "gfarm_server_initialize: %s\n",
		    gfarm_error_string(e));
		exit(1);
	}
	if (syslog_level != -1)
		gflog_set_priority_level(syslog_level);

	e = gfarm_global_to_local_username(GFSD_USERNAME, &local_gfsd_user);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "no local user for the global `%s' user.\n",
		    GFSD_USERNAME);
		exit(1);
	}
	gfsd_pw = getpwnam(local_gfsd_user);
	if (gfsd_pw == NULL) {
		fprintf(stderr, "user `%s' is necessary, but doesn't exist.\n",
		    local_gfsd_user);
		exit(1);
	}
	gfsd_uid = gfsd_pw->pw_uid;

	if (seteuid(gfsd_uid) == -1 && is_root)
		gflog_error_errno(GFARM_MSG_1002393,
		    "seteuid(%d)", (int)gfsd_uid);

	e = gfarm_set_local_user_for_this_local_account();
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "acquiring information about user `%s': %s\n",
		    local_gfsd_user, gfarm_error_string(e));
		exit(1);
	}
	free(local_gfsd_user);
	e = gfarm_set_global_username(GFSD_USERNAME);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, ": gfarm_set_global_username: %s\n",
		    gfarm_error_string(e));
		exit(1);
	}

	/* sanity check on a spool directory */
	if (stat(gfarm_spool_root, &sb) == -1)
		gflog_fatal_errno(GFARM_MSG_1000588, "%s", gfarm_spool_root);
	else if (!S_ISDIR(sb.st_mode))
		gflog_fatal(GFARM_MSG_1000589, "%s: %s", gfarm_spool_root,
		    gfarm_error_string(GFARM_ERR_NOT_A_DIRECTORY));

	if (pid_file != NULL) {
		/*
		 * We do this before calling gfarm_daemon()
		 * to print the error message to stderr.
		 */
		if (seteuid(0) == -1 && is_root)
			gflog_error_errno(GFARM_MSG_1002394, "seteuid(0)");
		pid_fp = fopen(pid_file, "w");
		if (seteuid(gfsd_uid) == -1 && is_root)
			gflog_error_errno(GFARM_MSG_1002395,
			    "seteuid(%d)", (int)gfsd_uid);
		if (pid_fp == NULL)
			accepting_fatal_errno(GFARM_MSG_1000590,
				"failed to open file: %s", pid_file);
	}

	if (!debug_mode) {
		gflog_syslog_open(LOG_PID, syslog_facility);
		if (gfarm_daemon(0, 0) == -1)
			gflog_warning_errno(GFARM_MSG_1002203, "daemon");
	}

	/* We do this after calling gfarm_daemon(), because it changes pid. */
	master_gfsd_pid = getpid();
	sa.sa_handler = cleanup_handler;
	if (sigemptyset(&sa.sa_mask) == -1)
		gflog_fatal_errno(GFARM_MSG_1002396, "sigemptyset()");
	sa.sa_flags = 0;
	if (sigaction(SIGHUP, &sa, NULL) == -1) /* XXX - need to restart gfsd */
		gflog_fatal_errno(GFARM_MSG_1002397, "sigaction(SIGHUP)");
	if (sigaction(SIGINT, &sa, NULL) == -1)
		gflog_fatal_errno(GFARM_MSG_1002398, "sigaction(SIGINT)");
	if (sigaction(SIGTERM, &sa, NULL) == -1)
		gflog_fatal_errno(GFARM_MSG_1002399, "sigaction(SIGTERM)");

	if (pid_file != NULL) {
		if (fprintf(pid_fp, "%ld\n", (long)master_gfsd_pid) == -1)
			gflog_error_errno(GFARM_MSG_1002400,
			    "writing PID to %s", pid_file);
		if (fclose(pid_fp) != 0)
			gflog_error_errno(GFARM_MSG_1002401,
			    "fclose(%s)", pid_file);
	}

	gfarm_set_auth_id_type(GFARM_AUTH_ID_TYPE_SPOOL_HOST);
	gfm_client_connect_with_reconnection();
	/*
	 * in case of canonical_self_name != NULL, get_canonical_self_name()
	 * cannot be used because host_get_self_name() may not be registered.
	 */
	if (canonical_self_name == NULL &&
	    (e = gfm_host_get_canonical_self_name(gfm_server,
	    &canonical_self_name, &p)) != GFARM_ERR_NO_ERROR) {
		gflog_fatal(GFARM_MSG_1000591,
		    "cannot get canonical hostname of %s, ask admin to "
		    "register this node in Gfarm metadata server, died: %s\n",
		    gfarm_host_get_self_name(), gfarm_error_string(e));
	}
	/* avoid gcc warning "passing arg 3 from incompatible pointer type" */
	{	
		const char *n = canonical_self_name;

		e = gfm_client_host_info_get_by_names(gfm_server,
		    1, &n, &e2, &self_info);
	}
	if (e == GFARM_ERR_NO_ERROR)
		e = e2;
	if (e != GFARM_ERR_NO_ERROR) {
		gflog_fatal(GFARM_MSG_1000592,
		    "cannot get canonical hostname of %s, ask admin to "
		    "register this node in Gfarm metadata server, died: %s\n",
		    canonical_self_name, gfarm_error_string(e));
	}

	if (seteuid(0) == -1 && is_root)
		gflog_error_errno(GFARM_MSG_1002402, "seteuid(0)");

	if (listen_addrname == NULL)
		listen_addrname = gfarm_spool_server_listen_address;
	if (listen_addrname == NULL) {
		e = gfarm_get_ip_addresses(
		    &self_addresses_count, &self_addresses);
		if (e != GFARM_ERR_NO_ERROR)
			gflog_fatal(GFARM_MSG_1000593, "get_ip_addresses: %s",
			    gfarm_error_string(e));
		listen_address.s_addr = INADDR_ANY;
	} else {
		struct hostent *hp = gethostbyname(listen_addrname);

		if (hp == NULL || hp->h_addrtype != AF_INET)
			gflog_fatal(GFARM_MSG_1000594,
			    "listen address can't be resolved: %s",
			    listen_addrname);
		self_addresses_count = 1;
		GFARM_MALLOC(self_addresses);
		if (self_addresses == NULL)
			gflog_fatal(GFARM_MSG_1000595, "%s",
			    gfarm_error_string(GFARM_ERR_NO_MEMORY));
		memcpy(self_addresses, hp->h_addr, sizeof(*self_addresses));
		listen_address = *self_addresses;
	}
	GFARM_MALLOC_ARRAY(self_sockaddr_array, self_addresses_count);
	if (self_sockaddr_array == NULL)
		gflog_fatal(GFARM_MSG_1000596, "%s",
			    gfarm_error_string(GFARM_ERR_NO_MEMORY));
	for (i = 0; i < self_addresses_count; i++) {
		memset(&self_sockaddr_array[i], 0,
		    sizeof(self_sockaddr_array[i]));
		self_sockaddr_array[i].sin_family = AF_INET;
		self_sockaddr_array[i].sin_addr = self_addresses[i];
		self_sockaddr_array[i].sin_port = htons(self_info.port);
	}

	accepting.tcp_sock = open_accepting_tcp_socket(
	    listen_address, self_info.port);
	/* sets accepting.local_socks_count and accepting.local_socks */
	open_accepting_local_sockets(
	    self_addresses_count, self_addresses, self_info.port,
	    &accepting);
	accepting.udp_socks = open_datagram_service_sockets(
	    self_addresses_count, self_addresses, self_info.port);
	accepting.udp_socks_count = self_addresses_count;

	max_fd = accepting.tcp_sock;
	for (i = 0; i < accepting.local_socks_count; i++) {
		if (max_fd < accepting.local_socks[i].sock)
			max_fd = accepting.local_socks[i].sock;
	}
	for (i = 0; i < accepting.udp_socks_count; i++) {
		if (max_fd < accepting.udp_socks[i])
			max_fd = accepting.udp_socks[i];
	}
	if (max_fd > FD_SETSIZE)
		accepting_fatal(GFARM_MSG_1000597,
		    "too big socket file descriptor: %d", max_fd);

	if (seteuid(gfsd_uid) == -1) {
		int save_errno = errno;

		if (geteuid() == 0)
			gflog_error(GFARM_MSG_1002403,
			    "seteuid(%d): %s", gfsd_uid, strerror(save_errno));
	}

	/* XXX - kluge for gfrcmd (to mkdir HOME....) for now */
	/* XXX - kluge for GFS_PROTO_STATFS for now */
	if (chdir(gfarm_spool_root) == -1)
		gflog_fatal_errno(GFARM_MSG_1000598, "chdir(%s)",
		    gfarm_spool_root);

	/* spool check */
	if (spool_check_level > 0)
		(void)gfsd_spool_check(spool_check_level);

	/*
	 * We don't want SIGPIPE, but want EPIPE on write(2)/close(2).
	 */
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		gflog_fatal_errno(GFARM_MSG_1002404,
		    "signal(SIGPIPE, SIG_IGN)");

	/* start back channel server */
	start_back_channel_server();

	table_size = FILE_TABLE_LIMIT;
	gfarm_unlimit_nofiles(&table_size);
	if (table_size > FILE_TABLE_LIMIT)
		table_size = FILE_TABLE_LIMIT;
	file_table_init(table_size);
	OpenSSL_add_all_digests(); /* for EVP_get_digestbyname() */

	/*
	 * Because SA_NOCLDWAIT is not implemented on some OS,
	 * we do not rely on the feature.
	 */
	sa.sa_handler = sigchld_handler;
	if (sigemptyset(&sa.sa_mask) == -1)
		gflog_fatal_errno(GFARM_MSG_1002405, "sigemptyset");
	sa.sa_flags = SA_NOCLDSTOP;
	if (sigaction(SIGCHLD, &sa, NULL) == -1)
		gflog_fatal_errno(GFARM_MSG_1002406, "sigaction(SIGCHLD)");

	/*
	 * To deal with race condition which may be caused by RST,
	 * listening socket must be O_NONBLOCK, if the socket will be
	 * used as a file descriptor for select(2) .
	 * See section 16.6 of "UNIX NETWORK PROGRAMMING, Volume1,
	 * Third Edition" by W. Richard Stevens, for detail.
	 */
	if (fcntl(accepting.tcp_sock, F_SETFL,
	    fcntl(accepting.tcp_sock, F_GETFL, NULL) | O_NONBLOCK) == -1)
		gflog_warning_errno(GFARM_MSG_1000599,
		    "accepting TCP socket O_NONBLOCK");

	for (;;) {
		FD_ZERO(&requests);
		FD_SET(accepting.tcp_sock, &requests);
		for (i = 0; i < accepting.local_socks_count; i++)
			FD_SET(accepting.local_socks[i].sock, &requests);
		for (i = 0; i < accepting.udp_socks_count; i++)
			FD_SET(accepting.udp_socks[i], &requests);
		nfound = select(max_fd + 1, &requests, NULL, NULL, NULL);
		if (nfound <= 0) {
			if (nfound == 0 || errno == EINTR || errno == EAGAIN)
				continue;
			fatal_errno(GFARM_MSG_1000600, "select");
		}

		if (FD_ISSET(accepting.tcp_sock, &requests)) {
			start_server(accepting.tcp_sock,
			    (struct sockaddr*)&client_addr,sizeof(client_addr),
			    (struct sockaddr*)&client_addr, NULL, &accepting);
		}
		for (i = 0; i < accepting.local_socks_count; i++) {
			if (FD_ISSET(accepting.local_socks[i].sock,&requests)){
				start_server(accepting.local_socks[i].sock,
				    (struct sockaddr *)&client_local_addr,
				    sizeof(client_local_addr),
				    (struct sockaddr*)&self_sockaddr_array[i],
				    canonical_self_name,
				    &accepting);
			}
		}
		for (i = 0; i < accepting.udp_socks_count; i++) {
			if (FD_ISSET(accepting.udp_socks[i], &requests))
				datagram_server(accepting.udp_socks[i]);
		}
	}
	/*NOTREACHED*/
#ifdef __GNUC__ /* to shut up warning */
	return (0);
#endif
}
