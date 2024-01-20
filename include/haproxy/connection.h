/*
 * include/haproxy/connection.h
 * This file contains connection function prototypes
 *
 * Copyright (C) 2000-2002 Willy Tarreau - w@1wt.eu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _HAPROXY_CONNECTION_H
#define _HAPROXY_CONNECTION_H

#include <import/ist.h>

#include <haproxy/api.h>
#include <haproxy/buf.h>
#include <haproxy/connection-t.h>
#include <haproxy/stconn-t.h>
#include <haproxy/fd.h>
#include <haproxy/list.h>
#include <haproxy/listener-t.h>
#include <haproxy/obj_type.h>
#include <haproxy/pool-t.h>
#include <haproxy/server.h>
#include <haproxy/session-t.h>
#include <haproxy/task-t.h>

extern struct pool_head *pool_head_connection;
extern struct pool_head *pool_head_conn_hash_node;
extern struct pool_head *pool_head_sockaddr;
extern struct pool_head *pool_head_authority;
extern struct pool_head *pool_head_uniqueid;
extern struct xprt_ops *registered_xprt[XPRT_ENTRIES];
extern struct mux_proto_list mux_proto_list;
extern struct mux_stopping_data mux_stopping_data[MAX_THREADS];

#define IS_HTX_CONN(conn) ((conn)->mux && ((conn)->mux->flags & MX_FL_HTX))

/* receive a PROXY protocol header over a connection */
int conn_recv_proxy(struct connection *conn, int flag);
int conn_send_proxy(struct connection *conn, unsigned int flag);
int make_proxy_line(char *buf, int buf_len, struct server *srv, struct connection *remote, struct stream *strm);

int conn_append_debug_info(struct buffer *buf, const struct connection *conn, const char *pfx);

int conn_subscribe(struct connection *conn, void *xprt_ctx, int event_type, struct wait_event *es);
int conn_unsubscribe(struct connection *conn, void *xprt_ctx, int event_type, struct wait_event *es);

/* receive a NetScaler Client IP insertion header over a connection */
int conn_recv_netscaler_cip(struct connection *conn, int flag);

/* raw send() directly on the socket */
int conn_ctrl_send(struct connection *conn, const void *buf, int len, int flags);

/* drains any pending bytes from the socket */
int conn_ctrl_drain(struct connection *conn);

/* scoks4 proxy handshake */
int conn_send_socks4_proxy_request(struct connection *conn);
int conn_recv_socks4_proxy_response(struct connection *conn);

/* If we delayed the mux creation because we were waiting for the handshake, do it now */
int conn_create_mux(struct connection *conn);
int conn_notify_mux(struct connection *conn, int old_flags, int forced_wake);
int conn_upgrade_mux_fe(struct connection *conn, void *ctx, struct buffer *buf,
                        struct ist mux_proto, int mode);
int conn_install_mux_fe(struct connection *conn, void *ctx);
int conn_install_mux_be(struct connection *conn, void *ctx, struct session *sess,
                        const struct mux_ops *force_mux_ops);
int conn_install_mux_chk(struct connection *conn, void *ctx, struct session *sess);

void conn_delete_from_tree(struct eb64_node *node);

void conn_init(struct connection *conn, void *target);
struct connection *conn_new(void *target);
void conn_free(struct connection *conn);
struct conn_hash_node *conn_alloc_hash_node(struct connection *conn);
struct sockaddr_storage *sockaddr_alloc(struct sockaddr_storage **sap, const struct sockaddr_storage *orig, socklen_t len);
void sockaddr_free(struct sockaddr_storage **sap);


/* connection hash stuff */
uint64_t conn_calculate_hash(const struct conn_hash_params *params);
uint64_t conn_hash_prehash(char *buf, size_t size);
void conn_hash_update(char *buf, size_t *idx,
                      const void *data, size_t size,
                      enum conn_hash_params_t *flags,
                      enum conn_hash_params_t type);
uint64_t conn_hash_digest(char *buf, size_t bufsize,
                          enum conn_hash_params_t flags);
const char *conn_err_code_str(struct connection *c);
int xprt_add_hs(struct connection *conn);
void register_mux_proto(struct mux_proto_list *list);

extern struct idle_conns idle_conns[MAX_THREADS];

