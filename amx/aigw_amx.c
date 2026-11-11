/* SPDX-License-Identifier: Apache-2.0 */
/*
 * aigw-amx: AIGW OS's data model as an Ambiorix component, for prplOS.
 *
 * prplOS builds its TR-181 tree from Ambiorix components, each publishing its
 * objects on the system bus; prplOS's own USP agent, TR-069 client, CLI and web
 * UI read them from there. This plug-in, loaded by amxrt, makes AIGW one of
 * them: it publishes AIGW's own subtree, Device.X_<prefix>_. (alerts,
 * decisions, the mode, their commands and events). prplOS keeps what is its
 * own (Device.WiFi, Hosts, DeviceInfo).
 *
 * Like the USP and TR-069 adapters it holds no data: at start it reads agd's
 * schema (ubus ag.dm schema) and builds the objects, so the vendor prefix and
 * new parameters follow agd. Reads, writes and commands are ag.dm calls (origin
 * "prplos", or AIGW_AMX_ORIGIN); instances are kept in step with agd; agd's
 * events (ag.dm.event) become Ambiorix signals, e.g. "Alert!".
 *
 *   amxrt [-B mod-amxb-ubus.so -u ubus:<socket>] aigw-amx.odl
 *
 * Environment: AIGW_UBUS (agd's ubus socket), AIGW_AMX_ORIGIN.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <amxc/amxc.h>
#include <amxp/amxp.h>
#include <amxd/amxd_types.h>
#include <amxd/amxd_action.h>
#include <amxd/amxd_dm.h>
#include <amxd/amxd_function.h>
#include <amxd/amxd_object.h>
#include <amxd/amxd_object_event.h>
#include <amxd/amxd_object_function.h>
#include <amxd/amxd_object_hierarchy.h>
#include <amxd/amxd_object_parameter.h>
#include <amxd/amxd_parameter.h>
#include <amxd/amxd_parameter_action.h>
#include <amxd/amxd_transaction.h>
#include <amxo/amxo.h>
#include <amxp/amxp_timer.h>

#include <libubox/blobmsg.h>
#include <libubus.h>

#define TIMEOUT   10000
#define SYNC_MS   5000
#define PATH_LEN  256

static amxd_dm_t *dm;
static struct ubus_context *out;        /* calls to agd */
static struct ubus_context *evctx;      /* agd's events, on amxrt's loop */
static struct ubus_event_handler ev_handler;
static const char *ubus_path;
static const char *origin = "prplos";
static struct blob_attr *schema;
static char root[64];                   /* "X_AIGW_": the vendor object */
static amxp_timer_t *sync_timer;

/* ---------- agd ---------- */

struct reply {
	struct blob_attr *msg;
};

static void reply_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct reply *r = req->priv;

	(void)type;
	free(r->msg);
	r->msg = NULL;
	if (msg && (r->msg = malloc(blob_pad_len(msg))))
		memcpy(r->msg, msg, blob_pad_len(msg));
}

static struct blob_attr *call(const char *method, struct blob_attr *args)
{
	struct reply r = { NULL };
	uint32_t id;

	if (!out && !(out = ubus_connect(ubus_path)))
		return NULL;
	if (ubus_lookup_id(out, "ag.dm", &id) &&
	    (ubus_reconnect(out, ubus_path) || ubus_lookup_id(out, "ag.dm", &id)))
		return NULL;
	if (ubus_invoke(out, id, method, args, reply_cb, &r, TIMEOUT)) {
		free(r.msg);
		return NULL;
	}
	return r.msg;
}

static struct blob_attr *field(struct blob_attr *tbl, const char *name)
{
	struct blob_attr *cur;
	size_t rem;

	if (!tbl)
		return NULL;
	blobmsg_for_each_attr(cur, tbl, rem)
		if (!strcmp(blobmsg_name(cur), name))
			return cur;
	return NULL;
}

static struct blob_attr *top(struct blob_attr *msg, const char *name)
{
	struct blob_attr *cur;
	size_t rem;

	if (!msg)
		return NULL;
	blob_for_each_attr(cur, msg, rem)
		if (!strcmp(blobmsg_name(cur), name))
			return cur;
	return NULL;
}

static const char *str(struct blob_attr *a)
{
	return a && blobmsg_type(a) == BLOBMSG_TYPE_STRING ? blobmsg_get_string(a) : "";
}

/* "X_AIGW_.Alert.3" (amx, indexed) -> "Device.X_AIGW_.Alert.3." */
static void dm_path(amxd_object_t *object, const char *leaf, char *buf, size_t n)
{
	char *p = amxd_object_get_path(object, AMXD_OBJECT_INDEXED);

	snprintf(buf, n, "Device.%s.%s", p ? p : "", leaf ? leaf : "");
	free(p);
}

