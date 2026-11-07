/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ag-mqtt: AIGW's MQTT connector plug-in, with Home Assistant discovery.
 *
 *   MQTT broker (mosquitto, ...)                     AIGW brain (agd)
 *   <prefix>/sensor/<anything>  ──────────────────►  event iot.mqtt (telemetry as context;
 *                                                     a "mac" or "ip" field ties it to a device)
 *   <prefix>/cmd/<action>       ──────────────────►  request (same guardrails as agctl)
 *   <prefix>/device/<id>/quarantine/set  ─────────►  quarantine / undo
 *   <prefix>/status, alerts, waiting, event/..  ◄───  state and events
 *   <prefix>/device/<id>/presence, quarantine  ◄────  per device
 *   homeassistant/... /config (retained)        ◄────  discovery: a presence tracker and a
 *                                                     "Quarantine" switch per device, alert and
 *                                                     waiting-decision sensors for the gateway
 *
 * Speaks the AIGW plug-in protocol (JSON lines on stdin/stdout) and MQTT 3.1.1
 * (QoS 0) itself, so it needs nothing but libubox. Plain TCP: point it at a
 * broker on the gateway or the home network.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <libubox/blobmsg_json.h>

#define DEV_MAX     256
#define KEEPALIVE   60
#define POLL_DEVS   30
#define RBUF        65536

struct dev {
	int used, announced, home, quarantined;
	uint32_t q_id;
	char mac[18], id[13], name[64], ip[48], type[24], zone[16];
};

static struct {
	char broker[128], user[64], pass[64], prefix[64], disc[64], aigw[16], host[16];
	int port, ha;
	int can_read, can_request;
} c = { "127.0.0.1", "", "", "aigw", "homeassistant", "", "", 1883, 1, 0, 0 };

static struct dev devs[DEV_MAX];
static int sock = -1, connected;
static time_t last_tx, next_try, last_poll;
static uint8_t rbuf[RBUF];
static size_t rlen;
static char *in;
static size_t inlen, incap;
static int next_req = 10;
static volatile sig_atomic_t quit;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ---------- to agd ---------- */

static void to_agd(struct blob_buf *b)
{
	char *s = blobmsg_format_json(b->head, true);

	if (s) {
		printf("%s\n", s);
		fflush(stdout);
		free(s);
	}
}

static void query(const char *what, int id)
{
	printf("{\"query\":\"%s\",\"id\":%d}\n", what, id);
	fflush(stdout);
}

/* ---------- MQTT 3.1.1 ---------- */

