/*
 * stream connector management functions
 *
 * Copyright 2021 Christopher Faulet <cfaulet@haproxy.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <haproxy/api.h>
#include <haproxy/applet.h>
#include <haproxy/connection.h>
#include <haproxy/check.h>
#include <haproxy/http_ana.h>
#include <haproxy/pipe.h>
#include <haproxy/pool.h>
#include <haproxy/sc_strm.h>
#include <haproxy/stconn.h>
#include <haproxy/xref.h>

DECLARE_POOL(pool_head_connstream, "stconn", sizeof(struct stconn));
DECLARE_POOL(pool_head_sedesc, "sedesc", sizeof(struct sedesc));

/* functions used by default on a detached stream connector */
static void sc_app_abort(struct stconn *sc);
static void sc_app_shut(struct stconn *sc);
static void sc_app_chk_rcv(struct stconn *sc);
static void sc_app_chk_snd(struct stconn *sc);

/* functions used on a mux-based stream connector */
static void sc_app_abort_conn(struct stconn *sc);
static void sc_app_shut_conn(struct stconn *sc);
static void sc_app_chk_rcv_conn(struct stconn *sc);
static void sc_app_chk_snd_conn(struct stconn *sc);

/* functions used on an applet-based stream connector */
static void sc_app_abort_applet(struct stconn *sc);
static void sc_app_shut_applet(struct stconn *sc);
static void sc_app_chk_rcv_applet(struct stconn *sc);
static void sc_app_chk_snd_applet(struct stconn *sc);

static int sc_conn_process(struct stconn *sc);
static int sc_conn_recv(struct stconn *sc);
static int sc_conn_send(struct stconn *sc);
static int sc_applet_process(struct stconn *sc);

/* stream connector operations for connections */
struct sc_app_ops sc_app_conn_ops = {
	.chk_rcv = sc_app_chk_rcv_conn,
	.chk_snd = sc_app_chk_snd_conn,
	.abort   = sc_app_abort_conn,
	.shutdown= sc_app_shut_conn,
	.wake    = sc_conn_process,
	.name    = "STRM",
};

/* stream connector operations for embedded tasks */
struct sc_app_ops sc_app_embedded_ops = {
	.chk_rcv = sc_app_chk_rcv,
	.chk_snd = sc_app_chk_snd,
	.abort   = sc_app_abort,
	.shutdown= sc_app_shut,
	.wake    = NULL,   /* may never be used */
	.name    = "NONE", /* may never be used */
};

/* stream connector operations for applets */
struct sc_app_ops sc_app_applet_ops = {
	.chk_rcv = sc_app_chk_rcv_applet,
	.chk_snd = sc_app_chk_snd_applet,
	.abort   = sc_app_abort_applet,
	.shutdown= sc_app_shut_applet,
	.wake    = sc_applet_process,
	.name    = "STRM",
};

/* stream connector for health checks on connections */
struct sc_app_ops sc_app_check_ops = {
	.chk_rcv = NULL,
	.chk_snd = NULL,
	.abort   = NULL,
	.shutdown= NULL,
	.wake    = wake_srv_chk,
	.name    = "CHCK",
};

/* Initializes an endpoint */
void sedesc_init(struct sedesc *sedesc)
{
	sedesc->se = NULL;
	sedesc->conn = NULL;
	sedesc->sc = NULL;
	sedesc->lra = TICK_ETERNITY;
	sedesc->fsb = TICK_ETERNITY;
	sedesc->xref.peer = NULL;
	se_fl_setall(sedesc, SE_FL_NONE);
}

/* Tries to alloc an endpoint and initialize it. Returns NULL on failure. */
struct sedesc *sedesc_new()
{
	struct sedesc *sedesc;

	sedesc = pool_alloc(pool_head_sedesc);
	if (unlikely(!sedesc))
		return NULL;

	sedesc_init(sedesc);
	return sedesc;
}

/* Releases an endpoint. It is the caller responsibility to be sure it is safe
 * and it is not shared with another entity
 */
void sedesc_free(struct sedesc *sedesc)
{
	pool_free(pool_head_sedesc, sedesc);
}

/* Tries to allocate a new stconn and initialize its main fields. On
 * failure, nothing is allocated and NULL is returned. It is an internal
 * function. The caller must, at least, set the SE_FL_ORPHAN or SE_FL_DETACHED
 * flag.
 */
static struct stconn *sc_new(struct sedesc *sedesc)
{
	struct stconn *sc;

	sc = pool_alloc(pool_head_connstream);

	if (unlikely(!sc))
		goto alloc_error;

	sc->obj_type = OBJ_TYPE_SC;
	sc->flags = SC_FL_NONE;
	sc->state = SC_ST_INI;
	sc->ioto = TICK_ETERNITY;
	sc->room_needed = 0;
	sc->app = NULL;
	sc->app_ops = NULL;
	sc->src = NULL;
	sc->dst = NULL;
	sc->wait_event.tasklet = NULL;
	sc->wait_event.events = 0;

	/* If there is no endpoint, allocate a new one now */
	if (!sedesc) {
		sedesc = sedesc_new();
		if (unlikely(!sedesc))
			goto alloc_error;
	}
	sc->sedesc = sedesc;
	sedesc->sc = sc;

	return sc;

  alloc_error:
	pool_free(pool_head_connstream, sc);
	return NULL;
}

/* Creates a new stream connector and its associated stream from a mux. <sd> must
 * be defined. It returns NULL on error. On success, the new stream connector is
 * returned. In this case, SE_FL_ORPHAN flag is removed.
 */
struct stconn *sc_new_from_endp(struct sedesc *sd, struct session *sess, struct buffer *input)
{
	struct stconn *sc;

	sc = sc_new(sd);
	if (unlikely(!sc))
		return NULL;
	if (unlikely(!stream_new(sess, sc, input))) {
		sd->sc = NULL;
		if (sc->sedesc != sd) {
			/* none was provided so sc_new() allocated one */
			sedesc_free(sc->sedesc);
		}
		pool_free(pool_head_connstream, sc);
		se_fl_set(sd, SE_FL_ORPHAN);
		return NULL;
	}
	se_fl_clr(sd, SE_FL_ORPHAN);
	return sc;
}

/* Creates a new stream connector from an stream. There is no endpoint here, thus it
 * will be created by sc_new(). So the SE_FL_DETACHED flag is set. It returns
 * NULL on error. On success, the new stream connector is returned.
 */
struct stconn *sc_new_from_strm(struct stream *strm, unsigned int flags)
{
	struct stconn *sc;

	sc = sc_new(NULL);
	if (unlikely(!sc))
		return NULL;
	sc->flags |= flags;
	sc_ep_set(sc, SE_FL_DETACHED);
	sc->app = &strm->obj_type;
	sc->app_ops = &sc_app_embedded_ops;
	return sc;
}

/* Creates a new stream connector from an health-check. There is no endpoint here,
 * thus it will be created by sc_new(). So the SE_FL_DETACHED flag is set. It
 * returns NULL on error. On success, the new stream connector is returned.
 */
struct stconn *sc_new_from_check(struct check *check, unsigned int flags)
{
	struct stconn *sc;

	sc = sc_new(NULL);
	if (unlikely(!sc))
		return NULL;
	sc->flags |= flags;
	sc_ep_set(sc, SE_FL_DETACHED);
	sc->app = &check->obj_type;
	sc->app_ops = &sc_app_check_ops;
	return sc;
}

/* Releases a stconn previously allocated by sc_new(), as well as its
 * endpoint, if it exists. This function is called internally or on error path.
 */
void sc_free(struct stconn *sc)
{
	sockaddr_free(&sc->src);
	sockaddr_free(&sc->dst);
	if (sc->sedesc) {
		BUG_ON(!sc_ep_test(sc, SE_FL_DETACHED));
		sedesc_free(sc->sedesc);
	}
	tasklet_free(sc->wait_event.tasklet);
	pool_free(pool_head_connstream, sc);
}

/* Conditionally removes a stream connector if it is detached and if there is no app
 * layer defined. Except on error path, this one must be used. if release, the
 * pointer on the SC is set to NULL.
 */
static void sc_free_cond(struct stconn **scp)
{
	struct stconn *sc = *scp;

	if (!sc->app && (!sc->sedesc || sc_ep_test(sc, SE_FL_DETACHED))) {
		sc_free(sc);
		*scp = NULL;
	}
}


/* Attaches a stconn to a mux endpoint and sets the endpoint ctx. Returns
 * -1 on error and 0 on success. SE_FL_DETACHED flag is removed. This function is
 * called from a mux when it is attached to a stream or a health-check.
 */
int sc_attach_mux(struct stconn *sc, void *sd, void *ctx)
{
	struct connection *conn = ctx;
	struct sedesc *sedesc = sc->sedesc;

	if (sc_strm(sc)) {
		if (!sc->wait_event.tasklet) {
			sc->wait_event.tasklet = tasklet_new();
			if (!sc->wait_event.tasklet)
				return -1;
			sc->wait_event.tasklet->process = sc_conn_io_cb;
			sc->wait_event.tasklet->context = sc;
			sc->wait_event.events = 0;
		}

		sc->app_ops = &sc_app_conn_ops;
		xref_create(&sc->sedesc->xref, &sc_opposite(sc)->sedesc->xref);
	}
	else if (sc_check(sc)) {
		if (!sc->wait_event.tasklet) {
			sc->wait_event.tasklet = tasklet_new();
			if (!sc->wait_event.tasklet)
				return -1;
			sc->wait_event.tasklet->process = srv_chk_io_cb;
			sc->wait_event.tasklet->context = sc;
			sc->wait_event.events = 0;
		}

		sc->app_ops = &sc_app_check_ops;
	}

	sedesc->se = sd;
	sedesc->conn = ctx;
	se_fl_set(sedesc, SE_FL_T_MUX);
	se_fl_clr(sedesc, SE_FL_DETACHED);
	if (!conn->ctx)
		conn->ctx = sc;
	return 0;
}

