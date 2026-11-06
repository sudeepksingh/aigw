/* SPDX-License-Identifier: Apache-2.0 */
/* Device table: identity (IP, MAC, DHCP), per-device DNS/flow/Wi-Fi state,
 * and saving it across restarts. Fixed size; the least recently seen device
 * makes room for a new one. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <ifaddrs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libubox/blobmsg_json.h>

#include "agd.h"

struct device devs[DEV_MAX];

/* ---------- the gateway's own addresses ---------- */

#define LOCAL_MAX 32
static char local_ips[LOCAL_MAX][IP_LEN];
static int n_local;

void local_addrs_refresh(void)
{
	struct ifaddrs *ifa, *i;
	int n = 0;

	if (getifaddrs(&ifa) < 0)
		return;
	for (i = ifa; i && n < LOCAL_MAX; i = i->ifa_next) {
		const void *a;

		if (!i->ifa_addr)
			continue;
		if (i->ifa_addr->sa_family == AF_INET)
			a = &((struct sockaddr_in *)i->ifa_addr)->sin_addr;
		else if (i->ifa_addr->sa_family == AF_INET6)
			a = &((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr;
		else
			continue;
		if (inet_ntop(i->ifa_addr->sa_family, a, local_ips[n], sizeof(local_ips[n])))
			n++;
	}
	n_local = n;
	freeifaddrs(ifa);
}

int is_local(const char *ip)
{
	int i;

	for (i = 0; i < n_local; i++)
		if (!strcmp(local_ips[i], ip))
			return 1;
	return 0;
}

/* Private, link-local, CGNAT, multicast: never reached via public DNS names */
int is_private(const char *ip)
{
	struct in_addr a4;
	struct in6_addr a6;

	if (inet_pton(AF_INET, ip, &a4) == 1) {
		uint32_t h = ntohl(a4.s_addr);

		return (h >> 24) == 10 || (h >> 24) == 127 || (h >> 20) == 0xac1 ||
		       (h >> 16) == 0xc0a8 || (h >> 16) == 0xa9fe ||
		       (h & 0xffc00000u) == 0x64400000u || (h >> 28) == 0xe || h == 0xffffffffu;
	}
	if (inet_pton(AF_INET6, ip, &a6) == 1)
		return (a6.s6_addr[0] & 0xfe) == 0xfc || a6.s6_addr[0] == 0xff ||
		       (a6.s6_addr[0] == 0xfe && (a6.s6_addr[1] & 0xc0) == 0x80) ||
		       IN6_IS_ADDR_LOOPBACK(&a6);
	return 1;
}

/* ---------- top-k counters ---------- */

/* Space-saving top-k: exact for frequent items, fixed memory. */
void top_add(struct top_item *t, int n, const char *name, uint64_t inc)
{
	struct top_item *min = &t[0];
	int i;

	for (i = 0; i < n; i++) {
		if (t[i].n && !strncmp(t[i].name, name, NAME_LEN - 1)) {
			t[i].n += inc;
			return;
		}
		if (t[i].n < min->n)
			min = &t[i];
	}
	snprintf(min->name, sizeof(min->name), "%s", name);
	min->n += inc;
}

static int top_cmp(const void *a, const void *b)
{
	const struct top_item *x = a, *y = b;

	return (y->n > x->n) - (y->n < x->n);
}

void top_sorted(const struct top_item *t, int n, struct top_item *out)
{
	memcpy(out, t, (size_t)n * sizeof(*t));
	qsort(out, (size_t)n, sizeof(*out), top_cmp);
}

/* ---------- lookup ---------- */

static struct device *dev_alloc(void)
{
	struct device *oldest = NULL;
	int i;

	for (i = 0; i < DEV_MAX; i++) {
		if (!devs[i].used) {
			oldest = &devs[i];
			break;
		}
		if (!oldest || devs[i].last_seen < oldest->last_seen)
			oldest = &devs[i];
	}
	memset(oldest, 0, sizeof(*oldest));
	oldest->used = 1;
	oldest->first_seen = oldest->last_seen = ag_wall_s();
	return oldest;
}

struct device *dev_find_ip(const char *ip, int create)
{
	struct device *d;
	int i;

	if (!ip || !*ip)
		return NULL;
	for (i = 0; i < DEV_MAX; i++)
		if (devs[i].used && !strcmp(devs[i].ip, ip))
			return &devs[i];
	if (!create)
		return NULL;
	d = dev_alloc();
	snprintf(d->ip, sizeof(d->ip), "%s", ip);
	return d;
}

struct device *dev_find_mac(const char *mac, int create)
{
	struct device *d;
	int i;

	if (!mac || !*mac)
		return NULL;
	for (i = 0; i < DEV_MAX; i++)
		if (devs[i].used && !strcasecmp(devs[i].mac, mac))
			return &devs[i];
	if (!create)
		return NULL;
	d = dev_alloc();
	snprintf(d->mac, sizeof(d->mac), "%.17s", mac);
	return d;
}

/* Fold everything known about `from` into `into` (same device, new identity). */
static void dev_merge(struct device *into, struct device *from)
{
	int i;

	into->queries += from->queries;
	into->flows += from->flows;
	into->flows_no_dns += from->flows_no_dns;
	into->bytes_up += from->bytes_up;
	into->bytes_down += from->bytes_down;
	for (i = 0; i < DEV_DOM_MAX; i++)
		if (from->doms[i].n)
			top_add(into->doms, DEV_DOM_MAX, from->doms[i].name, from->doms[i].n);
	for (i = 0; i < DEV_DST_MAX; i++)
		if (from->dsts[i].n)
			top_add(into->dsts, DEV_DST_MAX, from->dsts[i].name, from->dsts[i].n);
	if (from->first_seen && from->first_seen < into->first_seen)
		into->first_seen = from->first_seen;
	if (from->last_seen > into->last_seen)
		into->last_seen = from->last_seen;
	if (!into->hostname[0])
		memcpy(into->hostname, from->hostname, sizeof(into->hostname));
	from->used = 0;
}

/* The MAC is the stronger identity: a device that changes IP keeps its history,
 * and an IP handed to a different MAC starts a new device. */
struct device *dev_learn(const char *ip, const char *mac, const char *hostname)
{
	struct device *d = dev_find_mac(mac, 0), *o;

	if (d) {
		if (ip && *ip && strcmp(d->ip, ip)) {
			o = dev_find_ip(ip, 0);
			if (o && o != d) {
				if (!o->mac[0])
					dev_merge(d, o);  /* traffic seen before the MAC was known */
				else
					o->ip[0] = '\0';  /* address moved to this device */
			}
			snprintf(d->ip, sizeof(d->ip), "%s", ip);
		}
	} else {
		d = dev_find_ip(ip, 1);
		if (!d)
			d = dev_find_mac(mac, 1);
		if (!d)
			return NULL;
		if (mac && *mac && d->mac[0] && strcasecmp(d->mac, mac)) {
			/* same IP, different device */
			char keep[IP_LEN];

			snprintf(keep, sizeof(keep), "%s", d->ip);
			d->ip[0] = '\0';
			d = dev_alloc();
			snprintf(d->ip, sizeof(d->ip), "%s", keep);
		}
		if (mac && *mac)
			snprintf(d->mac, sizeof(d->mac), "%.17s", mac);
	}
	if (hostname && *hostname && strcmp(hostname, "*"))
		snprintf(d->hostname, sizeof(d->hostname), "%.63s", hostname);
	dev_guess_type(d);
	return d;
}

int dev_count(void)
{
	int i, n = 0;

	for (i = 0; i < DEV_MAX; i++)
		n += devs[i].used;
	return n;
}

/* ---------- device type (rules for now; a trained model replaces this in P2) ---------- */

static int has(const char *s, const char *needle)
{
	return s && *s && strcasestr(s, needle) != NULL;
}

void dev_guess_type(struct device *d)
{
	static const struct { const char *needle; const char *type; } host_rules[] = {
		{ "iphone", "iPhone" }, { "ipad", "iPad" }, { "macbook", "Mac" }, { "imac", "Mac" },
		{ "watch", "watch" }, { "android", "Android" }, { "galaxy", "Android" },
		{ "oneplus", "Android" }, { "pixel", "Android" }, { "redmi", "Android" },
		{ "xiaomi", "Android" }, { "oppo", "Android" }, { "vivo", "Android" },
		{ "realme", "Android" }, { "moto", "Android" }, { "desktop-", "Windows" },
		{ "laptop-", "Windows" }, { "chromecast", "media player" }, { "roku", "media player" },
		{ "firetv", "media player" }, { "tv", "TV" }, { "printer", "printer" },
		{ "epson", "printer" }, { "brother", "printer" }, { "esp_", "IoT" }, { "esp32", "IoT" },
		{ "esp8266", "IoT" }, { "tasmota", "IoT" }, { "shelly", "IoT" }, { "tuya", "IoT" },
		{ "camera", "camera" }, { "cam", "camera" }, { "echo", "smart speaker" },
		{ "alexa", "smart speaker" }, { "google-home", "smart speaker" }, { "nest", "smart home" },
	};
	size_t i;

	if (has(d->vendor_class, "android-dhcp")) {
		snprintf(d->type, sizeof(d->type), "Android");
		return;
	}
	if (has(d->vendor_class, "MSFT")) {
		snprintf(d->type, sizeof(d->type), "Windows");
		return;
	}
	if (has(d->vendor_class, "dhcpcd") || has(d->vendor_class, "udhcp")) {
		snprintf(d->type, sizeof(d->type), "Linux");
		return;
	}
	for (i = 0; i < sizeof(host_rules) / sizeof(host_rules[0]); i++) {
		if (has(d->hostname, host_rules[i].needle)) {
			snprintf(d->type, sizeof(d->type), "%s", host_rules[i].type);
			return;
		}
	}
}

/* ---------- host data: DHCP leases, ARP ---------- */

/* dnsmasq lease lines: "<expiry> <mac> <ip> <hostname|*> <client-id|*>" */
void dev_poll_leases(void)
{
	FILE *f = fopen(cfg.leases, "r");
	char line[512];

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char mac[32], ip[64], host[128];
		long long exp;

		if (sscanf(line, "%lld %31s %63s %127s", &exp, mac, ip, host) != 4)
			continue;
		if (!is_local(ip))
			dev_learn(ip, mac, host);
	}
	fclose(f);
}

/* /proc/net/arp fills in MACs for devices with static addresses */
void dev_poll_arp(void)
{
	FILE *f = fopen("/proc/net/arp", "r");
	char line[256];

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char ip[64], hw[32], mac[32], mask[32], dev[32];
		unsigned flags;
		struct device *d;

		if (sscanf(line, "%63s %31s %x %31s %31s %31s", ip, hw, &flags, mac, mask, dev) != 6)
			continue;
		if (!(flags & 0x2))
			continue;
		d = dev_find_ip(ip, 0);
		if (d && !d->mac[0])
			dev_learn(ip, mac, NULL);
	}
	fclose(f);
}

