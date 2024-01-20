/*
 * Runtime tracing API
 *
 * Copyright (C) 2000-2019 Willy Tarreau - w@1wt.eu
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

#include <import/ist.h>
#include <haproxy/api.h>
#include <haproxy/buf.h>
#include <haproxy/cfgparse.h>
#include <haproxy/cli.h>
#include <haproxy/errors.h>
#include <haproxy/istbuf.h>
#include <haproxy/list.h>
#include <haproxy/log.h>
#include <haproxy/sink.h>
#include <haproxy/trace.h>

struct list trace_sources = LIST_HEAD_INIT(trace_sources);
THREAD_LOCAL struct buffer trace_buf = { };

/* allocates the trace buffers. Returns 0 in case of failure. It is safe to
 * call to call this function multiple times if the size changes.
 */
static int alloc_trace_buffers_per_thread()
{
	chunk_init(&trace_buf, my_realloc2(trace_buf.area, global.tune.bufsize), global.tune.bufsize);
	return !!trace_buf.area;
}

static void free_trace_buffers_per_thread()
{
	chunk_destroy(&trace_buf);
}

REGISTER_PER_THREAD_ALLOC(alloc_trace_buffers_per_thread);
REGISTER_PER_THREAD_FREE(free_trace_buffers_per_thread);

/* pick the lowest non-null argument with a non-null arg_def mask */
static inline const void *trace_pick_arg(uint32_t arg_def, const void *a1, const void *a2, const void *a3, const void *a4)
{
	if (arg_def & 0x0000FFFF) {
		if ((arg_def & 0x000000FF) && a1)
			return a1;
		if ((arg_def & 0x0000FF00) && a2)
			return a2;
	}

	if (arg_def & 0xFFFF0000) {
		if ((arg_def & 0x00FF0000) && a3)
			return a3;
		if ((arg_def & 0xFF000000) && a4)
			return a4;
	}

	return NULL;
}

/* Reports whether the trace is enabled for the specified arguments, needs to enable
 * or disable tracking. It gets the same API as __trace() except for <cb> and <msg>
 * which are not used and were dropped, and plockptr which is an optional pointer to
 * the lockptr to be updated (or NULL) for tracking. The function returns:
 *   0 if the trace is not enabled for the module or these values
 *  <0 if the trace matches some locking criteria but don't have the proper level.
 *     In this case the interested caller might have to consider disabling tracking.
 *  >0 if the trace is enabled for the given criteria.
 * In all cases, <plockptr> will only be set if non-null and if a locking criterion
 * matched. It will be up to the caller to enable tracking if desired. A casual
 * tester not interested in adjusting tracking (i.e. calling the function before
 * deciding so prepare a buffer to be dumped) will only need to pass 0 for plockptr
 * and check if the result is >0.
 */