/* Attaches a stconn to an applet endpoint and sets the endpoint
 * ctx. Returns -1 on error and 0 on success. SE_FL_DETACHED flag is
 * removed. This function is called by a stream when a backend applet is
 * registered.
 */
static void sc_attach_applet(struct stconn *sc, void *sd)
{
	sc->sedesc->se = sd;
	sc_ep_set(sc, SE_FL_T_APPLET);
	sc_ep_clr(sc, SE_FL_DETACHED);
	if (sc_strm(sc)) {
		sc->app_ops = &sc_app_applet_ops;
		xref_create(&sc->sedesc->xref, &sc_opposite(sc)->sedesc->xref);
	}
}

/* Attaches a stconn to a app layer and sets the relevant
 * callbacks. Returns -1 on error and 0 on success. SE_FL_ORPHAN flag is
 * removed. This function is called by a stream when it is created to attach it
 * on the stream connector on the client side.
 */
int sc_attach_strm(struct stconn *sc, struct stream *strm)
{
	sc->app = &strm->obj_type;
	sc_ep_clr(sc, SE_FL_ORPHAN);
	sc_ep_report_read_activity(sc);
	if (sc_ep_test(sc, SE_FL_T_MUX)) {
		sc->wait_event.tasklet = tasklet_new();
		if (!sc->wait_event.tasklet)
			return -1;
		sc->wait_event.tasklet->process = sc_conn_io_cb;
		sc->wait_event.tasklet->context = sc;
		sc->wait_event.events = 0;

		sc->app_ops = &sc_app_conn_ops;
	}
	else if (sc_ep_test(sc, SE_FL_T_APPLET)) {
		sc->app_ops = &sc_app_applet_ops;
	}
	else {
		sc->app_ops = &sc_app_embedded_ops;
	}
	return 0;
}

/* Detaches the stconn from the endpoint, if any. For a connecrion, if a
 * mux owns the connection ->detach() callback is called. Otherwise, it means
 * the stream connector owns the connection. In this case the connection is closed
 * and released. For an applet, the appctx is released. If still allocated, the
 * endpoint is reset and flag as detached. If the app layer is also detached,
 * the stream connector is released.
 */
static void sc_detach_endp(struct stconn **scp)
{
	struct stconn *sc = *scp;
	struct xref *peer;

	if (!sc)
		return;


	/* Remove my link in the original objects. */
	peer = xref_get_peer_and_lock(&sc->sedesc->xref);
	if (peer)
		xref_disconnect(&sc->sedesc->xref, peer);

	if (sc_ep_test(sc, SE_FL_T_MUX)) {
		struct connection *conn = __sc_conn(sc);
		struct sedesc *sedesc = sc->sedesc;

		if (conn->mux) {
			if (sc->wait_event.events != 0)
				conn->mux->unsubscribe(sc, sc->wait_event.events, &sc->wait_event);
			se_fl_set(sedesc, SE_FL_ORPHAN);
			sedesc->sc = NULL;
			sc->sedesc = NULL;
			conn->mux->detach(sedesc);
		}
		else {
			/* It's too early to have a mux, let's just destroy
			 * the connection
			 */
			conn_stop_tracking(conn);
			conn_full_close(conn);
			if (conn->destroy_cb)
				conn->destroy_cb(conn);
			conn_free(conn);
		}
	}
	else if (sc_ep_test(sc, SE_FL_T_APPLET)) {
		struct appctx *appctx = __sc_appctx(sc);

		sc_ep_set(sc, SE_FL_ORPHAN);
		sc->sedesc->sc = NULL;
		sc->sedesc = NULL;
		appctx_shut(appctx);
		appctx_free(appctx);
	}

	if (sc->sedesc) {
		/* the SD wasn't used and can be recycled */
		sc->sedesc->se     = NULL;
		sc->sedesc->conn   = NULL;
		sc->sedesc->flags  = 0;
		sc_ep_set(sc, SE_FL_DETACHED);
	}

	/* FIXME: Rest SC for now but must be reviewed. SC flags are only
	 *        connection related for now but this will evolved
	 */
	sc->flags &= SC_FL_ISBACK;
	if (sc_strm(sc))
		sc->app_ops = &sc_app_embedded_ops;
	else
		sc->app_ops = NULL;
	sc_free_cond(scp);
}

/* Detaches the stconn from the app layer. If there is no endpoint attached
 * to the stconn
 */
static void sc_detach_app(struct stconn **scp)
{
	struct stconn *sc = *scp;

	if (!sc)
		return;

	sc->app = NULL;
	sc->app_ops = NULL;
	sockaddr_free(&sc->src);
	sockaddr_free(&sc->dst);

	tasklet_free(sc->wait_event.tasklet);
	sc->wait_event.tasklet = NULL;
	sc->wait_event.events = 0;
	sc_free_cond(scp);
}

/* Destroy the stconn. It is detached from its endpoint and its
 * application. After this call, the stconn must be considered as released.
 */
void sc_destroy(struct stconn *sc)
{
	sc_detach_endp(&sc);
	sc_detach_app(&sc);
	BUG_ON_HOT(sc);
}

/* Resets the stream connector endpoint. It happens when the app layer want to renew
 * its endpoint. For a connection retry for instance. If a mux or an applet is
 * attached, a new endpoint is created. Returns -1 on error and 0 on success.
 */
int sc_reset_endp(struct stconn *sc)
{
	struct sedesc *new_sd;

	BUG_ON(!sc->app);

	if (!__sc_endp(sc)) {
		/* endpoint not attached or attached to a mux with no
		 * target. Thus the endpoint will not be release but just
		 * reset. The app is still attached, the sc will not be
		 * released.
		 */
		sc_detach_endp(&sc);
		return 0;
	}

	/* allocate the new endpoint first to be able to set error if it
	 * fails */
	new_sd = sedesc_new();
	if (!unlikely(new_sd))
		return -1;

	/* The app is still attached, the sc will not be released */
	sc_detach_endp(&sc);
	BUG_ON(!sc);
	BUG_ON(sc->sedesc);
	sc->sedesc = new_sd;
	sc->sedesc->sc = sc;
	sc_ep_set(sc, SE_FL_DETACHED);
	return 0;
}


/* Create an applet to handle a stream connector as a new appctx. The SC will
 * wake it up every time it is solicited. The appctx must be deleted by the task
 * handler using sc_detach_endp(), possibly from within the function itself.
 * It also pre-initializes the applet's context and returns it (or NULL in case
 * it could not be allocated).
 */
struct appctx *sc_applet_create(struct stconn *sc, struct applet *app)
{
	struct appctx *appctx;

	appctx = appctx_new_here(app, sc->sedesc);
	if (!appctx)
		return NULL;
	sc_attach_applet(sc, appctx);
	appctx->t->nice = __sc_strm(sc)->task->nice;
	applet_need_more_data(appctx);
	appctx_wakeup(appctx);

	sc->state = SC_ST_RDY;
	return appctx;
}

/* Conditionally forward the close to the write side. It return 1 if it can be
 * forwarded. It is the caller responsibility to forward the close to the write
 * side. Otherwise, 0 is returned. In this case, SC_FL_SHUT_WANTED flag may be set on
 * the consumer SC if we are only waiting for the outgoing data to be flushed.
 */
static inline int sc_cond_forward_shut(struct stconn *sc)
{
	/* Foward the shutdown if an write error occurred on the input channel */
	if (sc_ic(sc)->flags & CF_WRITE_TIMEOUT)
		return 1;

	/* The close must not be forwarded */
	if (!(sc->flags & (SC_FL_EOS|SC_FL_ABRT_DONE)) || !(sc->flags & SC_FL_NOHALF))
		return 0;

	if (!channel_is_empty(sc_ic(sc))) {
		/* the shutdown cannot be forwarded now because
		 * we should flush outgoing data first. But instruct the output
		 * channel it should be done ASAP.
		 */
		sc_schedule_shutdown(sc);
		return 0;
	}

	/* the close can be immediately forwarded to the write side */
	return 1;
}

/*
 * This function performs a shutdown-read on a detached stream connector in a
 * connected or init state (it does nothing for other states). It either shuts
 * the read side or marks itself as closed. The buffer flags are updated to
 * reflect the new state. If the stream connector has SC_FL_NOHALF, we also
 * forward the close to the write side. The owner task is woken up if it exists.
 */