static amxd_status_t status_of(const char *code)
{
	if (!strcmp(code, "not_found"))
		return amxd_status_object_not_found;
	if (!strcmp(code, "read_only"))
		return amxd_status_read_only;
	if (!strcmp(code, "invalid_value"))
		return amxd_status_invalid_value;
	if (!strcmp(code, "invalid_arguments"))
		return amxd_status_invalid_function_argument;
	if (!strcmp(code, "refused"))
		return amxd_status_permission_denied;
	return amxd_status_unknown_error;
}

/* ---------- parameters ---------- */

static uint32_t amx_type(const char *t)
{
	if (!strcmp(t, "boolean")) return AMXC_VAR_ID_BOOL;
	if (!strcmp(t, "unsignedInt")) return AMXC_VAR_ID_UINT32;
	if (!strcmp(t, "int")) return AMXC_VAR_ID_INT32;
	if (!strcmp(t, "unsignedLong")) return AMXC_VAR_ID_UINT64;
	if (!strcmp(t, "dateTime")) return AMXC_VAR_ID_TIMESTAMP;
	if (!strcmp(t, "decimal")) return AMXC_VAR_ID_DOUBLE;
	return AMXC_VAR_ID_CSTRING;
}

/* one value from agd, as the parameter's type */
static amxd_status_t p_read(amxd_object_t * const object, amxd_param_t * const param, amxd_action_t reason,
			    const amxc_var_t * const args, amxc_var_t * const retval, void *priv)
{
	struct blob_buf b = { 0 };
	struct blob_attr *r;
	char path[PATH_LEN];
	uint32_t type = amxd_param_get_type(param);
	void *a;

	(void)reason;
	(void)args;
	(void)priv;
	if (amxd_object_get_type(object) == amxd_object_template)
		return amxd_status_ok;  /* the template itself has no values */
	dm_path(object, amxd_param_get_name(param), path, sizeof(path));
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "origin", origin);
	a = blobmsg_open_array(&b, "paths");
	blobmsg_add_string(&b, NULL, path);
	blobmsg_close_array(&b, a);
	r = call("get", b.head);
	blob_buf_free(&b);
	if (!r)
		return amxd_status_unknown_error;
	amxc_var_set(cstring_t, retval, str(field(top(r, "values"), path)));
	free(r);
	if (type != AMXC_VAR_ID_CSTRING && amxc_var_cast(retval, type) != 0)
		amxc_var_set_type(retval, type);        /* an empty value: the type's zero */
	return amxd_status_ok;
}

/* a set in agd: check only (validate) or for real (write) */
static amxd_status_t dm_set(amxd_object_t *object, amxd_param_t *param, const amxc_var_t *value, int check)
{
	struct blob_buf b = { 0 };
	struct blob_attr *r, *e;
	char path[PATH_LEN], *v = amxc_var_dyncast(cstring_t, value);
	amxd_status_t st = amxd_status_ok;
	void *t;

	dm_path(object, amxd_param_get_name(param), path, sizeof(path));
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "origin", origin);
	t = blobmsg_open_table(&b, "params");
	blobmsg_add_string(&b, path, v ? v : "");
	blobmsg_close_table(&b, t);
	if (check)
		blobmsg_add_u8(&b, "check", 1);
	free(v);
	r = call("set", b.head);
	blob_buf_free(&b);
	if (!r)
		return amxd_status_unknown_error;
	if (!top(r, "ok") || !blobmsg_get_u8(top(r, "ok")))
		st = (e = field(top(r, "errors"), path)) ? status_of(str(field(e, "code"))) : amxd_status_unknown_error;
	free(r);
	return st;
}

static amxd_status_t p_validate(amxd_object_t * const object, amxd_param_t * const param, amxd_action_t reason,
				const amxc_var_t * const args, amxc_var_t * const retval, void *priv)
{
	(void)reason;
	(void)retval;
	(void)priv;
	if (amxd_object_get_type(object) == amxd_object_template)
		return amxd_status_ok;
	return dm_set(object, param, args, 1);
}

static amxd_status_t p_write(amxd_object_t * const object, amxd_param_t * const param, amxd_action_t reason,
			     const amxc_var_t * const args, amxc_var_t * const retval, void *priv)
{
	amxd_status_t st;

	(void)reason;
	(void)retval;
	(void)priv;
	if (amxd_object_get_type(object) == amxd_object_template)
		return amxd_status_ok;
	st = dm_set(object, param, args, 0);
	if (st == amxd_status_ok)
		amxc_var_copy(&param->value, args);     /* what change events report */
	return st;
}

