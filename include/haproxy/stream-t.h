/*
 * include/haproxy/stream-t.h
 * This file defines everything related to streams.
 *
 * Copyright (C) 2000-2020 Willy Tarreau - w@1wt.eu
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

#ifndef _HAPROXY_STREAM_T_H
#define _HAPROXY_STREAM_T_H

#include <sys/time.h>

#include <haproxy/api-t.h>
#include <haproxy/channel-t.h>
#include <haproxy/stconn-t.h>
#include <haproxy/dynbuf-t.h>
#include <haproxy/filters-t.h>
#include <haproxy/obj_type-t.h>
#include <haproxy/show_flags-t.h>
#include <haproxy/stick_table-t.h>
#include <haproxy/vars-t.h>


/* Various Stream Flags, bits values 0x01 to 0x100 (shift 0).
 * Please also update the txn_show_flags() function below in case of changes.
 */
#define SF_DIRECT	0x00000001	/* connection made on the server matching the client cookie */
#define SF_ASSIGNED	0x00000002	/* no need to assign a server to this stream */
/* unused: 0x00000004 */
#define SF_BE_ASSIGNED	0x00000008	/* a backend was assigned. Conns are accounted. */

#define SF_FORCE_PRST	0x00000010	/* force persistence here, even if server is down */
#define SF_MONITOR	0x00000020	/* this stream comes from a monitoring system */
#define SF_CURR_SESS	0x00000040	/* a connection is currently being counted on the server */
#define SF_CONN_EXP     0x00000080      /* timeout has expired */
#define SF_REDISP	0x00000100	/* set if this stream was redispatched from one server to another */
#define SF_IGNORE	0x00000200      /* The stream lead to a mux upgrade, and should be ignored */
#define SF_REDIRECTABLE	0x00000400	/* set if this stream is redirectable (GET or HEAD) */
#define SF_HTX          0x00000800      /* set if this stream is an htx stream */

/* stream termination conditions, bits values 0x1000 to 0x7000 (0-9 shift 12) */
#define SF_ERR_NONE     0x00000000	/* normal end of request */
#define SF_ERR_LOCAL    0x00001000	/* the proxy locally processed this request => not an error */
#define SF_ERR_CLITO    0x00002000	/* client time-out */
#define SF_ERR_CLICL    0x00003000	/* client closed (read/write error) */
#define SF_ERR_SRVTO    0x00004000	/* server time-out, connect time-out */
#define SF_ERR_SRVCL    0x00005000	/* server closed (connect/read/write error) */
#define SF_ERR_PRXCOND  0x00006000	/* the proxy decided to close (deny...) */
#define SF_ERR_RESOURCE 0x00007000	/* the proxy encountered a lack of a local resources (fd, mem, ...) */
#define SF_ERR_INTERNAL 0x00008000	/* the proxy encountered an internal error */
#define SF_ERR_DOWN     0x00009000	/* the proxy killed a stream because the backend became unavailable */
#define SF_ERR_KILLED   0x0000a000	/* the proxy killed a stream because it was asked to do so */
#define SF_ERR_UP       0x0000b000	/* the proxy killed a stream because a preferred backend became available */
#define SF_ERR_CHK_PORT 0x0000c000	/* no port could be found for a health check. TODO: check SF_ERR_SHIFT */
#define SF_ERR_MASK     0x0000f000	/* mask to get only stream error flags */
#define SF_ERR_SHIFT    12		/* bit shift */

/* stream state at termination, bits values 0x10000 to 0x70000 (0-7 shift 16) */
#define SF_FINST_R	0x00010000	/* stream ended during client request */
#define SF_FINST_C	0x00020000	/* stream ended during server connect */
#define SF_FINST_H	0x00030000	/* stream ended during server headers */
#define SF_FINST_D	0x00040000	/* stream ended during data phase */
#define SF_FINST_L	0x00050000	/* stream ended while pushing last data to client */
#define SF_FINST_Q	0x00060000	/* stream ended while waiting in queue for a server slot */
#define SF_FINST_T	0x00070000	/* stream ended tarpitted */
#define SF_FINST_MASK	0x00070000	/* mask to get only final stream state flags */
#define	SF_FINST_SHIFT	16		/* bit shift */

