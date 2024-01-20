/*
 * include/haproxy/mworker-t.h
 * Master Worker function prototypes.
 *
 * Copyright HAProxy Technologies 2019 - William Lallemand <wlallemand@haproxy.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#ifndef _HAPROXY_MWORKER_H_
#define _HAPROXY_MWORKER_H_

#include <haproxy/mworker-t.h>
#include <haproxy/signal-t.h>

extern struct mworker_proc *proc_self;

void mworker_proc_list_to_env(void);
int mworker_env_to_proc_list(void);


void mworker_block_signals(void);
void mworker_unblock_signals(void);

void mworker_broadcast_signal(struct sig_handler *sh);
void mworker_catch_sighup(struct sig_handler *sh);
void mworker_catch_sigterm(struct sig_handler *sh);
void mworker_catch_sigchld(struct sig_handler *sh);

void mworker_accept_wrapper(int fd);

void mworker_cleanlisteners(void);

int mworker_child_nb(void);

int mworker_ext_launch_all(void);

void mworker_kill_max_reloads(int sig);

struct mworker_proc *mworker_proc_new();
void mworker_free_child(struct mworker_proc *);
void mworker_cleanup_proc();

#endif /* _HAPROXY_MWORKER_H_ */
