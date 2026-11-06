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
