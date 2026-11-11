/* SPDX-License-Identifier: Apache-2.0 */
/*
 * aigw-usp: AIGW OS's data model for OB-USP-Agent (USP / TR-369).
 *
 * A vendor plug-in, loaded by an unmodified obuspa with "-x aigw-usp.so". It
 * holds no data: at start it reads agd's schema (ubus ag.dm schema) and
 * registers every path with the agent; the agent's get, set, operate and
 * instance callbacks become ag.dm calls, and agd's ag.dm.event events become
 * USP events. What a controller may do is decided in agd (mgmt.* in
 * /etc/config/aigw), whatever this plug-in asks.
 *
 *   DM thread (obuspa's)   synchronous ubus calls on one connection
 *   event thread (ours)    its own ubus connection and uloop; USP_SIGNAL_* is
 *                          the agent's thread-safe way in
 *
 * Built against the same obuspa source tree as the agent (its headers must
 * match the binary): cmake -DAIGW_OBUSPA_SRC=<obuspa checkout>.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubus.h>

#include "usp_api.h"
#include "usp_err_codes.h"
#include "vendor_defs.h"

/* from the agent's core (exported by the obuspa binary) */
void *USP_MEM_Strdup(const char *func, int line, void *ptr);
#define STRDUP(x) USP_MEM_Strdup(__FUNCTION__, __LINE__, (void *)(x))

#define GROUP   (MAX_VENDOR_PARAM_GROUPS - 1)   /* ours; USP Services take the low ids */
#define ORIGIN  "usp"
#define TIMEOUT 10000
#define EXPIRY  5                               /* seconds the agent may keep an instance list */

static struct ubus_context *dm_ctx;             /* used only from the agent's data model thread */
static const char *ubus_path;

/* ---------- calls to agd ---------- */

struct reply {
	struct blob_attr *msg;
};

static void reply_cb(struct ubus_request *req, int type, struct blob_attr *msg)
{
	struct reply *r = req->priv;

	free(r->msg);
	r->msg = NULL;
	if (msg && (r->msg = malloc(blob_pad_len(msg))))
		memcpy(r->msg, msg, blob_pad_len(msg));
}

/* one call to ag.dm; the reply (malloc'd, the caller frees) or NULL */
static struct blob_attr *dm_call(const char *method, struct blob_attr *args)
{
	struct reply r = { NULL };
	uint32_t id;
	int rc;

