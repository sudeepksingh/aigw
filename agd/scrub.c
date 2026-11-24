/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Names out of what the ISP sees (core): the home's device names, MACs,
 * addresses and the sites alerts and decisions name, replaced by kinds
 * ("a laptop", "a site") unless brief.names is 1. Every text for the ISP goes
 * through it: the help-desk brief, proactive tickets, mesh Wi-Fi, coverage.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag/brief.h"
#include "agd.h"

/* ---------- names ---------- */

#define SITE_MAX  256
#define NAMES_MAX (DEV_MAX * 3 + 2 * SITE_MAX)
static char nm_buf[NAMES_MAX][64], kd_buf[NAMES_MAX][40];
static const char *nm[NAMES_MAX], *kd[NAMES_MAX];
static int nnm;

void scrub_names(void)
{
	int i;

	nnm = 0;
	for (i = 0; i < DEV_MAX; i++) {
		const struct device *d = &devs[i];
		char k[40];

		if (!d->used)
			continue;
		brief_kind(d->type, d->hostname, k, sizeof(k));
		if (d->hostname[0] && strlen(d->hostname) >= 2) {
			snprintf(nm_buf[nnm], sizeof(nm_buf[0]), "%.63s", d->hostname);
			snprintf(kd_buf[nnm], sizeof(kd_buf[0]), "%s", k);
			nnm++;
		}
		if (d->mac[0]) {
			snprintf(nm_buf[nnm], sizeof(nm_buf[0]), "%s", d->mac);
			snprintf(kd_buf[nnm], sizeof(kd_buf[0]), "%s", k);
			nnm++;
		}
		if (d->ip[0]) {
			snprintf(nm_buf[nnm], sizeof(nm_buf[0]), "%s", d->ip);
			snprintf(kd_buf[nnm], sizeof(kd_buf[0]), "%s", k);
			nnm++;
		}
	}
	/* the sites alerts and decisions name (a domain a device looked up, one blocked): never for the ISP */
	{
		uint32_t ids[SITE_MAX];
		int k, n = alerts_ids(ids, SITE_MAX);

		for (k = 0; k < n && nnm < NAMES_MAX; k++)
			if (alerts_field(ids[k], "subject", nm_buf[nnm], sizeof(nm_buf[0])) == 0 && strlen(nm_buf[nnm]) >= 3) {
				snprintf(kd_buf[nnm], sizeof(kd_buf[0]), "a site");
				nnm++;
			}
		n = act_ids(ids, SITE_MAX);
		for (k = 0; k < n && nnm < NAMES_MAX; k++) {
			char act[32];

			if (act_field(ids[k], "action", act, sizeof(act)) == 0 && !strcmp(act, "block") &&
			    act_field(ids[k], "target", nm_buf[nnm], sizeof(nm_buf[0])) == 0 && strlen(nm_buf[nnm]) >= 3) {
				snprintf(kd_buf[nnm], sizeof(kd_buf[0]), "a site");
				nnm++;
			}
		}
	}
	for (i = 0; i < nnm; i++) {
		nm[i] = nm_buf[i];
		kd[i] = kd_buf[i];
	}
}

void scrub_text(char *s, size_t n)
{
	brief_scrub(s, n, nm, kd, nnm);
}

/* a text for the ISP: the home's device names, MACs, addresses and sites out (unless brief.names) */
void brief_scrub_isp(char *text, size_t n)
{
	if (cfg.brief_names)
		return;
	scrub_names();
	scrub_text(text, n);
}