#define SF_IGNORE_PRST	0x00080000	/* ignore persistence */

#define SF_SRV_REUSED   0x00100000	/* the server-side connection was reused */
#define SF_SRV_REUSED_ANTICIPATED  0x00200000  /* the connection was reused but the mux is not ready yet */
#define SF_WEBSOCKET    0x00400000	/* websocket stream */ // TODO: must be removed
#define SF_SRC_ADDR     0x00800000	/* get the source ip/port with getsockname */

/* This function is used to report flags in debugging tools. Please reflect
 * below any single-bit flag addition above in the same order via the
 * __APPEND_FLAG and __APPEND_ENUM macros. The new end of the buffer is
 * returned.
 */
static forceinline char *strm_show_flags(char *buf, size_t len, const char *delim, uint flg)
{
#define _(f, ...)     __APPEND_FLAG(buf, len, delim, flg, f, #f, __VA_ARGS__)
#define _e(m, e, ...) __APPEND_ENUM(buf, len, delim, flg, m, e, #e, __VA_ARGS__)
	/* prologue */
	_(0);
	/* flags & enums */
	_(SF_IGNORE_PRST, _(SF_SRV_REUSED, _(SF_SRV_REUSED_ANTICIPATED,
	_(SF_WEBSOCKET, _(SF_SRC_ADDR)))));

	_e(SF_FINST_MASK, SF_FINST_R,    _e(SF_FINST_MASK, SF_FINST_C,
	_e(SF_FINST_MASK, SF_FINST_H,    _e(SF_FINST_MASK, SF_FINST_D,
	_e(SF_FINST_MASK, SF_FINST_L,    _e(SF_FINST_MASK, SF_FINST_Q,
	_e(SF_FINST_MASK, SF_FINST_T)))))));

	_e(SF_ERR_MASK, SF_ERR_LOCAL,    _e(SF_ERR_MASK, SF_ERR_CLITO,
	_e(SF_ERR_MASK, SF_ERR_CLICL,    _e(SF_ERR_MASK, SF_ERR_SRVTO,
	_e(SF_ERR_MASK, SF_ERR_SRVCL,    _e(SF_ERR_MASK, SF_ERR_PRXCOND,
	_e(SF_ERR_MASK, SF_ERR_RESOURCE, _e(SF_ERR_MASK, SF_ERR_INTERNAL,
	_e(SF_ERR_MASK, SF_ERR_DOWN,     _e(SF_ERR_MASK, SF_ERR_KILLED,
	_e(SF_ERR_MASK, SF_ERR_UP,       _e(SF_ERR_MASK, SF_ERR_CHK_PORT))))))))))));

	_(SF_DIRECT, _(SF_ASSIGNED, _(SF_BE_ASSIGNED, _(SF_FORCE_PRST,
	_(SF_MONITOR, _(SF_CURR_SESS, _(SF_CONN_EXP, _(SF_REDISP,
	_(SF_IGNORE, _(SF_REDIRECTABLE, _(SF_HTX)))))))))));

	/* epilogue */
	_(~0U);
	return buf;
#undef _e
#undef _
}


/* flags for the proxy of the master CLI */
/* 0x0001.. to 0x8000 are reserved for ACCESS_* flags from cli-t.h */

#define PCLI_F_PROMPT   0x10000
#define PCLI_F_PAYLOAD  0x20000
#define PCLI_F_RELOAD   0x40000 /* this is the "reload" stream, quits after displaying reload status */
#define PCLI_F_TIMED    0x80000 /* the prompt shows the process' uptime */


