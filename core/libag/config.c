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
	}
	uci_free_context(ctx);
}

const char *ag_ubus_path(void)
{
	const char *p = getenv("AIGW_UBUS");

	return (p && *p) ? p : NULL;
}
