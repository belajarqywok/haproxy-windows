/*
 * Action management functions.
 *
 * Copyright 2017 HAProxy Technologies, Christopher Faulet <cfaulet@haproxy.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <haproxy/acl.h>
#include <haproxy/action.h>
#include <haproxy/api.h>
#include <haproxy/cfgparse.h>
#include <haproxy/errors.h>
#include <haproxy/list.h>
#include <haproxy/obj_type.h>
#include <haproxy/pool.h>
#include <haproxy/proxy.h>
#include <haproxy/stick_table.h>
#include <haproxy/task.h>
#include <haproxy/tools.h>


/* Check an action ruleset validity. It returns the number of error encountered
 * and err_code is updated if a warning is emitted.
 */
int check_action_rules(struct list *rules, struct proxy *px, int *err_code)
{
	struct act_rule *rule;
	char *errmsg = NULL;
	int err = 0;

	list_for_each_entry(rule, rules, list) {
		if (rule->check_ptr && !rule->check_ptr(rule, px, &errmsg)) {
			ha_alert("Proxy '%s': %s.\n", px->id, errmsg);
			err++;
		}
		*err_code |= warnif_tcp_http_cond(px, rule->cond);
		ha_free(&errmsg);
	}

	return err;
}

/* Find and check the target table used by an action track-sc*. This
 * function should be called during the configuration validity check.
 *
 * The function returns 1 in success case, otherwise, it returns 0 and err is
 * filled.
 */
int check_trk_action(struct act_rule *rule, struct proxy *px, char **err)
{
	struct stktable *target;

	if (rule->arg.trk_ctr.table.n)
		target = stktable_find_by_name(rule->arg.trk_ctr.table.n);
	else
		target = px->table;

	if (!target) {
		memprintf(err, "unable to find table '%s' referenced by track-sc%d",
			  rule->arg.trk_ctr.table.n ?  rule->arg.trk_ctr.table.n : px->id,
			  rule->action);
		return 0;
	}

	if (!stktable_compatible_sample(rule->arg.trk_ctr.expr,  target->type)) {
		memprintf(err, "stick-table '%s' uses a type incompatible with the 'track-sc%d' rule",
			  rule->arg.trk_ctr.table.n ? rule->arg.trk_ctr.table.n : px->id,
			  rule->action);
		return 0;
	}
	else {
		if (!in_proxies_list(target->proxies_list, px)) {
			px->next_stkt_ref = target->proxies_list;
			target->proxies_list = px;
		}
		free(rule->arg.trk_ctr.table.n);
		rule->arg.trk_ctr.table.t = target;
		/* Note: if we decide to enhance the track-sc syntax, we may be
		 * able to pass a list of counters to track and allocate them
		 * right here using stktable_alloc_data_type().
		 */
	}

	if (rule->from == ACT_F_TCP_REQ_CNT && (px->cap & PR_CAP_FE)) {
		if (!px->tcp_req.inspect_delay && !(rule->arg.trk_ctr.expr->fetch->val & SMP_VAL_FE_SES_ACC)) {
			ha_warning("%s '%s' : a 'tcp-request content track-sc*' rule explicitly depending on request"
				   " contents without any 'tcp-request inspect-delay' setting."
				   " This means that this rule will randomly find its contents. This can be fixed by"
				   " setting the tcp-request inspect-delay.\n",
				   proxy_type_str(px), px->id);
		}

		/* The following warning is emitted because HTTP multiplexers are able to catch errors
		 * or timeouts at the session level, before instantiating any stream.
		 * Thus the tcp-request content ruleset will not be evaluated in such case. It means,
		 * http_req and http_err counters will not be incremented as expected, even if the tracked
		 * counter does not use the request content. To track invalid requests it should be
		 * performed at the session level using a tcp-request session rule.
		 */
		if (px->mode == PR_MODE_HTTP &&
		    !(rule->arg.trk_ctr.expr->fetch->use & (SMP_USE_L6REQ|SMP_USE_HRQHV|SMP_USE_HRQHP|SMP_USE_HRQBO)) &&
		    (!rule->cond || !(rule->cond->use & (SMP_USE_L6REQ|SMP_USE_HRQHV|SMP_USE_HRQHP|SMP_USE_HRQBO)))) {
			ha_warning("%s '%s' : a 'tcp-request content track-sc*' rule not depending on request"
				   " contents for an HTTP frontend should be executed at the session level, using a"
				   " 'tcp-request session' rule (mandatory to track invalid HTTP requests).\n",
				   proxy_type_str(px), px->id);
		}
	}

	return 1;
}