/* returns true if the transport layer is ready */
static inline int conn_xprt_ready(const struct connection *conn)
{
	return (conn->flags & CO_FL_XPRT_READY);
}

/* returns true if the control layer is ready */
static inline int conn_ctrl_ready(const struct connection *conn)
{
	return (conn->flags & CO_FL_CTRL_READY);
}

/*
 * Calls the start() function of the transport layer, if needed.
 * Returns < 0 in case of error.
*/

static inline int conn_xprt_start(struct connection *conn)
{
	int ret = 0;

	if (!conn_xprt_ready(conn) && conn->xprt && conn->xprt->start)
		ret = conn->xprt->start(conn, conn->xprt_ctx);

	if (ret >= 0)
		conn->flags |= CO_FL_XPRT_READY;

	return ret;
}

/* Calls the close() function of the transport layer if any and if not done
 * yet, and clears the CO_FL_XPRT_READY flags
 * However this is not done if the CO_FL_XPRT_TRACKED flag is set,
 * which allows logs to take data from the transport layer very late if needed.
 */
static inline void conn_xprt_close(struct connection *conn)
{
	if (conn->xprt && !(conn->flags & CO_FL_XPRT_TRACKED)) {
		if (conn->xprt->close)
			conn->xprt->close(conn, conn->xprt_ctx);
		conn->xprt_ctx = NULL;
		conn->flags &= ~CO_FL_XPRT_READY;
		conn->xprt = NULL;
	}
}

/* Initializes the connection's control layer which essentially consists in
 * registering the connection handle (e.g. file descriptor) for events and
 * setting the CO_FL_CTRL_READY flag. The caller is responsible for ensuring
 * that the control layer is already assigned to the connection prior to the
 * call.
 */
static inline void conn_ctrl_init(struct connection *conn)
{
	if (!conn_ctrl_ready(conn)) {
		conn->flags |= CO_FL_CTRL_READY;
		if (conn->ctrl->ctrl_init)
			conn->ctrl->ctrl_init(conn);
	}
}

/* Deletes the connection's handle (e.g. FD) if the transport layer is already
 * gone, and removes the CO_FL_CTRL_READY flag.
 */
static inline void conn_ctrl_close(struct connection *conn)
{
	if (!conn->xprt && (conn->flags & CO_FL_CTRL_READY)) {
		if ((conn->flags & (CO_FL_WANT_DRAIN | CO_FL_SOCK_RD_SH)) == CO_FL_WANT_DRAIN)
			conn_ctrl_drain(conn);
		conn->flags &= ~CO_FL_CTRL_READY;
		if (conn->ctrl->ctrl_close)
			conn->ctrl->ctrl_close(conn);
	}
}

/* If the connection still has a transport layer, then call its close() function
 * if any, and delete the file descriptor if a control layer is set. This is
 * used to close everything at once and atomically. However this is not done if
 * the CO_FL_XPRT_TRACKED flag is set, which allows logs to take data from the
 * transport layer very late if needed.
 */
static inline void conn_full_close(struct connection *conn)
{
	conn_xprt_close(conn);
	conn_ctrl_close(conn);
}

/* stop tracking a connection, allowing conn_full_close() to always
 * succeed.
 */
static inline void conn_stop_tracking(struct connection *conn)
{
	conn->flags &= ~CO_FL_XPRT_TRACKED;
}

/* returns the connection's FD if the connection exists, its control is ready,
 * and the connection has an FD, otherwise -1.
 */
static inline int conn_fd(const struct connection *conn)
{
	if (!conn || !conn_ctrl_ready(conn) || (conn->flags & CO_FL_FDLESS))
		return -1;
	return conn->handle.fd;
}

/* read shutdown, called from the rcv_buf/rcv_pipe handlers when
 * detecting an end of connection.
 */
static inline void conn_sock_read0(struct connection *c)
{
	c->flags |= CO_FL_SOCK_RD_SH;
	if (conn_ctrl_ready(c)) {
		/* we don't risk keeping ports unusable if we found the
		 * zero from the other side.
		 */
		BUG_ON(c->flags & CO_FL_FDLESS);
		HA_ATOMIC_AND(&fdtab[c->handle.fd].state, ~FD_LINGER_RISK);
	}
}

