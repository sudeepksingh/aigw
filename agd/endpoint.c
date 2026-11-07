/* SPDX-License-Identifier: Apache-2.0 */
/* Endpoint table: IP address -> the name a device looked it up as. Filled
 * from DNS answers, read when a connection starts. This join is what lets
 * AIGW say "the TV is talking to ads.example.com" instead of "to 1.2.3.4",
 * and spot connections made without any DNS lookup. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libubox/avl.h>
#include <libubox/avl-cmp.h>

#include "agd.h"

#define EP_MAX      8192
#define EP_MIN_KEEP 600   /* connections often start after the DNS TTL ran out */

struct endpoint {
	struct avl_node avl;
	char ip[IP_LEN];
	char domain[NAME_LEN];
	char client[IP_LEN];
	int64_t expires;
};

static AVL_TREE(eps, avl_strcmp, false, NULL);

/* IPv4 /24 -> a name recently resolved inside it. Apps such as WhatsApp
 * connect to server addresses from their own lists, usually next to ones
 * they did look up; those connections get the neighbour's name. */
struct netname {
	struct avl_node avl;
	char net[16];
	char domain[NAME_LEN];
};

static AVL_TREE(nets, avl_strcmp, false, NULL);

static int net24(const char *ip, char *out, size_t n)
{
	const char *last = strrchr(ip, '.');

	if (strchr(ip, ':') || !last || (size_t)(last - ip) >= n)
		return -1;
	memcpy(out, ip, (size_t)(last - ip));
	out[last - ip] = '\0';
	return 0;
}

static void net_add(const char *ip, const char *domain)
{
	struct netname *e;
	char key[16];

	if (net24(ip, key, sizeof(key)) < 0)
		return;
	e = avl_find_element(&nets, key, e, avl);
	if (!e) {
		if (nets.count >= EP_MAX) {
			struct netname *x, *tmp;
			unsigned drop = EP_MAX / 8;

			avl_for_each_element_safe(&nets, x, avl, tmp) {
				if (!drop--)
					break;
				avl_delete(&nets, &x->avl);
				free(x);
			}
		}
		e = calloc(1, sizeof(*e));
		if (!e)
			return;
		snprintf(e->net, sizeof(e->net), "%s", key);
		e->avl.key = e->net;
		avl_insert(&nets, &e->avl);
	}
	snprintf(e->domain, sizeof(e->domain), "%s", domain);
}

const char *ep_lookup_near(const char *ip)
{
	struct netname *e;
	char key[16];

	if (net24(ip, key, sizeof(key)) < 0)
		return NULL;
	e = avl_find_element(&nets, key, e, avl);
	return e ? e->domain : NULL;
}

static void ep_evict(void)
{
	struct endpoint *e, *tmp;
	int64_t now = ag_wall_s();
	unsigned drop;

	avl_for_each_element_safe(&eps, e, avl, tmp) {
		if (e->expires < now) {
			avl_delete(&eps, &e->avl);
			free(e);
		}
	}
	/* still full: drop an eighth (address order, effectively arbitrary) */
	drop = eps.count >= EP_MAX ? EP_MAX / 8 : 0;
	avl_for_each_element_safe(&eps, e, avl, tmp) {
		if (!drop--)
			break;
		avl_delete(&eps, &e->avl);
		free(e);
	}
}

void ep_add(const char *ip, const char *domain, const char *client, uint32_t ttl)
{
	struct endpoint *e = avl_find_element(&eps, ip, e, avl);

	if (!e) {
		if (eps.count >= EP_MAX)
			ep_evict();
		e = calloc(1, sizeof(*e));
		if (!e)
			return;
		snprintf(e->ip, sizeof(e->ip), "%s", ip);
		e->avl.key = e->ip;
		avl_insert(&eps, &e->avl);
	}
	/* shared CDN addresses: the most recent lookup wins */
	snprintf(e->domain, sizeof(e->domain), "%s", domain);
	snprintf(e->client, sizeof(e->client), "%s", client);
	e->expires = ag_wall_s() + (ttl > EP_MIN_KEEP ? ttl : EP_MIN_KEEP);
	net_add(ip, domain);
}

const char *ep_lookup(const char *ip)
{
	struct endpoint *e = avl_find_element(&eps, ip, e, avl);

	return e ? e->domain : NULL;
}

int ep_count(void)
{
	return (int)eps.count;
}

/* Saved with the device history: devices keep using cached DNS answers
 * after agd restarts, and without these their connections would look like
 * "no DNS lookup". Only entries that have not expired are kept. */
void ep_save(struct blob_buf *b)
{
	struct endpoint *e;
	int64_t now = ag_wall_s();
	void *arr, *t;

	arr = blobmsg_open_array(b, "endpoints");
	avl_for_each_element(&eps, e, avl) {
		if (e->expires < now)
			continue;
		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "ip", e->ip);
		blobmsg_add_string(b, "domain", e->domain);
		blobmsg_add_u64(b, "expires", (uint64_t)e->expires);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, arr);
}

int ep_load(struct blob_attr *arr)
{
	struct blob_attr *e, *cur;
	int64_t now = ag_wall_s();
	size_t rem, r2;
	int n = 0;

	blobmsg_for_each_attr(e, arr, rem) {
		const char *ip = NULL, *dom = NULL;
		int64_t exp = 0;

		blobmsg_for_each_attr(cur, e, r2) {
			const char *k = blobmsg_name(cur);

			if (!strcmp(k, "ip") && blobmsg_type(cur) == BLOBMSG_TYPE_STRING)
				ip = blobmsg_get_string(cur);
			else if (!strcmp(k, "domain") && blobmsg_type(cur) == BLOBMSG_TYPE_STRING)
				dom = blobmsg_get_string(cur);
			else if (!strcmp(k, "expires"))
				exp = blobmsg_type(cur) == BLOBMSG_TYPE_INT64 ? (int64_t)blobmsg_get_u64(cur) :
				      blobmsg_type(cur) == BLOBMSG_TYPE_INT32 ? (int64_t)blobmsg_get_u32(cur) : 0;
		}
		if (!ip || !dom || exp < now)
			continue;
		ep_add(ip, dom, "", (uint32_t)(exp - now));
		n++;
	}
	return n;
}