static void sc_app_abort(struct stconn *sc)
{
	struct channel *ic = sc_ic(sc);

	if (sc->flags & (SC_FL_EOS|SC_FL_ABRT_DONE))
		return;

	sc->flags |= SC_FL_ABRT_DONE;
	ic->flags |= CF_READ_EVENT;

	if (!sc_state_in(sc->state, SC_SB_CON|SC_SB_RDY|SC_SB_EST))
		return;

	if (sc->flags & SC_FL_SHUT_DONE) {
		sc->state = SC_ST_DIS;
		if (sc->flags & SC_FL_ISBACK)
			__sc_strm(sc)->conn_exp = TICK_ETERNITY;
	}
	else if (sc_cond_forward_shut(sc))
		return sc_app_shut(sc);

	/* note that if the task exists, it must unregister itself once it runs */
	if (!(sc->flags & SC_FL_DONT_WAKE))
		task_wakeup(sc_strm_task(sc), TASK_WOKEN_IO);
}

/*
 * This function performs a shutdown-write on a detached stream connector in a
 * connected or init state (it does nothing for other states). It either shuts
 * the write side or marks itself as closed. The buffer flags are updated to
 * reflect the new state. It does also close everything if the SC was marked as
 * being in error state. The owner task is woken up if it exists.
 */
static void sc_app_shut(struct stconn *sc)
{
	struct channel *ic = sc_ic(sc);
	struct channel *oc = sc_oc(sc);

	sc->flags &= ~SC_FL_SHUT_WANTED;
	if (sc->flags & SC_FL_SHUT_DONE)
		return;
	sc->flags |= SC_FL_SHUT_DONE;
	oc->flags |= CF_WRITE_EVENT;
	sc_set_hcto(sc);

	switch (sc->state) {
	case SC_ST_RDY:
	case SC_ST_EST:
		/* we have to shut before closing, otherwise some short messages
		 * may never leave the system, especially when there are remaining
		 * unread data in the socket input buffer, or when nolinger is set.
		 * However, if SC_FL_NOLINGER is explicitly set, we know there is
		 * no risk so we close both sides immediately.
		 */
		if (!(sc->flags & (SC_FL_ERROR|SC_FL_NOLINGER|SC_FL_EOS|SC_FL_ABRT_DONE)) &&
		    !(ic->flags & CF_DONT_READ))
			return;

		__fallthrough;
	case SC_ST_CON:
	case SC_ST_CER:
	case SC_ST_QUE:
	case SC_ST_TAR:
		/* Note that none of these states may happen with applets */
		sc->state = SC_ST_DIS;
		__fallthrough;
	default:
		sc->flags &= ~SC_FL_NOLINGER;
		sc->flags |= SC_FL_ABRT_DONE;
		if (sc->flags & SC_FL_ISBACK)
			__sc_strm(sc)->conn_exp = TICK_ETERNITY;
	}

	/* note that if the task exists, it must unregister itself once it runs */
	if (!(sc->flags & SC_FL_DONT_WAKE))
		task_wakeup(sc_strm_task(sc), TASK_WOKEN_IO);
}

/* default chk_rcv function for scheduled tasks */
static void sc_app_chk_rcv(struct stconn *sc)
{
	struct channel *ic = sc_ic(sc);

	if (ic->pipe) {
		/* stop reading */
		sc_need_room(sc, -1);
	}
	else {
		/* (re)start reading */
		if (!(sc->flags & SC_FL_DONT_WAKE))
			task_wakeup(sc_strm_task(sc), TASK_WOKEN_IO);
	}
}

/* default chk_snd function for scheduled tasks */
static void sc_app_chk_snd(struct stconn *sc)
{
	struct channel *oc = sc_oc(sc);

	if (unlikely(sc->state != SC_ST_EST || (sc->flags & SC_FL_SHUT_DONE)))
		return;

	if (!sc_ep_test(sc, SE_FL_WAIT_DATA) ||  /* not waiting for data */
	    channel_is_empty(oc))                  /* called with nothing to send ! */
		return;

	/* Otherwise there are remaining data to be sent in the buffer,
	 * so we tell the handler.
	 */
	sc_ep_clr(sc, SE_FL_WAIT_DATA);
	if (!(sc->flags & SC_FL_DONT_WAKE))
		task_wakeup(sc_strm_task(sc), TASK_WOKEN_IO);
}

/*
 * This function performs a shutdown-read on a stream connector attached to
 * a connection in a connected or init state (it does nothing for other
 * states). It either shuts the read side or marks itself as closed. The buffer
 * flags are updated to reflect the new state. If the stream connector has
 * SC_FL_NOHALF, we also forward the close to the write side. If a control
 * layer is defined, then it is supposed to be a socket layer and file
 * descriptors are then shutdown or closed accordingly. The function
 * automatically disables polling if needed.
 */
static void sc_app_abort_conn(struct stconn *sc)
{
	struct channel *ic = sc_ic(sc);

	BUG_ON(!sc_conn(sc));

	if (sc->flags & (SC_FL_EOS|SC_FL_ABRT_DONE))
		return;
	sc->flags |= SC_FL_ABRT_DONE;
	ic->flags |= CF_READ_EVENT;

	if (!sc_state_in(sc->state, SC_SB_CON|SC_SB_RDY|SC_SB_EST))
		return;

	if (sc->flags & SC_FL_SHUT_DONE) {
		sc_conn_shut(sc);
		sc->state = SC_ST_DIS;
		if (sc->flags & SC_FL_ISBACK)
			__sc_strm(sc)->conn_exp = TICK_ETERNITY;
	}
	else if (sc_cond_forward_shut(sc))
		return sc_app_shut_conn(sc);
}

/*
 * This function performs a shutdown-write on a stream connector attached to
 * a connection in a connected or init state (it does nothing for other
 * states). It either shuts the write side or marks itself as closed. The
 * buffer flags are updated to reflect the new state.  It does also close
 * everything if the SC was marked as being in error state. If there is a
 * data-layer shutdown, it is called.
 */
static void sc_app_shut_conn(struct stconn *sc)
{
	struct channel *ic = sc_ic(sc);
	struct channel *oc = sc_oc(sc);

	BUG_ON(!sc_conn(sc));

	sc->flags &= ~SC_FL_SHUT_WANTED;
	if (sc->flags & SC_FL_SHUT_DONE)
		return;
	sc->flags |= SC_FL_SHUT_DONE;
	oc->flags |= CF_WRITE_EVENT;
	sc_set_hcto(sc);

	switch (sc->state) {
	case SC_ST_RDY:
	case SC_ST_EST:
		/* we have to shut before closing, otherwise some short messages
		 * may never leave the system, especially when there are remaining
		 * unread data in the socket input buffer, or when nolinger is set.
		 * However, if SC_FL_NOLINGER is explicitly set, we know there is
		 * no risk so we close both sides immediately.
		 */

		if (sc->flags & SC_FL_ERROR) {
			/* quick close, the socket is already shut anyway */
		}
		else if (sc->flags & SC_FL_NOLINGER) {
			/* unclean data-layer shutdown, typically an aborted request
			 * or a forwarded shutdown from a client to a server due to
			 * option abortonclose. No need for the TLS layer to try to
			 * emit a shutdown message.
			 */
			sc_conn_shutw(sc, CO_SHW_SILENT);
		}
		else {
			/* clean data-layer shutdown. This only happens on the
			 * frontend side, or on the backend side when forwarding
			 * a client close in TCP mode or in HTTP TUNNEL mode
			 * while option abortonclose is set. We want the TLS
			 * layer to try to signal it to the peer before we close.
			 */
			sc_conn_shutw(sc, CO_SHW_NORMAL);

			if (!(sc->flags & (SC_FL_EOS|SC_FL_ABRT_DONE)) && !(ic->flags & CF_DONT_READ))
				return;
		}

		__fallthrough;
	case SC_ST_CON:
		/* we may have to close a pending connection, and mark the
		 * response buffer as abort
		 */
		sc_conn_shut(sc);
		__fallthrough;
	case SC_ST_CER:
	case SC_ST_QUE:
	case SC_ST_TAR:
		sc->state = SC_ST_DIS;
		__fallthrough;
	default:
		sc->flags &= ~SC_FL_NOLINGER;
		sc->flags |= SC_FL_ABRT_DONE;
		if (sc->flags & SC_FL_ISBACK)
			__sc_strm(sc)->conn_exp = TICK_ETERNITY;
	}
}

/* This function is used for inter-stream connector calls. It is called by the
 * consumer to inform the producer side that it may be interested in checking
 * for free space in the buffer. Note that it intentionally does not update
 * timeouts, so that we can still check them later at wake-up. This function is
 * dedicated to connection-based stream connectors.
 */
static void sc_app_chk_rcv_conn(struct stconn *sc)
{
	BUG_ON(!sc_conn(sc));

	/* (re)start reading */
	if (sc_state_in(sc->state, SC_SB_CON|SC_SB_RDY|SC_SB_EST))
		tasklet_wakeup(sc->wait_event.tasklet);
}


/* This function is used for inter-stream connector calls. It is called by the
 * producer to inform the consumer side that it may be interested in checking
 * for data in the buffer. Note that it intentionally does not update timeouts,
 * so that we can still check them later at wake-up.
 */