int __trace_enabled(enum trace_level level, uint64_t mask, struct trace_source *src,
		    const struct ist where, const char *func,
		    const void *a1, const void *a2, const void *a3, const void *a4,
		    const void **plockptr)
{
	const struct listener *li = NULL;
	const struct proxy *fe = NULL;
	const struct proxy *be = NULL;
	const struct server *srv = NULL;
	const struct session *sess = NULL;
	const struct stream *strm = NULL;
	const struct connection *conn = NULL;
	const struct check *check = NULL;
	const struct quic_conn *qc = NULL;
	const struct appctx *appctx = NULL;
	const void *lockon_ptr = NULL;

	if (likely(src->state == TRACE_STATE_STOPPED))
		return 0;

	/* check that at least one action is interested by this event */
	if (((src->report_events | src->start_events | src->pause_events | src->stop_events) & mask) == 0)
		return 0;

	/* retrieve available information from the caller's arguments */
	if (src->arg_def & TRC_ARGS_CONN)
		conn = trace_pick_arg(src->arg_def & TRC_ARGS_CONN, a1, a2, a3, a4);

	if (src->arg_def & TRC_ARGS_SESS)
		sess = trace_pick_arg(src->arg_def & TRC_ARGS_SESS, a1, a2, a3, a4);

	if (src->arg_def & TRC_ARGS_STRM)
		strm = trace_pick_arg(src->arg_def & TRC_ARGS_STRM, a1, a2, a3, a4);

	if (src->arg_def & TRC_ARGS_CHK)
		check = trace_pick_arg(src->arg_def & TRC_ARGS_CHK, a1, a2, a3, a4);

	if (src->arg_def & TRC_ARGS_QCON)
		qc = trace_pick_arg(src->arg_def & TRC_ARGS_QCON, a1, a2, a3, a4);

	if (src->arg_def & TRC_ARGS_APPCTX)
		appctx = trace_pick_arg(src->arg_def & TRC_ARGS_APPCTX, a1, a2, a3, a4);

	if (!sess && strm)
		sess = strm->sess;
	else if (!sess && conn && LIST_INLIST(&conn->session_list))
		sess = conn->owner;
	else if (!sess && check)
		sess = check->sess;
	else if (!sess && appctx)
		sess = appctx->sess;

	if (sess) {
		fe = sess->fe;
		li = sess->listener;
	}

	if (!li && conn)
		li = objt_listener(conn->target);

	if (li && !fe)
		fe = li->bind_conf->frontend;

	if (strm) {
		be = strm->be;
		srv = strm->srv_conn;
	}
	if (check) {
		srv = check->server;
		be = (srv ? srv->proxy : NULL);
	}

	if (!srv && conn)
		srv = objt_server(conn->target);

	if (srv && !be)
		be = srv->proxy;

	if (!be && conn)
		be = objt_proxy(conn->target);

	/* TODO: add handling of filters here, return if no match (not even update states) */

	/* check if we need to start the trace now */
	if (src->state == TRACE_STATE_WAITING) {
		if ((src->start_events & mask) == 0)
			return 0;

		/* TODO: add update of lockon+lockon_ptr here */
		HA_ATOMIC_STORE(&src->state, TRACE_STATE_RUNNING);
	}

	/* we may want to lock on a particular object */
	if (src->lockon != TRACE_LOCKON_NOTHING) {
		switch (src->lockon) {
		case TRACE_LOCKON_BACKEND:    lockon_ptr = be;     break;
		case TRACE_LOCKON_CONNECTION: lockon_ptr = conn;   break;
		case TRACE_LOCKON_FRONTEND:   lockon_ptr = fe;     break;
		case TRACE_LOCKON_LISTENER:   lockon_ptr = li;     break;
		case TRACE_LOCKON_SERVER:     lockon_ptr = srv;    break;
		case TRACE_LOCKON_SESSION:    lockon_ptr = sess;   break;
		case TRACE_LOCKON_STREAM:     lockon_ptr = strm;   break;
		case TRACE_LOCKON_CHECK:      lockon_ptr = check;  break;
		case TRACE_LOCKON_THREAD:     lockon_ptr = ti;     break;
		case TRACE_LOCKON_QCON:       lockon_ptr = qc;     break;
		case TRACE_LOCKON_APPCTX:     lockon_ptr = appctx; break;
		case TRACE_LOCKON_ARG1:       lockon_ptr = a1;     break;
		case TRACE_LOCKON_ARG2:       lockon_ptr = a2;     break;
		case TRACE_LOCKON_ARG3:       lockon_ptr = a3;     break;
		case TRACE_LOCKON_ARG4:       lockon_ptr = a4;     break;
		default: break; // silence stupid gcc -Wswitch
		}

		if (src->lockon_ptr && src->lockon_ptr != lockon_ptr)
			return 0;

		if (*plockptr && !src->lockon_ptr && lockon_ptr && src->state == TRACE_STATE_RUNNING)
			*plockptr = lockon_ptr;
	}

	/* here the trace is running and is tracking a desired item */
	if ((src->report_events & mask) == 0 || level > src->level) {
		/* tracking did match, and might have to be disabled */
		return -1;
	}

	/* OK trace still enabled */
	return 1;
}