/* write shutdown, indication that the upper layer is not willing to send
 * anything anymore and wants to close after pending data are sent. The
 * <clean> argument will allow not to perform the socket layer shutdown if
 * equal to 0.
 */
static inline void conn_sock_shutw(struct connection *c, int clean)
{
	c->flags |= CO_FL_SOCK_WR_SH;
	if (conn_ctrl_ready(c)) {
		/* don't perform a clean shutdown if we're going to reset or
		 * if the shutr was already received.
		 */
		BUG_ON(c->flags & CO_FL_FDLESS);
		if (!(c->flags & CO_FL_SOCK_RD_SH) && clean)
			shutdown(c->handle.fd, SHUT_WR);
	}
}

static inline void conn_xprt_shutw(struct connection *c)
{
	/* clean data-layer shutdown */
	if (c->xprt && c->xprt->shutw)
		c->xprt->shutw(c, c->xprt_ctx, 1);
}

static inline void conn_xprt_shutw_hard(struct connection *c)
{
	/* unclean data-layer shutdown */
	if (c->xprt && c->xprt->shutw)
		c->xprt->shutw(c, c->xprt_ctx, 0);
}


/* detect sock->data read0 transition */
static inline int conn_xprt_read0_pending(struct connection *c)
{
	return (c->flags & CO_FL_SOCK_RD_SH) != 0;
}

/* prepares a connection to work with protocol <proto> and transport <xprt>.
 * The transport's is initialized as well, and the mux and its context are
 * cleared. The target is not reinitialized and it is recommended that it is
 * set prior to calling this function so that the function may make use of it
 * in the future to refine the mux choice if needed.
 */
static inline int conn_prepare(struct connection *conn, const struct protocol *proto, const struct xprt_ops *xprt)
{
	int ret = 0;

	conn->ctrl = proto;
	conn->xprt = xprt;
	conn->mux  = NULL;
	conn->xprt_ctx = NULL;
	conn->ctx = NULL;
	if (xprt->init) {
		ret = xprt->init(conn, &conn->xprt_ctx);
		if (ret < 0)
			conn->xprt = NULL;
	}
	return ret;
}

/* returns 0 if the connection is valid and is a frontend connection, otherwise
 * returns 1 indicating it's a backend connection. And uninitialized connection
 * also returns 1 to better handle the usage in the middle of initialization.
 */
static inline int conn_is_back(const struct connection *conn)
{
	return !objt_listener(conn->target);
}

/* sets <owner> as the connection's owner */
static inline void conn_set_owner(struct connection *conn, void *owner, void (*cb)(struct connection *))
{
	conn->owner = owner;
	conn->destroy_cb = cb;
}


/* Mark the connection <conn> as private and remove it from the available connection list */
static inline void conn_set_private(struct connection *conn)
{
	if (!(conn->flags & CO_FL_PRIVATE)) {
		conn->flags |= CO_FL_PRIVATE;

		if (obj_type(conn->target) == OBJ_TYPE_SERVER)
			srv_release_conn(__objt_server(conn->target), conn);
	}
}

/* Used to know if a connection is in an idle list. It returns connection flag
 * corresponding to the idle list if the connection is idle (CO_FL_SAFE_LIST or
 * CO_FL_IDLE_LIST) or 0 otherwise. Note that if the connection is scheduled to
 * be removed, 0 is returned, regardless the connection flags.
 */
static inline unsigned int conn_get_idle_flag(const struct connection *conn)
{
	return (!MT_LIST_INLIST(&conn->toremove_list) ? conn->flags & CO_FL_LIST_MASK : 0);
}

static inline void conn_force_unsubscribe(struct connection *conn)
{
	if (!conn->subs)
		return;
	conn->subs->events = 0;
	conn->subs = NULL;
}

/* Returns the source address of the connection or NULL if not set */
static inline const struct sockaddr_storage *conn_src(struct connection *conn)
{
	return conn->src;
}

/* Returns the destination address of the connection or NULL if not set */
static inline const struct sockaddr_storage *conn_dst(struct connection *conn)
{
	return conn->dst;
}

/* Retrieves the connection's original source address. Returns non-zero on
 * success or zero on failure. The operation is only performed once and the
 * address is stored in the connection for future use.
 */