static void sc_app_chk_snd_conn(struct stconn *sc)
{
	struct channel *oc = sc_oc(sc);

	BUG_ON(!sc_conn(sc));

	if (unlikely(!sc_state_in(sc->state, SC_SB_RDY|SC_SB_EST) ||
		     (sc->flags & SC_FL_SHUT_DONE)))
		return;

	if (unlikely(channel_is_empty(oc)))  /* called with nothing to send ! */
		return;

	if (!oc->pipe &&                          /* spliced data wants to be forwarded ASAP */
	    !sc_ep_test(sc, SE_FL_WAIT_DATA))       /* not waiting for data */
		return;

	if (!(sc->wait_event.events & SUB_RETRY_SEND) && !channel_is_empty(sc_oc(sc)))
		sc_conn_send(sc);

	if (sc_ep_test(sc, SE_FL_ERROR | SE_FL_ERR_PENDING) || sc_is_conn_error(sc)) {
		/* Write error on the file descriptor */
		BUG_ON(sc_ep_test(sc, SE_FL_EOS|SE_FL_ERROR|SE_FL_ERR_PENDING) == (SE_FL_EOS|SE_FL_ERR_PENDING));
		goto out_wakeup;
	}

	/* OK, so now we know that some data might have been sent, and that we may
	 * have to poll first. We have to do that too if the buffer is not empty.
	 */
	if (channel_is_empty(oc)) {
		/* the connection is established but we can't write. Either the
		 * buffer is empty, or we just refrain from sending because the
		 * ->o limit was reached. Maybe we just wrote the last
		 * chunk and need to close.
		 */
		if ((oc->flags & CF_AUTO_CLOSE) &&
		    ((sc->flags & (SC_FL_SHUT_DONE|SC_FL_SHUT_WANTED)) == SC_FL_SHUT_WANTED) &&
		    sc_state_in(sc->state, SC_SB_RDY|SC_SB_EST)) {
			sc_shutdown(sc);
			goto out_wakeup;
		}

		if ((sc->flags & (SC_FL_SHUT_DONE|SC_FL_SHUT_WANTED)) == 0)
			sc_ep_set(sc, SE_FL_WAIT_DATA);
	}
	else {
		/* Otherwise there are remaining data to be sent in the buffer,
		 * which means we have to poll before doing so.
		 */
		sc_ep_clr(sc, SE_FL_WAIT_DATA);
	}

	/* in case of special condition (error, shutdown, end of write...), we
	 * have to notify the task.
	 */
	if (likely((sc->flags & SC_FL_SHUT_DONE) ||
		   ((oc->flags & CF_WRITE_EVENT) && sc->state < SC_ST_EST) ||
		   ((oc->flags & CF_WAKE_WRITE) &&
		    ((channel_is_empty(oc) && !oc->to_forward) ||
		     !sc_state_in(sc->state, SC_SB_EST))))) {
	out_wakeup:
		if (!(sc->flags & SC_FL_DONT_WAKE))
			task_wakeup(sc_strm_task(sc), TASK_WOKEN_IO);
	}
}

/*
 * This function performs a shutdown-read on a stream connector attached to an
 * applet in a connected or init state (it does nothing for other states). It
 * either shuts the read side or marks itself as closed. The buffer flags are
 * updated to reflect the new state. If the stream connector has SC_FL_NOHALF,
 * we also forward the close to the write side. The owner task is woken up if
 * it exists.
 */
static void sc_app_abort_applet(struct stconn *sc)
{
	struct channel *ic = sc_ic(sc);

	BUG_ON(!sc_appctx(sc));

	if (sc->flags & (SC_FL_EOS|SC_FL_ABRT_DONE))
		return;
	sc->flags |= SC_FL_ABRT_DONE;
	ic->flags |= CF_READ_EVENT;

	/* Note: on abort, we don't call the applet */

	if (!sc_state_in(sc->state, SC_SB_CON|SC_SB_RDY|SC_SB_EST))
		return;

	if (sc->flags & SC_FL_SHUT_DONE) {
		appctx_shut(__sc_appctx(sc));
		sc->state = SC_ST_DIS;
		if (sc->flags & SC_FL_ISBACK)
			__sc_strm(sc)->conn_exp = TICK_ETERNITY;
	}
	else if (sc_cond_forward_shut(sc))
		return sc_app_shut_applet(sc);
}

/*
 * This function performs a shutdown-write on a stream connector attached to an
 * applet in a connected or init state (it does nothing for other states). It
 * either shuts the write side or marks itself as closed. The buffer flags are
 * updated to reflect the new state. It does also close everything if the SI
 * was marked as being in error state. The owner task is woken up if it exists.
 */
static void sc_app_shut_applet(struct stconn *sc)
{
	struct channel *ic = sc_ic(sc);
	struct channel *oc = sc_oc(sc);

	BUG_ON(!sc_appctx(sc));

	sc->flags &= ~SC_FL_SHUT_WANTED;
	if (sc->flags & SC_FL_SHUT_DONE)
		return;
	sc->flags |= SC_FL_SHUT_DONE;
	oc->flags |= CF_WRITE_EVENT;
	sc_set_hcto(sc);

	/* on shutw we always wake the applet up */
	appctx_wakeup(__sc_appctx(sc));

	switch (sc->state) {
	case SC_ST_RDY:
	case SC_ST_EST:
		/* we have to shut before closing, otherwise some short messages
		 * may never leave the system, especially when there are remaining
		 * unread data in the socket input buffer, or when nolinger is set.
		 * However, if SC_FL_NOLINGER is explicitly set, we know there is
		 * no risk so we close both sides immediately.
		 */
		if (!(sc->flags & (SC_FL_ERROR|SC_FL_NOLINGER|SC_FL_EOS|SC_FL_ABRT_DONE)) &&
		    !(ic->flags & CF_DONT_READ))
			return;

		__fallthrough;
	case SC_ST_CON:
	case SC_ST_CER:
	case SC_ST_QUE:
	case SC_ST_TAR:
		/* Note that none of these states may happen with applets */
		appctx_shut(__sc_appctx(sc));
		sc->state = SC_ST_DIS;
		__fallthrough;
	default:
		sc->flags &= ~SC_FL_NOLINGER;
		sc->flags |= SC_FL_ABRT_DONE;
		if (sc->flags & SC_FL_ISBACK)
			__sc_strm(sc)->conn_exp = TICK_ETERNITY;
	}
}

/* chk_rcv function for applets */
static void sc_app_chk_rcv_applet(struct stconn *sc)
{
	struct channel *ic = sc_ic(sc);

	BUG_ON(!sc_appctx(sc));

	if (!ic->pipe) {
		/* (re)start reading */
		appctx_wakeup(__sc_appctx(sc));
	}
}

/* chk_snd function for applets */
static void sc_app_chk_snd_applet(struct stconn *sc)
{
	struct channel *oc = sc_oc(sc);

	BUG_ON(!sc_appctx(sc));

	if (unlikely(sc->state != SC_ST_EST || (sc->flags & SC_FL_SHUT_DONE)))
		return;

	/* we only wake the applet up if it was waiting for some data  and is ready to consume it
	 * or if there is a pending shutdown
	 */
	if (!sc_ep_test(sc, SE_FL_WAIT_DATA|SE_FL_WONT_CONSUME) && !(sc->flags & SC_FL_SHUT_WANTED))
		return;

	if (!channel_is_empty(oc)) {
		/* (re)start sending */
		appctx_wakeup(__sc_appctx(sc));
	}
}


/* This function is designed to be called from within the stream handler to
 * update the input channel's expiration timer and the stream connector's
 * Rx flags based on the channel's flags. It needs to be called only once
 * after the channel's flags have settled down, and before they are cleared,
 * though it doesn't harm to call it as often as desired (it just slightly
 * hurts performance). It must not be called from outside of the stream
 * handler, as what it does will be used to compute the stream task's
 * expiration.
 */
void sc_update_rx(struct stconn *sc)
{
	struct channel *ic = sc_ic(sc);

	if (sc->flags & (SC_FL_EOS|SC_FL_ABRT_DONE))
		return;

	/* Unblock the SC if it needs room and the free space is large enough (0
	 * means it can always be unblocked). Do not unblock it if -1 was
	 * specified.
	 */
	if (!sc->room_needed || (sc->room_needed > 0 && channel_recv_max(ic) >= sc->room_needed))
		sc_have_room(sc);

	/* Read not closed, update FD status and timeout for reads */
	if (ic->flags & CF_DONT_READ)
		sc_wont_read(sc);
	else
		sc_will_read(sc);

	sc_chk_rcv(sc);
}

/* This function is designed to be called from within the stream handler to
 * update the output channel's expiration timer and the stream connector's
 * Tx flags based on the channel's flags. It needs to be called only once
 * after the channel's flags have settled down, and before they are cleared,
 * though it doesn't harm to call it as often as desired (it just slightly
 * hurts performance). It must not be called from outside of the stream
 * handler, as what it does will be used to compute the stream task's
 * expiration.
 */
void sc_update_tx(struct stconn *sc)
{
	struct channel *oc = sc_oc(sc);

	if (sc->flags & SC_FL_SHUT_DONE)
		return;

	/* Write not closed, update FD status and timeout for writes */
	if (channel_is_empty(oc)) {
		/* stop writing */
		if (!sc_ep_test(sc, SE_FL_WAIT_DATA)) {
			if ((sc->flags & SC_FL_SHUT_WANTED) == 0)
				sc_ep_set(sc, SE_FL_WAIT_DATA);
		}
		return;
	}

	/* (re)start writing */
	sc_ep_clr(sc, SE_FL_WAIT_DATA);
}