/* ---------- ubus / JSON representation ---------- */

static void top_blob(struct blob_buf *b, const char *name, const char *count_name,
		     const struct top_item *t, int n, int limit)
{
	struct top_item sorted[DEV_DOM_MAX > DEV_DST_MAX ? DEV_DOM_MAX : DEV_DST_MAX];
	void *arr, *tt;
	int k;

	top_sorted(t, n, sorted);
	arr = blobmsg_open_array(b, name);
	for (k = 0; k < limit && k < n && sorted[k].n; k++) {
		tt = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "name", sorted[k].name);
		blobmsg_add_u64(b, count_name, sorted[k].n);
		blobmsg_close_table(b, tt);
	}
	blobmsg_close_array(b, arr);
}

void dev_blob(struct blob_buf *b, const struct device *d)
{
	void *w;

	blobmsg_add_string(b, "ip", d->ip);
	blobmsg_add_string(b, "mac", d->mac);
	blobmsg_add_string(b, "hostname", d->hostname);
	blobmsg_add_string(b, "type", d->type);
	blobmsg_add_string(b, "vendor_class", d->vendor_class);
	blobmsg_add_string(b, "dhcp_options", d->dhcp_options);
	blobmsg_add_u64(b, "first_seen", (uint64_t)d->first_seen);
	blobmsg_add_u64(b, "last_seen", (uint64_t)d->last_seen);
	blobmsg_add_u32(b, "queries", d->queries);
	blobmsg_add_u32(b, "flows", d->flows);
	blobmsg_add_u32(b, "flows_no_dns", d->flows_no_dns);
	blobmsg_add_u64(b, "bytes_up", d->bytes_up);
	blobmsg_add_u64(b, "bytes_down", d->bytes_down);
	w = blobmsg_open_table(b, "wifi");
	blobmsg_add_u8(b, "connected", d->wifi_connected ? 1 : 0);
	if (d->wifi_connected) {
		blobmsg_add_u32(b, "signal", (uint32_t)d->wifi_signal);
		blobmsg_add_u32(b, "rate_kbps", d->wifi_rate_kbps);
		blobmsg_add_u64(b, "since", (uint64_t)d->wifi_since);
		blobmsg_add_string(b, "iface", d->wifi_iface);
	}
	blobmsg_close_table(b, w);
	top_blob(b, "top_domains", "lookups", d->doms, DEV_DOM_MAX, 5);
	top_blob(b, "top_destinations", "bytes", d->dsts, DEV_DST_MAX, 5);
}