static int send_all(const uint8_t *p, size_t n)
{
	while (n) {
		ssize_t w = send(sock, p, n, MSG_NOSIGNAL);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return -1;
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

static int packet(uint8_t type, const uint8_t *body, size_t len)
{
	uint8_t hdr[5];
	size_t n = 1, l = len;

	hdr[0] = type;
	do {
		uint8_t byte = l % 128;

		l /= 128;
		hdr[n++] = byte | (l ? 0x80 : 0);
	} while (l && n < 5);
	if (sock < 0 || send_all(hdr, n) || (len && send_all(body, len))) {
		if (sock >= 0)
			close(sock);
		sock = -1;
		connected = 0;
		return -1;
	}
	last_tx = time(NULL);
	return 0;
}

static size_t put_str(uint8_t *b, const char *s, size_t len)
{
	b[0] = (uint8_t)(len >> 8);
	b[1] = (uint8_t)len;
	memcpy(b + 2, s, len);
	return len + 2;
}

static void publish(const char *topic, const char *payload, int retain)
{
	size_t tl = strlen(topic), pl = strlen(payload), off;
	uint8_t *b;

	if (!connected || !(b = malloc(tl + pl + 2)))
		return;
	off = put_str(b, topic, tl);
	memcpy(b + off, payload, pl);
	packet((uint8_t)(0x30 | (retain ? 1 : 0)), b, off + pl);
	free(b);
}

static void publish_blob(const char *topic, struct blob_buf *b, int retain)
{
	char *s = blobmsg_format_json(b->head, true);

	if (s) {
		publish(topic, s, retain);
		free(s);
	}
}

static void subscribe(void)
{
	char t[4][200];
	uint8_t b[1024];
	size_t off = 0;
	int i;

	snprintf(t[0], sizeof(t[0]), "%s/sensor/#", c.prefix);
	snprintf(t[1], sizeof(t[1]), "%s/cmd/#", c.prefix);
	snprintf(t[2], sizeof(t[2]), "%s/device/+/quarantine/set", c.prefix);
	snprintf(t[3], sizeof(t[3]), "%s/status", c.disc);
	b[off++] = 0;
	b[off++] = 1;                       /* packet id */
	for (i = 0; i < (c.ha ? 4 : 3); i++) {
		off += put_str(b + off, t[i], strlen(t[i]));
		b[off++] = 0;                    /* QoS 0 */
	}
	packet(0x82, b, off);
}

static void announce_all(void);
static void publish_device(struct dev *d);

static int mqtt_connect(void)
{
	struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *res = NULL, *ai;
	char port[8], will[160], id[64], hn[40] = "aigw";
	uint8_t b[600], r[4];
	size_t off = 0;
	struct pollfd pfd;
	int fd = -1, flags = 0x02 | 0x04 | 0x20;   /* clean session, will, will retained */
	struct timeval tv = { 3, 0 };

	snprintf(port, sizeof(port), "%d", c.port);
	if (getaddrinfo(c.broker, port, &hints, &res) != 0)
		return -1;
	for (ai = res; ai && fd < 0; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, 0);
		if (fd < 0)
			continue;
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
			close(fd);
			fd = -1;
		}
	}
	freeaddrinfo(res);
	if (fd < 0)
		return -1;
	sock = fd;
	gethostname(hn, sizeof(hn) - 1);
	snprintf(id, sizeof(id), "aigw-%.18s", hn);
	snprintf(will, sizeof(will), "%s/status", c.prefix);
	if (c.user[0])
		flags |= 0x80;
	if (c.pass[0])
		flags |= 0x40;
	off += put_str(b + off, "MQTT", 4);
	b[off++] = 4;
	b[off++] = (uint8_t)flags;
	b[off++] = 0;
	b[off++] = KEEPALIVE;
	off += put_str(b + off, id, strlen(id));
	off += put_str(b + off, will, strlen(will));
	off += put_str(b + off, "offline", 7);
	if (c.user[0])
		off += put_str(b + off, c.user, strlen(c.user));
	if (c.pass[0])
		off += put_str(b + off, c.pass, strlen(c.pass));
	if (packet(0x10, b, off))
		return -1;
	pfd.fd = sock;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 3000) != 1 || recv(sock, r, 4, MSG_WAITALL) != 4 || r[0] != 0x20 || r[3] != 0) {
		logf_("broker %s:%d refused the connection%s", c.broker, c.port,
		      r[3] == 4 || r[3] == 5 ? " (user name or password)" : "");
		close(sock);
		sock = -1;
		return -1;
	}
	connected = 1;
	rlen = 0;
	subscribe();
	publish(will, "online", 1);
	logf_("connected to MQTT broker %s:%d (topics under %s/)", c.broker, c.port, c.prefix);
	return 0;
}

/* ---------- Home Assistant discovery ---------- */

static void ha_device(struct blob_buf *b, const struct dev *d)
{
	void *t = blobmsg_open_table(b, "device"), *a, *pair;

	if (d) {
		char ident[32];

		a = blobmsg_open_array(b, "identifiers");
		snprintf(ident, sizeof(ident), "aigw_%s", d->id);
		blobmsg_add_string(b, NULL, ident);
		blobmsg_close_array(b, a);
		blobmsg_add_string(b, "name", d->name[0] ? d->name : d->mac);
		if (d->type[0])
			blobmsg_add_string(b, "model", d->type);
		a = blobmsg_open_array(b, "connections");
		pair = blobmsg_open_array(b, NULL);
		blobmsg_add_string(b, NULL, "mac");
		blobmsg_add_string(b, NULL, d->mac);
		blobmsg_close_array(b, pair);
		blobmsg_close_array(b, a);
		blobmsg_add_string(b, "via_device", "aigw_gateway");
	} else {
		a = blobmsg_open_array(b, "identifiers");
		blobmsg_add_string(b, NULL, "aigw_gateway");
		blobmsg_close_array(b, a);
		blobmsg_add_string(b, "name", "AIGW");
		blobmsg_add_string(b, "manufacturer", "AIGW");
		blobmsg_add_string(b, "model", c.host[0] ? c.host : "gateway");
		blobmsg_add_string(b, "sw_version", c.aigw);
	}
	blobmsg_close_table(b, t);
}