/* ---------- commands ---------- */

static amxd_status_t f_call(amxd_object_t *object, amxd_function_t *func, amxc_var_t *args, amxc_var_t *ret)
{
	struct blob_buf b = { 0 };
	struct blob_attr *r, *o, *cur;
	char path[PATH_LEN], name[96];
	amxd_status_t st = amxd_status_ok;
	size_t rem;
	void *t;

	snprintf(name, sizeof(name), "%s()", amxd_function_get_name(func));
	dm_path(object, name, path, sizeof(path));
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "origin", origin);
	blobmsg_add_string(&b, "command", path);
	t = blobmsg_open_table(&b, "args");
	amxc_var_for_each(a, args) {
		char *v = amxc_var_dyncast(cstring_t, a);

		if (v && *v)
			blobmsg_add_string(&b, amxc_var_key(a), v);
		free(v);
	}
	blobmsg_close_table(&b, t);
	r = call("operate", b.head);
	blob_buf_free(&b);
	if (!r)
		return amxd_status_unknown_error;
	amxc_var_set_type(ret, AMXC_VAR_ID_HTABLE);
	if (top(r, "ok") && blobmsg_get_u8(top(r, "ok"))) {
		/* outputs: in the return value and, as USP wants them, as out arguments */
		amxc_var_clean(args);
		amxc_var_set_type(args, AMXC_VAR_ID_HTABLE);
		if ((o = top(r, "out")))
			blobmsg_for_each_attr(cur, o, rem) {
				amxc_var_add_key(cstring_t, ret, blobmsg_name(cur), str(cur));
				amxc_var_add_key(cstring_t, args, blobmsg_name(cur), str(cur));
			}
	} else {
		st = status_of(str(top(r, "code")));
		amxc_var_add_key(cstring_t, ret, "message", str(top(r, "message")));
	}
	free(r);
	return st;
}

/* ---------- building the objects from agd's schema ---------- */

/* "Device.X_AIGW_.Alert.{i}.Kind" -> its object (made as needed), leaf "Kind" */
static amxd_object_t *object_for(const char *path, char *leaf, size_t ln, int whole)
{
	char buf[PATH_LEN], *tok, *save, *next;
	amxd_object_t *cur = amxd_dm_get_root(dm), *child;

	if (strncmp(path, "Device.", 7))
		return NULL;
	snprintf(buf, sizeof(buf), "%s", path + 7);
	if (!whole) {                   /* split off the leaf (parameter, command, event) */
		char *dot;
		size_t l = strlen(buf);

		if (l && buf[l - 1] == '.')
			buf[--l] = '\0';
		dot = strrchr(buf, '.');
		if (!dot)
			return NULL;
		snprintf(leaf, ln, "%s", dot + 1);
		*dot = '\0';
	}
	for (tok = strtok_r(buf, ".", &save); tok; tok = next) {
		int templ;

		next = strtok_r(NULL, ".", &save);
		if (!strcmp(tok, "{i}"))
			continue;
		templ = next && !strcmp(next, "{i}");
		child = amxd_object_get_child(cur, tok);
		if (!child) {
			if (amxd_object_new(&child, templ ? amxd_object_template : amxd_object_singleton, tok) != amxd_status_ok)
				return NULL;
			/* AIGW's objects are agd's: no one adds or deletes instances through the bus */
			amxd_object_set_attr(child, amxd_oattr_read_only, true);
			if (cur == amxd_dm_get_root(dm))
				amxd_dm_add_root_object(dm, child);
			else
				amxd_object_add_object(cur, child);
		}
		cur = child;
	}
	return cur;
}

static int add_param(struct blob_attr *p)
{
	const char *path = str(field(p, "path"));
	struct blob_attr *w = field(p, "writable");
	char leaf[96];
	amxd_object_t *o = object_for(path, leaf, sizeof(leaf), 0);
	amxd_param_t *param = NULL;

	if (!o || amxd_param_new(&param, leaf, amx_type(str(field(p, "type")))) != amxd_status_ok)
		return -1;
	if (amxd_object_get_type(o) == amxd_object_template) {
		amxd_param_set_attr(param, amxd_pattr_template, false);
		amxd_param_set_attr(param, amxd_pattr_instance, true);
	}
	amxd_param_add_action_cb(param, action_param_read, p_read, NULL);
	if (w && blobmsg_get_u8(w)) {
		amxd_param_add_action_cb(param, action_param_validate, p_validate, NULL);
		amxd_param_add_action_cb(param, action_param_write, p_write, NULL);
	} else
		amxd_param_set_attr(param, amxd_pattr_read_only, true);
	return amxd_object_add_param(o, param) == amxd_status_ok ? 0 : -1;
}