/* This function is the equivalent to sc_update() except that it's
 * designed to be called from outside the stream handlers, typically the lower
 * layers (applets, connections) after I/O completion. After updating the stream
 * interface and timeouts, it will try to forward what can be forwarded, then to
 * wake the associated task up if an important event requires special handling.
 * It may update SE_FL_WAIT_DATA and/or SC_FL_NEED_ROOM, that the callers are
 * encouraged to watch to take appropriate action.
 * It should not be called from within the stream itself, sc_update()
 * is designed for this.
 */
static void sc_notify(struct stconn *sc)
{
	struct channel *ic = sc_ic(sc);
	struct channel *oc = sc_oc(sc);
	struct stconn *sco = sc_opposite(sc);
	struct task *task = sc_strm_task(sc);

	/* process consumer side */
	if (channel_is_empty(oc)) {
		struct connection *conn = sc_conn(sc);

		if (((sc->flags & (SC_FL_SHUT_DONE|SC_FL_SHUT_WANTED)) == SC_FL_SHUT_WANTED) &&
		    (sc->state == SC_ST_EST) && (!conn || !(conn->flags & (CO_FL_WAIT_XPRT | CO_FL_EARLY_SSL_HS))))
			sc_shutdown(sc);
	}

	/* indicate that we may be waiting for data from the output channel or
	 * we're about to close and can't expect more data if SC_FL_SHUT_WANTED is there.
	 */
	if (!(sc->flags & (SC_FL_SHUT_DONE|SC_FL_SHUT_WANTED)))
		sc_ep_set(sc, SE_FL_WAIT_DATA);
	else if ((sc->flags & (SC_FL_SHUT_DONE|SC_FL_SHUT_WANTED)) == SC_FL_SHUT_WANTED)
		sc_ep_clr(sc, SE_FL_WAIT_DATA);

	if (oc->flags & CF_DONT_READ)
		sc_wont_read(sco);
	else
		sc_will_read(sco);

	/* Notify the other side when we've injected data into the IC that
	 * needs to be forwarded. We can do fast-forwarding as soon as there
	 * are output data, but we avoid doing this if some of the data are
	 * not yet scheduled for being forwarded, because it is very likely
	 * that it will be done again immediately afterwards once the following
	 * data are parsed (eg: HTTP chunking). We only clear SC_FL_NEED_ROOM
	 * once we've emptied *some* of the output buffer, and not just when
	 * there is available room, because applets are often forced to stop
	 * before the buffer is full. We must not stop based on input data
	 * alone because an HTTP parser might need more data to complete the
	 * parsing.
	 */
	if (!channel_is_empty(ic) &&
	    sc_ep_test(sco, SE_FL_WAIT_DATA) &&
	     (!(sc->flags & SC_FL_SND_EXP_MORE) || channel_full(ic, co_data(ic)) || channel_input_data(ic) == 0)) {
		int new_len, last_len;

		last_len = co_data(ic);
		if (ic->pipe)
			last_len += ic->pipe->data;

		sc_chk_snd(sco);

		new_len = co_data(ic);
		if (ic->pipe)
			new_len += ic->pipe->data;

		/* check if the consumer has freed some space either in the
		 * buffer or in the pipe.
		 */
		if (!sc->room_needed || (new_len < last_len && (sc->room_needed < 0 || channel_recv_max(ic) >= sc->room_needed)))
			sc_have_room(sc);
	}

	if (!(ic->flags & CF_DONT_READ))
		sc_will_read(sc);

	sc_chk_rcv(sc);
	sc_chk_rcv(sco);

	/* wake the task up only when needed */
	if (/* changes on the production side that must be handled:
	     *  - An error on receipt: SC_FL_ERROR
	     *  - A read event: shutdown for reads (CF_READ_EVENT + EOS/ABRT_DONE)
	     *                  end of input (CF_READ_EVENT + SC_FL_EOI)
	     *                  data received and no fast-forwarding (CF_READ_EVENT + !to_forward)
	     *                  read event while consumer side is not established (CF_READ_EVENT + sco->state != SC_ST_EST)
	     */
		((ic->flags & CF_READ_EVENT) && ((sc->flags & SC_FL_EOI) || (sc->flags & (SC_FL_EOS|SC_FL_ABRT_DONE)) || !ic->to_forward || sco->state != SC_ST_EST)) ||
		(sc->flags & SC_FL_ERROR) ||

	    /* changes on the consumption side */
	    sc_ep_test(sc, SE_FL_ERR_PENDING) ||
	    ((oc->flags & CF_WRITE_EVENT) &&
	     ((sc->state < SC_ST_EST) ||
	      (sc->flags & SC_FL_SHUT_DONE) ||
	      (((oc->flags & CF_WAKE_WRITE) ||
		(!(oc->flags & CF_AUTO_CLOSE) &&
		 !(sc->flags & (SC_FL_SHUT_WANTED|SC_FL_SHUT_DONE)))) &&
	       (sco->state != SC_ST_EST ||
		(channel_is_empty(oc) && !oc->to_forward)))))) {
		task_wakeup(task, TASK_WOKEN_IO);
	}
	else {
		/* Update expiration date for the task and requeue it if not already expired */
		if (!tick_is_expired(task->expire, now_ms)) {
			task->expire = tick_first(task->expire, sc_ep_rcv_ex(sc));
			task->expire = tick_first(task->expire, sc_ep_snd_ex(sc));
			task->expire = tick_first(task->expire, sc_ep_rcv_ex(sco));
			task->expire = tick_first(task->expire, sc_ep_snd_ex(sco));
			task->expire = tick_first(task->expire, ic->analyse_exp);
			task->expire = tick_first(task->expire, oc->analyse_exp);
			task->expire = tick_first(task->expire, __sc_strm(sc)->conn_exp);

			task_queue(task);
		}
	}

	if (ic->flags & CF_READ_EVENT)
		sc->flags &= ~SC_FL_RCV_ONCE;
}

/*
 * This function propagates an end-of-stream received on a socket-based connection.
 * It updates the stream connector. If the stream connector has SC_FL_NOHALF,
 * the close is also forwarded to the write side as an abort.
 */
static void sc_conn_eos(struct stconn *sc)
{
	struct channel *ic = sc_ic(sc);

	BUG_ON(!sc_conn(sc));

	if (sc->flags & (SC_FL_EOS|SC_FL_ABRT_DONE))
		return;
	sc->flags |= SC_FL_EOS;
	ic->flags |= CF_READ_EVENT;
	sc_ep_report_read_activity(sc);

	if (!sc_state_in(sc->state, SC_SB_CON|SC_SB_RDY|SC_SB_EST))
		return;

	if (sc->flags & SC_FL_SHUT_DONE)
		goto do_close;

	if (sc_cond_forward_shut(sc)) {
		/* we want to immediately forward this close to the write side */
		/* force flag on ssl to keep stream in cache */
		sc_conn_shutw(sc, CO_SHW_SILENT);
		goto do_close;
	}

	/* otherwise that's just a normal read shutdown */
	return;

 do_close:
	/* OK we completely close the socket here just as if we went through sc_shut[rw]() */
	sc_conn_shut(sc);

	sc->flags &= ~SC_FL_SHUT_WANTED;
	sc->flags |= SC_FL_SHUT_DONE;

	sc->state = SC_ST_DIS;
	if (sc->flags & SC_FL_ISBACK)
		__sc_strm(sc)->conn_exp = TICK_ETERNITY;
	return;
}

/*
 * This is the callback which is called by the connection layer to receive data
 * into the buffer from the connection. It iterates over the mux layer's
 * rcv_buf function.
 */