static void announce_gateway(void)
{
	static const char *const what[2][3] = {
		{ "alerts", "AIGW alerts", "mdi:alert" }, { "waiting", "AIGW decisions waiting", "mdi:gavel" },
	};
	struct blob_buf b = { 0 };
	char topic[200], st[200], av[200], uid[64];
	int i;

	if (!c.ha || !connected)
		return;
	snprintf(av, sizeof(av), "%s/status", c.prefix);
	for (i = 0; i < 2; i++) {
		blob_buf_init(&b, 0);
		snprintf(uid, sizeof(uid), "aigw_%s", what[i][0]);
		snprintf(st, sizeof(st), "%s/%s", c.prefix, what[i][0]);
		blobmsg_add_string(&b, "name", what[i][1]);
		blobmsg_add_string(&b, "unique_id", uid);
		blobmsg_add_string(&b, "state_topic", st);
		blobmsg_add_string(&b, "availability_topic", av);
		blobmsg_add_string(&b, "icon", what[i][2]);
		ha_device(&b, NULL);
		snprintf(topic, sizeof(topic), "%s/sensor/%s/config", c.disc, uid);
		publish_blob(topic, &b, 1);
	}
	blob_buf_free(&b);
}

static void announce(struct dev *d)
{
	struct blob_buf b = { 0 };
	char topic[200], t1[200], t2[200], av[200], uid[64];

	if (!c.ha || !connected)
		return;
	snprintf(av, sizeof(av), "%s/status", c.prefix);

	blob_buf_init(&b, 0);
	snprintf(uid, sizeof(uid), "aigw_%s_presence", d->id);
	snprintf(t1, sizeof(t1), "%s/device/%s/presence", c.prefix, d->id);
	snprintf(t2, sizeof(t2), "%s/device/%s/attributes", c.prefix, d->id);
	blobmsg_add_string(&b, "name", d->name[0] ? d->name : d->mac);
	blobmsg_add_string(&b, "unique_id", uid);
	blobmsg_add_string(&b, "state_topic", t1);
	blobmsg_add_string(&b, "json_attributes_topic", t2);
	blobmsg_add_string(&b, "payload_home", "home");
	blobmsg_add_string(&b, "payload_not_home", "not_home");
	blobmsg_add_string(&b, "source_type", "router");
	blobmsg_add_string(&b, "availability_topic", av);
	ha_device(&b, d);
	snprintf(topic, sizeof(topic), "%s/device_tracker/aigw_%s/config", c.disc, d->id);
	publish_blob(topic, &b, 1);

	if (c.can_request) {
		blob_buf_init(&b, 0);
		snprintf(uid, sizeof(uid), "aigw_%s_quarantine", d->id);
		snprintf(t1, sizeof(t1), "%s/device/%s/quarantine", c.prefix, d->id);
		snprintf(t2, sizeof(t2), "%s/device/%s/quarantine/set", c.prefix, d->id);
		blobmsg_add_string(&b, "name", "Quarantine");
		blobmsg_add_string(&b, "unique_id", uid);
		blobmsg_add_string(&b, "state_topic", t1);
		blobmsg_add_string(&b, "command_topic", t2);
		blobmsg_add_string(&b, "payload_on", "ON");
		blobmsg_add_string(&b, "payload_off", "OFF");
		blobmsg_add_string(&b, "icon", "mdi:shield-lock");
		blobmsg_add_string(&b, "availability_topic", av);
		ha_device(&b, d);
		snprintf(topic, sizeof(topic), "%s/switch/%s/config", c.disc, uid);
		publish_blob(topic, &b, 1);
	}
	blob_buf_free(&b);
	d->announced = 1;
}

static void announce_all(void)
{
	int i;

	announce_gateway();
	for (i = 0; i < DEV_MAX; i++)
		if (devs[i].used) {
			announce(&devs[i]);
			publish_device(&devs[i]);
		}
}

/* ---------- devices ---------- */

static struct dev *dev_get(const char *mac, int create)
{
	char id[13];
	int i, k = 0;