static int add_command(struct blob_attr *c)
{
	const char *path = str(field(c, "path"));
	struct blob_attr *in = field(c, "in"), *cur;
	char leaf[96];
	amxd_object_t *o = object_for(path, leaf, sizeof(leaf), 0);
	amxd_function_t *f = NULL;
	size_t rem, l = strlen(leaf);

	if (l > 2 && !strcmp(leaf + l - 2, "()"))
		leaf[l - 2] = '\0';
	if (!o || amxd_function_new(&f, leaf, AMXC_VAR_ID_HTABLE, f_call) != amxd_status_ok)
		return -1;
	if (amxd_object_get_type(o) == amxd_object_template) {
		amxd_function_set_attr(f, amxd_fattr_template, false);
		amxd_function_set_attr(f, amxd_fattr_instance, true);
	}
	if (in)
		blobmsg_for_each_attr(cur, in, rem)
			amxd_function_new_arg(f, str(cur), AMXC_VAR_ID_CSTRING, NULL);
	return amxd_object_add_function(o, f) == amxd_status_ok ? 0 : -1;
}

static int add_event(struct blob_attr *e)
{
	char leaf[96];
	amxd_object_t *o = object_for(str(field(e, "path")), leaf, sizeof(leaf), 0);

	return o && amxd_object_add_event(o, leaf) == amxd_status_ok ? 0 : -1;
}

/* is this schema path AIGW's own (under Device.<prefix>.)? */
static int ours(const char *path)
{
	char pre[80];

	snprintf(pre, sizeof(pre), "Device.%s.", root);
	return !strncmp(path, pre, strlen(pre));
}

static int build(void)
{
	struct blob_attr *cur;
	size_t rem;
	int np = 0, nc = 0, ne = 0, bad = 0;

	if (top(schema, "objects"))
		blobmsg_for_each_attr(cur, top(schema, "objects"), rem)
			if (ours(str(field(cur, "path"))))
				bad += !object_for(str(field(cur, "path")), NULL, 0, 1);
	if (top(schema, "params"))
		blobmsg_for_each_attr(cur, top(schema, "params"), rem)
			if (ours(str(field(cur, "path")))) {
				bad += add_param(cur) < 0;
				np++;
			}
	if (top(schema, "commands"))
		blobmsg_for_each_attr(cur, top(schema, "commands"), rem)
			if (ours(str(field(cur, "path")))) {
				bad += add_command(cur) < 0;
				nc++;
			}
	if (top(schema, "events"))
		blobmsg_for_each_attr(cur, top(schema, "events"), rem)
			if (ours(str(field(cur, "path")))) {
				bad += add_event(cur) < 0;
				ne++;
			}
	fprintf(stderr, "aigw-amx: AIGW OS data model %s: %s with %d parameters, %d commands, %d events%s\n",
		str(top(schema, "version")), root, np, nc, ne, bad ? " (some not published)" : "");
	return 0;
}

/* ---------- instances, kept in step with agd ---------- */

static void sync_table(const char *templ_path)
{
	struct blob_buf b = { 0 };
	struct blob_attr *r, *list, *cur;
	char q[PATH_LEN], leaf[8];
	amxd_object_t *t = object_for(templ_path, leaf, sizeof(leaf), 1);
	uint32_t want[512];
	int nw = 0, i;
	size_t rem, l;
	amxd_trans_t tr;
	int changes = 0;

	(void)leaf;
	if (!t || amxd_object_get_type(t) != amxd_object_template)
		return;
	/* "Device.X_AIGW_.Alert.{i}." -> ask for "Device.X_AIGW_.Alert." */
	snprintf(q, sizeof(q), "%s", templ_path);
	l = strlen(q);
	if (l > 4 && !strcmp(q + l - 4, "{i}."))
		q[l - 4] = '\0';
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "path", q);
	r = call("instances", b.head);
	blob_buf_free(&b);
	if (!r)
		return;
	if ((list = top(r, "instances")))
		blobmsg_for_each_attr(cur, list, rem)
			if (nw < 512)
				want[nw++] = (uint32_t)strtoul(str(cur) + strlen(q), NULL, 10);
	free(r);
	amxd_trans_init(&tr);
	amxd_trans_set_attr(&tr, amxd_tattr_change_ro, true);
	amxd_trans_select_object(&tr, t);
	amxd_object_for_each(instance, it, t) {
		amxd_object_t *inst = amxc_container_of(it, amxd_object_t, it);
		uint32_t idx = amxd_object_get_index(inst);

		for (i = 0; i < nw && want[i] != idx; i++)
			;
		if (i == nw) {
			amxd_trans_del_inst(&tr, idx, NULL);
			changes++;
		}
	}
	for (i = 0; i < nw; i++)
		if (want[i] && !amxd_object_get_instance(t, NULL, want[i])) {
			amxd_trans_add_inst(&tr, want[i], NULL);
			amxd_trans_select_object(&tr, t);
			changes++;
		}
	if (changes && amxd_trans_apply(&tr, dm) != amxd_status_ok)
		fprintf(stderr, "aigw-amx: could not update %s's instances\n", q);
	amxd_trans_clean(&tr);
}