/* error types reported on the streams for more accurate reporting.
 * Please also update the strm_et_show_flags() function below in case of changes.
 */
enum {
	STRM_ET_NONE       = 0x0000,  /* no error yet, leave it to zero */
	STRM_ET_QUEUE_TO   = 0x0001,  /* queue timeout */
	STRM_ET_QUEUE_ERR  = 0x0002,  /* queue error (eg: full) */
	STRM_ET_QUEUE_ABRT = 0x0004,  /* aborted in queue by external cause */
	STRM_ET_CONN_TO    = 0x0008,  /* connection timeout */
	STRM_ET_CONN_ERR   = 0x0010,  /* connection error (eg: no server available) */
	STRM_ET_CONN_ABRT  = 0x0020,  /* connection aborted by external cause (eg: abort) */
	STRM_ET_CONN_RES   = 0x0040,  /* connection aborted due to lack of resources */
	STRM_ET_CONN_OTHER = 0x0080,  /* connection aborted for other reason (eg: 500) */
	STRM_ET_DATA_TO    = 0x0100,  /* timeout during data phase */
	STRM_ET_DATA_ERR   = 0x0200,  /* error during data phase */
	STRM_ET_DATA_ABRT  = 0x0400,  /* data phase aborted by external cause */
};

/* This function is used to report flags in debugging tools. Please reflect
 * below any single-bit flag addition above in the same order via the
 * __APPEND_FLAG macro. The new end of the buffer is returned.
 */
static forceinline char *strm_et_show_flags(char *buf, size_t len, const char *delim, uint flg)
{
#define _(f, ...) __APPEND_FLAG(buf, len, delim, flg, f, #f, __VA_ARGS__)
	/* prologue */
	_(0);
	/* flags */
	_(STRM_ET_QUEUE_TO, _(STRM_ET_QUEUE_ERR, _(STRM_ET_QUEUE_ABRT,
	_(STRM_ET_CONN_TO, _(STRM_ET_CONN_ERR, _(STRM_ET_CONN_ABRT,
	_(STRM_ET_CONN_RES, _(STRM_ET_CONN_OTHER, _(STRM_ET_DATA_TO,
	_(STRM_ET_DATA_ERR, _(STRM_ET_DATA_ABRT)))))))))));
	/* epilogue */
	_(~0U);
	return buf;
#undef _
}

struct hlua;
struct proxy;
struct pendconn;
struct session;
struct server;
struct task;
struct sockaddr_storage;

/* some external definitions */
struct strm_logs {
	int logwait;                    /* log fields waiting to be collected : LW_* */
	int level;                      /* log level to force + 1 if > 0, -1 = no log */
	struct timeval accept_date;     /* date of the stream's accept() in user date */
	ullong accept_ts;               /* date of the session's accept() in internal date (monotonic) */
	long t_handshake;               /* handshake duration, -1 if never occurs */
	long t_idle;                    /* idle duration, -1 if never occurs */
	ullong request_ts;              /* date when the request arrives in internal date */
	long  t_queue;                  /* delay before the stream gets out of the connect queue, -1 if never occurs */
	long  t_connect;                /* delay before the connect() to the server succeeds, -1 if never occurs */
	long  t_data;                   /* delay before the first data byte from the server ... */
	unsigned long t_close;          /* total stream duration */
	unsigned long srv_queue_pos;    /* number of streams de-queued while waiting for a connection slot on this server */
	unsigned long prx_queue_pos;    /* number of streams de-qeuued while waiting for a connection slot on this instance */
	long long bytes_in;             /* number of bytes transferred from the client to the server */
	long long bytes_out;            /* number of bytes transferred from the server to the client */
};

struct stream {
	enum obj_type obj_type;         /* object type == OBJ_TYPE_STREAM */
	enum sc_state prev_conn_state;  /* CS_ST*, copy of previous state of the server stream connector */