	if (!mac || strlen(mac) != 17)
		return NULL;
	for (i = 0; mac[i] && k < 12; i++)
		if (isxdigit((unsigned char)mac[i]))
			id[k++] = (char)tolower((unsigned char)mac[i]);
	id[k] = '\0';
	if (k != 12)
		return NULL;
	for (i = 0; i < DEV_MAX; i++)
		if (devs[i].used && !strcmp(devs[i].id, id))
			return &devs[i];
	if (!create)
		return NULL;
	for (i = 0; i < DEV_MAX; i++)
		if (!devs[i].used) {
			memset(&devs[i], 0, sizeof(devs[i]));
			devs[i].used = 1;
			snprintf(devs[i].id, sizeof(devs[i].id), "%s", id);
			for (k = 0; k < 17; k++)
				devs[i].mac[k] = (char)tolower((unsigned char)mac[k]);
			devs[i].mac[17] = '\0';
			return &devs[i];
		}
	return NULL;
}

static void publish_device(struct dev *d)
{
	struct blob_buf b = { 0 };
	char topic[200];

	if (!connected)
		return;
	snprintf(topic, sizeof(topic), "%s/device/%s/presence", c.prefix, d->id);
	publish(topic, d->home ? "home" : "not_home", 1);
	snprintf(topic, sizeof(topic), "%s/device/%s/quarantine", c.prefix, d->id);
	publish(topic, d->quarantined ? "ON" : "OFF", 1);
	blob_buf_init(&b, 0);
	blobmsg_add_string(&b, "mac", d->mac);
	blobmsg_add_string(&b, "ip", d->ip);
	blobmsg_add_string(&b, "type", d->type);
	blobmsg_add_string(&b, "zone", d->zone);
	snprintf(topic, sizeof(topic), "%s/device/%s/attributes", c.prefix, d->id);
	publish_blob(topic, &b, 1);
	blob_buf_free(&b);
}

static struct blob_attr *get(struct blob_attr *tbl, const char *name)
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

static const char *gs(struct blob_attr *t, const char *n)
{
	struct blob_attr *a = get(t, n);

	return a && blobmsg_type(a) == BLOBMSG_TYPE_STRING ? blobmsg_get_string(a) : "";
}

static long gn(struct blob_attr *t, const char *n)
{
	struct blob_attr *a = get(t, n);

	if (!a)
		return 0;
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_INT64: return (long)blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return (long)(int32_t)blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT8: return blobmsg_get_u8(a);
	case BLOBMSG_TYPE_DOUBLE: return (long)blobmsg_get_double(a);
	case BLOBMSG_TYPE_STRING: return atol(blobmsg_get_string(a));
	default: return 0;
	}
}

static void on_devices(struct blob_attr *data)
{
	struct blob_attr *arr = get(data, "devices"), *e;
	time_t now = time(NULL);
	size_t rem;

	if (!arr)
		return;
	blobmsg_for_each_attr(e, arr, rem) {
		struct blob_attr *w = get(e, "wifi");
		struct dev *d = dev_get(gs(e, "mac"), 1);
		int home;

		if (!d)
			continue;
		home = (w && gn(w, "connected")) || now - gn(e, "last_seen") < 300;
		snprintf(d->ip, sizeof(d->ip), "%s", gs(e, "ip"));
		snprintf(d->type, sizeof(d->type), "%s", gs(e, "type"));
		snprintf(d->zone, sizeof(d->zone), "%s", gs(e, "zone"));
		if (strcmp(d->name, gs(e, "hostname"))) {
			snprintf(d->name, sizeof(d->name), "%s", gs(e, "hostname"));
			d->announced = 0;
		}
		if (!d->announced)
			announce(d);
		d->home = home;
		publish_device(d);
	}
}

static void on_status(struct blob_attr *data)
{
	char topic[200], v[32];

	snprintf(topic, sizeof(topic), "%s/alerts", c.prefix);
	snprintf(v, sizeof(v), "%ld", gn(data, "alerts"));
	publish(topic, v, 1);
	snprintf(topic, sizeof(topic), "%s/waiting", c.prefix);
	snprintf(v, sizeof(v), "%ld", gn(data, "waiting"));
	publish(topic, v, 1);
}

