/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Names out of what the ISP sees (core): a device as its kind ("a laptop"),
 * and a text with the home's names replaced by their kinds. The help-desk
 * brief, proactive care, mesh Wi-Fi and coverage advice use it (ag/brief.h).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ag/brief.h"

/* ---------- names ---------- */

void brief_kind(const char *type, const char *host, char *buf, size_t n)
{
	static const struct { const char *needle, *kind; } hints[] = {
		{ "laptop", "laptop" }, { "macbook", "laptop" }, { "notebook", "laptop" }, { "iphone", "phone" },
		{ "phone", "phone" }, { "ipad", "tablet" }, { "tablet", "tablet" }, { "tv", "TV" }, { "xbox", "games console" },
		{ "playstation", "games console" }, { "ps5", "games console" }, { "switch", "games console" },
		{ "printer", "printer" }, { "camera", "camera" }, { "desktop", "computer" }, { "pc", "computer" },
	};
	static const struct { const char *type, *kind; } types[] = {
		{ "Android", "Android phone" }, { "Windows", "Windows computer" }, { "Linux", "Linux device" },
		{ "iPhone", "iPhone" }, { "iPad", "iPad" }, { "Mac", "Mac" }, { "IoT", "smart-home device" },
		{ "media player", "media player" }, { "smart home", "smart-home device" },
	};
	const char *k = NULL;
	size_t i;

	for (i = 0; type && *type && i < sizeof(types) / sizeof(types[0]); i++)
		if (!strcmp(type, types[i].type))
			k = types[i].kind;
	if (!k && type && *type)
		k = type;
	for (i = 0; !k && host && *host && i < sizeof(hints) / sizeof(hints[0]); i++) {
		const char *h;

		for (h = host; *h && !k; h++)
			if (!strncasecmp(h, hints[i].needle, strlen(hints[i].needle)))
				k = hints[i].kind;
	}
	if (!k)
		k = "device";
	snprintf(buf, n, "%s %s", strchr("aeiouAEIOU", k[0]) ? "an" : "a", k);
}

static int word_char(char c)
{
	return isalnum((unsigned char)c) || c == '_' || c == '-' || c == ':' || c == '.';
}

void brief_scrub(char *text, size_t n, const char *const *name, const char *const *kind, int k)
{
	int stack[256], *order = stack, i, j, m = k;

	if (!text || !n || k <= 0)
		return;
	if (k > 256 && !(order = malloc(sizeof(*order) * (size_t)k))) {
		/* no memory: nothing may leave with a name in it */
		snprintf(text, n, "(not shown: no memory to take the names out)");
		return;
	}
	for (i = 0; i < m; i++)
		order[i] = i;
	for (i = 1; i < m; i++)         /* longest first: "tv-kitchen" before "tv" */
		for (j = i; j > 0 && strlen(name[order[j]]) > strlen(name[order[j - 1]]); j--) {
			int t = order[j];

			order[j] = order[j - 1];
			order[j - 1] = t;
		}
	for (i = 0; i < m; i++) {
		const char *nm = name[order[i]], *kd = kind[order[i]];
		size_t nl = strlen(nm), kl = strlen(kd);
		char *p = text;

		if (!nl)
			continue;
		while ((p = strstr(p, nm))) {
			size_t len = strlen(text), rest;
			int start = p == text || !word_char(p[-1]);
			char after = p[nl];
			int end = !after || !word_char(after) || ((after == '.' || after == ':') && (!p[nl + 1] || p[nl + 1] == ' '));

			if (!start || !end) {
				p += nl;
				continue;
			}
			rest = len - (size_t)(p - text) - nl;
			if ((size_t)(p - text) + kl + rest >= n) {
				*p = '\0';      /* no room: cut it here, so no name is left after */
				break;
			}
			memmove(p + kl, p + nl, rest + 1);
			memcpy(p, kd, kl);
			p += kl;
		}
	}
	if (order != stack)
		free(order);
}
