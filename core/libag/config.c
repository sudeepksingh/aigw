/* SPDX-License-Identifier: Apache-2.0 */
#include <stddef.h>
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

/* An option that may be a single value or a UCI list -> joined by sep. */
static void read_list_sep(struct uci_context *ctx, struct uci_section *s, const char *option,
			  char *out, size_t n, const char *sep)
{
	struct uci_option *o = s ? uci_lookup_option(ctx, s, option) : NULL;
	struct uci_element *e;
	size_t len = 0;

	if (o && o->type == UCI_TYPE_STRING) {
		copy(out, n, o->v.string);
	} else if (o && o->type == UCI_TYPE_LIST) {
		out[0] = '\0';
		uci_foreach_element(&o->v.list, e) {
			int w = snprintf(out + len, n - len, "%s%s", len ? sep : "", e->name);

			if (w < 0 || (size_t)w >= n - len)
				break;
			len += (size_t)w;
		}
	}
}

/* An option that may be a single value or a UCI list -> space-separated. */
static void read_list(struct uci_context *ctx, struct uci_section *s, const char *option,
		      char *out, size_t n)
{
	read_list_sep(ctx, s, option, out, n, " ");
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
	    !strcmp(action, "deprioritize") || !strcmp(action, "priority") || !strcmp(action, "channel") ||
	    !strcmp(action, "stay24") || !strcmp(action, "sqm"))
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

/* the sections of features this build may not have (brief: brief.names also rules the core's name scrubbing) */
static const struct { const char *section, *feature; int built; } feature_sections[] = {
	{ "meter", "METER", AIGW_FEATURE_METER }, { "fastpath", "FASTPATH", AIGW_FEATURE_FASTPATH },
	{ "care", "CARE", AIGW_FEATURE_CARE }, { "upgrade", "UPGRADE", AIGW_FEATURE_UPGRADE },
	{ "why", "WHY", AIGW_FEATURE_WHY }, { "sqm", "SQM", AIGW_FEATURE_SQM }, { "calls", "CALLS", AIGW_FEATURE_CALLS },
	{ "offers", "OFFERS", AIGW_FEATURE_OFFERS }, { "area", "AREA", AIGW_FEATURE_AREA },
	{ "proactive", "PROACTIVE", AIGW_FEATURE_PROACTIVE }, { "mesh", "MESH", AIGW_FEATURE_MESH },
	{ "coverage", "COVERAGE", AIGW_FEATURE_COVERAGE }, { "plugins", "PLUGINS", AIGW_FEATURE_PLUGINS },
	{ "remote", "REMOTE", AIGW_FEATURE_REMOTE }, { "fleet", "FLEETLEARN", AIGW_FEATURE_FLEETLEARN }, { "repair", "PROVEN", AIGW_FEATURE_PROVEN },
	{ "line", "AREAPREDICT", AIGW_FEATURE_AREAPREDICT },
};

/* fleet.line: the kinds of line the fleet's buckets know */
int ag_fleet_line_ok(const char *v)
{
	static const char *const ok[] = { "", "fibre", "dsl", "cable", "fwa", "satellite", "other", NULL };
	int i;

	for (i = 0; ok[i]; i++)
		if (!strcmp(v, ok[i]))
			return 1;
	return 0;
}

