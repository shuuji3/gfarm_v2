#include <stddef.h>
#include <stdarg.h>

#include <gfarm/gfarm.h>

#include "id_table.h"

#include "liberror.h"
#include "gfp_xdr.h"

/*
 * asynchronous RPC related functions
 */

#define XID_TYPE_BIT		0x80000000
#define XID_TYPE_REQUEST	0x00000000
#define XID_TYPE_RESULT		0x80000000

struct gfp_xdr_async_callback {
	gfarm_int32_t (*callback)(void *, void *, size_t);
	void *closure;
};

static struct gfarm_id_table_entry_ops gfp_xdr_async_xid_table_ops = {
	sizeof(struct gfp_xdr_async_callback)
};

/*
 * asynchronous protocol client side functions
 *
 * currently, all clients of asynchronous protocl are servers too.
 */

gfarm_error_t
gfp_xdr_async_peer_new(gfp_xdr_async_peer_t *async_serverp)
{
	struct gfarm_id_table *idtab =
		gfarm_id_table_alloc(&gfp_xdr_async_xid_table_ops);

	if (idtab == NULL)
		return (GFARM_ERR_NO_MEMORY);
	*async_serverp = idtab;
	return (GFARM_ERR_NO_ERROR);
}

void
gfp_xdr_async_peer_free(gfp_xdr_async_peer_t async_server,
	void (*rpc_free)(void *, gfp_xdr_xid_t, void *), void *closure)
{
	gfarm_id_table_free(async_server, rpc_free, closure);
}

gfarm_error_t
gfp_xdr_callback_async_result(gfp_xdr_async_peer_t async_server,
	void *peer, gfp_xdr_xid_t xid, size_t size, gfarm_int32_t *resultp)
{
	struct gfp_xdr_async_callback *cb = gfarm_id_lookup(async_server, xid);
	gfarm_int32_t (*callback)(void *, void *, size_t);
	void *closure;

	if (cb == NULL)
		return (GFARM_ERR_NO_SUCH_OBJECT);
	callback = cb->callback;
	closure = cb->closure;
	gfarm_id_free(async_server, xid);
	*resultp = (*callback)(peer, closure, size);
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfp_xdr_vsend_async_request(struct gfp_xdr *server,
	gfp_xdr_async_peer_t async_server,
	gfarm_int32_t (*callback)(void *, void *, size_t), void *closure,
	gfarm_int32_t command, const char *format, va_list *app)
{
	gfarm_error_t e;
	size_t size = 0;
	va_list ap;
	const char *fmt;

	e = gfp_xdr_send_size_add(&size, "i", command);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	va_copy(ap, *app);
	fmt = format;
	e = gfp_xdr_vsend_size_add(&size, &fmt, &ap);
	va_end(ap);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);

	e = gfp_xdr_send_async_request_header(server, async_server, size,
	    callback, closure);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	e = gfp_xdr_vrpc_request(server, command, &format, app);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (*format != '\0') {
		gflog_debug(GFARM_MSG_1001016, "gfp_xdr_vsend_async_request: "
		    "invalid format character: %c(%x)", *format, *format);
		return (GFARM_ERRMSG_GFP_XDR_VRPC_INVALID_FORMAT_CHARACTER);
	}
	return (GFARM_ERR_NO_ERROR);
}

gfarm_error_t
gfp_xdr_send_async_request_header(struct gfp_xdr *server,
	gfp_xdr_async_peer_t async_server, size_t size,
	gfarm_int32_t (*callback)(void *, void *, size_t), void *closure)
{
	gfarm_error_t e;
	gfarm_int32_t xid, xid_and_type;
	struct gfp_xdr_async_callback *cb = gfarm_id_alloc(async_server, &xid);

	if (cb == NULL)
		return (GFARM_ERR_NO_MEMORY);
	cb->callback = callback;
	cb->closure = closure;
	xid_and_type = (xid | XID_TYPE_REQUEST);
	e = gfp_xdr_send(server, "ii", xid_and_type, (gfarm_int32_t)size);
	if (e != GFARM_ERR_NO_ERROR) {
		gfarm_id_free(async_server, xid);
		return (e);
	}
	return (GFARM_ERR_NO_ERROR);
}

/*
 * used by both client and server side
 */

gfarm_error_t
gfp_xdr_recv_async_header(struct gfp_xdr *conn, int just,
	enum gfp_xdr_msg_type *typep, gfp_xdr_xid_t *xidp, size_t *sizep)
{
	gfarm_error_t e;
	gfarm_uint32_t xid;
	gfarm_uint32_t size;
	int eof;