/* check a capture rule. This function should be called during the configuration
 * validity check.
 *
 * The function returns 1 in success case, otherwise, it returns 0 and err is
 * filled.
 */
int check_capture(struct act_rule *rule, struct proxy *px, char **err)
{
	if (rule->from == ACT_F_TCP_REQ_CNT && (px->cap & PR_CAP_FE) && !px->tcp_req.inspect_delay &&
	    !(rule->arg.cap.expr->fetch->val & SMP_VAL_FE_SES_ACC)) {
		ha_warning("%s '%s' : a 'tcp-request capture' rule explicitly depending on request"
			   " contents without any 'tcp-request inspect-delay' setting."
			   " This means that this rule will randomly find its contents. This can be fixed by"
			   " setting the tcp-request inspect-delay.\n",
			   proxy_type_str(px), px->id);
	}

	return 1;
}

int act_resolution_cb(struct resolv_requester *requester, struct dns_counters *counters)
{
	struct stream *stream;

	if (requester->resolution == NULL)
		return 0;

	stream = objt_stream(requester->owner);
	if (stream == NULL)
		return 0;

	task_wakeup(stream->task, TASK_WOKEN_MSG);

	return 0;
}

/*
 * Do resolve error management callback
 * returns:
 *  0 if we can trash answser items.
 *  1 when safely ignored and we must kept answer items
 */
int act_resolution_error_cb(struct resolv_requester *requester, int error_code)
{
	struct stream *stream;

	if (requester->resolution == NULL)
		return 0;

	stream = objt_stream(requester->owner);
	if (stream == NULL)
		return 0;

	task_wakeup(stream->task, TASK_WOKEN_MSG);

	return 0;
}

/* Parse a set-timeout rule statement. It first checks if the timeout name is
 * valid and returns it in <name>. Then the timeout is parsed as a plain value
 * and * returned in <out_timeout>. If there is a parsing error, the value is
 * reparsed as an expression and returned in <expr>.
 *
 * Returns -1 if the name is invalid or neither a time or an expression can be
 * parsed, or if the timeout value is 0.
 */
int cfg_parse_rule_set_timeout(const char **args, int idx, int *out_timeout,
                               enum act_timeout_name *name,
                               struct sample_expr **expr, char **err,
                               const char *file, int line, struct arg_list *al)
{
	const char *res;
	const char *timeout_name = args[idx++];

	if (strcmp(timeout_name, "server") == 0) {
		*name = ACT_TIMEOUT_SERVER;
	}
	else if (strcmp(timeout_name, "tunnel") == 0) {
		*name = ACT_TIMEOUT_TUNNEL;
	}
	else {
		memprintf(err,
		          "'set-timeout' rule supports 'server'/'tunnel' (got '%s')",
		          timeout_name);
		return -1;
	}

	res = parse_time_err(args[idx], (unsigned int *)out_timeout, TIME_UNIT_MS);
	if (res == PARSE_TIME_OVER) {
		memprintf(err, "timer overflow in argument '%s' to rule 'set-timeout %s' (maximum value is 2147483647 ms or ~24.8 days)",
			  args[idx], timeout_name);
		return -1;
	}
	else if (res == PARSE_TIME_UNDER) {
		memprintf(err, "timer underflow in argument '%s' to rule 'set-timeout %s' (minimum value is 1 ms)",
			  args[idx], timeout_name);
		return -1;
	}
	/* res not NULL, parsing error */
	else if (res) {
		*expr = sample_parse_expr((char **)args, &idx, file, line, err, al, NULL);
		if (!*expr) {
			memprintf(err, "unexpected character '%c' in rule 'set-timeout %s'", *res, timeout_name);
			return -1;
		}
	}
	/* res NULL, parsing ok but value is 0 */
	else if (!(*out_timeout)) {
		memprintf(err, "null value is not valid for a 'set-timeout %s' rule",
			  timeout_name);
		return -1;
	}

	return 0;
}