	int16_t priority_class;         /* priority class of the stream for the pending queue */
	int32_t priority_offset;        /* priority offset of the stream for the pending queue */

	int flags;                      /* some flags describing the stream */
	unsigned int uniq_id;           /* unique ID used for the traces */
	enum obj_type *target;          /* target to use for this stream */

	struct session *sess;           /* the session this stream is attached to */

	struct channel req;             /* request channel */
	struct channel res;             /* response channel */

	struct proxy *be;               /* the proxy this stream depends on for the server side */

	struct server *srv_conn;        /* stream already has a slot on a server and is not in queue */
	struct pendconn *pend_pos;      /* if not NULL, points to the pending position in the pending queue */

	struct http_txn *txn;           /* current HTTP transaction being processed. Should become a list. */

	struct task *task;              /* the task associated with this stream */
	unsigned int pending_events;	/* the pending events not yet processed by the stream.
					 * This is a bit field of TASK_WOKEN_* */
	int conn_retries;               /* number of connect retries performed */
	unsigned int conn_exp;          /* wake up time for connect, queue, turn-around, ... */
	unsigned int conn_err_type;     /* first error detected, one of STRM_ET_* */
	struct list list;               /* position in the thread's streams list */
	struct mt_list by_srv;          /* position in server stream list */
	struct list back_refs;          /* list of users tracking this stream */
	struct buffer_wait buffer_wait; /* position in the list of objects waiting for a buffer */

	uint64_t lat_time;		/* total latency time experienced */
	uint64_t cpu_time;              /* total CPU time consumed */
	struct freq_ctr call_rate;      /* stream task call rate without making progress */

	short store_count;
	/* 2 unused bytes here */

	struct {
		struct stksess *ts;
		struct stktable *table;
	} store[8];                     /* tracked stickiness values to store */

	struct stkctr *stkctr;                  /* content-aware stick counters */

	struct strm_flt strm_flt;               /* current state of filters active on this stream */

	char **req_cap;                         /* array of captures from the request (may be NULL) */
	char **res_cap;                         /* array of captures from the response (may be NULL) */
	struct vars vars_txn;                   /* list of variables for the txn scope. */
	struct vars vars_reqres;                /* list of variables for the request and resp scope. */

	struct stconn *scf;                     /* frontend stream connector */
	struct stconn *scb;                     /* backend stream connector */

	struct strm_logs logs;                  /* logs for this stream */

	void (*do_log)(struct stream *s);       /* the function to call in order to log (or NULL) */
	void (*srv_error)(struct stream *s,     /* the function to call upon unrecoverable server errors (or NULL) */
			  struct stconn *sc);

	int pcli_next_pid;                      /* next target PID to use for the CLI proxy */
	int pcli_flags;                         /* flags for CLI proxy */

	struct ist unique_id;                   /* custom unique ID */

	/* These two pointers are used to resume the execution of the rule lists. */
	struct list *current_rule_list;         /* this is used to store the current executed rule list. */
	void *current_rule;                     /* this is used to store the current rule to be resumed. */
	int rules_exp;                          /* expiration date for current rules execution */
	int tunnel_timeout;
	const char *last_rule_file;             /* last evaluated final rule's file (def: NULL) */
	int last_rule_line;                     /* last evaluated final rule's line (def: 0) */

	unsigned int stream_epoch;              /* copy of stream_epoch when the stream was created */
	struct hlua *hlua;                      /* lua runtime context */

	/* Context */
	struct {
		struct resolv_requester *requester; /* owner of the resolution */
		struct act_rule *parent;        /* rule which requested this resolution */
		char *hostname_dn;              /* hostname being resolve, in domain name format */
		int hostname_dn_len;            /* size of hostname_dn */
		/* 4 unused bytes here, recoverable via packing if needed */
	} resolv_ctx;                           /* context information for DNS resolution */
};

#endif /* _HAPROXY_STREAM_T_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