	e = gfp_xdr_recv(conn, just, &eof, "ii", &xid, &size);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (eof)
		return (GFARM_ERR_UNEXPECTED_EOF);
	*typep = (xid & XID_TYPE_BIT) == XID_TYPE_REQUEST ?
	    GFP_XDR_TYPE_REQUEST : GFP_XDR_TYPE_RESULT;
	*xidp = (xid & ~XID_TYPE_BIT);
	*sizep = size;
	return (GFARM_ERR_NO_ERROR);
}

/*
 * server side functions
 */

gfarm_error_t
gfp_xdr_send_async_result_header(struct gfp_xdr *server,
	gfarm_int32_t xid, size_t size)
{
	xid = (xid | XID_TYPE_RESULT);
	return (gfp_xdr_send(server, "ii", xid, (gfarm_int32_t)size));
}

/*
 * used by both synchronous and asynchronous protocol.
 * if sizep == NULL, it's a synchronous protocol, otherwise asynchronous.
 * Note that this function assumes that async_header is already received.
 */
gfarm_error_t
gfp_xdr_recv_request_command(struct gfp_xdr *client, int just, size_t *sizep,
	gfarm_int32_t *commandp)
{
	gfarm_error_t e;
	int eof;

	e = gfp_xdr_recv_sized(client, just, sizep, &eof, "i", commandp);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (eof)
		return (GFARM_ERR_UNEXPECTED_EOF);
	return (GFARM_ERR_NO_ERROR);
}

/*
 * used by both synchronous and asynchronous protocol.
 * if sizep == NULL, it's a synchronous protocol, otherwise asynchronous.
 */
gfarm_error_t
gfp_xdr_vrecv_request_parameters(struct gfp_xdr *client, int just,
	size_t *sizep, const char *format, va_list *app)
{
	gfarm_error_t e;
	int eof;

	e = gfp_xdr_vrecv_sized(client, just, sizep, &eof, &format, app);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (eof)
		return (GFARM_ERR_UNEXPECTED_EOF);
	if (*format != '\0') {
		gflog_debug(GFARM_MSG_1001017,
		    "gfp_xdr_vrecv_request_parameters: "
		    "invalid format character: %c(%x)", *format, *format);
		return (GFARM_ERRMSG_GFP_XDR_VRPC_INVALID_FORMAT_CHARACTER);
	}
	if (sizep != NULL && *sizep != 0) {
		gflog_debug(GFARM_MSG_1001018,
		    "gfp_xdr_vrecv_request_parameters: residual %d bytes",
		    (int)*sizep);
		return (GFARM_ERR_PROTOCOL);
	}
	return (GFARM_ERR_NO_ERROR);
}

/* the caller should call gfp_xdr_flush() after this function */
gfarm_error_t
gfp_xdr_vsend_result(struct gfp_xdr *client,
	gfarm_int32_t ecode, const char *format, va_list *app)
{
	gfarm_error_t e;

	e = gfp_xdr_send(client, "i", ecode);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (ecode == GFARM_ERR_NO_ERROR) {
		e = gfp_xdr_vsend(client, &format, app);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
		if (*format != '\0') {
			gflog_debug(GFARM_MSG_1001019, "gfp_xdr_vsend_result: "
			    "invalid format character: %c(%x)",
			    *format, *format);
			e = GFARM_ERRMSG_GFP_XDR_VRPC_INVALID_FORMAT_CHARACTER;
		}
	}
	return (e);
}

/* used by asynchronous protocol */
gfarm_error_t
gfp_xdr_vsend_async_result(struct gfp_xdr *client, gfp_xdr_xid_t xid,
	gfarm_int32_t ecode, const char *format, va_list *app)
{
	gfarm_error_t e;
	size_t size = 0;

	e = gfp_xdr_send_size_add(&size, "i", ecode);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	if (ecode == GFARM_ERR_NO_ERROR) {
		va_list ap;
		const char *fmt;

		va_copy(ap, *app);
		fmt = format;
		e = gfp_xdr_vsend_size_add(&size, &fmt, &ap);
		va_end(ap);
		if (e != GFARM_ERR_NO_ERROR)
			return (e);
	}
	e = gfp_xdr_send_async_result_header(client, xid, size);
	if (e != GFARM_ERR_NO_ERROR)
		return (e);
	return (gfp_xdr_vsend_result(client, ecode, format, app));
}