static void sync_all(void)
{
	struct blob_attr *cur;
	size_t rem;

	if (top(schema, "objects"))
		blobmsg_for_each_attr(cur, top(schema, "objects"), rem)
			if (ours(str(field(cur, "path"))))
				sync_table(str(field(cur, "path")));
}

static void on_timer(amxp_timer_t *timer, void *priv)
{
	(void)timer;
	(void)priv;
	sync_all();
}

/* ---------- agd's events -> signals ---------- */

static void on_event(struct ubus_context *c, struct ubus_event_handler *ev, const char *type, struct blob_attr *msg)
{
	const char *name = str(top(msg, "event"));
	struct blob_attr *args = top(msg, "args"), *cur;
	char leaf[96];
	amxd_object_t *o;
	amxc_var_t data;
	size_t rem;

	(void)c;
	(void)ev;
	(void)type;
	if (!*name || !ours(name))
		return;
	sync_all();             /* an alert or a decision: maybe a new instance */
	if (!(o = object_for(name, leaf, sizeof(leaf), 0)))
		return;
	amxc_var_init(&data);
	amxc_var_set_type(&data, AMXC_VAR_ID_HTABLE);
	if (args)
		blobmsg_for_each_attr(cur, args, rem)
			amxc_var_add_key(cstring_t, &data, blobmsg_name(cur), str(cur));
	amxd_object_emit_signal(o, leaf, &data);
	amxc_var_clean(&data);
}

static void ev_read(int fd, void *priv)
{
	(void)fd;
	(void)priv;
	ubus_handle_event(evctx);
}

/* ---------- entry point ---------- */

static int start(amxd_dm_t *d, amxo_parser_t *parser)
{
	const char *o = getenv("AIGW_AMX_ORIGIN");
	int i;

	dm = d;
	ubus_path = getenv("AIGW_UBUS");
	if (ubus_path && !*ubus_path)
		ubus_path = NULL;
	if (o && *o)
		origin = o;
	/* agd may still be starting */
	for (i = 0; i < 30 && !(schema = call("schema", NULL)); i++)
		sleep(1);
	if (!schema) {
		fprintf(stderr, "aigw-amx: no AIGW OS data model on ubus (is agd running?)\n");
		return -1;
	}
	snprintf(root, sizeof(root), "%s", *str(top(schema, "vendor_prefix")) ? str(top(schema, "vendor_prefix")) : "X_AIGW_");
	build();
	sync_all();
	if ((evctx = ubus_connect(ubus_path))) {
		ev_handler.cb = on_event;
		if (ubus_register_event_handler(evctx, &ev_handler, "ag.dm.event") ||
		    amxo_connection_add(parser, evctx->sock.fd, ev_read, NULL, AMXO_CUSTOM, NULL))
			fprintf(stderr, "aigw-amx: cannot follow AIGW OS events\n");
	}
	amxp_timer_new(&sync_timer, on_timer, NULL);
	amxp_timer_set_interval(sync_timer, SYNC_MS);
	amxp_timer_start(sync_timer, SYNC_MS);
	return 0;
}

static void stop(amxo_parser_t *parser)
{
	amxp_timer_delete(&sync_timer);
	if (evctx) {
		amxo_connection_remove(parser, evctx->sock.fd);
		ubus_free(evctx);
		evctx = NULL;
	}
	if (out) {
		ubus_free(out);
		out = NULL;
	}
	free(schema);
	schema = NULL;
}

/* "entry-point aigw.aigw_main" in the ODL: reason 0 start, 1 stop */
int _aigw_main(int reason, amxd_dm_t *d, amxo_parser_t *parser);
int _aigw_main(int reason, amxd_dm_t *d, amxo_parser_t *parser)
{
	if (reason == 0)
		return start(d, parser);
	if (reason == 1)
		stop(parser);
	return 0;
}