/* ---------- persistence ---------- */

static uint64_t num(struct blob_attr *a)
{
	if (!a)
		return 0;
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_INT64: return blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT16: return blobmsg_get_u16(a);
	case BLOBMSG_TYPE_INT8:  return blobmsg_get_u8(a);
	case BLOBMSG_TYPE_DOUBLE: return (uint64_t)blobmsg_get_double(a);
	default: return 0;
	}
}

static void str(char *dst, size_t n, struct blob_attr *a)
{
	if (a && blobmsg_type(a) == BLOBMSG_TYPE_STRING)
		snprintf(dst, n, "%s", blobmsg_get_string(a));
}

int dev_state_save(void)
{
	struct blob_buf b = { 0 };
	char tmp[300];
	void *arr, *t;
	char *json;
	FILE *f;
	int i, rc = -1;

	if (!cfg.state_file[0])
		return 0;
	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "version", 1);
	blobmsg_add_u64(&b, "saved", (uint64_t)ag_wall_s());
	arr = blobmsg_open_array(&b, "devices");
	for (i = 0; i < DEV_MAX; i++) {
		struct device *d = &devs[i];

		if (!d->used || (!d->mac[0] && !d->queries && !d->flows))
			continue;
		t = blobmsg_open_table(&b, NULL);
		dev_blob(&b, d);
		blobmsg_close_table(&b, t);
	}
	blobmsg_close_array(&b, arr);

	json = blobmsg_format_json(b.head, true);
	snprintf(tmp, sizeof(tmp), "%s.tmp", cfg.state_file);
	if (json) {
		char dir[256], *slash;

		snprintf(dir, sizeof(dir), "%s", cfg.state_file);
		slash = strrchr(dir, '/');
		if (slash && slash != dir) {
			*slash = '\0';
			mkdir(dir, 0755);
		}
		f = fopen(tmp, "w");
		if (f) {
			fputs(json, f);
			if (fclose(f) == 0 && rename(tmp, cfg.state_file) == 0)
				rc = 0;
		}
		free(json);
	}
	if (rc)
		AG_WARN("could not save state to %s: %s", cfg.state_file, strerror(errno));
	blob_buf_free(&b);
	return rc;
}