/* write a message for the given trace source */
void __trace(enum trace_level level, uint64_t mask, struct trace_source *src,
             const struct ist where, const char *func,
             const void *a1, const void *a2, const void *a3, const void *a4,
             void (*cb)(enum trace_level level, uint64_t mask, const struct trace_source *src,
                        const struct ist where, const struct ist func,
                        const void *a1, const void *a2, const void *a3, const void *a4),
             const struct ist msg)
{
	const void *lockon_ptr;
	struct ist ist_func = ist(func);
	char tnum[4];
	struct ist line[12];
	int words = 0;
	int ret;

	lockon_ptr = NULL;
	ret = __trace_enabled(level, mask, src, where, func, a1, a2, a3, a4, &lockon_ptr);
	if (lockon_ptr)
		HA_ATOMIC_STORE(&src->lockon_ptr, lockon_ptr);

	if (ret <= 0) {
		if (ret < 0) // may have to disable tracking
			goto end;
		return;
	}

	/* log the logging location truncated to 10 chars from the right so that
	 * the line number and the end of the file name are there.
	 */
	line[words++] = ist("[");
	tnum[0] = '0' + tid / 10;
	tnum[1] = '0' + tid % 10;
	tnum[2] = '|';
	tnum[3] = 0;
	line[words++] = ist(tnum);
	line[words++] = src->name;
	line[words++] = ist("|");
	line[words++] = ist2("012345" + level, 1); // "0" to "5"
	line[words++] = ist("|");
	line[words] = where;
	if (line[words].len > 13) {
		line[words].ptr += (line[words].len - 13);
		line[words].len = 13;
	}
	words++;
	line[words++] = ist("] ");

	if (isttest(ist_func)) {
		line[words++] = ist_func;
		line[words++] = ist("(): ");
	}

	if (!cb)
		cb = src->default_cb;

	if (cb && src->verbosity) {
		/* decode function passed, we want to pre-fill the
		 * buffer with the message and let the decode function
		 * do its job, possibly even overwriting it.
		 */
		b_reset(&trace_buf);
		b_istput(&trace_buf, msg);
		cb(level, mask, src, where, ist_func, a1, a2, a3, a4);
		line[words] = ist2(trace_buf.area, trace_buf.data);
		words++;
	}
	else {
		/* Note that here we could decide to print some args whose type
		 * is known, when verbosity is above the quiet level, and even
		 * to print the name and values of those which are declared for
		 * lock-on.
		 */
		line[words++] = msg;
	}

	if (src->sink)
		sink_write(src->sink, 0, line, words, 0, 0, NULL);

 end:
	/* check if we need to stop the trace now */
	if ((src->stop_events & mask) != 0) {
		HA_ATOMIC_STORE(&src->lockon_ptr, NULL);
		HA_ATOMIC_STORE(&src->state, TRACE_STATE_STOPPED);
	}
	else if ((src->pause_events & mask) != 0) {
		HA_ATOMIC_STORE(&src->lockon_ptr, NULL);
		HA_ATOMIC_STORE(&src->state, TRACE_STATE_WAITING);
	}
}

/* this callback may be used when no output modification is desired */
void trace_no_cb(enum trace_level level, uint64_t mask, const struct trace_source *src,
		 const struct ist where, const struct ist func,
		 const void *a1, const void *a2, const void *a3, const void *a4)
{
	/* do nothing */
}

/* registers trace source <source>. Modifies the list element!
 * The {start,pause,stop,report} events are not changed so the source may
 * preset them.
 */
void trace_register_source(struct trace_source *source)
{
	source->lockon = TRACE_LOCKON_NOTHING;
	source->level = TRACE_LEVEL_USER;
	source->verbosity = 1;
	source->sink = NULL;
	source->state = TRACE_STATE_STOPPED;
	source->lockon_ptr = NULL;
	LIST_APPEND(&trace_sources, &source->source_link);
}

struct trace_source *trace_find_source(const char *name)
{
	struct trace_source *src;
	const struct ist iname = ist(name);

	list_for_each_entry(src, &trace_sources, source_link)
		if (isteq(src->name, iname))
			return src;
	return NULL;
}

const struct trace_event *trace_find_event(const struct trace_event *ev, const char *name)
{
	for (; ev && ev->mask; ev++)
		if (strcmp(ev->name, name) == 0)
			return ev;
	return NULL;
}

/* Parse a "trace" statement. Returns a severity as a LOG_* level and a status
 * message that may be delivered to the user, in <msg>. The message will be
 * nulled first and msg must be an allocated pointer. A null status message output
 * indicates no error. Be careful not to use the return value as a boolean, as
 * LOG_* values are not ordered as one could imagine (LOG_EMERG is zero). The
 * function may/will use the trash buffer as the storage for the response
 * message so that the caller never needs to release anything.
 */