static void on_event(const char *type, struct blob_attr *body)
{
	struct blob_buf b = { 0 };
	char topic[200];

	if (!strcmp(type, "ag.wifi")) {
		struct dev *d = dev_get(gs(body, "mac"), 0);

		if (!d) {
			if (c.can_read)
				query("devices", 1);
			return;
		}
		d->home = !strcmp(gs(body, "event"), "connected") || !strcmp(gs(body, "event"), "joined-with-key") ||
			  (d->home && strcmp(gs(body, "event"), "disconnected"));
		publish_device(d);
		return;
	}
	if (!strcmp(type, "ag.decision")) {
		const char *st = gs(body, "state");
		struct dev *d = !strcmp(gs(body, "action"), "quarantine") ? dev_get(gs(body, "target"), 0) : NULL;

		if (d) {
			if (!strcmp(st, "active")) {
				d->quarantined = 1;
				d->q_id = (uint32_t)gn(body, "id");
			} else if (strcmp(st, "proposed") && (uint32_t)gn(body, "id") == d->q_id) {
				d->quarantined = 0;
			}
			publish_device(d);
		}
	}
	if ((!strcmp(type, "ag.alert") || !strcmp(type, "ag.decision")) && body) {
		struct blob_attr *cur;
		size_t rem;

		/* the body is a named table: copy its fields to format it as a JSON object */
		blob_buf_init(&b, 0);
		blobmsg_for_each_attr(cur, body, rem)
			blobmsg_add_blob(&b, cur);
		snprintf(topic, sizeof(topic), "%s/event/%s", c.prefix, type + 3);
		publish_blob(topic, &b, 0);
		blob_buf_free(&b);
		query("status", 2);
	}
}

static void on_reply(int id, struct blob_attr *data, const char *error)
{
	char topic[200];

	if (id == 1 && data)
		on_devices(data);
	else if (id == 2 && data)
		on_status(data);
	else if (id >= 10) {             /* a command from MQTT: say how it went */
		struct blob_buf b = { 0 };

		blob_buf_init(&b, 0);
		blobmsg_add_u8(&b, "ok", data && gn(data, "ok"));
		blobmsg_add_u32(&b, "decision", data ? (uint32_t)gn(data, "decision") : 0);
		blobmsg_add_string(&b, "error", error ? error : "");
		snprintf(topic, sizeof(topic), "%s/result", c.prefix);
		publish_blob(topic, &b, 0);
		blob_buf_free(&b);
	}
}

static void on_hello(struct blob_attr *h)
{
	struct blob_attr *conf = get(h, "config"), *cur;
	size_t rem;

	snprintf(c.aigw, sizeof(c.aigw), "%s", gs(h, "aigw"));
	snprintf(c.host, sizeof(c.host), "%s", gs(h, "host"));
	if (conf)
		blobmsg_for_each_attr(cur, conf, rem) {
			const char *k = blobmsg_name(cur), *v;

			if (blobmsg_type(cur) != BLOBMSG_TYPE_STRING)
				continue;
			v = blobmsg_get_string(cur);
			if (!strcmp(k, "broker"))
				snprintf(c.broker, sizeof(c.broker), "%s", v);
			else if (!strcmp(k, "port") && atoi(v) > 0)
				c.port = atoi(v);
			else if (!strcmp(k, "username"))
				snprintf(c.user, sizeof(c.user), "%s", v);
			else if (!strcmp(k, "password"))
				snprintf(c.pass, sizeof(c.pass), "%s", v);
			else if (!strcmp(k, "prefix") && *v)
				snprintf(c.prefix, sizeof(c.prefix), "%s", v);
			else if (!strcmp(k, "discovery_prefix") && *v)
				snprintf(c.disc, sizeof(c.disc), "%s", v);
			else if (!strcmp(k, "homeassistant"))
				c.ha = atoi(v) != 0;
		}
	if ((cur = get(h, "granted")))
		blobmsg_for_each_attr(conf, cur, rem) {
			if (!strcmp(blobmsg_get_string(conf), "devices.read"))
				c.can_read = 1;
			if (!strcmp(blobmsg_get_string(conf), "decision.request"))
				c.can_request = 1;
		}
	if (!c.can_read)
		logf_("devices.read not granted: no per-device topics or Home Assistant devices");
	if (!c.can_request)
		logf_("decision.request not granted: commands and Quarantine switches are off");
}

static void from_agd(char *line)
{
	struct blob_buf b = { 0 };
	struct blob_attr *cur;
	size_t rem;

	blob_buf_init(&b, 0);
	if (!blobmsg_add_json_from_string(&b, line)) {
		blob_buf_free(&b);
		return;
	}
	blob_for_each_attr(cur, b.head, rem) {
		const char *k = blobmsg_name(cur);

		if (!strcmp(k, "hello"))
			on_hello(cur);
		else if (!strcmp(k, "event"))
			on_event(gs(cur, "type"), get(cur, "body"));
		else if (!strcmp(k, "reply"))
			on_reply((int)gn(b.head, "reply"), get(b.head, "data"), gs(b.head, "error"));
		else
			continue;
		break;
	}
	blob_buf_free(&b);
}