	if (!dm_ctx && !(dm_ctx = ubus_connect(ubus_path)))
		return NULL;
	if (ubus_lookup_id(dm_ctx, "ag.dm", &id)) {
		/* agd restarted: one reconnect */
		if (ubus_reconnect(dm_ctx, ubus_path) || ubus_lookup_id(dm_ctx, "ag.dm", &id))
			return NULL;
	}
	rc = ubus_invoke(dm_ctx, id, method, args, reply_cb, &r, TIMEOUT);
	if (rc) {
		USP_LOG_Error("aigw-usp: ag.dm %s: %s", method, ubus_strerror(rc));
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

/* a reply's top-level fields are a table body: wrap it to use field() */
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

static int usp_err(const char *code, int for_command)
{
	if (!strcmp(code, "not_found"))
		return for_command ? USP_ERR_OBJECT_DOES_NOT_EXIST : USP_ERR_INVALID_PATH;
	if (!strcmp(code, "read_only"))
		return USP_ERR_PARAM_READ_ONLY;
	if (!strcmp(code, "invalid_value"))
		return USP_ERR_INVALID_VALUE;
	if (!strcmp(code, "invalid_arguments"))
		return USP_ERR_INVALID_ARGUMENTS;
	if (!strcmp(code, "refused"))
		return for_command ? USP_ERR_COMMAND_FAILURE : USP_ERR_PERMISSION_DENIED;
	return USP_ERR_INTERNAL_ERROR;
}

/* ---------- the agent's callbacks ---------- */

static int get_cb(int group_id, kv_vector_t *params)
{
	struct blob_buf b = { 0 };
	struct blob_attr *reply, *vals;
	void *a;
	int i;

	(void)group_id;
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "origin", ORIGIN);
	a = blobmsg_open_array(&b, "paths");
	for (i = 0; i < params->num_entries; i++)
		blobmsg_add_string(&b, NULL, params->vector[i].key);
	blobmsg_close_array(&b, a);
	reply = dm_call("get", b.head);
	blob_buf_free(&b);
	if (!reply) {
		USP_ERR_SetMessage("AIGW OS (agd) is not answering");
		return USP_ERR_INTERNAL_ERROR;
	}
	vals = top(reply, "values");
	for (i = 0; i < params->num_entries; i++) {
		struct blob_attr *v = field(vals, params->vector[i].key);

		/* an instance that went away since the agent listed it reads as empty */
		params->vector[i].value = STRDUP(str(v));
	}
	free(reply);
	return USP_ERR_OK;
}

static int set_cb(int group_id, kv_vector_t *params, unsigned *types, int *failure_index)
{
	struct blob_buf b = { 0 };
	struct blob_attr *reply, *errs, *e;
	void *t;
	int i, rc = USP_ERR_OK;

	(void)group_id;
	(void)types;
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "origin", ORIGIN);
	t = blobmsg_open_table(&b, "params");
	for (i = 0; i < params->num_entries; i++)
		blobmsg_add_string(&b, params->vector[i].key, params->vector[i].value ? params->vector[i].value : "");
	blobmsg_close_table(&b, t);
	reply = dm_call("set", b.head);
	blob_buf_free(&b);
	if (!reply) {
		USP_ERR_SetMessage("AIGW OS (agd) is not answering");
		return USP_ERR_INTERNAL_ERROR;
	}
	errs = top(reply, "errors");
	for (i = 0; errs && i < params->num_entries; i++)
		if ((e = field(errs, params->vector[i].key))) {
			*failure_index = i;
			USP_ERR_SetMessage("%s", str(field(e, "message")));
			rc = usp_err(str(field(e, "code")), 0);
			break;
		}
	free(reply);
	return rc;
}

static int refresh_cb(int group_id, char *path, int *expiry_period)
{
	struct blob_buf b = { 0 };
	struct blob_attr *reply, *arr, *cur;
	char q[MAX_DM_PATH];
	size_t rem;

	(void)group_id;
	*expiry_period = EXPIRY;
	/* the agent passes the object's path up to its instance number: "Device.Hosts.Host." */
	snprintf(q, sizeof(q), "%s%s", path, path[0] && path[strlen(path) - 1] == '.' ? "" : ".");
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "path", q);
	reply = dm_call("instances", b.head);
	blob_buf_free(&b);
	if (!reply) {
		USP_ERR_SetMessage("AIGW OS (agd) is not answering");
		return USP_ERR_INTERNAL_ERROR;
	}
	if ((arr = top(reply, "instances")))
		blobmsg_for_each_attr(cur, arr, rem) {
			char p[MAX_DM_PATH];
			size_t n;

			snprintf(p, sizeof(p), "%s", str(cur));
			n = strlen(p);
			if (n && p[n - 1] == '.')
				p[n - 1] = '\0';
			USP_DM_RefreshInstance(p);
		}
	free(reply);
	return USP_ERR_OK;
}

static int oper_cb(dm_req_t *req, char *command_key, kv_vector_t *input_args, kv_vector_t *output_args)
{
	struct blob_buf b = { 0 };
	struct blob_attr *reply, *out, *cur;
	struct blob_attr *ok;
	size_t rem;
	void *t;
	int i, rc = USP_ERR_OK;

	(void)command_key;
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "origin", ORIGIN);
	blobmsg_add_string(&b, "command", req->path);
	t = blobmsg_open_table(&b, "args");
	for (i = 0; input_args && i < input_args->num_entries; i++)
		blobmsg_add_string(&b, input_args->vector[i].key, input_args->vector[i].value ? input_args->vector[i].value : "");
	blobmsg_close_table(&b, t);
	reply = dm_call("operate", b.head);
	blob_buf_free(&b);
	if (!reply) {
		USP_ERR_SetMessage("AIGW OS (agd) is not answering");
		return USP_ERR_INTERNAL_ERROR;
	}
	ok = top(reply, "ok");
	if (!ok || !blobmsg_get_u8(ok)) {
		USP_ERR_SetMessage("%s", str(top(reply, "message")));
		rc = usp_err(str(top(reply, "code")), 1);
	} else if ((out = top(reply, "out"))) {
		blobmsg_for_each_attr(cur, out, rem)
			USP_ARG_Add(output_args, (char *)blobmsg_name(cur), (char *)str(cur));
	}
	free(reply);
	return rc;
}