enum {
	S_IP, S_MAC, S_HOST, S_TYPE, S_VC, S_OPTS, S_FIRST, S_LAST, S_QUERIES,
	S_FLOWS, S_NODNS, S_UP, S_DOWN, S_DOMS, S_DSTS, __S_MAX
};

static const struct blobmsg_policy dev_policy[__S_MAX] = {
	[S_IP] = { "ip", BLOBMSG_TYPE_STRING },
	[S_MAC] = { "mac", BLOBMSG_TYPE_STRING },
	[S_HOST] = { "hostname", BLOBMSG_TYPE_STRING },
	[S_TYPE] = { "type", BLOBMSG_TYPE_STRING },
	[S_VC] = { "vendor_class", BLOBMSG_TYPE_STRING },
	[S_OPTS] = { "dhcp_options", BLOBMSG_TYPE_STRING },
	[S_FIRST] = { "first_seen", BLOBMSG_TYPE_UNSPEC },
	[S_LAST] = { "last_seen", BLOBMSG_TYPE_UNSPEC },
	[S_QUERIES] = { "queries", BLOBMSG_TYPE_UNSPEC },
	[S_FLOWS] = { "flows", BLOBMSG_TYPE_UNSPEC },
	[S_NODNS] = { "flows_no_dns", BLOBMSG_TYPE_UNSPEC },
	[S_UP] = { "bytes_up", BLOBMSG_TYPE_UNSPEC },
	[S_DOWN] = { "bytes_down", BLOBMSG_TYPE_UNSPEC },
	[S_DOMS] = { "top_domains", BLOBMSG_TYPE_ARRAY },
	[S_DSTS] = { "top_destinations", BLOBMSG_TYPE_ARRAY },
};