/* ---------- from MQTT ---------- */

static void request(const char *action, const char *target, long ttl, const char *reason, long undo)
{
	struct blob_buf b = { 0 };
	void *t;

	if (!c.can_request) {
		on_reply(next_req, NULL, "decision.request not granted (agctl plugin allow mqtt)");
		return;
	}
	blob_buf_init(&b, 0);
	t = blobmsg_open_table(&b, "request");
	if (undo) {
		blobmsg_add_u32(&b, "undo", (uint32_t)undo);
	} else {
		blobmsg_add_string(&b, "action", action);
		blobmsg_add_string(&b, "target", target);
		if (ttl > 0)
			blobmsg_add_u32(&b, "ttl", (uint32_t)ttl);
		blobmsg_add_string(&b, "reason", reason);
	}
	blobmsg_add_u32(&b, "id", (uint32_t)next_req++);
	blobmsg_close_table(&b, t);
	to_agd(&b);
	blob_buf_free(&b);
}

static void on_message(const char *topic, const char *payload, size_t plen)
{
	size_t pl = strlen(c.prefix);
	char *p = strndup(payload, plen), rest[200];
	struct blob_buf b = { 0 };

	if (!p)
		return;
	if (c.ha && !strncmp(topic, c.disc, strlen(c.disc)) && !strcmp(topic + strlen(c.disc), "/status")) {
		if (!strcmp(p, "online"))
			announce_all();          /* Home Assistant restarted */
		goto out;
	}
	if (strncmp(topic, c.prefix, pl) || topic[pl] != '/')
		goto out;
	snprintf(rest, sizeof(rest), "%s", topic + pl + 1);

	if (!strncmp(rest, "sensor/", 7)) {
		/* telemetry as context: {"emit": "iot.mqtt", "body": {topic, ...payload fields}} */
		struct blob_buf v = { 0 };
		void *t;

		blob_buf_init(&b, 0);
		blobmsg_add_string(&b, "emit", "iot.mqtt");
		t = blobmsg_open_table(&b, "body");
		blobmsg_add_string(&b, "topic", rest + 7);
		blob_buf_init(&v, 0);
		if (p[0] == '{' && blobmsg_add_json_from_string(&v, p)) {
			struct blob_attr *cur;
			size_t rem;

			blob_for_each_attr(cur, v.head, rem)
				if (strcmp(blobmsg_name(cur), "topic"))
					blobmsg_add_blob(&b, cur);
		} else {
			blobmsg_add_string(&b, "value", p);
		}
		blob_buf_free(&v);
		blobmsg_close_table(&b, t);
		to_agd(&b);
		blob_buf_free(&b);
	} else if (!strncmp(rest, "cmd/", 4) && rest[4]) {
		/* payload: a target ("10.0.0.7", "tv") or {"target":..., "ttl":..., "reason":...}; undo: the id */
		struct blob_buf v = { 0 };
		const char *action = rest + 4;

		blob_buf_init(&v, 0);
		if (p[0] == '{' && blobmsg_add_json_from_string(&v, p))
			request(action, gs(v.head, "target"), gn(v.head, "ttl"),
				*gs(v.head, "reason") ? gs(v.head, "reason") : "asked for over MQTT",
				!strcmp(action, "undo") ? gn(v.head, "id") : 0);
		else
			request(action, p, 0, "asked for over MQTT", !strcmp(action, "undo") ? atol(p) : 0);
		blob_buf_free(&v);
	} else if (!strncmp(rest, "device/", 7) && strstr(rest, "/quarantine/set")) {
		char id[16];
		struct dev *d = NULL;
		int i;

		snprintf(id, sizeof(id), "%.*s", (int)strcspn(rest + 7, "/"), rest + 7);
		for (i = 0; i < DEV_MAX && !d; i++)
			if (devs[i].used && !strcmp(devs[i].id, id))
				d = &devs[i];
		if (d && !strcasecmp(p, "ON"))
			request("quarantine", d->mac, 0, "switched on in Home Assistant", 0);
		else if (d && !strcasecmp(p, "OFF") && d->quarantined && d->q_id)
			request(NULL, NULL, 0, NULL, (long)d->q_id);
	}
out:
	free(p);
}

