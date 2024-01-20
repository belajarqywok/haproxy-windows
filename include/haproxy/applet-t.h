/*
 * include/haproxy/applet-t.h
 * This file describes the applet struct and associated constants.
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

#ifndef _HAPROXY_APPLET_T_H
#define _HAPROXY_APPLET_T_H

#include <haproxy/api-t.h>
#include <haproxy/buf-t.h>
#include <haproxy/dynbuf-t.h>
#include <haproxy/freq_ctr-t.h>
#include <haproxy/obj_type-t.h>
#include <haproxy/xref-t.h>

/* flags for appctx->state */
#define APPLET_WANT_DIE     0x01  /* applet was running and requested to die */

/* Room for per-command context (mostly CLI commands but not only) */
#define APPLET_MAX_SVCCTX 88

struct appctx;
struct proxy;
struct stconn;
struct sedesc;
struct session;

/* Applet descriptor */
struct applet {
	enum obj_type obj_type;            /* object type = OBJ_TYPE_APPLET */
	/* 3 unused bytes here */
	char *name;                        /* applet's name to report in logs */
	int (*init)(struct appctx *);      /* callback to init resources, may be NULL.
					      expect 0 if ok, -1 if an error occurs. */
	void (*fct)(struct appctx *);      /* internal I/O handler, may never be NULL */
	void (*release)(struct appctx *);  /* callback to release resources, may be NULL */
	unsigned int timeout;              /* execution timeout. */
};

/* Context of a running applet. */
struct appctx {
	enum obj_type obj_type;    /* OBJ_TYPE_APPCTX */
	/* 3 unused bytes here */
	unsigned short state;      /* Internal appctx state */
	unsigned int st0;          /* CLI state for stats, session state for peers */
	unsigned int st1;          /* prompt/payload (bitwise OR of APPCTX_CLI_ST1_*) for stats, session error for peers */
	struct buffer *chunk;       /* used to store unfinished commands */
	struct applet *applet;     /* applet this context refers to */
	struct session *sess;      /* session for frontend applets (NULL for backend applets) */
	struct sedesc *sedesc;     /* stream endpoint descriptor the applet is attached to */
	struct act_rule *rule;     /* rule associated with the applet. */
	int (*io_handler)(struct appctx *appctx);  /* used within the cli_io_handler when st0 = CLI_ST_CALLBACK */
	void (*io_release)(struct appctx *appctx);  /* used within the cli_io_handler when st0 = CLI_ST_CALLBACK,
	                                               if the command is terminated or the session released */
	int cli_severity_output;        /* used within the cli_io_handler to format severity output of informational feedback */
	int cli_level;              /* the level of CLI which can be lowered dynamically */
	uint32_t cli_anon_key;       /* the key to anonymise with the hash in cli */
	struct buffer_wait buffer_wait; /* position in the list of objects waiting for a buffer */
	struct task *t;                  /* task associated to the applet */
	struct freq_ctr call_rate;       /* appctx call rate */
	struct list wait_entry;          /* entry in a list of waiters for an event (e.g. ring events) */

	/* The pointer seen by application code is appctx->svcctx. In 2.7 the
	 * anonymous union and the "ctx" struct disappeared, and the struct
	 * "svc" became svc_storage, which is never accessed directly by
	 * application code. Look at "show fd" for an example.
	 */

	/* here we have the service's context (CLI command, applet, etc) */
	void *svcctx;                            /* pointer to a context used by the command, e.g. <storage> below */
	struct {
		void *shadow;                    /* shadow of svcctx above, do not use! */
		char storage[APPLET_MAX_SVCCTX]; /* storage of svcctx above */
	} svc;                                   /* generic storage for most commands */
};

#endif /* _HAPROXY_APPLET_T_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