static void load_top(struct top_item *t, int n, struct blob_attr *arr, const char *count_name)
{
	struct blob_attr *e, *cur;
	size_t rem, r2;

	if (!arr)
		return;
	blobmsg_for_each_attr(e, arr, rem) {
		const char *name = NULL;
		uint64_t cnt = 0;

		blobmsg_for_each_attr(cur, e, r2) {
			if (!strcmp(blobmsg_name(cur), "name") && blobmsg_type(cur) == BLOBMSG_TYPE_STRING)
				name = blobmsg_get_string(cur);
			else if (!strcmp(blobmsg_name(cur), count_name))
				cnt = num(cur);
		}
		if (name && cnt)
			top_add(t, n, name, cnt);
	}
}

int dev_state_load(void)
{
	struct blob_buf b = { 0 };
	struct blob_attr *cur, *arr = NULL, *e;
	size_t rem;
	int n = 0;

	if (!cfg.state_file[0] || access(cfg.state_file, R_OK))
		return 0;
	blob_buf_init(&b, 0);
	if (!blobmsg_add_json_from_file(&b, cfg.state_file)) {
		AG_WARN("ignoring unreadable state file %s", cfg.state_file);
		blob_buf_free(&b);
		return -1;
	}
	blob_for_each_attr(cur, b.head, rem)
		if (!strcmp(blobmsg_name(cur), "devices") && blobmsg_type(cur) == BLOBMSG_TYPE_ARRAY)
			arr = cur;
	if (arr) {
		size_t r2;

		blobmsg_for_each_attr(e, arr, r2) {
			struct blob_attr *tb[__S_MAX];
			struct device *d;

			if (n >= DEV_MAX)
				break;
			blobmsg_parse(dev_policy, __S_MAX, tb, blobmsg_data(e), blobmsg_data_len(e));
			d = dev_alloc();
			str(d->ip, sizeof(d->ip), tb[S_IP]);
			str(d->mac, sizeof(d->mac), tb[S_MAC]);
			str(d->hostname, sizeof(d->hostname), tb[S_HOST]);
			str(d->type, sizeof(d->type), tb[S_TYPE]);
			str(d->vendor_class, sizeof(d->vendor_class), tb[S_VC]);
			str(d->dhcp_options, sizeof(d->dhcp_options), tb[S_OPTS]);
			d->first_seen = (int64_t)num(tb[S_FIRST]);
			d->last_seen = (int64_t)num(tb[S_LAST]);
			d->queries = (uint32_t)num(tb[S_QUERIES]);
			d->flows = (uint32_t)num(tb[S_FLOWS]);
			d->flows_no_dns = (uint32_t)num(tb[S_NODNS]);
			d->bytes_up = num(tb[S_UP]);
			d->bytes_down = num(tb[S_DOWN]);
			load_top(d->doms, DEV_DOM_MAX, tb[S_DOMS], "lookups");
			load_top(d->dsts, DEV_DST_MAX, tb[S_DSTS], "bytes");
			n++;
		}
	}
	blob_buf_free(&b);
	AG_INFO("restored %d devices from %s", n, cfg.state_file);
	return n;
}