static int trace_parse_statement(char **args, char **msg)
{
	struct trace_source *src;
	uint64_t *ev_ptr = NULL;

	/* no error by default */
	*msg = NULL;

	if (!*args[1]) {
		/* no arg => report the list of supported sources as a warning */
		chunk_printf(&trash,
			     "Supported trace sources and states (.=stopped, w=waiting, R=running) :\n"
			     " [.] 0          : not a source, will immediately stop all traces\n"
			     );

		list_for_each_entry(src, &trace_sources, source_link)
			chunk_appendf(&trash, " [%c] %-10s : %s\n", trace_state_char(src->state), src->name.ptr, src->desc);

		trash.area[trash.data] = 0;
		*msg = strdup(trash.area);
		return LOG_WARNING;
	}

	if (strcmp(args[1], "0") == 0) {
		/* emergency stop of all traces */
		list_for_each_entry(src, &trace_sources, source_link)
			HA_ATOMIC_STORE(&src->state, TRACE_STATE_STOPPED);
		*msg = strdup("All traces now stopped");
		return LOG_NOTICE;
	}

	src = trace_find_source(args[1]);
	if (!src) {
		memprintf(msg, "No such trace source '%s'", args[1]);
		return LOG_ERR;
	}

	if (!*args[2]) {
		*msg =  "Supported commands:\n"
			"  event     : list/enable/disable source-specific event reporting\n"
			//"  filter    : list/enable/disable generic filters\n"
			"  level     : list/set trace reporting level\n"
			"  lock      : automatic lock on thread/connection/stream/...\n"
			"  pause     : pause and automatically restart after a specific event\n"
			"  sink      : list/set event sinks\n"
			"  start     : start immediately or after a specific event\n"
			"  stop      : stop immediately or after a specific event\n"
			"  verbosity : list/set trace output verbosity\n";
		*msg = strdup(*msg);
		return LOG_WARNING;
	}
	else if ((strcmp(args[2], "event") == 0 && (ev_ptr = &src->report_events)) ||
	         (strcmp(args[2], "pause") == 0 && (ev_ptr = &src->pause_events)) ||
	         (strcmp(args[2], "start") == 0 && (ev_ptr = &src->start_events)) ||
	         (strcmp(args[2], "stop")  == 0 && (ev_ptr = &src->stop_events))) {
		const struct trace_event *ev;
		const char *name = args[3];
		int neg = 0;
		int i;

		/* skip prefix '!', '-', '+' and remind negation */
		while (*name) {
			if (*name == '!' || *name == '-')
				neg = 1;
			else if (*name == '+')
				neg = 0;
			else
				break;
			name++;
		}

		if (!*name) {
			chunk_printf(&trash, "Supported events for source %s (+=enabled, -=disabled):\n", src->name.ptr);
			if (ev_ptr != &src->report_events)
				chunk_appendf(&trash, "  - now          : don't wait for events, immediately change the state\n");
			chunk_appendf(&trash, "  - none         : disable all event types\n");
			chunk_appendf(&trash, "  - any          : enable all event types\n");
			for (i = 0; src->known_events && src->known_events[i].mask; i++) {
				chunk_appendf(&trash, "  %c %-12s : %s\n",
					      trace_event_char(*ev_ptr, src->known_events[i].mask),
					      src->known_events[i].name, src->known_events[i].desc);
			}
			trash.area[trash.data] = 0;
			*msg = strdup(trash.area);
			return LOG_WARNING;
		}

		if (strcmp(name, "now") == 0 && ev_ptr != &src->report_events) {
			HA_ATOMIC_STORE(ev_ptr, 0);
			if (ev_ptr == &src->pause_events) {
				HA_ATOMIC_STORE(&src->lockon_ptr, NULL);
				HA_ATOMIC_STORE(&src->state, TRACE_STATE_WAITING);
			}
			else if (ev_ptr == &src->start_events) {
				HA_ATOMIC_STORE(&src->state, TRACE_STATE_RUNNING);
			}
			else if (ev_ptr == &src->stop_events) {
				HA_ATOMIC_STORE(&src->lockon_ptr, NULL);
				HA_ATOMIC_STORE(&src->state, TRACE_STATE_STOPPED);
			}
			return 0;
		}

		if (strcmp(name, "none") == 0)
			HA_ATOMIC_STORE(ev_ptr, 0);
		else if (strcmp(name, "any") == 0)
			HA_ATOMIC_STORE(ev_ptr, ~0);
		else {
			ev = trace_find_event(src->known_events, name);
			if (!ev) {
				memprintf(msg, "No such trace event '%s'", name);
				return LOG_ERR;
			}

			if (!neg)
				HA_ATOMIC_OR(ev_ptr, ev->mask);
			else
				HA_ATOMIC_AND(ev_ptr, ~ev->mask);
		}
	}
	else if (strcmp(args[2], "sink") == 0) {
		const char *name = args[3];
		struct sink *sink;

		if (!*name) {
			chunk_printf(&trash, "Supported sinks for source %s (*=current):\n", src->name.ptr);
			chunk_appendf(&trash, "  %c none       : no sink\n", src->sink ? ' ' : '*');
			list_for_each_entry(sink, &sink_list, sink_list) {
				chunk_appendf(&trash, "  %c %-10s : %s\n",
					      src->sink == sink ? '*' : ' ',
					      sink->name, sink->desc);
			}
			trash.area[trash.data] = 0;
			*msg = strdup(trash.area);
			return LOG_WARNING;
		}

		if (strcmp(name, "none") == 0)
			sink = NULL;
		else {
			sink = sink_find(name);
			if (!sink) {
				memprintf(msg, "No such trace sink '%s'", name);
				return LOG_ERR;
			}
		}

		HA_ATOMIC_STORE(&src->sink, sink);
	}
	else if (strcmp(args[2], "level") == 0) {
		const char *name = args[3];

		if (!*name) {
			chunk_printf(&trash, "Supported trace levels for source %s:\n", src->name.ptr);
			chunk_appendf(&trash, "  %c error      : report errors\n",
				      src->level == TRACE_LEVEL_ERROR ? '*' : ' ');
			chunk_appendf(&trash, "  %c user       : also information useful to the end user\n",
				      src->level == TRACE_LEVEL_USER ? '*' : ' ');
			chunk_appendf(&trash, "  %c proto      : also protocol-level updates\n",
				      src->level == TRACE_LEVEL_PROTO ? '*' : ' ');
			chunk_appendf(&trash, "  %c state      : also report internal state changes\n",
				      src->level == TRACE_LEVEL_STATE ? '*' : ' ');
			chunk_appendf(&trash, "  %c data       : also report data transfers\n",
				      src->level == TRACE_LEVEL_DATA ? '*' : ' ');
			chunk_appendf(&trash, "  %c developer  : also report information useful only to the developer\n",
				      src->level == TRACE_LEVEL_DEVELOPER ? '*' : ' ');
			trash.area[trash.data] = 0;
			*msg = strdup(trash.area);
			return LOG_WARNING;
		}

		if (strcmp(name, "error") == 0)
			HA_ATOMIC_STORE(&src->level, TRACE_LEVEL_ERROR);
		else if (strcmp(name, "user") == 0)
			HA_ATOMIC_STORE(&src->level, TRACE_LEVEL_USER);
		else if (strcmp(name, "proto") == 0)
			HA_ATOMIC_STORE(&src->level, TRACE_LEVEL_PROTO);
		else if (strcmp(name, "state") == 0)
			HA_ATOMIC_STORE(&src->level, TRACE_LEVEL_STATE);
		else if (strcmp(name, "data") == 0)
			HA_ATOMIC_STORE(&src->level, TRACE_LEVEL_DATA);
		else if (strcmp(name, "developer") == 0)
			HA_ATOMIC_STORE(&src->level, TRACE_LEVEL_DEVELOPER);
		else {
			memprintf(msg, "No such trace level '%s'", name);
			return LOG_ERR;
		}
	}
	else if (strcmp(args[2], "lock") == 0) {
		const char *name = args[3];

		if (!*name) {
			chunk_printf(&trash, "Supported lock-on criteria for source %s:\n", src->name.ptr);
			if (src->arg_def & (TRC_ARGS_CONN|TRC_ARGS_STRM))
				chunk_appendf(&trash, "  %c backend    : lock on the backend that started the trace\n",
				              src->lockon == TRACE_LOCKON_BACKEND ? '*' : ' ');

			if (src->arg_def & TRC_ARGS_CHK)
				chunk_appendf(&trash, "  %c check      : lock on the check that started the trace\n",
				              src->lockon == TRACE_LOCKON_CHECK ? '*' : ' ');

			if (src->arg_def & TRC_ARGS_CONN)
				chunk_appendf(&trash, "  %c connection : lock on the connection that started the trace\n",
				              src->lockon == TRACE_LOCKON_CONNECTION ? '*' : ' ');

			if (src->arg_def & (TRC_ARGS_CONN|TRC_ARGS_SESS|TRC_ARGS_STRM))
				chunk_appendf(&trash, "  %c frontend   : lock on the frontend that started the trace\n",
				              src->lockon == TRACE_LOCKON_FRONTEND ? '*' : ' ');

			if (src->arg_def & (TRC_ARGS_CONN|TRC_ARGS_SESS|TRC_ARGS_STRM))
				chunk_appendf(&trash, "  %c listener   : lock on the listener that started the trace\n",
				              src->lockon == TRACE_LOCKON_LISTENER ? '*' : ' ');

			chunk_appendf(&trash, "  %c nothing    : do not lock on anything\n",
				      src->lockon == TRACE_LOCKON_NOTHING ? '*' : ' ');

			if (src->arg_def & (TRC_ARGS_CONN|TRC_ARGS_STRM))
				chunk_appendf(&trash, "  %c server     : lock on the server that started the trace\n",
				              src->lockon == TRACE_LOCKON_SERVER ? '*' : ' ');

			if (src->arg_def & (TRC_ARGS_CONN|TRC_ARGS_SESS|TRC_ARGS_STRM))
				chunk_appendf(&trash, "  %c session    : lock on the session that started the trace\n",
				              src->lockon == TRACE_LOCKON_SESSION ? '*' : ' ');

			if (src->arg_def & TRC_ARGS_STRM)
				chunk_appendf(&trash, "  %c stream     : lock on the stream that started the trace\n",
				              src->lockon == TRACE_LOCKON_STREAM ? '*' : ' ');

			if (src->arg_def & TRC_ARGS_APPCTX)
				chunk_appendf(&trash, "  %c applet     : lock on the applet that started the trace\n",
				              src->lockon == TRACE_LOCKON_APPCTX ? '*' : ' ');

			chunk_appendf(&trash, "  %c thread     : lock on the thread that started the trace\n",
				      src->lockon == TRACE_LOCKON_THREAD ? '*' : ' ');

			if (src->lockon_args && src->lockon_args[0].name)
				chunk_appendf(&trash, "  %c %-10s : %s\n",
				              src->lockon == TRACE_LOCKON_ARG1 ? '*' : ' ',
				              src->lockon_args[0].name, src->lockon_args[0].desc);

			if (src->lockon_args && src->lockon_args[1].name)
				chunk_appendf(&trash, "  %c %-10s : %s\n",
				              src->lockon == TRACE_LOCKON_ARG2 ? '*' : ' ',
				              src->lockon_args[1].name, src->lockon_args[1].desc);

			if (src->lockon_args && src->lockon_args[2].name)
				chunk_appendf(&trash, "  %c %-10s : %s\n",
				              src->lockon == TRACE_LOCKON_ARG3 ? '*' : ' ',
				              src->lockon_args[2].name, src->lockon_args[2].desc);

			if (src->lockon_args && src->lockon_args[3].name)
				chunk_appendf(&trash, "  %c %-10s : %s\n",
				              src->lockon == TRACE_LOCKON_ARG4 ? '*' : ' ',
				              src->lockon_args[3].name, src->lockon_args[3].desc);

			trash.area[trash.data] = 0;
			*msg = strdup(trash.area);
			return LOG_WARNING;
		}
		else if ((src->arg_def & (TRC_ARGS_CONN|TRC_ARGS_STRM)) && strcmp(name, "backend") == 0) {
			HA_ATOMIC_STORE(&src->lockon, TRACE_LOCKON_BACKEND);
			HA_ATOMIC_STORE(&src->lockon_ptr, NULL);
		}
		else if ((src->arg_def & TRC_ARGS_CHK) && strcmp(name, "check") == 0) {
			HA_ATOMIC_STORE(&src->lockon, TRACE_LOCKON_CHECK);
			HA_ATOMIC_STORE(&src->lockon_ptr, NULL);
		}
		else if ((src->arg_def & TRC_ARGS_CONN) && strcmp(name, "connection") == 0) {
			HA_ATOMIC_STORE(&src->lockon, TRACE_LOCKON_CONNECTION);
			HA_ATOMIC_STORE(&src->lockon_ptr, NULL);
		}
		else if ((src->arg_def & (TRC_ARGS_CONN|TRC_ARGS_SESS|TRC_ARGS_STRM)) && strcmp(name, "frontend") == 0) {
			HA_ATOMIC_STORE(&src->lockon, TRACE_LOCKON_FRONTEND);
			HA_ATOMIC_STORE(&src->lockon_ptr, NULL);
		}
		else if ((src->arg_def & (TRC_ARGS_CONN|TRC_ARGS_SESS|TRC_ARGS_STRM)) && strcmp(name, "listener") == 0) {
			HA_ATOMIC_STORE(&src->lockon, TRACE_LOCKON_LISTENER);
			HA_ATOMIC_STORE(&src->lockon_ptr, NULL);
		}
		else if (strcmp(name, "nothing") == 0) {
			HA_ATOMIC_STORE(&src->lockon, TRACE_LOCKON_NOTHING);
			HA_ATOMIC_STORE(&src->lockon_ptr, NULL);
		}
		else if ((src->arg_def & (TRC_ARGS_CONN|TRC_ARGS_STRM)) && strcmp(name, "server") == 0) {
			HA_ATOMIC_STORE(&src->lockon, TRACE_LOCKON_SERVER);
			HA_ATOMIC_STORE(&src->lockon_ptr, NULL);
		}
		else if ((src->arg_def & (TRC_ARGS_CONN|TRC_ARGS_SESS|TRC_ARGS_STRM)) && strcmp(name, "session") == 0) {
			HA_ATOMIC_STORE(&src->lockon, TRACE_LOCKON_SESSION);
			HA_ATOMIC_STORE(&src->lockon_ptr, NULL);
		}
		else if ((src->arg_def & TRC_ARGS_STRM) && strcmp(name, "stream") == 0) {
			HA_ATOMIC_STORE(&src->lockon, TRACE_LOCKON_STREAM);
			HA_ATOMIC_STORE(&src->lockon_ptr, NULL);
		}
		else if ((src->arg_def & TRC_ARGS_APPCTX) && strcmp(name, "appctx") == 0) {
			HA_ATOMIC_STORE(&src->lockon, TRACE_LOCKON_APPCTX);
			HA_ATOMIC_STORE(&src->lockon_ptr, NULL);
		}
		else if (strcmp(name, "thread") == 0) {
			HA_ATOMIC_STORE(&src->lockon, TRACE_LOCKON_THREAD);
			HA_ATOMIC_STORE(&src->lockon_ptr, NULL);
		}
		else if (src->lockon_args && src->lockon_args[0].name && strcmp(name, src->lockon_args[0].name) == 0) {
			HA_ATOMIC_STORE(&src->lockon, TRACE_LOCKON_ARG1);
			HA_ATOMIC_STORE(&src->lockon_ptr, NULL);
		}
		else if (src->lockon_args && src->lockon_args[1].name && strcmp(name, src->lockon_args[1].name) == 0) {
			HA_ATOMIC_STORE(&src->lockon, TRACE_LOCKON_ARG2);
			HA_ATOMIC_STORE(&src->lockon_ptr, NULL);
		}
		else if (src->lockon_args && src->lockon_args[2].name && strcmp(name, src->lockon_args[2].name) == 0) {
			HA_ATOMIC_STORE(&src->lockon, TRACE_LOCKON_ARG3);
			HA_ATOMIC_STORE(&src->lockon_ptr, NULL);
		}
		else if (src->lockon_args && src->lockon_args[3].name && strcmp(name, src->lockon_args[3].name) == 0) {
			HA_ATOMIC_STORE(&src->lockon, TRACE_LOCKON_ARG4);
			HA_ATOMIC_STORE(&src->lockon_ptr, NULL);
		}
		else {
			memprintf(msg, "Unsupported lock-on criterion '%s'", name);
			return LOG_ERR;
		}
	}
	else if (strcmp(args[2], "verbosity") == 0) {
		const char *name = args[3];
		const struct name_desc *nd;

		if (!*name) {
			chunk_printf(&trash, "Supported trace verbosities for source %s:\n", src->name.ptr);
			chunk_appendf(&trash, "  %c quiet      : only report basic information with no decoding\n",
				      src->verbosity == 0 ? '*' : ' ');
			if (!src->decoding || !src->decoding[0].name) {
				chunk_appendf(&trash, "  %c default    : report extra information when available\n",
					      src->verbosity > 0 ? '*' : ' ');
			} else {
				for (nd = src->decoding; nd->name && nd->desc; nd++)
					chunk_appendf(&trash, "  %c %-10s : %s\n",
					              nd == (src->decoding + src->verbosity - 1) ? '*' : ' ',
						      nd->name, nd->desc);
			}
			trash.area[trash.data] = 0;
			*msg = strdup(trash.area);
			return LOG_WARNING;
		}

		if (strcmp(name, "quiet") == 0)
			HA_ATOMIC_STORE(&src->verbosity, 0);
		else if (!src->decoding || !src->decoding[0].name) {
			if (strcmp(name, "default") == 0)
				HA_ATOMIC_STORE(&src->verbosity, 1);
			else {
				memprintf(msg, "No such verbosity level '%s'", name);
				return LOG_ERR;
			}
		} else {
			for (nd = src->decoding; nd->name && nd->desc; nd++)
				if (strcmp(name, nd->name) == 0)
					break;

			if (!nd->name || !nd->desc) {
				memprintf(msg, "No such verbosity level '%s'", name);
				return LOG_ERR;
			}

			HA_ATOMIC_STORE(&src->verbosity, (nd - src->decoding) + 1);
		}
	}
	else {
		memprintf(msg, "Unknown trace keyword '%s'", args[2]);
		return LOG_ERR;
	}
	return 0;

}