/*
 * Asynchronous commands (firmware Download(), Activate()): agd answers when it
 * is done, which may take minutes. Each runs on a thread of its own, with its
 * own ubus connection, and ends with USP_SIGNAL_OperationComplete.
 */
#define AOP_TIMEOUT (60 * 60 * 1000)
#define AOP_ARGS    16

struct aop {
	int instance;
	char path[MAX_DM_PATH];
	int n;
	char *key[AOP_ARGS], *val[AOP_ARGS];
};

static void aop_free(struct aop *o)
{
	int i;

	for (i = 0; i < o->n; i++) {
		free(o->key[i]);
		free(o->val[i]);
	}
	free(o);
}

static void *aop_main(void *arg)
{
	struct aop *o = arg;
	struct ubus_context *c = ubus_connect(ubus_path);
	struct blob_buf b = { 0 };
	struct reply r = { NULL };
	struct blob_attr *ok, *out, *cur;
	kv_vector_t *outv = USP_ARG_Create();
	char msg[256] = "AIGW OS (agd) is not answering";
	uint32_t id;
	size_t rem;
	void *t;
	int i, err = USP_ERR_COMMAND_FAILURE;

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "origin", ORIGIN);
	blobmsg_add_string(&b, "command", o->path);
	t = blobmsg_open_table(&b, "args");
	for (i = 0; i < o->n; i++)
		blobmsg_add_string(&b, o->key[i], o->val[i]);
	blobmsg_close_table(&b, t);
	if (c && !ubus_lookup_id(c, "ag.dm", &id) &&
	    !ubus_invoke(c, id, "operate", b.head, reply_cb, &r, AOP_TIMEOUT) && r.msg) {
		ok = top(r.msg, "ok");
		if (ok && blobmsg_get_u8(ok)) {
			err = USP_ERR_OK;
			msg[0] = '\0';
			if ((out = top(r.msg, "out")))
				blobmsg_for_each_attr(cur, out, rem)
					USP_ARG_Add(outv, (char *)blobmsg_name(cur), (char *)str(cur));
		} else {
			snprintf(msg, sizeof(msg), "%s", str(top(r.msg, "message")));
			err = usp_err(str(top(r.msg, "code")), 1);
		}
	}
	free(r.msg);
	blob_buf_free(&b);
	if (c)
		ubus_free(c);
	USP_LOG_Info("aigw-usp: %s: %s%s%s", o->path, err == USP_ERR_OK ? "done" : "failed",
		     msg[0] ? " -- " : "", msg);
	USP_SIGNAL_OperationComplete(o->instance, err, err == USP_ERR_OK ? NULL : msg, outv);   /* outv is the agent's now */
	aop_free(o);
	return NULL;
}

static int aop_start(dm_req_t *req, kv_vector_t *input_args, int instance)
{
	struct aop *o = calloc(1, sizeof(*o));
	pthread_attr_t at;
	pthread_t th;
	int i;

	if (!o)
		return USP_ERR_RESOURCES_EXCEEDED;
	o->instance = instance;
	snprintf(o->path, sizeof(o->path), "%s", req->path);
	/* the agent keeps its input arguments: copy what agd needs */
	for (i = 0; input_args && i < input_args->num_entries && o->n < AOP_ARGS; i++) {
		if (!strncmp(input_args->vector[i].key, "Internal_", 9))
			continue;       /* the agent's own (its time reference) */
		o->key[o->n] = strdup(input_args->vector[i].key);
		o->val[o->n] = strdup(input_args->vector[i].value ? input_args->vector[i].value : "");
		o->n++;
	}
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&th, &at, aop_main, o)) {
		pthread_attr_destroy(&at);
		aop_free(o);
		USP_ERR_SetMessage("cannot start the operation");
		return USP_ERR_INTERNAL_ERROR;
	}
	pthread_attr_destroy(&at);
	return USP_ERR_OK;
}

