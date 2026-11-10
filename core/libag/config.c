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

/* An option that may be a single value or a UCI list -> space-separated. */
static void read_list(struct uci_context *ctx, struct uci_section *s, const char *option,
		      char *out, size_t n)
{
	struct uci_option *o = s ? uci_lookup_option(ctx, s, option) : NULL;
	struct uci_element *e;
	size_t len = 0;

	if (o && o->type == UCI_TYPE_STRING) {
		copy(out, n, o->v.string);
	} else if (o && o->type == UCI_TYPE_LIST) {
		out[0] = '\0';
		uci_foreach_element(&o->v.list, e) {
			int w = snprintf(out + len, n - len, "%s%s", len ? " " : "", e->name);

			if (w < 0 || (size_t)w >= n - len)
				break;
			len += (size_t)w;
		}
	}
}

int ag_mode_parse(const char *s, int dflt)
{
	if (!s || !*s)
		return dflt;
	if (!strcmp(s, "off") || !strcmp(s, "observe"))
		return AG_MODE_OFF;
	if (!strcmp(s, "advise"))
		return AG_MODE_ADVISE;
	if (!strcmp(s, "enforce"))
		return AG_MODE_ENFORCE;
	return dflt;
}

const char *ag_mode_str(int mode)
{
	return mode == AG_MODE_ENFORCE ? "enforce" : mode == AG_MODE_ADVISE ? "advise" : "off";
}

int ag_plugin_name_ok(const char *name)
{
	size_t i, n = name ? strlen(name) : 0;

	if (n < 1 || n > 31 || name[0] < 'a' || name[0] > 'z')
		return 0;
	for (i = 0; i < n; i++)
		if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9') || name[i] == '-'))
			return 0;
	return 1;
}

