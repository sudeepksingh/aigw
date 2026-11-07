/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uci.h>

#include "ag/ag.h"

static void copy(char *dst, size_t n, const char *src)
{
	if (src && *src)
		snprintf(dst, n, "%s", src);
}

static const char *opt(struct uci_context *ctx, struct uci_package *p,
		       const char *section, const char *option)
{
	struct uci_section *s = uci_lookup_section(ctx, p, section);

	return s ? uci_lookup_option_string(ctx, s, option) : NULL;
}

void ag_config_load(struct ag_config *c)
{
	struct uci_context *ctx;
	struct uci_package *p = NULL;
	const char *confdir = getenv("AIGW_UCI_CONFDIR");
	const char *v;
	int owrt = ag_is_openwrt();

	memset(c, 0, sizeof(*c));
	copy(c->log_level, sizeof(c->log_level), "info");
	copy(c->host, sizeof(c->host), owrt ? "openwrt" : "linux");
	copy(c->leases, sizeof(c->leases), owrt ? "/tmp/dhcp.leases" : "/var/lib/aigw/dnsmasq.leases");
	copy(c->dns_listen, sizeof(c->dns_listen), "0.0.0.0");
	c->dns_port = 5300;
	copy(c->dns_upstream, sizeof(c->dns_upstream), "auto");
	/* OpenWRT: flash wears out, so save hourly; PCs: every 5 minutes */
	copy(c->state_file, sizeof(c->state_file), owrt ? "/etc/aigw/agd-state.json" : "/var/lib/aigw/agd-state.json");
	c->state_interval = owrt ? 3600 : 300;
	c->flow_enabled = 1;
	c->flow_poll = 30;
	c->flow_warmup = 600;
	copy(c->wifi_ctrl, sizeof(c->wifi_ctrl), owrt ? "/var/run/hostapd" : "/run/aigw/hostapd");
	copy(c->model_dir, sizeof(c->model_dir), AG_DATADIR "/models");
	c->alerts_enabled = 1;
	c->dga_threshold = 0.98;
	c->dga_min_label = 8;
	c->bucket_seconds = 3600;
	c->learn_minutes = 1440;
	c->min_known = 20;
	c->novelty_max = 2.0;
	c->spike_factor = 6.0;
	c->spike_min_mb = 50.0;
	c->unusual_min_days = 7;

	ctx = uci_alloc_context();
	if (!ctx)
		return;
	if (confdir)
		uci_set_confdir(ctx, confdir);
	if (uci_load(ctx, "aigw", &p) == UCI_OK && p) {
		copy(c->log_level, sizeof(c->log_level), opt(ctx, p, "main", "log_level"));
		copy(c->leases, sizeof(c->leases), opt(ctx, p, "main", "leases"));
		copy(c->dns_listen, sizeof(c->dns_listen), opt(ctx, p, "dns", "listen"));
		copy(c->dns_upstream, sizeof(c->dns_upstream), opt(ctx, p, "dns", "upstream"));
		v = opt(ctx, p, "dns", "port");
		if (v && atoi(v) > 0 && atoi(v) < 65536)
			c->dns_port = atoi(v);
		v = opt(ctx, p, "main", "state_file");
		if (v)
			snprintf(c->state_file, sizeof(c->state_file), "%s", v); /* may be "" */
		v = opt(ctx, p, "main", "state_interval");
		if (v && atoi(v) >= 10)
			c->state_interval = atoi(v);
		v = opt(ctx, p, "flow", "enabled");
		if (v)
			c->flow_enabled = atoi(v) != 0;
		v = opt(ctx, p, "flow", "poll");
		if (v && atoi(v) >= 1)
			c->flow_poll = atoi(v);
		v = opt(ctx, p, "flow", "warmup");
		if (v && atoi(v) >= 0)
			c->flow_warmup = atoi(v);
		copy(c->wifi_ctrl, sizeof(c->wifi_ctrl), opt(ctx, p, "wifi", "ctrl_dir"));
		copy(c->model_dir, sizeof(c->model_dir), opt(ctx, p, "main", "model_dir"));
		{
			struct uci_section *s = uci_lookup_section(ctx, p, "lan");
			struct uci_option *o = s ? uci_lookup_option(ctx, s, "iface") : NULL;
			struct uci_element *e;
			size_t len = 0;

			if (o && o->type == UCI_TYPE_STRING) {
				copy(c->lan_ifaces, sizeof(c->lan_ifaces), o->v.string);
			} else if (o && o->type == UCI_TYPE_LIST) {
				uci_foreach_element(&o->v.list, e) {
					int w = snprintf(c->lan_ifaces + len, sizeof(c->lan_ifaces) - len, "%s%s",
							 len ? " " : "", e->name);

					if (w < 0 || (size_t)w >= sizeof(c->lan_ifaces) - len)
						break;
					len += (size_t)w;
				}
			}
		}
		v = opt(ctx, p, "alerts", "enabled");
		if (v)
			c->alerts_enabled = atoi(v) != 0;
		v = opt(ctx, p, "alerts", "dga_threshold");
		if (v && atof(v) > 0 && atof(v) <= 1)
			c->dga_threshold = atof(v);
		v = opt(ctx, p, "alerts", "dga_min_label");
		if (v && atoi(v) >= 1)
			c->dga_min_label = atoi(v);
		v = opt(ctx, p, "baseline", "bucket_seconds");
		if (v && atoi(v) >= 1)
			c->bucket_seconds = atoi(v);
		v = opt(ctx, p, "baseline", "learn_minutes");
		if (v && atoi(v) >= 0)
			c->learn_minutes = atoi(v);
		v = opt(ctx, p, "baseline", "min_known");
		if (v && atoi(v) >= 0)
			c->min_known = atoi(v);
		v = opt(ctx, p, "baseline", "novelty_max");
		if (v && atof(v) >= 0)
			c->novelty_max = atof(v);
		v = opt(ctx, p, "baseline", "spike_factor");
		if (v && atof(v) > 0)
			c->spike_factor = atof(v);
		v = opt(ctx, p, "baseline", "spike_min_mb");
		if (v && atof(v) >= 0)
			c->spike_min_mb = atof(v);
		v = opt(ctx, p, "baseline", "unusual_min_days");
		if (v && atoi(v) >= 1)
			c->unusual_min_days = atoi(v);
	}
	uci_free_context(ctx);
}

const char *ag_ubus_path(void)
{
	const char *p = getenv("AIGW_UBUS");

	return (p && *p) ? p : NULL;
}