/* one value from agd, for the agent's own parameters (DeviceInfo.SoftwareVersion) */
static int dm_value(const char *path, char *buf, int len)
{
	struct blob_buf b = { 0 };
	struct blob_attr *reply;
	void *a;

	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "origin", ORIGIN);
	a = blobmsg_open_array(&b, "paths");
	blobmsg_add_string(&b, NULL, path);
	blobmsg_close_array(&b, a);
	reply = dm_call("get", b.head);
	blob_buf_free(&b);
	snprintf(buf, (size_t)len, "%s", reply ? str(field(top(reply, "values"), path)) : "");
	free(reply);
	return USP_ERR_OK;
}

static int sw_version_cb(char *buf, int len)
{
	return dm_value("Device.DeviceInfo.SoftwareVersion", buf, len);
}

/* ---------- registration ---------- */

/* obuspa implements these itself (values from its database, set by the init script) */
static int core_owned(const char *p)
{
	static const char *const own[] = {
		"Device.DeviceInfo.Manufacturer", "Device.DeviceInfo.ModelName", "Device.DeviceInfo.ProductClass",
		"Device.DeviceInfo.SoftwareVersion", "Device.DeviceInfo.UpTime", "Device.DeviceInfo.SerialNumber",
		"Device.DeviceInfo.HardwareVersion", "Device.DeviceInfo.ManufacturerOUI", NULL
	};
	int i;

	for (i = 0; own[i]; i++)
		if (!strcmp(p, own[i]))
			return 1;
	return 0;
}

static unsigned usp_type(const char *t)
{
	if (!strcmp(t, "boolean")) return DM_BOOL;
	if (!strcmp(t, "unsignedInt")) return DM_UINT;
	if (!strcmp(t, "int")) return DM_INT;
	if (!strcmp(t, "unsignedLong")) return DM_ULONG;
	if (!strcmp(t, "dateTime")) return DM_DATETIME;
	if (!strcmp(t, "decimal")) return DM_DECIMAL;
	return DM_STRING;
}

/* the strings of a blobmsg array, into names[] (pointers into the reply) */
static int names(struct blob_attr *arr, char **out, int max)
{
	struct blob_attr *cur;
	size_t rem;
	int n = 0;

	if (arr)
		blobmsg_for_each_attr(cur, arr, rem)
			if (n < max && blobmsg_type(cur) == BLOBMSG_TYPE_STRING)
				out[n++] = blobmsg_get_string(cur);
	return n;
}

static int register_schema(struct blob_attr *schema)
{
	struct blob_attr *cur;
	size_t rem;
	int err = 0, np = 0, nc = 0, ne = 0, no = 0;

	if (top(schema, "objects"))
		blobmsg_for_each_attr(cur, top(schema, "objects"), rem) {
			char p[MAX_DM_PATH];
			size_t n;

			snprintf(p, sizeof(p), "%s", str(field(cur, "path")));
			n = strlen(p);
			if (n && p[n - 1] == '.')
				p[n - 1] = '\0';
			err |= USP_REGISTER_GroupedObject(GROUP, p, false);
			err |= USP_REGISTER_Object_RefreshInstances(p, refresh_cb);
			no++;
		}
	if (top(schema, "params"))
		blobmsg_for_each_attr(cur, top(schema, "params"), rem) {
			char *p = (char *)str(field(cur, "path"));
			struct blob_attr *w = field(cur, "writable");
			unsigned type = usp_type(str(field(cur, "type")));

			if (core_owned(p))
				continue;
			if (w && blobmsg_get_u8(w))
				err |= USP_REGISTER_GroupedVendorParam_ReadWrite(GROUP, p, type);
			else
				err |= USP_REGISTER_GroupedVendorParam_ReadOnly(GROUP, p, type);
			np++;
		}
	if (top(schema, "commands"))
		blobmsg_for_each_attr(cur, top(schema, "commands"), rem) {
			char *p = (char *)str(field(cur, "path")), *in[16], *out[16];
			int ni = names(field(cur, "in"), in, 16), nout = names(field(cur, "out"), out, 16);

			struct blob_attr *as = field(cur, "async");

			if (as && blobmsg_get_u8(as))
				err |= USP_REGISTER_AsyncOperation(p, aop_start, NULL);
			else
				err |= USP_REGISTER_SyncOperation(p, oper_cb);
			err |= USP_REGISTER_OperationArguments(p, in, ni, out, nout);
			nc++;
		}
	if (top(schema, "events"))
		blobmsg_for_each_attr(cur, top(schema, "events"), rem) {
			char *p = (char *)str(field(cur, "path")), *args[16];
			int na = names(field(cur, "args"), args, 16);

			err |= USP_REGISTER_Event(p);
			err |= USP_REGISTER_EventArguments(p, args, na);
			ne++;
		}
	err |= USP_REGISTER_GroupVendorHooks(GROUP, get_cb, set_cb, NULL, NULL);
	{
		static vendor_hook_cb_t hooks;

		memset(&hooks, 0, sizeof(hooks));
		hooks.get_active_software_version_cb = sw_version_cb;
		err |= USP_REGISTER_CoreVendorHooks(&hooks);
	}
	USP_LOG_Info("aigw-usp: AIGW OS data model %s: %d objects, %d parameters, %d commands, %d events",
		     str(top(schema, "version")), no, np, nc, ne);
	return err ? USP_ERR_INTERNAL_ERROR : USP_ERR_OK;
}