static int sc_conn_recv(struct stconn *sc)
{
	struct connection *conn = __sc_conn(sc);
	struct channel *ic = sc_ic(sc);
	int ret, max, cur_read = 0;
	int read_poll = MAX_READ_POLL_LOOPS;
	int flags = 0;

	/* If not established yet, do nothing. */
	if (sc->state != SC_ST_EST)
		return 0;

	/* If another call to sc_conn_recv() failed, and we subscribed to
	 * recv events already, give up now.
	 */
	if ((sc->wait_event.events & SUB_RETRY_RECV) || sc_waiting_room(sc))
		return 0;

	/* maybe we were called immediately after an asynchronous abort */
	if (sc->flags & (SC_FL_EOS|SC_FL_ABRT_DONE))
		return 1;

	/* we must wait because the mux is not installed yet */
	if (!conn->mux)
		return 0;

	/* stop immediately on errors. Note that we DON'T want to stop on
	 * POLL_ERR, as the poller might report a write error while there
	 * are still data available in the recv buffer. This typically
	 * happens when we send too large a request to a backend server
	 * which rejects it before reading it all.
	 */
	if (!sc_ep_test(sc, SE_FL_RCV_MORE)) {
		if (!conn_xprt_ready(conn))
			return 0;
		if (sc_ep_test(sc, SE_FL_ERROR))
			goto end_recv;
	}

	/* prepare to detect if the mux needs more room */
	sc_ep_clr(sc, SE_FL_WANT_ROOM);

	if ((ic->flags & (CF_STREAMER | CF_STREAMER_FAST)) && !co_data(ic) &&
	    global.tune.idle_timer &&
	    (unsigned short)(now_ms - ic->last_read) >= global.tune.idle_timer) {
		/* The buffer was empty and nothing was transferred for more
		 * than one second. This was caused by a pause and not by
		 * congestion. Reset any streaming mode to reduce latency.
		 */
		ic->xfer_small = 0;
		ic->xfer_large = 0;
		ic->flags &= ~(CF_STREAMER | CF_STREAMER_FAST);
	}

	/* First, let's see if we may splice data across the channel without
	 * using a buffer.
	 */
	if (sc_ep_test(sc, SE_FL_MAY_SPLICE) &&
	    (ic->pipe || ic->to_forward >= MIN_SPLICE_FORWARD) &&
	    ic->flags & CF_KERN_SPLICING) {
		if (channel_data(ic)) {
			/* We're embarrassed, there are already data pending in
			 * the buffer and we don't want to have them at two
			 * locations at a time. Let's indicate we need some
			 * place and ask the consumer to hurry.
			 */
			flags |= CO_RFL_BUF_FLUSH;
			goto abort_splice;
		}

		if (unlikely(ic->pipe == NULL)) {
			if (pipes_used >= global.maxpipes || !(ic->pipe = get_pipe())) {
				ic->flags &= ~CF_KERN_SPLICING;
				goto abort_splice;
			}
		}

		ret = conn->mux->rcv_pipe(sc, ic->pipe, ic->to_forward);
		if (ret < 0) {
			/* splice not supported on this end, let's disable it */
			ic->flags &= ~CF_KERN_SPLICING;
			goto abort_splice;
		}

		if (ret > 0) {
			if (ic->to_forward != CHN_INFINITE_FORWARD)
				ic->to_forward -= ret;
			ic->total += ret;
			cur_read += ret;
			ic->flags |= CF_READ_EVENT;
		}

		if (sc_ep_test(sc, SE_FL_EOS | SE_FL_ERROR))
			goto end_recv;

		if (conn->flags & CO_FL_WAIT_ROOM) {
			/* the pipe is full or we have read enough data that it
			 * could soon be full. Let's stop before needing to poll.
			 */
			sc_need_room(sc, 0);
			goto done_recv;
		}

		/* splice not possible (anymore), let's go on on standard copy */
	}

 abort_splice:
	if (ic->pipe && unlikely(!ic->pipe->data)) {
		put_pipe(ic->pipe);
		ic->pipe = NULL;
	}

	if (ic->pipe && ic->to_forward && !(flags & CO_RFL_BUF_FLUSH) && sc_ep_test(sc, SE_FL_MAY_SPLICE)) {
		/* don't break splicing by reading, but still call rcv_buf()
		 * to pass the flag.
		 */
		goto done_recv;
	}

	/* now we'll need a input buffer for the stream */
	if (!sc_alloc_ibuf(sc, &(__sc_strm(sc)->buffer_wait)))
		goto end_recv;

	/* For an HTX stream, if the buffer is stuck (no output data with some
	 * input data) and if the HTX message is fragmented or if its free space
	 * wraps, we force an HTX deframentation. It is a way to have a
	 * contiguous free space nad to let the mux to copy as much data as
	 * possible.
	 *
	 * NOTE: A possible optim may be to let the mux decides if defrag is
	 *       required or not, depending on amount of data to be xferred.
	 */
	if (IS_HTX_STRM(__sc_strm(sc)) && !co_data(ic)) {
		struct htx *htx = htxbuf(&ic->buf);

		if (htx_is_not_empty(htx) && ((htx->flags & HTX_FL_FRAGMENTED) || htx_space_wraps(htx)))
			htx_defrag(htx, NULL, 0);
	}

	/* Instruct the mux it must subscribed for read events */
	if (!conn_is_back(conn) &&                                 /* for frontend conns only */
	    (sc_opposite(sc)->state != SC_ST_INI) &&               /* before backend connection setup */
	    (__sc_strm(sc)->be->options & PR_O_ABRT_CLOSE))        /* if abortonclose option is set for the current backend */
		flags |= CO_RFL_KEEP_RECV;

	/* Important note : if we're called with POLL_IN|POLL_HUP, it means the read polling
	 * was enabled, which implies that the recv buffer was not full. So we have a guarantee
	 * that if such an event is not handled above in splice, it will be handled here by
	 * recv().
	 */
	while (sc_ep_test(sc, SE_FL_RCV_MORE) ||
	       (!(conn->flags & CO_FL_HANDSHAKE) &&
		(!sc_ep_test(sc, SE_FL_ERROR | SE_FL_EOS)) && !(sc->flags & (SC_FL_EOS|SC_FL_ABRT_DONE)))) {
		int cur_flags = flags;

		/* Compute transient CO_RFL_* flags */
		if (co_data(ic)) {
			cur_flags |= (CO_RFL_BUF_WET | CO_RFL_BUF_NOT_STUCK);
		}

		/* <max> may be null. This is the mux responsibility to set
		 * SE_FL_RCV_MORE on the SC if more space is needed.
		 */
		max = channel_recv_max(ic);
		ret = conn->mux->rcv_buf(sc, &ic->buf, max, cur_flags);

		if (sc_ep_test(sc, SE_FL_WANT_ROOM)) {
			/* SE_FL_WANT_ROOM must not be reported if the channel's
			 * buffer is empty.
			 */
			BUG_ON(c_empty(ic));

			sc_need_room(sc, channel_recv_max(ic) + 1);
			/* Add READ_PARTIAL because some data are pending but
			 * cannot be xferred to the channel
			 */
			ic->flags |= CF_READ_EVENT;
			sc_ep_report_read_activity(sc);
		}

		if (ret <= 0) {
			/* if we refrained from reading because we asked for a
			 * flush to satisfy rcv_pipe(), we must not subscribe
			 * and instead report that there's not enough room
			 * here to proceed.
			 */
			if (flags & CO_RFL_BUF_FLUSH)
				sc_need_room(sc, -1);
			break;
		}

		cur_read += ret;

		/* if we're allowed to directly forward data, we must update ->o */
		if (ic->to_forward && !(sc_opposite(sc)->flags & (SC_FL_SHUT_DONE|SC_FL_SHUT_WANTED))) {
			unsigned long fwd = ret;
			if (ic->to_forward != CHN_INFINITE_FORWARD) {
				if (fwd > ic->to_forward)
					fwd = ic->to_forward;
				ic->to_forward -= fwd;
			}
			c_adv(ic, fwd);
		}

		ic->flags |= CF_READ_EVENT;
		ic->total += ret;

		/* End-of-input reached, we can leave. In this case, it is
		 * important to break the loop to not block the SC because of
		 * the channel's policies.This way, we are still able to receive
		 * shutdowns.
		 */
		if (sc_ep_test(sc, SE_FL_EOI))
			break;

		if ((sc->flags & SC_FL_RCV_ONCE) || --read_poll <= 0) {
			/* we don't expect to read more data */
			sc_wont_read(sc);
			break;
		}

		/* if too many bytes were missing from last read, it means that
		 * it's pointless trying to read again because the system does
		 * not have them in buffers.
		 */
		if (ret < max) {
			/* if a streamer has read few data, it may be because we
			 * have exhausted system buffers. It's not worth trying
			 * again.
			 */
			if (ic->flags & CF_STREAMER) {
				/* we're stopped by the channel's policy */
				sc_wont_read(sc);
				break;
			}

			/* if we read a large block smaller than what we requested,
			 * it's almost certain we'll never get anything more.
			 */
			if (ret >= global.tune.recv_enough) {
				/* we're stopped by the channel's policy */
				sc_wont_read(sc);
				break;
			}
		}

		/* if we are waiting for more space, don't try to read more data
		 * right now.
		 */
		if (sc->flags & (SC_FL_WONT_READ|SC_FL_NEED_BUFF|SC_FL_NEED_ROOM))
			break;
	} /* while !flags */

 done_recv:
	if (cur_read) {
		if ((ic->flags & (CF_STREAMER | CF_STREAMER_FAST)) &&
		    (cur_read <= ic->buf.size / 2)) {
			ic->xfer_large = 0;
			ic->xfer_small++;
			if (ic->xfer_small >= 3) {
				/* we have read less than half of the buffer in
				 * one pass, and this happened at least 3 times.
				 * This is definitely not a streamer.
				 */
				ic->flags &= ~(CF_STREAMER | CF_STREAMER_FAST);
			}
			else if (ic->xfer_small >= 2) {
				/* if the buffer has been at least half full twice,
				 * we receive faster than we send, so at least it
				 * is not a "fast streamer".
				 */
				ic->flags &= ~CF_STREAMER_FAST;
			}
		}
		else if (!(ic->flags & CF_STREAMER_FAST) && (cur_read >= channel_data_limit(ic))) {
			/* we read a full buffer at once */
			ic->xfer_small = 0;
			ic->xfer_large++;
			if (ic->xfer_large >= 3) {
				/* we call this buffer a fast streamer if it manages
				 * to be filled in one call 3 consecutive times.
				 */
				ic->flags |= (CF_STREAMER | CF_STREAMER_FAST);
			}
		}
		else {
			ic->xfer_small = 0;
			ic->xfer_large = 0;
		}
		ic->last_read = now_ms;
		sc_ep_report_read_activity(sc);
	}

 end_recv:
	ret = (cur_read != 0);

	/* Report EOI on the channel if it was reached from the mux point of
	 * view. */
	if (sc_ep_test(sc, SE_FL_EOI) && !(sc->flags & SC_FL_EOI)) {
		sc_ep_report_read_activity(sc);
		sc->flags |= SC_FL_EOI;
		ic->flags |= CF_READ_EVENT;
		ret = 1;
	}

	if (sc_ep_test(sc, SE_FL_EOS)) {
		/* we received a shutdown */
		if (ic->flags & CF_AUTO_CLOSE)
			sc_schedule_shutdown(sc_opposite(sc));
		sc_conn_eos(sc);
		ret = 1;
	}

	if (sc_ep_test(sc, SE_FL_ERROR)) {
		sc->flags |= SC_FL_ERROR;
		ret = 1;
	}
	else if (!(sc->flags & (SC_FL_WONT_READ|SC_FL_NEED_BUFF|SC_FL_NEED_ROOM)) &&
		 !(sc->flags & (SC_FL_EOS|SC_FL_ABRT_DONE))) {
		/* Subscribe to receive events if we're blocking on I/O */
		conn->mux->subscribe(sc, SUB_RETRY_RECV, &sc->wait_event);
		se_have_no_more_data(sc->sedesc);
	}
	else {
		se_have_more_data(sc->sedesc);
		ret = 1;
	}

	return ret;
}