/* tries to find in list <keywords> a similar looking action as the one in
 * <word>, and returns it otherwise NULL. <word> may be NULL or empty. An
 * optional array of extra words to compare may be passed in <extra>, but it
 * must then be terminated by a NULL entry. If unused it may be NULL.
 */
const char *action_suggest(const char *word, const struct list *keywords, const char **extra)
{
	uint8_t word_sig[1024];
	uint8_t list_sig[1024];
	const struct action_kw_list *kwl;
	const struct action_kw *best_kw = NULL;
	const char *best_ptr = NULL;
	int dist, best_dist = INT_MAX;
	int index;

	if (!word || !*word)
		return NULL;

	make_word_fingerprint(word_sig, word);
	list_for_each_entry(kwl, keywords, list) {
		for (index = 0; kwl->kw[index].kw != NULL; index++) {
			make_word_fingerprint(list_sig, kwl->kw[index].kw);
			dist = word_fingerprint_distance(word_sig, list_sig);
			if (dist < best_dist) {
				best_dist = dist;
				best_kw   = &kwl->kw[index];
				best_ptr  = best_kw->kw;
			}
		}
	}

	while (extra && *extra) {
		make_word_fingerprint(list_sig, *extra);
		dist = word_fingerprint_distance(word_sig, list_sig);
		if (dist < best_dist) {
			best_dist = dist;
			best_kw   = NULL;
			best_ptr  = *extra;
		}
		extra++;
	}

	/* eliminate too different ones, with more tolerance for prefixes
	 * when they're known to exist (not from extra list).
	 */
	if (best_ptr &&
	    (best_dist > (2 + (best_kw && (best_kw->flags & KWF_MATCH_PREFIX))) * strlen(word) ||
	     best_dist > (2 + (best_kw && (best_kw->flags & KWF_MATCH_PREFIX))) * strlen(best_ptr)))
		best_ptr = NULL;

	return best_ptr;
}

/* allocates a rule for ruleset <from> (ACT_F_*), from file name <file> and
 * line <linenum>. <file> and <linenum> may be zero if unknown. Returns the
 * rule, otherwise NULL in case of memory allocation error.
 */
struct act_rule *new_act_rule(enum act_from from, const char *file, int linenum)
{
	struct act_rule *rule;

	rule = calloc(1, sizeof(*rule));
	if (!rule)
		return NULL;
	rule->from = from;
	rule->conf.file = file ? strdup(file) : NULL;
	rule->conf.line = linenum;
	LIST_INIT(&rule->list);
	return rule;
}

/* fees rule <rule> and its elements as well as the condition */
void free_act_rule(struct act_rule *rule)
{
	LIST_DELETE(&rule->list);
	free_acl_cond(rule->cond);
	if (rule->release_ptr)
		rule->release_ptr(rule);
	free(rule->conf.file);
	free(rule);
}

void free_act_rules(struct list *rules)
{
	struct act_rule *rule, *ruleb;

	list_for_each_entry_safe(rule, ruleb, rules, list) {
		free_act_rule(rule);
	}
}

/* dumps all known actions registered in action rules <rules> after prefix
 * <pfx> to stdout. The actions are alphabetically sorted. Those with the
 * KWF_MATCH_PREFIX flag have their name suffixed with '*'.
 */
void dump_act_rules(const struct list *rules, const char *pfx)
{
	const struct action_kw *akwp, *akwn;
	struct action_kw_list *akwl;
	int index;

	for (akwn = akwp = NULL;; akwp = akwn) {
		list_for_each_entry(akwl, rules, list) {
			for (index = 0; akwl->kw[index].kw != NULL; index++)
				if (strordered(akwp ? akwp->kw : NULL,
					       akwl->kw[index].kw,
					       akwn != akwp ? akwn->kw : NULL))
					akwn = &akwl->kw[index];
		}
		if (akwn == akwp)
			break;
		printf("%s%s%s\n", pfx ? pfx : "", akwn->kw,
		       (akwn->flags & KWF_MATCH_PREFIX) ? "*" : "");
	}
}