static void mqtt_read(void)
{
	ssize_t n = recv(sock, rbuf + rlen, sizeof(rbuf) - rlen, 0);
	size_t off = 0;

	if (n <= 0) {
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			return;
		logf_("lost the MQTT broker");
		close(sock);
		sock = -1;
		connected = 0;
		return;
	}
	rlen += (size_t)n;
	for (;;) {
		size_t len = 0, mul = 1, h = 1;
		uint8_t type;

		if (rlen - off < 2)
			break;
		while (h < 5 && off + h < rlen) {
			len += (rbuf[off + h] & 127u) * mul;
			mul *= 128;
			if (!(rbuf[off + h++] & 128))
				break;
		}
		if (off + h > rlen || (rbuf[off + h - 1] & 128) || off + h + len > rlen)
			break;
		type = rbuf[off];
		if ((type & 0xf0) == 0x30 && len >= 2) {
			const uint8_t *v = rbuf + off + h;
			size_t tl = ((size_t)v[0] << 8) | v[1], skip = 2 + tl + (((type >> 1) & 3) ? 2 : 0);

			if (skip <= len) {
				char *topic = strndup((const char *)v + 2, tl);

				if (topic) {
					on_message(topic, (const char *)v + skip, len - skip);
					free(topic);
				}
			}
		}
		off += h + len;
	}
	memmove(rbuf, rbuf + off, rlen - off);
	rlen -= off;
	if (rlen == sizeof(rbuf)) {      /* one message bigger than we take: start over */
		close(sock);
		sock = -1;
		connected = 0;
	}
}

/* ---------- main loop ---------- */

static void on_term(int sig)
{
	quit = 1;
}

static int read_agd(void)
{
	char *nl;
	ssize_t n;

	if (inlen + 65536 > incap) {
		size_t cap = incap ? incap * 2 : 262144;
		char *nb = cap <= 8 * 1024 * 1024 ? realloc(in, cap) : NULL;

		if (!nb) {
			inlen = 0;           /* an absurd line: drop it */
		} else {
			in = nb;
			incap = cap;
		}
	}
	n = read(0, in + inlen, incap - inlen - 1);
	if (n == 0)
		return -1;               /* agd went away */
	if (n < 0)
		return errno == EINTR || errno == EAGAIN ? 0 : -1;
	inlen += (size_t)n;
	in[inlen] = '\0';
	while ((nl = memchr(in, '\n', inlen))) {
		*nl = '\0';
		from_agd(in);
		inlen -= (size_t)(nl + 1 - in);
		memmove(in, nl + 1, inlen);
	}
	return 0;
}

int main(void)
{
	struct sigaction sa = { 0 };
	unsigned backoff = 2;

	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	while (!quit) {
		struct pollfd pfd[2] = { { .fd = 0, .events = POLLIN }, { .fd = sock, .events = POLLIN } };
		time_t now = time(NULL);

		if (!connected && now >= next_try && c.aigw[0]) {
			if (mqtt_connect() == 0) {
				backoff = 2;
				announce_gateway();
				if (c.can_read)
					query("devices", 1);
				query("status", 2);
				last_poll = now;
			} else {
				if (backoff == 2)
					logf_("no MQTT broker at %s:%d yet; retrying", c.broker, c.port);
				next_try = now + backoff;
				backoff = backoff < 60 ? backoff * 2 : 60;
			}
		}
		if (connected && now - last_tx >= KEEPALIVE / 2)
			packet(0xc0, NULL, 0);       /* PINGREQ */
		if (connected && now - last_poll >= POLL_DEVS) {
			last_poll = now;
			if (c.can_read)
				query("devices", 1);
			query("status", 2);
		}
		pfd[1].fd = connected ? sock : -1;
		if (poll(pfd, 2, 1000) < 0)
			continue;
		if (pfd[0].revents & (POLLIN | POLLHUP) && read_agd() < 0)
			break;
		if (connected && pfd[1].revents & (POLLIN | POLLHUP | POLLERR))
			mqtt_read();
	}
	if (connected) {
		char will[160];

		snprintf(will, sizeof(will), "%s/status", c.prefix);
		publish(will, "offline", 1);
		packet(0xe0, NULL, 0);       /* DISCONNECT */
	}
	return 0;
}