int VENDOR_Init(void)
{
	struct blob_attr *schema = NULL;
	int i, rc;

	ubus_path = getenv("AIGW_UBUS");
	if (ubus_path && !*ubus_path)
		ubus_path = NULL;
	/* agd may still be starting */
	for (i = 0; i < 30 && !schema; i++) {
		if ((schema = dm_call("schema", NULL)))
			break;
		sleep(1);
	}
	if (!schema) {
		USP_LOG_Error("aigw-usp: no AIGW OS data model on ubus (is agd running?)");
		return USP_ERR_INTERNAL_ERROR;
	}
	rc = register_schema(schema);
	/* the reply is kept: the registered names point into it, and it is small */
	return rc;
}

/* ---------- events: ag.dm.event -> USP ---------- */

static struct ubus_auto_conn ev_conn;
static struct ubus_event_handler ev_handler;
static pthread_t ev_thread;
static int ev_running;

static void on_event(struct ubus_context *ctx, struct ubus_event_handler *ev, const char *type, struct blob_attr *msg)
{
	struct blob_attr *name = top(msg, "event"), *args = top(msg, "args"), *cur;
	kv_vector_t *kv;
	size_t rem;

	(void)ctx;
	(void)ev;
	(void)type;
	if (!name || !*str(name))
		return;
	kv = USP_ARG_Create();
	if (args)
		blobmsg_for_each_attr(cur, args, rem)
			USP_ARG_Add(kv, (char *)blobmsg_name(cur), (char *)str(cur));
	USP_SIGNAL_DataModelEvent((char *)str(name), kv);      /* kv now belongs to the agent */
}

static void on_connect(struct ubus_context *ctx)
{
	ev_handler.cb = on_event;
	if (ubus_register_event_handler(ctx, &ev_handler, "ag.dm.event"))
		USP_LOG_Error("aigw-usp: cannot follow AIGW OS events");
}

static void *ev_main(void *arg)
{
	(void)arg;
	uloop_init();
	ev_conn.path = ubus_path;
	ev_conn.cb = on_connect;
	ubus_auto_connect(&ev_conn);
	uloop_run();
	uloop_done();
	return NULL;
}

int VENDOR_Start(void)
{
	if (pthread_create(&ev_thread, NULL, ev_main, NULL)) {
		USP_LOG_Error("aigw-usp: cannot start the event thread");
		return USP_ERR_INTERNAL_ERROR;
	}
	ev_running = 1;
	return USP_ERR_OK;
}

int VENDOR_Stop(void)
{
	if (ev_running) {
		uloop_end();
		pthread_join(ev_thread, NULL);
		ev_running = 0;
	}
	if (dm_ctx) {
		ubus_free(dm_ctx);
		dm_ctx = NULL;
	}
	return USP_ERR_OK;
}