static inline int conn_get_src(struct connection *conn)
{
	if (conn->src)
		return 1;

	if (!conn_ctrl_ready(conn))
		goto fail;

	if (!sockaddr_alloc(&conn->src, NULL, 0))
		goto fail;

	/* some stream protocols may provide their own get_src/dst functions */
	if (conn->ctrl->get_src &&
	    conn->ctrl->get_src(conn, (struct sockaddr *)conn->src, sizeof(*conn->src)) != -1)
		goto done;

	if (conn->ctrl->proto_type != PROTO_TYPE_STREAM)
		goto fail;

	/* most other socket-based stream protocols will use their socket family's functions */
	if (conn->ctrl->fam->get_src && !(conn->flags & CO_FL_FDLESS) &&
	    conn->ctrl->fam->get_src(conn->handle.fd, (struct sockaddr *)conn->src,
	                        sizeof(*conn->src),
	                        obj_type(conn->target) != OBJ_TYPE_LISTENER) != -1)
		goto done;

	/* no other means */
 fail:
	sockaddr_free(&conn->src);
	return 0;
 done:
	return 1;
}

/* Retrieves the connection's original destination address. Returns non-zero on
 * success or zero on failure. The operation is only performed once and the
 * address is stored in the connection for future use.
 */
static inline int conn_get_dst(struct connection *conn)
{
	if (conn->dst)
		return 1;

	if (!conn_ctrl_ready(conn))
		goto fail;

	if (!sockaddr_alloc(&conn->dst, NULL, 0))
		goto fail;

	/* some stream protocols may provide their own get_src/dst functions */
	if (conn->ctrl->get_dst &&
	    conn->ctrl->get_dst(conn, (struct sockaddr *)conn->dst, sizeof(*conn->dst)) != -1)
		goto done;

	if (conn->ctrl->proto_type != PROTO_TYPE_STREAM)
		goto fail;

	/* most other socket-based stream protocols will use their socket family's functions */
	if (conn->ctrl->fam->get_dst && !(conn->flags & CO_FL_FDLESS) &&
	    conn->ctrl->fam->get_dst(conn->handle.fd, (struct sockaddr *)conn->dst,
	                        sizeof(*conn->dst),
	                        obj_type(conn->target) != OBJ_TYPE_LISTENER) != -1)
		goto done;

	/* no other means */
 fail:
	sockaddr_free(&conn->dst);
	return 0;
 done:
	return 1;
}

/* Sets the TOS header in IPv4 and the traffic class header in IPv6 packets
 * (as per RFC3260 #4 and BCP37 #4.2 and #5.2). The connection is tested and if
 * it is null, nothing is done.
 */