int ag_policy_action_ok(const char *action)
{
	char plugin[48];
	const char *dot, *p;

	if (!action || !*action)
		return 0;
	/* blocking a domain is never automatic: the user names it */
	if (!strcmp(action, "quarantine") || !strcmp(action, "disconnect") || !strcmp(action, "throttle") ||
	    !strcmp(action, "deprioritize") || !strcmp(action, "priority") || !strcmp(action, "channel"))
		return 1;
	dot = strchr(action, '.');
	if (!dot || dot == action || (size_t)(dot - action) >= sizeof(plugin) || !dot[1] || strlen(dot + 1) > 23)
		return 0;
	snprintf(plugin, sizeof(plugin), "%.*s", (int)(dot - action), action);
	for (p = dot + 1; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
			return 0;
	return ag_plugin_name_ok(plugin);
}

static void add_policy(struct ag_config *c, const char *name, const char *alert, const char *action,
		       const char *zone, int ttl, int rate, int mode)
{
	struct ag_policy *p;

	if (c->n_policies >= AG_POLICY_MAX || !alert || !*alert || !ag_policy_action_ok(action))
		return;
	p = &c->policies[c->n_policies++];
	snprintf(p->name, sizeof(p->name), "%s", name ? name : alert);
	snprintf(p->alert, sizeof(p->alert), "%s", alert);
	snprintf(p->action, sizeof(p->action), "%s", action);
	snprintf(p->zone, sizeof(p->zone), "%s", zone ? zone : "");
	p->ttl = ttl > 0 ? ttl : 0;
	p->rate_kbps = rate > 0 ? rate : 0;
	p->mode = mode;
}

void ag_config_load(struct ag_config *c)
{
	struct uci_context *ctx;
	struct uci_package *p = NULL;
	const char *confdir = getenv("AIGW_UCI_CONFDIR");
	const char *v;
	int owrt = ag_is_openwrt(), policies_configured = 0;

	memset(c, 0, sizeof(*c));
	copy(c->log_level, sizeof(c->log_level), "info");
	copy(c->host, sizeof(c->host), owrt ? "openwrt" : "linux");
	copy(c->leases, sizeof(c->leases), owrt ? "/tmp/dhcp.leases" : "/var/lib/aigw/dnsmasq.leases");
	copy(c->dns_listen, sizeof(c->dns_listen), "0.0.0.0");
	c->dns_port = 5300;
	copy(c->dns_upstream, sizeof(c->dns_upstream), "auto");
	c->dns_cache = 1000;
	c->dns_prefetch = 1;
	c->dns_stale = 86400;
	c->dns_max_ttl = 86400;
	/* OpenWRT: flash wears out, so save hourly; PCs: every 5 minutes */
	copy(c->state_file, sizeof(c->state_file), owrt ? "/etc/aigw/agd-state.json" : "/var/lib/aigw/agd-state.json");
	c->state_interval = owrt ? 3600 : 300;
	c->flow_enabled = 1;
	c->flow_poll = 30;
	c->flow_warmup = 600;
	c->flow_sni = 1;
	c->flow_sni_group = 4419;
	copy(c->wifi_ctrl, sizeof(c->wifi_ctrl), owrt ? "/var/run/hostapd" : "/run/aigw/hostapd");
	c->steer = 1;
	c->steer_up = -60;
	c->steer_down = -75;
	c->steer_hold = 30;
	c->steer_gap = 300;
	c->steer_kick = 1;
	c->steer_rate = 2;
	c->presence_away = 600;
	c->chan_heal = 1;
	c->chan_busy = 60;
	c->chan_hold = 1800;
	c->chan_poll = 30;
	c->chan_scan_gap = 21600;
	copy(c->model_dir, sizeof(c->model_dir), AG_DATADIR "/models");
	c->alerts_enabled = 1;
	c->dga_threshold = 0.98;
	c->dga_min_label = 8;
	c->burst_enabled = 1;
	c->mgmt_enabled = 0;
	c->mgmt_actions = 0;
	c->mgmt_alert_details = 0;
	c->burst_window = 600;
	c->burst_min = 8;
	c->burst_share = 0.75;
	c->meter_enabled = 1;
	c->fed_enabled = 1;
	c->fed_noise = 1.0;
	c->fed_min_examples = 3;
	c->bucket_seconds = 3600;
	c->learn_minutes = 1440;
	c->min_known = 20;
	c->novelty_max = 2.0;
	c->spike_factor = 6.0;
	c->spike_min_mb = 50.0;
	c->unusual_min_days = 7;
	c->act_mode = AG_MODE_ADVISE;
	c->act_ttl = 3600;
	c->act_max_ttl = 86400;
	c->act_max_auto = 3;
	c->act_rate_kbps = 2000;
	copy(c->dns_policy, sizeof(c->dns_policy), owrt ? "/var/run/aigw/dns-policy" : "/run/aigw/dns-policy");
	c->plugins_enabled = 1;
	copy(c->plugin_dir, sizeof(c->plugin_dir), AG_DATADIR "/plugins");
	copy(c->plugin_user_dir, sizeof(c->plugin_user_dir), "/etc/aigw/plugins");
	copy(c->plugin_user, sizeof(c->plugin_user), "nobody");
	copy(c->plugin_state, sizeof(c->plugin_state), owrt ? "/var/run/aigw/plugins" : "/var/lib/aigw/plugins");
	copy(c->models_dir, sizeof(c->models_dir), "/etc/aigw/models");
	copy(c->trusted_keys, sizeof(c->trusted_keys), "/etc/aigw/keys.d");
	c->models_require_sig = 1;
	copy(c->model_backend, sizeof(c->model_backend), "auto");
	copy(c->backend_dir, sizeof(c->backend_dir), "/usr/lib/aigw/backends");
	c->remote_port = 51820;
	copy(c->remote_subnet, sizeof(c->remote_subnet), "10.77.0.0/24");
	copy(c->remote_iface, sizeof(c->remote_iface), "agwg");
	c->remote_full = 1;
	copy(c->remote_dir, sizeof(c->remote_dir), "/etc/aigw/remote");
	copy(c->wifi_keys, sizeof(c->wifi_keys), "/etc/aigw/wifi-keys");
	c->key_ttl = 86400;

	ctx = uci_alloc_context();
	if (!ctx)
		goto defaults;
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
		v = opt(ctx, p, "dns", "cache");
		if (v && atoi(v) >= 0)
			c->dns_cache = atoi(v) > 100000 ? 100000 : atoi(v);
		v = opt(ctx, p, "dns", "prefetch");
		if (v)
			c->dns_prefetch = atoi(v) != 0;
		v = opt(ctx, p, "dns", "stale");
		if (v && atoi(v) >= 0)
			c->dns_stale = atoi(v);
		v = opt(ctx, p, "dns", "max_ttl");
		if (v && atoi(v) >= 1)
			c->dns_max_ttl = atoi(v);
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
		v = opt(ctx, p, "flow", "sni");
		if (v)
			c->flow_sni = atoi(v) != 0;
		v = opt(ctx, p, "flow", "sni_group");
		if (v && atoi(v) > 0 && atoi(v) < 65536)
			c->flow_sni_group = atoi(v);
		copy(c->wifi_ctrl, sizeof(c->wifi_ctrl), opt(ctx, p, "wifi", "ctrl_dir"));
		v = opt(ctx, p, "wifi", "steer");
		if (v)
			c->steer = atoi(v) != 0;
		v = opt(ctx, p, "wifi", "steer_up");
		if (v && atoi(v) < 0 && atoi(v) > -100)
			c->steer_up = atoi(v);
		v = opt(ctx, p, "wifi", "steer_down");
		if (v && atoi(v) < 0 && atoi(v) > -100)
			c->steer_down = atoi(v);
		v = opt(ctx, p, "wifi", "steer_hold");
		if (v && atoi(v) >= 0)
			c->steer_hold = atoi(v);
		v = opt(ctx, p, "wifi", "steer_gap");
		if (v && atoi(v) >= 10)
			c->steer_gap = atoi(v);
		v = opt(ctx, p, "wifi", "steer_kick");
		if (v)
			c->steer_kick = atoi(v) != 0;
		v = opt(ctx, p, "wifi", "steer_rate");
		if (v && atoi(v) >= 1)
			c->steer_rate = atoi(v);
		v = opt(ctx, p, "wifi", "presence_away");
		if (v && atoi(v) >= 10)
			c->presence_away = atoi(v);
		v = opt(ctx, p, "wifi", "chan_heal");
		if (v)
			c->chan_heal = atoi(v) != 0;
		v = opt(ctx, p, "wifi", "chan_busy");
		if (v && atoi(v) >= 10 && atoi(v) <= 100)
			c->chan_busy = atoi(v);
		v = opt(ctx, p, "wifi", "chan_hold");
		if (v && atoi(v) >= 0)
			c->chan_hold = atoi(v);
		v = opt(ctx, p, "wifi", "chan_poll");
		if (v && atoi(v) >= 5)
			c->chan_poll = atoi(v);
		v = opt(ctx, p, "wifi", "chan_scan_gap");
		if (v && atoi(v) >= 10)
			c->chan_scan_gap = atoi(v);
		copy(c->model_dir, sizeof(c->model_dir), opt(ctx, p, "main", "model_dir"));
		read_list(ctx, uci_lookup_section(ctx, p, "lan"), "iface", c->lan_ifaces, sizeof(c->lan_ifaces));
		v = opt(ctx, p, "alerts", "enabled");
		if (v)
			c->alerts_enabled = atoi(v) != 0;
		v = opt(ctx, p, "alerts", "dga_threshold");
		if (v && atof(v) > 0 && atof(v) <= 1)
			c->dga_threshold = atof(v);
		v = opt(ctx, p, "alerts", "dga_min_label");
		if (v && atoi(v) >= 1)
			c->dga_min_label = atoi(v);
		v = opt(ctx, p, "mgmt", "enabled");
		if (v)
			c->mgmt_enabled = atoi(v) != 0;
		v = opt(ctx, p, "mgmt", "actions");
		if (v)
			c->mgmt_actions = atoi(v) != 0;
		v = opt(ctx, p, "mgmt", "alert_details");
		if (v)
			c->mgmt_alert_details = atoi(v) != 0;
		v = opt(ctx, p, "alerts", "burst");
		if (v)
			c->burst_enabled = atoi(v) != 0;
		v = opt(ctx, p, "alerts", "burst_window");
		if (v && atoi(v) >= 10)
			c->burst_window = atoi(v);
		v = opt(ctx, p, "alerts", "burst_min");
		if (v && atoi(v) >= 2 && atoi(v) <= 32)
			c->burst_min = atoi(v);
		v = opt(ctx, p, "alerts", "burst_share");
		if (v && atof(v) > 0 && atof(v) <= 1)
			c->burst_share = atof(v);
		v = opt(ctx, p, "fastpath", "enabled");
		if (v)
			c->fastpath_enabled = atoi(v) != 0;
		v = opt(ctx, p, "meter", "enabled");
		if (v)
			c->meter_enabled = atoi(v) != 0;
		v = opt(ctx, p, "alerts", "fed");
		if (v)
			c->fed_enabled = atoi(v) != 0;
		v = opt(ctx, p, "alerts", "fed_noise");
		if (v && atof(v) >= 0 && atof(v) <= 100)
			c->fed_noise = atof(v);
		v = opt(ctx, p, "alerts", "fed_min_examples");
		if (v && atoi(v) >= 1)
			c->fed_min_examples = atoi(v);
		v = opt(ctx, p, "alerts", "fed_budget");
		if (v && atof(v) >= 0)
			c->fed_budget = atof(v);
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

		c->act_mode = ag_mode_parse(opt(ctx, p, "act", "mode"), c->act_mode);
		v = opt(ctx, p, "act", "max_ttl");
		if (v && atoi(v) >= 60)
			c->act_max_ttl = atoi(v);
		v = opt(ctx, p, "act", "ttl");
		if (v && atoi(v) >= 10)
			c->act_ttl = atoi(v);
		if (c->act_ttl > c->act_max_ttl)
			c->act_ttl = c->act_max_ttl;
		v = opt(ctx, p, "act", "max_auto_per_hour");
		if (v && atoi(v) >= 0)
			c->act_max_auto = atoi(v);
		v = opt(ctx, p, "act", "throttle_kbps");
		if (v && atoi(v) >= 64)
			c->act_rate_kbps = atoi(v);
		read_list(ctx, uci_lookup_section(ctx, p, "act"), "protect", c->act_protect, sizeof(c->act_protect));
		copy(c->dns_policy, sizeof(c->dns_policy), opt(ctx, p, "act", "dns_policy"));
		v = opt(ctx, p, "plugins", "enabled");
		if (v)
			c->plugins_enabled = atoi(v) != 0;
		copy(c->plugin_dir, sizeof(c->plugin_dir), opt(ctx, p, "plugins", "dir"));
		copy(c->plugin_user_dir, sizeof(c->plugin_user_dir), opt(ctx, p, "plugins", "user_dir"));
		v = opt(ctx, p, "plugins", "user");
		if (v)
			snprintf(c->plugin_user, sizeof(c->plugin_user), "%s", v);   /* may be "" */
		copy(c->plugin_state, sizeof(c->plugin_state), opt(ctx, p, "plugins", "state_dir"));
		copy(c->models_dir, sizeof(c->models_dir), opt(ctx, p, "models", "dir"));
		copy(c->trusted_keys, sizeof(c->trusted_keys), opt(ctx, p, "models", "keys"));
		v = opt(ctx, p, "models", "require_signature");
		if (v)
			c->models_require_sig = atoi(v) != 0;
		copy(c->model_backend, sizeof(c->model_backend), opt(ctx, p, "models", "backend"));
		copy(c->backend_dir, sizeof(c->backend_dir), opt(ctx, p, "models", "backend_dir"));
		v = opt(ctx, p, "remote", "enabled");
		if (v)
			c->remote_enabled = atoi(v) != 0;
		copy(c->remote_endpoint, sizeof(c->remote_endpoint), opt(ctx, p, "remote", "endpoint"));
		v = opt(ctx, p, "remote", "port");
		if (v && atoi(v) > 0 && atoi(v) < 65536)
			c->remote_port = atoi(v);
		copy(c->remote_subnet, sizeof(c->remote_subnet), opt(ctx, p, "remote", "subnet"));
		copy(c->remote_iface, sizeof(c->remote_iface), opt(ctx, p, "remote", "iface"));
		v = opt(ctx, p, "remote", "full_tunnel");
		if (v)
			c->remote_full = atoi(v) != 0;
		copy(c->remote_dir, sizeof(c->remote_dir), opt(ctx, p, "remote", "dir"));
		copy(c->wifi_keys, sizeof(c->wifi_keys), opt(ctx, p, "wifi", "keys"));
		v = opt(ctx, p, "wifi", "key_ttl");
		if (v && atoi(v) >= 60)
			c->key_ttl = atoi(v);
		{
			struct uci_element *e;

			uci_foreach_element(&p->sections, e) {
				struct uci_section *s = uci_to_section(e);
				const char *ttl, *rate;

				if (strcmp(s->type, "policy"))
					continue;
				/* any policy section replaces the built-in ones, even one set to 'off' */
				policies_configured = 1;
				ttl = uci_lookup_option_string(ctx, s, "ttl");
				rate = uci_lookup_option_string(ctx, s, "rate");
				add_policy(c, s->anonymous ? NULL : s->e.name,
					   uci_lookup_option_string(ctx, s, "alert"),
					   uci_lookup_option_string(ctx, s, "action"),
					   uci_lookup_option_string(ctx, s, "zone"), ttl ? atoi(ttl) : 0,
					   rate ? atoi(rate) : 0,
					   ag_mode_parse(uci_lookup_option_string(ctx, s, "mode"), AG_MODE_ENFORCE));
			}
		}
	}
	uci_free_context(ctx);
defaults:
	if (!policies_configured) {
		add_policy(c, "dga", "dga", "quarantine", NULL, 0, 0, AG_MODE_ENFORCE);
		add_policy(c, "iot_new_site", "new-destination", "quarantine", NULL, 0, 0, AG_MODE_ENFORCE);
		add_policy(c, "known_threat", "known-threat", "quarantine", NULL, 0, 0, AG_MODE_ENFORCE);
		add_policy(c, "dga_burst", "dga-burst", "quarantine", NULL, 0, 0, AG_MODE_ENFORCE);
		/* a quieter channel for a day; then back, and judged again */
		add_policy(c, "wifi_busy", "wifi-busy", "channel", NULL, 86400, 0, AG_MODE_ENFORCE);
	}
}

int ag_config_set(const char *section, const char *option, const char *value)
{
	const char *confdir = getenv("AIGW_UCI_CONFDIR");
	struct uci_context *ctx = uci_alloc_context();
	struct uci_ptr ptr = { 0 };
	char spec[256];
	int rc = -1;

	if (!ctx)
		return -1;
	if (confdir)
		uci_set_confdir(ctx, confdir);
	if (strpbrk(section, ".=") || strpbrk(option, ".=") || strchr(value, '\n'))
		goto out;
	snprintf(spec, sizeof(spec), "aigw.%s.%s=%s", section, option, value);
	if (uci_lookup_ptr(ctx, &ptr, spec, true) != UCI_OK || !ptr.s)
		goto out;               /* the section must exist */
	if (uci_set(ctx, &ptr) == UCI_OK && uci_save(ctx, ptr.p) == UCI_OK && uci_commit(ctx, &ptr.p, false) == UCI_OK)
		rc = 0;
out:
	uci_free_context(ctx);
	return rc;
}

const char *ag_ubus_path(void)
{
	const char *p = getenv("AIGW_UBUS");

	return (p && *p) ? p : NULL;
}