/* "mesh (AIGW_FEATURE_MESH), ..." for each such section the config has */
static void note_unbuilt(struct ag_config *c, struct uci_context *ctx, struct uci_package *p)
{
	size_t k, len = 0;

	c->unbuilt[0] = '\0';
	for (k = 0; k < sizeof(feature_sections) / sizeof(feature_sections[0]); k++) {
		int w;

		if (feature_sections[k].built || !uci_lookup_section(ctx, p, feature_sections[k].section))
			continue;
		w = snprintf(c->unbuilt + len, sizeof(c->unbuilt) - len, "%s%s (AIGW_FEATURE_%s)", len ? ", " : "",
			     feature_sections[k].section, feature_sections[k].feature);
		if (w < 0 || (size_t)w >= sizeof(c->unbuilt) - len)
			break;
		len += (size_t)w;
	}
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
	c->heal = 1;
	c->heal_loss = 5;
	c->heal_delay = 100;
	c->heal_weak = -75;
	c->heal_hops = 4;
	c->heal_act = 1;
	c->heal_hold = 300;
	c->heal_trial = 180;
	c->heal_retry = 1800;
	c->heal_weak_margin = 3;
	copy(c->diag_targets, sizeof(c->diag_targets), "1.1.1.1 8.8.8.8 9.9.9.9");
	copy(c->diag_name, sizeof(c->diag_name), "example.com");
	c->care = 1;
	c->care_interval = 600;
	c->care_recheck = 60;
	c->care_wifi_grace = 1800;
	c->care_warn = 1;
	c->care_learn_days = 3;
	c->care_speed_hour = 3;
	c->care_speed_seconds = 8;
	c->care_speed_min_pct = 50;
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
	c->chan_plan = 1;
	c->chan_plan_days = 3;
	c->chan_plan_better = 30;
	c->chan_plan_hold = 3;
	c->chan_plan_every = 3600;
	c->chan_plan_gap = 604800;
	c->chan_plan_trial = 3600;
	c->chan_avoid = 604800;
	c->chan_learn_gap = 10800;
	c->chan_quiet = 5;
	copy(c->model_dir, sizeof(c->model_dir), AG_DATADIR "/models");
	c->alerts_enabled = 1;
	c->dga_threshold = 0.98;
	c->dga_min_label = 8;
	c->burst_enabled = 1;
	c->mgmt_enabled = 0;
	c->mgmt_actions = 0;
	c->mgmt_alert_details = 0;
	c->mgmt_wifi = 1;
	c->mgmt_firmware = 1;
	c->upgrade_check = 1;
	c->upgrade_rollback = 1;
	c->upgrade_watch = 86400;
	c->upgrade_reconnect = 1800;
	c->upgrade_hold_days = 7;
	c->upgrade_report_delay = 60;
	c->why = 1;
	c->why_hours = 72;
	c->why_interval = 60;
	c->why_probe = 1;
	c->offers = 1;
	c->offers_share = 1;
	c->brief = 1;
	c->area = 1;
	c->pro = 1;
	c->pro_fix = 1;
	c->pro_interval = 60;
	c->pro_threshold = 3;
	c->pro_min_s = 180;
	c->pro_clear_s = 600;
	c->pro_cooldown_s = 21600;
	c->pro_max_per_day = 3;
	c->mesh = 1;
	c->mesh_poll = 30;
	c->mesh_steer = 1;
	c->mesh_channel = 1;
	c->mesh_trial = 120;
	c->mesh_hold = 60;
	c->mesh_weak_dbm = -75;
	c->mesh_hyst_db = 8;
	c->mesh_cooldown = 900;
	c->mesh_quiet = 300;
	c->cov = 1;
	c->cov_interval = 60;
	c->cov_learn = 60;
	c->cov_share = 1;
	c->repair = AIGW_FEATURE_PROVEN;
	c->repair_window = 600;
	c->repair_settle = 60;
	c->repair_undo = 1;
	c->repair_shadow = 1;
	c->repair_gate = 0;               /* off: acting is as act.mode says */
	c->repair_gate_rate = 80;
	c->repair_gate_trials = 10;
	c->repair_tick = 10;
	c->line = 1;
	c->line_interval = 900;
	c->line_share = 1;
	c->sqm = 1;
	c->sqm_detect = 10;
	c->sqm_trial = 30;
	c->sqm_hold = 86400;
	c->calls = 1;
	c->calls_prio = 1;
	c->calls_share = 1;
	c->calls_poll = 10;
	c->calls_min_s = 60;
	/* in memory on a router (a reboot is itself in the story: the gap); on a PC, on disk */
	copy(c->why_file, sizeof(c->why_file), owrt ? "/tmp/aigw/agd-why.bin" : "/var/lib/aigw/agd-why.bin");
	c->mgmt_reset = 1;
	c->burst_window = 600;
	c->burst_min = 8;
	c->burst_share = 0.75;
	c->meter_enabled = 1;
	c->fed_enabled = 1;
	c->fed_noise = 1.0;
	c->fed_min_examples = 3;
	c->fleet = 1;
	c->fleet_share = 0;     /* opt-in: what leaves the home needs a yes (docs/fleet-learning.md) */
	c->fleet_k = 5;
	c->fleet_interval = 86400;
	c->fleet_noise = 1.0;
	c->fleet_cap = 50;
	c->fleet_strength = 20;
	c->fleet_choose = 1;
	c->fleet_margin = 0.15;
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
		note_unbuilt(c, ctx, p);
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
		v = opt(ctx, p, "wifi", "heal");
		if (v)
			c->heal = atoi(v) != 0;
		v = opt(ctx, p, "wifi", "heal_loss");
		if (v && atoi(v) >= 1 && atoi(v) <= 100)
			c->heal_loss = atoi(v);
		v = opt(ctx, p, "wifi", "heal_delay");
		if (v && atoi(v) >= 10)
			c->heal_delay = atoi(v);
		v = opt(ctx, p, "wifi", "heal_weak");
		if (v && atoi(v) < 0 && atoi(v) > -100)
			c->heal_weak = atoi(v);
		v = opt(ctx, p, "wifi", "heal_hops");
		if (v && atoi(v) >= 2)
			c->heal_hops = atoi(v);
		v = opt(ctx, p, "wifi", "heal_act");
		if (v)
			c->heal_act = atoi(v) != 0;
		v = opt(ctx, p, "wifi", "heal_hold");
		if (v && atoi(v) >= 10)
			c->heal_hold = atoi(v);
		v = opt(ctx, p, "wifi", "heal_trial");
		if (v && atoi(v) >= 10)
			c->heal_trial = atoi(v);
		v = opt(ctx, p, "wifi", "heal_retry");
		if (v && atoi(v) >= 10)
			c->heal_retry = atoi(v);
		v = opt(ctx, p, "wifi", "heal_weak_margin");
		if (v && atoi(v) >= 0 && atoi(v) <= 20)
			c->heal_weak_margin = atoi(v);
		v = opt(ctx, p, "diag", "targets");
		if (v)
			copy(c->diag_targets, sizeof(c->diag_targets), v);
		v = opt(ctx, p, "diag", "name");
		if (v)
			copy(c->diag_name, sizeof(c->diag_name), v);
		v = opt(ctx, p, "care", "enabled");
		if (v)
			c->care = atoi(v) != 0;
		v = opt(ctx, p, "care", "interval");
		if (v && atoi(v) >= 10 && atoi(v) <= 86400)
			c->care_interval = atoi(v);
		v = opt(ctx, p, "care", "recheck");
		if (v && atoi(v) >= 1 && atoi(v) <= 3600)
			c->care_recheck = atoi(v);
		v = opt(ctx, p, "care", "wifi_grace");
		if (v && atoi(v) >= 0 && atoi(v) <= 86400)
			c->care_wifi_grace = atoi(v);
		v = opt(ctx, p, "care", "warn");
		if (v)
			c->care_warn = atoi(v) != 0;
		v = opt(ctx, p, "care", "learn_days");
		if (v && atoi(v) >= 0 && atoi(v) <= 60)
			c->care_learn_days = atoi(v);
		v = opt(ctx, p, "care", "speed_url");
		if (v)
			copy(c->care_speed_url, sizeof(c->care_speed_url), v);
		v = opt(ctx, p, "care", "speed_hour");
		if (v && atoi(v) >= 0 && atoi(v) <= 23)
			c->care_speed_hour = atoi(v);
		v = opt(ctx, p, "care", "speed_seconds");
		if (v && atoi(v) >= 1 && atoi(v) <= 60)
			c->care_speed_seconds = atoi(v);
		v = opt(ctx, p, "care", "speed_mbps");
		if (v && atoi(v) >= 0 && atoi(v) <= 100000)
			c->care_speed_mbps = atoi(v);
		v = opt(ctx, p, "care", "speed_min_pct");
		if (v && atoi(v) >= 1 && atoi(v) <= 100)
			c->care_speed_min_pct = atoi(v);
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
		if (v && atoi(v) >= 1)
			c->chan_poll = atoi(v);
		v = opt(ctx, p, "wifi", "chan_scan_gap");
		if (v && atoi(v) >= 10)
			c->chan_scan_gap = atoi(v);
		{
			/* neighbour-aware channels: option, lowest value allowed, where */
			static const struct { const char *name; int min; size_t off; } plan[] = {
				{ "chan_plan", 0, offsetof(struct ag_config, chan_plan) },
				{ "chan_plan_days", 0, offsetof(struct ag_config, chan_plan_days) },
				{ "chan_plan_better", 10, offsetof(struct ag_config, chan_plan_better) },
				{ "chan_plan_hold", 1, offsetof(struct ag_config, chan_plan_hold) },
				{ "chan_plan_every", 1, offsetof(struct ag_config, chan_plan_every) },
				{ "chan_plan_gap", 0, offsetof(struct ag_config, chan_plan_gap) },
				{ "chan_plan_trial", 1, offsetof(struct ag_config, chan_plan_trial) },
				{ "chan_avoid", 0, offsetof(struct ag_config, chan_avoid) },
				{ "chan_learn_gap", 10, offsetof(struct ag_config, chan_learn_gap) },
				{ "chan_quiet", 0, offsetof(struct ag_config, chan_quiet) },
			};
			size_t k;

			for (k = 0; k < sizeof(plan) / sizeof(plan[0]); k++) {
				v = opt(ctx, p, "wifi", plan[k].name);
				if (v && atoi(v) >= plan[k].min)
					*(int *)((char *)c + plan[k].off) = k ? atoi(v) : atoi(v) != 0;
			}
			if (c->chan_plan_better > 90)
				c->chan_plan_better = 90;
		}
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
		copy(c->mgmt_serial, sizeof(c->mgmt_serial), opt(ctx, p, "mgmt", "serial"));
		copy(c->mgmt_oui, sizeof(c->mgmt_oui), opt(ctx, p, "mgmt", "oui"));
		copy(c->mgmt_prefix, sizeof(c->mgmt_prefix), opt(ctx, p, "mgmt", "vendor_prefix"));
		v = opt(ctx, p, "mgmt", "wifi");
		if (v)
			c->mgmt_wifi = atoi(v) != 0;
		v = opt(ctx, p, "mgmt", "firmware");
		if (v)
			c->mgmt_firmware = atoi(v) != 0;
		v = opt(ctx, p, "mgmt", "reset");
		if (v)
			c->mgmt_reset = atoi(v) != 0;
		copy(c->wifi_handler, sizeof(c->wifi_handler), opt(ctx, p, "wifi", "handler"));
		copy(c->fw_handler, sizeof(c->fw_handler), opt(ctx, p, "mgmt", "firmware_handler"));
		v = opt(ctx, p, "upgrade", "check");
		if (v)
			c->upgrade_check = atoi(v) != 0;
		v = opt(ctx, p, "upgrade", "rollback");
		if (v)
			c->upgrade_rollback = atoi(v) != 0;
		v = opt(ctx, p, "upgrade", "watch");
		if (v && atoi(v) >= 10 && atoi(v) <= 30 * 86400)
			c->upgrade_watch = atoi(v);
		v = opt(ctx, p, "upgrade", "reconnect");
		if (v && atoi(v) >= 5 && atoi(v) <= 86400)
			c->upgrade_reconnect = atoi(v);
		v = opt(ctx, p, "upgrade", "hold_days");
		if (v && atoi(v) >= 0 && atoi(v) <= 365)
			c->upgrade_hold_days = atoi(v);
		v = opt(ctx, p, "upgrade", "report_delay");
		if (v && atoi(v) >= 0 && atoi(v) <= 3600)
			c->upgrade_report_delay = atoi(v);
		v = opt(ctx, p, "upgrade", "file");
		if (v)
			copy(c->upgrade_file, sizeof(c->upgrade_file), v);
		v = opt(ctx, p, "why", "enabled");
		if (v)
			c->why = atoi(v) != 0;
		v = opt(ctx, p, "why", "hours");
		if (v && atoi(v) >= 1 && atoi(v) <= 168)
			c->why_hours = atoi(v);
		v = opt(ctx, p, "why", "interval");
		if (v && atoi(v) >= 2 && atoi(v) <= 600)
			c->why_interval = atoi(v);
		v = opt(ctx, p, "why", "probe");
		if (v)
			c->why_probe = atoi(v) != 0;
		v = opt(ctx, p, "why", "file");
		if (v)
			copy(c->why_file, sizeof(c->why_file), v);
		v = opt(ctx, p, "why", "wan");
		if (v)
			copy(c->why_wan, sizeof(c->why_wan), v);
		v = opt(ctx, p, "sqm", "enabled");
		if (v)
			c->sqm = atoi(v) != 0;
		v = opt(ctx, p, "sqm", "handler");
		if (v)
			copy(c->sqm_handler, sizeof(c->sqm_handler), v);
		v = opt(ctx, p, "sqm", "detect");
		if (v && atoi(v) >= 2 && atoi(v) <= 60)
			c->sqm_detect = atoi(v);
		v = opt(ctx, p, "sqm", "trial");
		if (v && atoi(v) >= 2 && atoi(v) <= 1440)
			c->sqm_trial = atoi(v);
		v = opt(ctx, p, "sqm", "hold");
		if (v && atoi(v) >= 0 && atoi(v) <= 30 * 86400)
			c->sqm_hold = atoi(v);
		v = opt(ctx, p, "calls", "enabled");
		if (v)
			c->calls = atoi(v) != 0;
		v = opt(ctx, p, "calls", "priority");
		if (v)
			c->calls_prio = atoi(v) != 0;
		v = opt(ctx, p, "calls", "share");
		if (v)
			c->calls_share = atoi(v) != 0;
		v = opt(ctx, p, "calls", "poll");
		if (v && atoi(v) >= 1 && atoi(v) <= 300)
			c->calls_poll = atoi(v);
		v = opt(ctx, p, "calls", "min_s");
		if (v && atoi(v) >= 0 && atoi(v) <= 86400)
			c->calls_min_s = atoi(v);
		v = opt(ctx, p, "fleet", "enabled");
		if (v)
			c->fleet = atoi(v) != 0;
		v = opt(ctx, p, "fleet", "share");
		if (v)
			c->fleet_share = atoi(v) != 0;
		v = opt(ctx, p, "fleet", "k");
		if (v && atoi(v) >= 2 && atoi(v) <= 10000)
			c->fleet_k = atoi(v);
		v = opt(ctx, p, "fleet", "interval");
		if (v && atoi(v) >= 1)
			c->fleet_interval = atoi(v);
		v = opt(ctx, p, "fleet", "line");
		if (v && ag_fleet_line_ok(v))
			copy(c->fleet_line, sizeof(c->fleet_line), v);
		v = opt(ctx, p, "fleet", "noise");
		if (v && atof(v) >= 0 && atof(v) <= 100)
			c->fleet_noise = atof(v);
		v = opt(ctx, p, "fleet", "cap");
		if (v && atoi(v) >= 1)
			c->fleet_cap = atoi(v);
		v = opt(ctx, p, "fleet", "strength");
		if (v && atof(v) >= 0 && atof(v) <= 10000)
			c->fleet_strength = atof(v);
		v = opt(ctx, p, "fleet", "choose");
		if (v)
			c->fleet_choose = atoi(v) != 0;
		v = opt(ctx, p, "fleet", "margin");
		if (v && atof(v) >= 0 && atof(v) <= 2)
			c->fleet_margin = atof(v);
		v = opt(ctx, p, "offers", "enabled");
		if (v)
			c->offers = atoi(v) != 0;
		v = opt(ctx, p, "offers", "share");
		if (v)
			c->offers_share = atoi(v) != 0;
		v = opt(ctx, p, "brief", "enabled");
		if (v)
			c->brief = atoi(v) != 0;
		v = opt(ctx, p, "brief", "names");
		if (v)
			c->brief_names = atoi(v) != 0;
		v = opt(ctx, p, "area", "enabled");
		if (v)
			c->area = atoi(v) != 0;
		copy(c->area_uplink_id, sizeof(c->area_uplink_id), opt(ctx, p, "area", "uplink_id"));
		v = opt(ctx, p, "proactive", "enabled");
		if (v)
			c->pro = atoi(v) != 0;
		v = opt(ctx, p, "proactive", "fix");
		if (v)
			c->pro_fix = atoi(v) != 0;
		v = opt(ctx, p, "proactive", "interval");
		if (v && atoi(v) >= 1)
			c->pro_interval = atoi(v);
		v = opt(ctx, p, "proactive", "threshold");
		if (v && atoi(v) >= 1)
			c->pro_threshold = atoi(v);
		v = opt(ctx, p, "proactive", "min_duration");
		if (v && atoi(v) >= 0)
			c->pro_min_s = atoi(v);
		v = opt(ctx, p, "proactive", "clear");
		if (v && atoi(v) >= 0)
			c->pro_clear_s = atoi(v);
		v = opt(ctx, p, "proactive", "cooldown");
		if (v && atoi(v) >= 0)
			c->pro_cooldown_s = atoi(v);
		v = opt(ctx, p, "proactive", "max_per_day");
		if (v && atoi(v) >= 0)
			c->pro_max_per_day = atoi(v);
		v = opt(ctx, p, "mesh", "enabled");
		if (v)
			c->mesh = atoi(v) != 0;
		copy(c->mesh_handler, sizeof(c->mesh_handler), opt(ctx, p, "mesh", "handler"));
		if (!strcmp(c->mesh_handler, "/usr/libexec/aigw/" MESH_HANDLER_OLD) ||
		    !strcmp(c->mesh_handler, "/usr/local/libexec/aigw/" MESH_HANDLER_OLD))
			c->mesh_handler[0] = '\0';      /* upgraded from 0.46 or older: the platform's handler now */
		copy(c->mesh_source, sizeof(c->mesh_source), opt(ctx, p, "mesh", "source"));
		v = opt(ctx, p, "mesh", "poll");
		if (v && atoi(v) >= 1 && atoi(v) <= 3600)
			c->mesh_poll = atoi(v);
		v = opt(ctx, p, "mesh", "steer");
		if (v)
			c->mesh_steer = atoi(v) != 0;
		v = opt(ctx, p, "mesh", "channel");
		if (v)
			c->mesh_channel = atoi(v) != 0;
		v = opt(ctx, p, "mesh", "trial");
		if (v && atoi(v) >= 1 && atoi(v) <= 3600)
			c->mesh_trial = atoi(v);
		v = opt(ctx, p, "mesh", "hold");
		if (v && atoi(v) >= 0 && atoi(v) <= 3600)
			c->mesh_hold = atoi(v);
		v = opt(ctx, p, "mesh", "legacy");
		if (v)
			c->mesh_legacy = atoi(v) != 0;
		v = opt(ctx, p, "mesh", "weak_dbm");
		if (v && atoi(v) <= -40 && atoi(v) >= -95)
			c->mesh_weak_dbm = atoi(v);
		v = opt(ctx, p, "mesh", "hysteresis");
		if (v && atoi(v) >= 3 && atoi(v) <= 30)
			c->mesh_hyst_db = atoi(v);
		v = opt(ctx, p, "mesh", "cooldown");
		if (v && atoi(v) >= 0)
			c->mesh_cooldown = atoi(v);
		v = opt(ctx, p, "mesh", "quiet");
		if (v && atoi(v) >= 0)
			c->mesh_quiet = atoi(v);
		read_list_sep(ctx, uci_lookup_section(ctx, p, "mesh"), "name", c->mesh_names, sizeof(c->mesh_names), ";");
		v = opt(ctx, p, "coverage", "enabled");
		if (v)
			c->cov = atoi(v) != 0;
		v = opt(ctx, p, "coverage", "interval");
		if (v && atoi(v) >= 1)
			c->cov_interval = atoi(v);
		v = opt(ctx, p, "coverage", "learn");
		if (v && atoi(v) >= 4)
			c->cov_learn = atoi(v);
		v = opt(ctx, p, "coverage", "share");
		if (v)
			c->cov_share = atoi(v) != 0;
		v = opt(ctx, p, "repair", "enabled");
		if (v)
			c->repair = atoi(v) != 0;
		v = opt(ctx, p, "repair", "window");
		if (v && atoi(v) >= 2 && atoi(v) <= 86400)
			c->repair_window = atoi(v);
		v = opt(ctx, p, "repair", "settle");
		if (v && atoi(v) >= 0 && atoi(v) <= 3600)
			c->repair_settle = atoi(v);
		v = opt(ctx, p, "repair", "undo");
		if (v)
			c->repair_undo = atoi(v) != 0;
		v = opt(ctx, p, "repair", "shadow");
		if (v)
			c->repair_shadow = atoi(v) != 0;
		v = opt(ctx, p, "repair", "gate");
		if (v)
			c->repair_gate = atoi(v) != 0;
		v = opt(ctx, p, "repair", "gate_fix_rate");
		if (v && atoi(v) >= 0 && atoi(v) <= 100)
			c->repair_gate_rate = atoi(v);
		v = opt(ctx, p, "repair", "gate_trials");
		if (v && atoi(v) >= 1 && atoi(v) <= 1000)
			c->repair_gate_trials = atoi(v);
		v = opt(ctx, p, "repair", "tick");
		if (v && atoi(v) >= 1 && atoi(v) <= 600)
			c->repair_tick = atoi(v);
		if (!AIGW_FEATURE_PROVEN)
			c->repair = 0;          /* not built in: other parts behave as without it */
		v = opt(ctx, p, "line", "enabled");
		if (v)
			c->line = atoi(v) != 0;
		v = opt(ctx, p, "line", "interval");
		if (v && atoi(v) >= 1 && atoi(v) <= 86400)
			c->line_interval = atoi(v);
		v = opt(ctx, p, "line", "share");
		if (v)
			c->line_share = atoi(v) != 0;
		copy(c->line_handler, sizeof(c->line_handler), opt(ctx, p, "line", "handler"));
		copy(c->mgmt_manufacturer, sizeof(c->mgmt_manufacturer), opt(ctx, p, "mgmt", "manufacturer"));
		copy(c->mgmt_model, sizeof(c->mgmt_model), opt(ctx, p, "mgmt", "model_name"));
		copy(c->mgmt_product, sizeof(c->mgmt_product), opt(ctx, p, "mgmt", "product_class"));
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
		/* Self-Healing Wi-Fi: a channel trial (kept when it measures better, else
		   back within the trial), and a device kept on 2.4 GHz for a day */
		add_policy(c, "wifi_congestion", "wifi-congestion", "channel", NULL, 3600, 0, AG_MODE_ENFORCE);
		add_policy(c, "wifi_hopping", "wifi-hopping", "stay24", NULL, 86400, 0, AG_MODE_ENFORCE);
		/* neighbour-aware channels: a channel learned clearly better over the busy hours,
		   a trial judged over them (kept, or back); a configuration without this policy
		   gets it all the same (act.c), unless wifi.chan_plan 0 */
		add_policy(c, "wifi_plan", "wifi-plan", "channel", NULL, 86400, 0, AG_MODE_ENFORCE);
		/* bufferbloat: smart queueing, judged on the timeline (kept when it helps); a configuration
		   without this policy gets it all the same (act.c), unless sqm.enabled 0 */
		add_policy(c, "bufferbloat", "bufferbloat", "sqm", NULL, 86400, 0, AG_MODE_ENFORCE);
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

/* "config <section> '<section>'" in /etc/config/aigw, made when a config from before it lacks it */
int ag_config_ensure(const char *section)
{
	const char *confdir = getenv("AIGW_UCI_CONFDIR");
	struct uci_context *ctx = uci_alloc_context();
	struct uci_ptr ptr = { 0 };
	char spec[128];
	int rc = -1;

	if (!ctx)
		return -1;
	if (confdir)
		uci_set_confdir(ctx, confdir);
	if (strpbrk(section, ".= \n'\""))
		goto out;
	snprintf(spec, sizeof(spec), "aigw.%s=%s", section, section);
	if (uci_lookup_ptr(ctx, &ptr, spec, true) != UCI_OK)
		goto out;
	if (ptr.s) {
		rc = 0;                 /* there already */
		goto out;
	}
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