static inline void conn_set_tos(const struct connection *conn, int tos)
{
	if (!conn || !conn_ctrl_ready(conn) || (conn->flags & CO_FL_FDLESS))
		return;

#ifdef IP_TOS
	if (conn->src->ss_family == AF_INET)
		setsockopt(conn->handle.fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
#endif
#ifdef IPV6_TCLASS
	if (conn->src->ss_family == AF_INET6) {
		if (IN6_IS_ADDR_V4MAPPED(&((struct sockaddr_in6 *)conn->src)->sin6_addr))
			/* v4-mapped addresses need IP_TOS */
			setsockopt(conn->handle.fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
		else
			setsockopt(conn->handle.fd, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof(tos));
	}
#endif
}

/* Sets the netfilter mark on the connection's socket. The connection is tested
 * and if it is null, nothing is done.
 */
static inline void conn_set_mark(const struct connection *conn, int mark)
{
	if (!conn || !conn_ctrl_ready(conn) || (conn->flags & CO_FL_FDLESS))
		return;

#if defined(SO_MARK)
	setsockopt(conn->handle.fd, SOL_SOCKET, SO_MARK, &mark, sizeof(mark));
#elif defined(SO_USER_COOKIE)
	setsockopt(conn->handle.fd, SOL_SOCKET, SO_USER_COOKIE, &mark, sizeof(mark));
#elif defined(SO_RTABLE)
	setsockopt(conn->handle.fd, SOL_SOCKET, SO_RTABLE, &mark, sizeof(mark));
#endif
}

/* Sets adjust the TCP quick-ack feature on the connection's socket. The
 * connection is tested and if it is null, nothing is done.
 */
static inline void conn_set_quickack(const struct connection *conn, int value)
{
	if (!conn || !conn_ctrl_ready(conn) || (conn->flags & CO_FL_FDLESS))
		return;

#ifdef TCP_QUICKACK
	setsockopt(conn->handle.fd, IPPROTO_TCP, TCP_QUICKACK, &value, sizeof(value));
#endif
}

static inline struct wait_event *wl_set_waitcb(struct wait_event *wl, struct task *(*cb)(struct task *, void *, unsigned int), void *ctx)
{
	if (!wl->tasklet->process) {
		wl->tasklet->process = cb;
		wl->tasklet->context = ctx;
	}
	return wl;
}

/* Installs the connection's mux layer for upper context <ctx>.
 * Returns < 0 on error.
 */
static inline int conn_install_mux(struct connection *conn, const struct mux_ops *mux,
                                   void *ctx, struct proxy *prx, struct session *sess)
{
	int ret;

	conn->mux = mux;
	conn->ctx = ctx;
	ret = mux->init ? mux->init(conn, prx, sess, &BUF_NULL) : 0;
	if (ret < 0) {
		conn->mux = NULL;
		conn->ctx = NULL;
	}
	return ret;
}

/* Retrieves any valid stream connector from this connection, preferably the first
 * valid one. The purpose is to be able to figure one other end of a private
 * connection for purposes like source binding or proxy protocol header
 * emission. In such cases, any stream connector is expected to be valid so the
 * mux is encouraged to return the first one it finds. If the connection has
 * no mux or the mux has no get_first_sc() method or the mux has no valid
 * stream connector, NULL is returned. The output pointer is purposely marked
 * const to discourage the caller from modifying anything there.
 */
static inline struct stconn *conn_get_first_sc(const struct connection *conn)
{
	BUG_ON(!conn || !conn->mux);

	if (!conn->mux->get_first_sc)
		return NULL;
	return conn->mux->get_first_sc(conn);
}

int conn_update_alpn(struct connection *conn, const struct ist alpn, int force);

static inline const char *conn_get_ctrl_name(const struct connection *conn)
{
	if (!conn || !conn_ctrl_ready(conn))
		return "NONE";
	return conn->ctrl->name;
}

static inline const char *conn_get_xprt_name(const struct connection *conn)
{
	if (!conn || !conn->xprt)
		return "NONE";
	return conn->xprt->name;
}

static inline const char *conn_get_mux_name(const struct connection *conn)
{
	if (!conn || !conn->mux)
		return "NONE";
	return conn->mux->name;
}

/* registers pointer to transport layer <id> (XPRT_*) */
static inline void xprt_register(int id, struct xprt_ops *xprt)
{
	if (id >= XPRT_ENTRIES)
		return;
	registered_xprt[id] = xprt;
}

/* returns pointer to transport layer <id> (XPRT_*) or NULL if not registered */
static inline struct xprt_ops *xprt_get(int id)
{
	if (id >= XPRT_ENTRIES)
		return NULL;
	return registered_xprt[id];
}

/* notify the next xprt that the connection is about to become idle and that it
 * may be stolen at any time after the function returns and that any tasklet in
 * the chain must be careful before dereferencing its context.
 */
static inline void xprt_set_idle(struct connection *conn, const struct xprt_ops *xprt, void *xprt_ctx)
{
	if (xprt->set_idle)
		xprt->set_idle(conn, conn->xprt_ctx);
}

/* notify the next xprt that the connection is not idle anymore and that it may
 * not be stolen before the next xprt_set_idle().
 */
static inline void xprt_set_used(struct connection *conn, const struct xprt_ops *xprt, void *xprt_ctx)
{
	if (xprt->set_used)
		xprt->set_used(conn, conn->xprt_ctx);
}

static inline int conn_get_alpn(const struct connection *conn, const char **str, int *len)
{
	if (!conn_xprt_ready(conn) || !conn->xprt->get_alpn)
		return 0;
	return conn->xprt->get_alpn(conn, conn->xprt_ctx, str, len);
}

/* unregisters proto mux list <list> */
static inline void unregister_mux_proto(struct mux_proto_list *list)
{
	LIST_DELETE(&list->list);
	LIST_INIT(&list->list);
}

static inline struct mux_proto_list *get_mux_proto(const struct ist proto)
{
	struct mux_proto_list *item;

	list_for_each_entry(item, &mux_proto_list.list, list) {
		if (isteq(proto, item->token))
			return item;
	}
	return NULL;
}

void list_mux_proto(FILE *out);
/* returns the first mux entry in the list matching the exact same <mux_proto>
 * and compatible with the <proto_side> (FE or BE) and the <proto_mode> (TCP or
 * HTTP). <mux_proto> can be empty. Will fall back to the first compatible mux
 * with exactly the same <proto_mode> or with an empty name. May return
 * null if the code improperly registered the default mux to use as a fallback.
 *
 * <proto_mode> expects PROTO_MODE_* value only: PROXY_MODE_* values should
 * never be used directly here (but you may use conn_pr_mode_to_proto_mode()
 * to map proxy mode to corresponding proto mode before calling the function).
 */
static inline const struct mux_proto_list *conn_get_best_mux_entry(
        const struct ist mux_proto,
        int proto_side, int proto_mode)
{
	struct mux_proto_list *item;
	struct mux_proto_list *fallback = NULL;

	list_for_each_entry(item, &mux_proto_list.list, list) {
		if (!(item->side & proto_side) || !(item->mode & proto_mode))
			continue;
		if (istlen(mux_proto) && isteq(mux_proto, item->token))
			return item;
		else if (!istlen(item->token)) {
			if (!fallback || (item->mode == proto_mode && fallback->mode != proto_mode))
				fallback = item;
		}
	}
	return fallback;

}

/* returns the first mux in the list matching the exact same <mux_proto> and
 * compatible with the <proto_side> (FE or BE) and the <proto_mode> (TCP or
 * HTTP). <mux_proto> can be empty. Will fall back to the first compatible mux
 * with exactly the same <proto_mode> or with an empty name. May return
 * null if the code improperly registered the default mux to use as a fallback.
 */
static inline const struct mux_ops *conn_get_best_mux(struct connection *conn,
						      const struct ist mux_proto,
						      int proto_side, int proto_mode)
{
	const struct mux_proto_list *item;

	item = conn_get_best_mux_entry(mux_proto, proto_side, proto_mode);

	return item ? item->mux : NULL;
}

/* returns a pointer to the proxy associated with this connection. For a front
 * connection it returns a pointer to the frontend ; for a back connection, it
 * returns a pointer to the backend.
 */
static inline struct proxy *conn_get_proxy(const struct connection *conn)
{
	struct listener *l;
	struct server *s;

	/* check if it's a frontend connection */
	l = objt_listener(conn->target);
	if (l)
		return l->bind_conf->frontend;

	/* check if it's a backend connection */
	s = objt_server(conn->target);
	if (s)
		return s->proxy;

	return objt_proxy(conn->target);
}

/* unconditionally retrieves the ssl_sock_ctx for this connection. Prefer using
 * the standard form conn_get_ssl_sock_ctx() which checks the transport layer
 * and the availability of the method.
 */
static inline struct ssl_sock_ctx *__conn_get_ssl_sock_ctx(struct connection *conn)
{
	return conn->xprt->get_ssl_sock_ctx(conn);
}

/* retrieves the ssl_sock_ctx for this connection otherwise NULL */
static inline struct ssl_sock_ctx *conn_get_ssl_sock_ctx(struct connection *conn)
{
	if (!conn || !conn->xprt || !conn->xprt->get_ssl_sock_ctx)
		return NULL;
	return conn->xprt->get_ssl_sock_ctx(conn);
}

/* boolean, returns true if connection is over SSL */
static inline int conn_is_ssl(struct connection *conn)
{
	return !!conn_get_ssl_sock_ctx(conn);
}

/*
 * Map proxy mode (PR_MODE_*) to equivalent proto_proxy_mode (PROTO_MODE_*)
 */
static inline int conn_pr_mode_to_proto_mode(int proxy_mode)
{
	int mode;

	/* for now we only support TCP and HTTP proto_modes, so we
	 * consider that if it's not HTTP, then it's TCP
	 */
	mode = 1 << (proxy_mode == PR_MODE_HTTP);

	return mode;
}

#endif /* _HAPROXY_CONNECTION_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