/* This tries to perform a synchronous receive on the stream connector to
 * try to collect last arrived data. In practice it's only implemented on
 * stconns. Returns 0 if nothing was done, non-zero if new data or a
 * shutdown were collected. This may result on some delayed receive calls
 * to be programmed and performed later, though it doesn't provide any
 * such guarantee.
 */
int sc_conn_sync_recv(struct stconn *sc)
{
	if (!sc_state_in(sc->state, SC_SB_RDY|SC_SB_EST))
		return 0;

	if (!sc_mux_ops(sc))
		return 0; // only stconns are supported

	if (sc->wait_event.events & SUB_RETRY_RECV)
		return 0; // already subscribed

	if (!sc_is_recv_allowed(sc))
		return 0; // already failed

	return sc_conn_recv(sc);
}

/*
 * This function is called to send buffer data to a stream socket.
 * It calls the mux layer's snd_buf function. It relies on the
 * caller to commit polling changes. The caller should check conn->flags
 * for errors.
 */
static int sc_conn_send(struct stconn *sc)
{
	struct connection *conn = __sc_conn(sc);
	struct stconn *sco = sc_opposite(sc);
	struct stream *s = __sc_strm(sc);
	struct channel *oc = sc_oc(sc);
	int ret;
	int did_send = 0;

	if (sc_ep_test(sc, SE_FL_ERROR | SE_FL_ERR_PENDING) || sc_is_conn_error(sc)) {
		/* We're probably there because the tasklet was woken up,
		 * but process_stream() ran before, detected there were an
		 * error and put the SC back to SC_ST_TAR. There's still
		 * CO_FL_ERROR on the connection but we don't want to add
		 * SE_FL_ERROR back, so give up
		 */
		if (sc->state < SC_ST_CON)
			return 0;
		BUG_ON(sc_ep_test(sc, SE_FL_EOS|SE_FL_ERROR|SE_FL_ERR_PENDING) == (SE_FL_EOS|SE_FL_ERR_PENDING));
		return 1;
	}

	/* We're already waiting to be able to send, give up */
	if (sc->wait_event.events & SUB_RETRY_SEND)
		return 0;

	/* we might have been called just after an asynchronous shutw */
	if (sc->flags & SC_FL_SHUT_DONE)
		return 1;

	/* we must wait because the mux is not installed yet */
	if (!conn->mux)
		return 0;

	if (oc->pipe && conn->xprt->snd_pipe && conn->mux->snd_pipe) {
		ret = conn->mux->snd_pipe(sc, oc->pipe);
		if (ret > 0)
			did_send = 1;

		if (!oc->pipe->data) {
			put_pipe(oc->pipe);
			oc->pipe = NULL;
		}

		if (oc->pipe)
			goto end;
	}

	/* At this point, the pipe is empty, but we may still have data pending
	 * in the normal buffer.
	 */
	if (co_data(oc)) {
		/* when we're here, we already know that there is no spliced
		 * data left, and that there are sendable buffered data.
		 */

		/* check if we want to inform the kernel that we're interested in
		 * sending more data after this call. We want this if :
		 *  - we're about to close after this last send and want to merge
		 *    the ongoing FIN with the last segment.
		 *  - we know we can't send everything at once and must get back
		 *    here because of unaligned data
		 *  - there is still a finite amount of data to forward
		 * The test is arranged so that the most common case does only 2
		 * tests.
		 */
		unsigned int send_flag = 0;

		if ((!(sc->flags & (SC_FL_SND_ASAP|SC_FL_SND_NEVERWAIT)) &&
		     ((oc->to_forward && oc->to_forward != CHN_INFINITE_FORWARD) ||
		      (sc->flags & SC_FL_SND_EXP_MORE) ||
		      (IS_HTX_STRM(s) &&
		       (!(sco->flags & (SC_FL_EOI|SC_FL_EOS|SC_FL_ABRT_DONE)) && htx_expect_more(htxbuf(&oc->buf)))))) ||
		    ((oc->flags & CF_ISRESP) &&
		     (oc->flags & CF_AUTO_CLOSE) &&
		     (sc->flags & SC_FL_SHUT_WANTED)))
			send_flag |= CO_SFL_MSG_MORE;

		if (oc->flags & CF_STREAMER)
			send_flag |= CO_SFL_STREAMER;

		if (s->txn && s->txn->flags & TX_L7_RETRY && !b_data(&s->txn->l7_buffer)) {
			/* If we want to be able to do L7 retries, copy
			 * the data we're about to send, so that we are able
			 * to resend them if needed
			 */
			/* Try to allocate a buffer if we had none.
			 * If it fails, the next test will just
			 * disable the l7 retries by setting
			 * l7_conn_retries to 0.
			 */
			if (s->txn->req.msg_state != HTTP_MSG_DONE)
				s->txn->flags &= ~TX_L7_RETRY;
			else {
				if (b_alloc(&s->txn->l7_buffer) == NULL)
					s->txn->flags &= ~TX_L7_RETRY;
				else {
					memcpy(b_orig(&s->txn->l7_buffer),
					       b_orig(&oc->buf),
					       b_size(&oc->buf));
					s->txn->l7_buffer.head = co_data(oc);
					b_add(&s->txn->l7_buffer, co_data(oc));
				}

			}
		}

		ret = conn->mux->snd_buf(sc, &oc->buf, co_data(oc), send_flag);
		if (ret > 0) {
			did_send = 1;
			c_rew(oc, ret);
			c_realign_if_empty(oc);

			if (!co_data(oc)) {
				/* Always clear both flags once everything has been sent, they're one-shot */
				sc->flags &= ~(SC_FL_SND_ASAP|SC_FL_SND_EXP_MORE);
			}
			/* if some data remain in the buffer, it's only because the
			 * system buffers are full, we will try next time.
			 */
		}
	}

 end:
	if (did_send) {
		oc->flags |= CF_WRITE_EVENT | CF_WROTE_DATA;
		if (sc->state == SC_ST_CON)
			sc->state = SC_ST_RDY;
	}

	if (!sco->room_needed || (did_send && (sco->room_needed < 0 || channel_recv_max(sc_oc(sc)) >= sco->room_needed)))
		sc_have_room(sco);

	if (sc_ep_test(sc, SE_FL_ERROR | SE_FL_ERR_PENDING)) {
		oc->flags |= CF_WRITE_EVENT;
		BUG_ON(sc_ep_test(sc, SE_FL_EOS|SE_FL_ERROR|SE_FL_ERR_PENDING) == (SE_FL_EOS|SE_FL_ERR_PENDING));
		if (sc_ep_test(sc, SE_FL_ERROR))
			sc->flags |= SC_FL_ERROR;
		return 1;
	}

	if (channel_is_empty(oc)) {
		if (did_send)
			sc_ep_report_send_activity(sc);
	}
	else {
		/* We couldn't send all of our data, let the mux know we'd like to send more */
		conn->mux->subscribe(sc, SUB_RETRY_SEND, &sc->wait_event);
		if (sc_state_in(sc->state, SC_SB_EST|SC_SB_DIS|SC_SB_CLO))
				sc_ep_report_blocked_send(sc, did_send);
	}

	return did_send;
}

/* perform a synchronous send() for the stream connector. The CF_WRITE_EVENT
 * flag are cleared prior to the attempt, and will possibly be updated in case
 * of success.
 */
void sc_conn_sync_send(struct stconn *sc)
{
	struct channel *oc = sc_oc(sc);

	oc->flags &= ~CF_WRITE_EVENT;

	if (sc->flags & SC_FL_SHUT_DONE)
		return;

	if (channel_is_empty(oc))
		return;

	if (!sc_state_in(sc->state, SC_SB_CON|SC_SB_RDY|SC_SB_EST))
		return;

	if (!sc_mux_ops(sc))
		return;

	sc_conn_send(sc);
}

/* Called by I/O handlers after completion.. It propagates
 * connection flags to the stream connector, updates the stream (which may or
 * may not take this opportunity to try to forward data), then update the
 * connection's polling based on the channels and stream connector's final
 * states. The function always returns 0.
 */