/* parse a "trace" statement in the "global" section, returns 1 if a message is returned, otherwise zero */
static int cfg_parse_trace(char **args, int section_type, struct proxy *curpx,
			   const struct proxy *defpx, const char *file, int line,
			   char **err)
{
	char *msg;
	int severity;

	severity = trace_parse_statement(args, &msg);
	if (msg) {
		if (severity >= LOG_NOTICE)
			ha_notice("parsing [%s:%d] : '%s': %s\n", file, line, args[0], msg);
		else if (severity >= LOG_WARNING)
			ha_warning("parsing [%s:%d] : '%s': %s\n", file, line, args[0], msg);
		else {
			/* let the caller free the message */
			*err = msg;
			return -1;
		}
		ha_free(&msg);
	}
	return 0;
}

/* parse the command, returns 1 if a message is returned, otherwise zero */
static int cli_parse_trace(char **args, char *payload, struct appctx *appctx, void *private)
{
	char *msg;
	int severity;

	if (!cli_has_level(appctx, ACCESS_LVL_OPER))
		return 1;

	severity = trace_parse_statement(args, &msg);
	if (msg)
		return cli_dynmsg(appctx, severity, msg);

	/* total success */
	return 0;
}

/* parse the command, returns 1 if a message is returned, otherwise zero */
static int cli_parse_show_trace(char **args, char *payload, struct appctx *appctx, void *private)
{
	struct trace_source *src;
	const struct sink *sink;
	int i;

	args++; // make args[1] the 1st arg

	if (!*args[1]) {
		/* no arg => report the list of supported sources */
		chunk_printf(&trash,
			     "Supported trace sources and states (.=stopped, w=waiting, R=running) :\n"
			     );

		list_for_each_entry(src, &trace_sources, source_link) {
			sink = src->sink;
			chunk_appendf(&trash, " [%c] %-10s -> %s [drp %u]  [%s]\n",
				      trace_state_char(src->state), src->name.ptr,
				      sink ? sink->name : "none",
				      sink ? sink->ctx.dropped : 0,
				      src->desc);
		}

		trash.area[trash.data] = 0;
		return cli_msg(appctx, LOG_INFO, trash.area);
	}

	if (!cli_has_level(appctx, ACCESS_LVL_OPER))
		return 1;

	src = trace_find_source(args[1]);
	if (!src)
		return cli_err(appctx, "No such trace source");

	sink = src->sink;
	chunk_printf(&trash, "Trace status for %s:\n", src->name.ptr);
	chunk_appendf(&trash, "  - sink: %s [%u dropped]\n",
		      sink ? sink->name : "none", sink ? sink->ctx.dropped : 0);

	chunk_appendf(&trash, "  - event name   :     report    start    stop    pause\n");
	for (i = 0; src->known_events && src->known_events[i].mask; i++) {
		chunk_appendf(&trash, "    %-12s :        %c        %c        %c       %c\n",
			      src->known_events[i].name,
			      trace_event_char(src->report_events, src->known_events[i].mask),
			      trace_event_char(src->start_events, src->known_events[i].mask),
			      trace_event_char(src->stop_events, src->known_events[i].mask),
			      trace_event_char(src->pause_events, src->known_events[i].mask));
	}

	trash.area[trash.data] = 0;
	return cli_msg(appctx, LOG_WARNING, trash.area);
}

static struct cli_kw_list cli_kws = {{ },{
	{ { "trace", NULL },         "trace [<module>|0] [cmd [args...]]      : manage live tracing (empty to list, 0 to stop all)", cli_parse_trace, NULL, NULL },
	{ { "show", "trace", NULL }, "show trace [<module>]                   : show live tracing state",                            cli_parse_show_trace, NULL, NULL },
	{{},}
}};

INITCALL1(STG_REGISTER, cli_register_kw, &cli_kws);

static struct cfg_kw_list cfg_kws = {ILH, {
	{ CFG_GLOBAL, "trace", cfg_parse_trace, KWF_EXPERIMENTAL },
	{ /* END */ },
}};

INITCALL1(STG_REGISTER, cfg_register_keywords, &cfg_kws);

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