static int sc_conn_process(struct stconn *sc)
{
	struct connection *conn = __sc_conn(sc);
	struct channel *ic = sc_ic(sc);
	struct channel *oc = sc_oc(sc);

	BUG_ON(!conn);

	/* If we have data to send, try it now */
	if (!channel_is_empty(oc) && !(sc->wait_event.events & SUB_RETRY_SEND))
		sc_conn_send(sc);

	/* First step, report to the stream connector what was detected at the
	 * connection layer : errors and connection establishment.
	 * Only add SC_FL_ERROR if we're connected, or we're attempting to
	 * connect, we may get there because we got woken up, but only run
	 * after process_stream() noticed there were an error, and decided
	 * to retry to connect, the connection may still have CO_FL_ERROR,
	 * and we don't want to add SC_FL_ERROR back
	 *
	 * Note: This test is only required because sc_conn_process is also the SI
	 *       wake callback. Otherwise sc_conn_recv()/sc_conn_send() already take
	 *       care of it.
	 */

	if (sc->state >= SC_ST_CON) {
		if (sc_is_conn_error(sc))
			sc->flags |= SC_FL_ERROR;
	}

	/* If we had early data, and the handshake ended, then
	 * we can remove the flag, and attempt to wake the task up,
	 * in the event there's an analyser waiting for the end of
	 * the handshake.
	 */
	if (!(conn->flags & (CO_FL_WAIT_XPRT | CO_FL_EARLY_SSL_HS)) &&
	    sc_ep_test(sc, SE_FL_WAIT_FOR_HS)) {
		sc_ep_clr(sc, SE_FL_WAIT_FOR_HS);
		task_wakeup(sc_strm_task(sc), TASK_WOKEN_MSG);
	}

	if (!sc_state_in(sc->state, SC_SB_EST|SC_SB_DIS|SC_SB_CLO) &&
	    (conn->flags & CO_FL_WAIT_XPRT) == 0) {
		if (sc->flags & SC_FL_ISBACK)
			__sc_strm(sc)->conn_exp = TICK_ETERNITY;
		oc->flags |= CF_WRITE_EVENT;
		if (sc->state == SC_ST_CON)
			sc->state = SC_ST_RDY;
	}

	/* Report EOS on the channel if it was reached from the mux point of
	 * view.
	 *
	 * Note: This test is only required because sc_conn_process is also the SI
	 *       wake callback. Otherwise sc_conn_recv()/sc_conn_send() already take
	 *       care of it.
	 */
	if (sc_ep_test(sc, SE_FL_EOS) && !(sc->flags & SC_FL_EOS)) {
		/* we received a shutdown */
		if (ic->flags & CF_AUTO_CLOSE)
			sc_schedule_shutdown(sc_opposite(sc));
		sc_conn_eos(sc);
	}

	/* Report EOI on the channel if it was reached from the mux point of
	 * view.
	 *
	 * Note: This test is only required because sc_conn_process is also the SI
	 *       wake callback. Otherwise sc_conn_recv()/sc_conn_send() already take
	 *       care of it.
	 */
	if (sc_ep_test(sc, SE_FL_EOI) && !(sc->flags & SC_FL_EOI)) {
		sc->flags |= SC_FL_EOI;
		ic->flags |= CF_READ_EVENT;
		sc_ep_report_read_activity(sc);
	}

	if (sc_ep_test(sc, SE_FL_ERROR))
		sc->flags |= SC_FL_ERROR;

	/* Second step : update the stream connector and channels, try to forward any
	 * pending data, then possibly wake the stream up based on the new
	 * stream connector status.
	 */
	sc_notify(sc);
	stream_release_buffers(__sc_strm(sc));
	return 0;
}

/* This is the ->process() function for any stream connector's wait_event task.
 * It's assigned during the stream connector's initialization, for any type of
 * stream connector. Thus it is always safe to perform a tasklet_wakeup() on a
 * stream connector, as the presence of the SC is checked there.
 */
struct task *sc_conn_io_cb(struct task *t, void *ctx, unsigned int state)
{
	struct stconn *sc = ctx;
	int ret = 0;

	if (!sc_conn(sc))
		return t;

	if (!(sc->wait_event.events & SUB_RETRY_SEND) && !channel_is_empty(sc_oc(sc)))
		ret = sc_conn_send(sc);
	if (!(sc->wait_event.events & SUB_RETRY_RECV))
		ret |= sc_conn_recv(sc);
	if (ret != 0)
		sc_conn_process(sc);

	stream_release_buffers(__sc_strm(sc));
	return t;
}

/*
 * This function propagates an end-of-stream received from an applet. It
 * updates the stream connector. If it is is already shut, the applet is
 * released. Otherwise, we try to forward the shutdown, immediately or ASAP.
 */
static void sc_applet_eos(struct stconn *sc)
{
	struct channel *ic = sc_ic(sc);

	BUG_ON(!sc_appctx(sc));

	if (sc->flags & (SC_FL_EOS|SC_FL_ABRT_DONE))
		return;
	sc->flags |= SC_FL_EOS;
	ic->flags |= CF_READ_EVENT;
	sc_ep_report_read_activity(sc);

	/* Note: on abort, we don't call the applet */

	if (!sc_state_in(sc->state, SC_SB_CON|SC_SB_RDY|SC_SB_EST))
		return;

	if (sc->flags & SC_FL_SHUT_DONE) {
		appctx_shut(__sc_appctx(sc));
		sc->state = SC_ST_DIS;
		if (sc->flags & SC_FL_ISBACK)
			__sc_strm(sc)->conn_exp = TICK_ETERNITY;
	}
	else if (sc_cond_forward_shut(sc))
		return sc_app_shut_applet(sc);
}

/* Callback to be used by applet handlers upon completion. It updates the stream
 * (which may or may not take this opportunity to try to forward data), then
 * may re-enable the applet's based on the channels and stream connector's final
 * states.
 */
static int sc_applet_process(struct stconn *sc)
{
	struct channel *ic = sc_ic(sc);

	BUG_ON(!sc_appctx(sc));

	/* Report EOI on the channel if it was reached from the applet point of
	 * view. */
	if (sc_ep_test(sc, SE_FL_EOI) && !(sc->flags & SC_FL_EOI)) {
		sc_ep_report_read_activity(sc);
		sc->flags |= SC_FL_EOI;
		ic->flags |= CF_READ_EVENT;
	}

	if (sc_ep_test(sc, SE_FL_ERROR))
		sc->flags |= SC_FL_ERROR;

	if (sc_ep_test(sc, SE_FL_EOS)) {
		/* we received a shutdown */
		sc_applet_eos(sc);
	}

	BUG_ON(sc_ep_test(sc, SE_FL_HAVE_NO_DATA|SE_FL_EOI) == SE_FL_EOI);

	/* If the applet wants to write and the channel is closed, it's a
	 * broken pipe and it must be reported.
	 */
	if (!sc_ep_test(sc, SE_FL_HAVE_NO_DATA) && (sc->flags & (SC_FL_EOS|SC_FL_ABRT_DONE)))
		sc_ep_set(sc, SE_FL_ERROR);

	/* automatically mark the applet having data available if it reported
	 * begin blocked by the channel.
	 */
	if ((sc->flags & (SC_FL_WONT_READ|SC_FL_NEED_BUFF|SC_FL_NEED_ROOM)) ||
	    sc_ep_test(sc, SE_FL_APPLET_NEED_CONN))
		applet_have_more_data(__sc_appctx(sc));

	/* update the stream connector, channels, and possibly wake the stream up */
	sc_notify(sc);
	stream_release_buffers(__sc_strm(sc));

	/* sc_notify may have passed through chk_snd and released some blocking
	 * flags. Process_stream will consider those flags to wake up the
	 * appctx but in the case the task is not in runqueue we may have to
	 * wakeup the appctx immediately.
	 */
	if (sc_is_recv_allowed(sc) || sc_is_send_allowed(sc))
		appctx_wakeup(__sc_appctx(sc));
	return 0;
}


/* Prepares an endpoint upgrade. We don't now at this stage if the upgrade will
 * succeed or not and if the stconn will be reused by the new endpoint. Thus,
 * for now, only pretend the stconn is detached.
 */
void sc_conn_prepare_endp_upgrade(struct stconn *sc)
{
	BUG_ON(!sc_conn(sc) || !sc->app);
	sc_ep_clr(sc, SE_FL_T_MUX);
	sc_ep_set(sc, SE_FL_DETACHED);
}

/* Endpoint upgrade failed. Restore the stconn state. */
void sc_conn_abort_endp_upgrade(struct stconn *sc)
{
	sc_ep_set(sc, SE_FL_T_MUX);
	sc_ep_clr(sc, SE_FL_DETACHED);
}

/* Commit the endpoint upgrade. If stconn is attached, it means the new endpoint
 * use it. So we do nothing. Otherwise, the stconn will be destroy with the
 * overlying stream. So, it means we must commit the detach.
*/
void sc_conn_commit_endp_upgrade(struct stconn *sc)
{
	if (!sc_ep_test(sc, SE_FL_DETACHED))
		return;
	sc_detach_endp(&sc);
	/* Because it was already set as detached, the sedesc must be preserved */
	BUG_ON(!sc);
	BUG_ON(!sc->sedesc);
}
