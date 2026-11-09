/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Decisions (phase 3, "act"): the brain turns alerts into actions -- safely.
 *
 *   alert ──► policy ──► proposal ──(you approve, or enforce mode)──► guardrails ──► actuator
 *                                                                       │
 *                                         every action has a TTL and an undo ◄──┘
 *
 * Actions, from mild to strong, and their actuators:
 *   priority <device>       IP in nft set priority4/6: DSCP CS5, the Wi-Fi (WMM) video queue
 *   deprioritize <device>   IP in nft set bulk4/6: DSCP CS1, the Wi-Fi background queue
 *   throttle <device> rate  IP in nft set throttle4/6 plus a per-device rate limit
 *   block <domain> [device] agdns answers "no such name" for it and its subdomains,
 *                           for every device or just one
 *   quarantine <device>     MAC in nft sets "inet ag quarantine" (routed traffic) and
 *                           "bridge ag quarantine" (LAN-to-LAN through a bridge): the
 *                           device still reaches the gateway (DHCP, DNS), nothing beyond
 *   disconnect <device>     hostapd: kicked off Wi-Fi and refused until the time is up
 *   channel <ap>:<channel>  an access point moves to a quieter channel (chan.c: hostapd
 *                           channel switch announcement); back to the old one at the end
 *   <plugin>.<action>       an actuator plug-in carries it out (plugin.c); same rules
 *
 * Policies come from /etc/config/aigw first, then from skills (plug-ins);
 * "option zone" limits a policy to devices in that zone.
 *
 * Guardrails, checked for every action whoever asks (a policy, you, later the cloud):
 *   - never the gateway itself, never a device outside the LAN, never a protected device
 *   - always a TTL (10 s .. max_ttl). nft timeouts and the block list's per-entry
 *     expiry end actions on their own, even if agd stops (Wi-Fi bans: agd lifts
 *     them, also after a restart)
 *   - automatic actions only in enforce mode, at most max_auto_per_hour, never "block"
 *   - DNS and DHCP to the gateway are never affected
 *   - only well-formed MACs/IPs ever reach nft and hostapd
 *
 * Modes: off (alerts only) · advise (default: proposals wait for approval) ·
 * enforce (policies act on their own, within the guardrails).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "agd.h"
#include "ag/domfeat.h"

#define DEC_MAX          128
#define PROPOSAL_SECS    86400       /* unanswered proposals lapse after a day */
#define REASSERT_SECS    60          /* re-apply active actions (nft reload, hostapd restart) */
#define MIN_TTL          10
#define MIN_RATE         64          /* kbit/s */
#define MAX_RATE         10000000

enum dec_state { D_PROPOSED, D_ACTIVE, D_REVERTED, D_EXPIRED, D_REJECTED, D_FAILED, D_LAPSED };
static const char *const state_names[] = {
	"proposed", "active", "reverted", "expired", "rejected", "failed", "lapsed",
};

struct decision {
	int used;
	uint32_t id;
	int state;
	char action[48];            /* built-in, or a plug-in's "<plugin>.<action>" */
	char target[NAME_LEN];      /* MAC (quarantine, disconnect), IP, or domain (block) */
	char scope[IP_LEN];         /* block: the one device's IP; "" = every device */
	int rate;                   /* throttle: kbit/s */
	char device[64];            /* who it is about, for people */
	char origin[48];            /* "you" or "policy <name>" */
	uint32_t alert_id;
	int ttl;
	int automatic;              /* applied without a person approving it */
	int64_t created, decided, applied, expires;
	char reason[400];
	char note[160];             /* guardrail adjustments, errors */
};

static struct decision decs[DEC_MAX];
static unsigned head;
static uint32_t next_id = 1;
static int64_t last_reassert;
static uint64_t applied_total;

static int is_device_action(const char *a)
{
	return !strcmp(a, "quarantine") || !strcmp(a, "priority") || !strcmp(a, "deprioritize") ||
	       !strcmp(a, "throttle") || !strcmp(a, "disconnect");
}

static int by_mac(const char *a)
{
	return !strcmp(a, "quarantine") || !strcmp(a, "disconnect");
}

/* carried out by an actuator plug-in: "<plugin>.<action>" */
static int is_plugin_action(const char *a)
{
	return strchr(a, '.') != NULL;
}

/* free-text targets of plug-in actions ("everyone", "kitchen light") */
static int safe_text(const char *s)
{
	size_t i, n = strlen(s);

	if (n < 1 || n > 64)
		return 0;
	for (i = 0; i < n; i++)
		if (!isalnum((unsigned char)s[i]) && !strchr(" -_.:@", s[i]))
			return 0;
	return 1;
}

/* ---------- small helpers ---------- */

static int in_words(const char *list, const char *s)
{
	char buf[512], *tok, *save = NULL;

	if (!s || !*s)
		return 0;
	snprintf(buf, sizeof(buf), "%s", list);
	for (tok = strtok_r(buf, " ,", &save); tok; tok = strtok_r(NULL, " ,", &save))
		if (!strcasecmp(tok, s))
			return 1;
	return 0;
}

/* the act.protect list: devices AIGW never acts on (also never band-steers) */
int act_is_protected(const struct device *d)
{
	return in_words(cfg.act_protect, d->mac) || in_words(cfg.act_protect, d->ip) ||
	       in_words(cfg.act_protect, d->hostname);
}

static struct decision *dec_get(uint32_t id)
{
	int i;

	for (i = 0; i < DEC_MAX; i++)
		if (decs[i].used && decs[i].id == id)
			return &decs[i];
	return NULL;
}

static struct decision *dec_find(const char *action, const char *target, const char *scope, int state)
{
	int i;

	for (i = 0; i < DEC_MAX; i++)
		if (decs[i].used && decs[i].state == state && !strcmp(decs[i].action, action) &&
		    !strcasecmp(decs[i].target, target) && !strcmp(decs[i].scope, scope))
			return &decs[i];
	return NULL;
}

static struct decision *dec_new(void)
{
	struct decision *d;
	int i;

	/* reuse the oldest finished slot; never overwrite something live */
	for (i = 0; i < DEC_MAX; i++) {
		d = &decs[(head + i) % DEC_MAX];
		if (!d->used || (d->state != D_ACTIVE && d->state != D_PROPOSED)) {
			head = (unsigned)((d - decs) + 1) % DEC_MAX;
			memset(d, 0, sizeof(*d));
			d->used = 1;
			d->id = next_id++;
			d->created = ag_wall_s();
			return d;
		}
	}
	return NULL;
}

static void dec_event(const struct decision *d)
{
	struct blob_buf b = { 0 };

	blob_buf_init(&b, 0);
	blobmsg_add_u32(&b, "id", d->id);
	blobmsg_add_string(&b, "action", d->action);
	blobmsg_add_string(&b, "target", d->target);
	blobmsg_add_string(&b, "scope", d->scope);
	blobmsg_add_u32(&b, "rate", (uint32_t)d->rate);
	blobmsg_add_string(&b, "device", d->device);
	blobmsg_add_string(&b, "state", state_names[d->state]);
	blobmsg_add_string(&b, "origin", d->origin);
	blobmsg_add_u32(&b, "ttl", (uint32_t)d->ttl);
	blobmsg_add_u64(&b, "expires", (uint64_t)d->expires);
	blobmsg_add_string(&b, "reason", d->reason);
	blobmsg_add_string(&b, "note", d->note);
	agd_emit(AG_EV_DECISION, &b);
	blob_buf_free(&b);
}

static void set_state(struct decision *d, int state)
{
	d->state = state;
	d->decided = ag_wall_s();
	AG_INFO("decision %u %s %s%s%s: %s%s%s", d->id, d->action, d->target, d->scope[0] ? " for " : "",
		d->scope, state_names[state], d->note[0] ? " -- " : "", d->note);
	dec_event(d);
}

/* ---------- actuators ---------- */

/* Runs "nft <command>" without a shell; returns its exit status. */
static int nft(const char *command, char *err, size_t errlen)
{
	const char *bin = getenv("AIGW_NFT");
	int pfd[2], status = -1;
	pid_t pid;
	ssize_t n;

	if (pipe(pfd) < 0) {
		snprintf(err, errlen, "pipe: %s", strerror(errno));
		return -1;
	}
	pid = fork();
	if (pid == 0) {
		int devnull = open("/dev/null", O_RDWR);

		dup2(devnull, 0);
		dup2(devnull, 1);
		dup2(pfd[1], 2);
		close(pfd[0]);
		execlp(bin && *bin ? bin : "nft", "nft", command, (char *)NULL);
		fprintf(stderr, "cannot run nft: %s", strerror(errno));
		_exit(127);
	}
	close(pfd[1]);
	if (pid < 0) {
		close(pfd[0]);
		snprintf(err, errlen, "fork: %s", strerror(errno));
		return -1;
	}
	n = read(pfd[0], err, errlen - 1);
	err[n > 0 ? n : 0] = '\0';
	err[strcspn(err, "\n")] = '\0';
	close(pfd[0]);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Targets come from DHCP/ARP data; only well-formed addresses ever reach nft or hostapd. */
static int valid_mac(const char *s)
{
	int i;

	for (i = 0; i < 17; i++)
		if (i % 3 == 2 ? s[i] != ':' : !isxdigit((unsigned char)s[i]))
			return 0;
	return s[17] == '\0';
}

static int valid_ip(const char *s)
{
	unsigned char buf[16];

	return inet_pton(AF_INET, s, buf) == 1 || inet_pton(AF_INET6, s, buf) == 1;
}

static struct device *find_device(const char *q);

static int valid_target(const struct decision *d)
{
	if (!strcmp(d->action, "channel")) {
		const struct wifi_ap *a;
		int ch;

		/* the access point may be away for a moment (hostapd restarting): judged when it is back */
		return chan_parse(d->target, &a, &ch) == 0 || (strchr(d->target, ':') && strlen(d->target) < 40);
	}
	if (!strcmp(d->action, "block"))
		return !d->scope[0] || valid_ip(d->scope);
	if (is_plugin_action(d->action))
		return valid_ip(d->target) || safe_text(d->target);
	return by_mac(d->action) ? valid_mac(d->target) : valid_ip(d->target);
}

/* nft set holding the target of an IP-based action */
static const char *ip_set(const struct decision *d, char *out, size_t n)
{
	const char *base = !strcmp(d->action, "priority") ? "priority" :
			   !strcmp(d->action, "deprioritize") ? "bulk" : "throttle";

	snprintf(out, n, "%s%s", base, strchr(d->target, ':') ? "6" : "4");
	return out;
}

/* Writes the active block list for agdns: "name expires [client-ip]" per
 * line. agdns re-reads it when it changes and ignores expired lines itself. */
static int dns_policy_write(char *err, size_t errlen)
{
	char tmp[300], dir[256], *slash;
	FILE *f;
	int i;

	if (!cfg.dns_policy[0]) {
		snprintf(err, errlen, "no dns_policy file configured");
		return -1;
	}
	snprintf(dir, sizeof(dir), "%s", cfg.dns_policy);
	slash = strrchr(dir, '/');
	if (slash && slash != dir) {
		*slash = '\0';
		mkdir(dir, 0755);
	}
	snprintf(tmp, sizeof(tmp), "%s.tmp", cfg.dns_policy);
	f = fopen(tmp, "w");
	if (!f) {
		snprintf(err, errlen, "%s: %s", tmp, strerror(errno));
		return -1;
	}
	fprintf(f, "# AIGW blocked names, written by agd: name expires-epoch [client-ip]\n");
	for (i = 0; i < DEC_MAX; i++)
		if (decs[i].used && decs[i].state == D_ACTIVE && !strcmp(decs[i].action, "block"))
			fprintf(f, "%s %lld%s%s\n", decs[i].target, (long long)decs[i].expires,
				decs[i].scope[0] ? " " : "", decs[i].scope);
	if (fclose(f) != 0 || rename(tmp, cfg.dns_policy) != 0) {
		snprintf(err, errlen, "%s: %s", cfg.dns_policy, strerror(errno));
		return -1;
	}
	return 0;
}

/* nft commands that rebuild the throttle chain from the active throttle
 * decisions. Each rule also requires the IP to be in the timed set, so a
 * rule goes inert by itself when its element expires. */
static void throttle_rules(char *cmd, size_t n)
{
	size_t len = strlen(cmd);
	int i;

	len += (size_t)snprintf(cmd + len, n - len, "flush chain inet ag throttle; ");
	for (i = 0; i < DEC_MAX && len < n; i++) {
		const struct decision *d = &decs[i];
		const char *fam;
		long bps, burst;
		char set[16];

		if (!d->used || d->state != D_ACTIVE || strcmp(d->action, "throttle") || !valid_ip(d->target))
			continue;
		fam = strchr(d->target, ':') ? "ip6" : "ip";
		bps = (long)d->rate * 1000 / 8;
		burst = bps / 4 > 16000 ? bps / 4 : 16000;
		ip_set(d, set, sizeof(set));
		len += (size_t)snprintf(cmd + len, n - len,
			"add rule inet ag throttle %s daddr @%s %s daddr %s limit rate over %ld bytes/second burst %ld bytes drop; "
			"add rule inet ag throttle %s saddr @%s %s saddr %s limit rate over %ld bytes/second burst %ld bytes drop; ",
			fam, set, fam, d->target, bps, burst, fam, set, fam, d->target, bps, burst);
	}
}

/* Puts the action into effect for its remaining lifetime. */
static int actuate(struct decision *d, char *err, size_t errlen)
{
	char cmd[8192], set[16];
	long left = (long)(d->expires - ag_wall_s());

	err[0] = '\0';
	if (left < 1)
		left = 1;
	if (!valid_target(d)) {
		snprintf(err, errlen, "malformed target '%.60s'", d->target);
		return -1;
	}
	if (!strcmp(d->action, "block"))
		return dns_policy_write(err, errlen);
	if (!strcmp(d->action, "disconnect"))
		return wifi_ban(d->target, 1, err, errlen);
	if (!strcmp(d->action, "channel")) {
		const char *colon = strrchr(d->target, ':');
		char iface[32];

		snprintf(iface, sizeof(iface), "%.*s", colon ? (int)(colon - d->target) : 0, d->target);
		return chan_switch(iface, colon ? atoi(colon + 1) : 0, err, errlen);
	}
	if (is_plugin_action(d->action))
		return plugin_act(d->action, d->id, d->target, valid_ip(d->target) ? find_device(d->target) : NULL,
				  (int)left, d->reason, 0, err, errlen);

	/* one nft transaction: (re)set the element with exactly the time it has left
	 * (a plain "add" would keep an existing element's old timeout) */
	if (!strcmp(d->action, "quarantine"))
		snprintf(set, sizeof(set), "quarantine");
	else
		ip_set(d, set, sizeof(set));
	snprintf(cmd, sizeof(cmd),
		 "add element inet ag %s { %s }; delete element inet ag %s { %s }; "
		 "add element inet ag %s { %s timeout %lds }; ",
		 set, d->target, set, d->target, set, d->target, left);
	if (!strcmp(d->action, "throttle"))
		throttle_rules(cmd, sizeof(cmd));
	if (nft(cmd, err, errlen) != 0) {
		if (!err[0])
			snprintf(err, errlen, "nft failed");
		return -1;
	}
	/* connections already in the fast path skip the rules: send them back (a no-op
	 * when none are offloaded, as on every re-check) */
	if (!strcmp(d->action, "quarantine"))
		fastpath_release_mac(d->target, d->action);
	else
		fastpath_release(d->target, d->action);
	if (!strcmp(d->action, "quarantine")) {
		/* LAN-to-LAN through a bridge; the table only exists where the kernel has nft bridge */
		snprintf(cmd, sizeof(cmd),
			 "add element bridge ag quarantine { %s }; delete element bridge ag quarantine { %s }; "
			 "add element bridge ag quarantine { %s timeout %lds }",
			 d->target, d->target, d->target, left);
		if (nft(cmd, set, sizeof(set)) != 0)
			AG_DEBUG("quarantine %s: no bridge table, routed traffic only", d->target);
	}
	return 0;
}

static void unactuate(struct decision *d)
{
	char cmd[8192], err[200], set[16];

	if (!valid_target(d))
		return;
	if (!strcmp(d->action, "block")) {
		if (dns_policy_write(err, sizeof(err)))
			AG_WARN("undo block %s: %s", d->target, err);
		return;
	}
	if (!strcmp(d->action, "disconnect")) {
		if (wifi_ban(d->target, 0, err, sizeof(err)))
			AG_DEBUG("undo disconnect %s: %s", d->target, err);
		return;
	}
	if (!strcmp(d->action, "channel")) {   /* back to the channel it had (kept in scope) */
		const char *colon = strrchr(d->target, ':');
		char iface[32];

		snprintf(iface, sizeof(iface), "%.*s", colon ? (int)(colon - d->target) : 0, d->target);
		if (atoi(d->scope) > 0 && chan_switch(iface, atoi(d->scope), err, sizeof(err)))
			AG_WARN("undo channel %s: %s", d->target, err);
		return;
	}
	if (is_plugin_action(d->action)) {
		if (plugin_act(d->action, d->id, d->target, valid_ip(d->target) ? find_device(d->target) : NULL,
			       0, d->reason, 1, err, sizeof(err)))
			AG_INFO("undo %s %s: %s", d->action, d->target, err);
		return;
	}
	if (!strcmp(d->action, "quarantine")) {
		snprintf(cmd, sizeof(cmd), "delete element bridge ag quarantine { %s }", d->target);
		nft(cmd, err, sizeof(err));
		snprintf(set, sizeof(set), "quarantine");
	} else {
		ip_set(d, set, sizeof(set));
	}
	/* fails harmlessly when the kernel already expired it */
	snprintf(cmd, sizeof(cmd), "delete element inet ag %s { %s }", set, d->target);
	if (nft(cmd, err, sizeof(err)) != 0)
		AG_DEBUG("undo %s %s: %s", d->action, d->target, err);
	if (!strcmp(d->action, "throttle")) {
		cmd[0] = '\0';
		throttle_rules(cmd, sizeof(cmd));
		if (nft(cmd, err, sizeof(err)) != 0)
			AG_WARN("throttle rules: %s", err);
	}
}

/* ---------- guardrails ---------- */

/* Resolves what a person typed (IP, MAC or name) to a device. */
static struct device *find_device(const char *q)
{
	struct device *d;
	int i;

	if ((d = dev_find_ip(q, 0)) || (d = dev_find_mac(q, 0)))
		return d;
	for (i = 0; i < DEV_MAX; i++)
		if (devs[i].used && devs[i].hostname[0] && !strcasecmp(devs[i].hostname, q))
			return &devs[i];
	return NULL;
}

static int normalize_domain(const char *in, char *out, size_t n)
{
	char reg[256];
	size_t i, len;

	snprintf(out, n, "%s", in);
	for (i = 0; out[i]; i++)
		out[i] = (char)tolower((unsigned char)out[i]);
	len = strlen(out);
	while (len && out[len - 1] == '.')
		out[--len] = '\0';
	for (i = 0; out[i]; i++)
		if (!isalnum((unsigned char)out[i]) && out[i] != '-' && out[i] != '.' && out[i] != '_')
			return -1;
	/* must be a real name: not a bare TLD or public suffix, not an address */
	if (ag_domain_registrable(out, reg, sizeof(reg)) == 0)
		return 0;
	/* home-network names ("nas.lan", "x.test") are fine too, with two labels or more */
	{
		static const char *const home_tld[] = {
			"lan", "home", "internal", "localdomain", "test", "corp", "private", "intranet", NULL
		};
		const char *dot = strrchr(out, '.');
		int k;

		if (!dot || dot == out)
			return -1;
		for (k = 0; home_tld[k]; k++)
			if (!strcmp(dot + 1, home_tld[k]))
				return 0;
	}
	return -1;
}

/* A device that may be acted on: known, on the LAN, not the gateway, not protected. */
static struct device *guard_device(const char *what, char *device, char *err, size_t errlen)
{
	struct device *dv = find_device(what);

	if (!dv) {
		if (is_local(what))
			snprintf(err, errlen, "%s is the gateway itself; AIGW never acts on the gateway", what);
		else
			snprintf(err, errlen, "no device '%s' (use its IP, MAC or name from 'agctl devices')", what);
		return NULL;
	}
	snprintf(device, 64, "%s", dv->hostname[0] ? dv->hostname : dv->mac[0] ? dv->mac : dv->ip);
	if (dv->ip[0] && is_local(dv->ip)) {
		snprintf(err, errlen, "%s is the gateway itself; AIGW never acts on the gateway", device);
		return NULL;
	}
	if (in_words(cfg.act_protect, dv->mac) || in_words(cfg.act_protect, dv->ip) ||
	    in_words(cfg.act_protect, dv->hostname)) {
		snprintf(err, errlen, "%s is protected (act.protect); AIGW never acts on it", device);
		return NULL;
	}
	if (!dv->ip[0] || !is_lan_client(dv->ip)) {
		snprintf(err, errlen, "%s is not on a LAN network AIGW looks after", device);
		return NULL;
	}
	return dv;
}

/*
 * Checks a requested action and fills in the decision's target, scope, rate
 * and device. Returns 0 when allowed, -1 with a plain-words reason when refused.
 */
static int guard(const char *action, const char *what, const struct act_opts *o, int *ttl,
		 struct decision *out, char *note, char *err, size_t errlen)
{
	struct device *dv;

	note[0] = '\0';
	out->scope[0] = '\0';
	out->rate = 0;
	if (*ttl <= 0)
		*ttl = cfg.act_ttl;
	if (*ttl < MIN_TTL)
		*ttl = MIN_TTL;
	if (*ttl > cfg.act_max_ttl) {
		snprintf(note, 160, "time limit capped at %ds (max_ttl)", cfg.act_max_ttl);
		*ttl = cfg.act_max_ttl;
	}

	if (!strcmp(action, "block")) {
		if (normalize_domain(what, out->target, sizeof(out->target)) < 0) {
			snprintf(err, errlen, "'%s' is not a domain AIGW can block (needs a registrable name)", what);
			return -1;
		}
		if (o && o->device && *o->device) {
			if (!(dv = guard_device(o->device, out->device, err, errlen)))
				return -1;
			snprintf(out->scope, sizeof(out->scope), "%s", dv->ip);
		} else {
			snprintf(out->device, sizeof(out->device), "every device");
		}
		return 0;
	}
	if (!strcmp(action, "channel")) {
		const struct wifi_ap *a;
		int ch;

		if (chan_parse(what, &a, &ch) < 0) {
			snprintf(err, errlen, "'%.40s' is not <access point or 2.4/5>:<channel> with a channel AIGW uses "
				 "(2.4 GHz 1-13; 5 GHz 36-48, 149-165)", what);
			return -1;
		}
		if (a->channel == ch) {
			snprintf(err, errlen, "%s is already on channel %d", a->iface, ch);
			return -1;
		}
		snprintf(out->target, sizeof(out->target), "%s:%d", a->iface, ch);
		snprintf(out->scope, sizeof(out->scope), "%d", a->channel);   /* to switch back to */
		snprintf(out->device, sizeof(out->device), "Wi-Fi %s GHz (%s), channel %d -> %d",
			 a->band == 5 ? "5" : "2.4", a->iface, a->channel, ch);
		return 0;
	}
	if (is_plugin_action(action)) {
		int device_subject = 1;

		if (plugin_action(action, &device_subject, err, errlen) < 0)
			return -1;
		if (!device_subject) {
			if (!safe_text(what)) {
				snprintf(err, errlen, "target '%.40s' must be 1-64 letters, digits, spaces or -_.:@", what);
				return -1;
			}
			snprintf(out->target, sizeof(out->target), "%s", what);
			snprintf(out->device, sizeof(out->device), "%s", what);
			return 0;
		}
		if (!(dv = guard_device(what, out->device, err, errlen)))
			return -1;
		snprintf(out->target, sizeof(out->target), "%s", dv->ip);
		return 0;
	}
	if (!is_device_action(action)) {
		snprintf(err, errlen, "unknown action '%s' (priority, deprioritize, throttle, block, "
			 "quarantine, disconnect, channel, or a plug-in's: agctl plugins)", action);
		return -1;
	}
	if (!(dv = guard_device(what, out->device, err, errlen)))
		return -1;
	if (by_mac(action)) {
		if (!dv->mac[0]) {
			snprintf(err, errlen, "%s has no known MAC address yet", out->device);
			return -1;
		}
		snprintf(out->target, sizeof(out->target), "%s", dv->mac);
	} else {
		snprintf(out->target, sizeof(out->target), "%s", dv->ip);
	}
	if (!strcmp(action, "throttle")) {
		out->rate = o && o->rate_kbps > 0 ? o->rate_kbps : cfg.act_rate_kbps;
		if (out->rate < MIN_RATE || out->rate > MAX_RATE) {
			snprintf(err, errlen, "rate %d kbit/s is outside %d kbit/s .. %d Gbit/s", out->rate,
				 MIN_RATE, MAX_RATE / 1000000);
			return -1;
		}
	}
	return 0;
}

static int auto_actions_last_hour(void)
{
	int64_t now = ag_wall_s();
	int i, n = 0;

	for (i = 0; i < DEC_MAX; i++)
		if (decs[i].used && decs[i].automatic && now - decs[i].applied < 3600)
			n++;
	return n;
}

static int apply(struct decision *d, int automatic, char *err, size_t errlen)
{
	d->applied = ag_wall_s();
	d->expires = d->applied + d->ttl;
	d->automatic = automatic;
	d->state = D_ACTIVE;            /* block list and throttle rules are built from active decisions */
	if (actuate(d, err, errlen) < 0) {
		snprintf(d->note, sizeof(d->note), "could not apply: %.140s", err);
		set_state(d, D_FAILED);
		if (!strcmp(d->action, "block"))
			dns_policy_write(err, errlen);
		return -1;
	}
	applied_total++;
	set_state(d, D_ACTIVE);
	return 0;
}

/* ---------- entry points ---------- */

int act_request(const char *action, const char *what, int ttl, const struct act_opts *o,
		const char *reason, uint32_t *id, char *err, size_t errlen)
{
	struct decision g = { 0 }, *d;
	const char *who = o && o->origin ? o->origin : "you";
	char note[160];

	if (guard(action, what, o, &ttl, &g, note, err, errlen) < 0) {
		AG_INFO("refused %s %s (asked by %s): %s", action, what, who, err);
		return -1;
	}
	if ((d = dec_find(action, g.target, g.scope, D_ACTIVE))) {
		/* asking again restarts its time limit (and sets a new rate) */
		*id = d->id;
		d->ttl = ttl;
		d->rate = g.rate;
		d->expires = ag_wall_s() + ttl;
		if (actuate(d, err, errlen) < 0)
			return -1;
		snprintf(d->note, sizeof(d->note), "time limit restarted by %.30s%s%.90s", who, note[0] ? "; " : "", note);
		set_state(d, D_ACTIVE);
		return 0;
	}
	if ((d = dec_find(action, g.target, g.scope, D_PROPOSED))) {
		/* asking for what AIGW proposed is approving it */
		snprintf(d->origin + strlen(d->origin), sizeof(d->origin) - strlen(d->origin), ", approved by %s", who);
	} else {
		d = dec_new();
		if (!d) {
			snprintf(err, errlen, "too many live decisions; undo or reject some first");
			return -1;
		}
		snprintf(d->action, sizeof(d->action), "%s", action);
		snprintf(d->target, sizeof(d->target), "%s", g.target);
		snprintf(d->scope, sizeof(d->scope), "%s", g.scope);
		snprintf(d->device, sizeof(d->device), "%s", g.device);
		snprintf(d->origin, sizeof(d->origin), "%s", who);
		if (reason && *reason)
			snprintf(d->reason, sizeof(d->reason), "%s", reason);
		else
			snprintf(d->reason, sizeof(d->reason), "asked for by %s", who);
	}
	d->ttl = ttl;
	d->rate = g.rate;
	snprintf(d->note, sizeof(d->note), "%s", note);
	*id = d->id;
	if (apply(d, 0, err, errlen) < 0)
		return -1;
	fed_on_answer(d->alert_id, 1);          /* asking for what was proposed: the alert was right */
	return 0;
}

int act_approve(uint32_t id, int ttl, char *err, size_t errlen)
{
	struct decision *d = dec_get(id), g = { 0 };
	struct act_opts o = { 0 };
	char note[160];

	if (!d || d->state != D_PROPOSED) {
		snprintf(err, errlen, "decision %u is not waiting for approval", id);
		return -1;
	}
	if (ttl <= 0)
		ttl = d->ttl;
	o.rate_kbps = d->rate;
	o.device = d->scope[0] ? d->scope : NULL;
	/* the device may have changed since the proposal: check again */
	if (guard(d->action, d->target, &o, &ttl, &g, note, err, errlen) < 0) {
		snprintf(d->note, sizeof(d->note), "refused: %.140s", err);
		set_state(d, D_REJECTED);
		return -1;
	}
	d->ttl = ttl;
	snprintf(d->origin + strlen(d->origin), sizeof(d->origin) - strlen(d->origin), ", approved by you");
	snprintf(d->note, sizeof(d->note), "%s", note);
	if (apply(d, 0, err, errlen) < 0)
		return -1;
	fed_on_answer(d->alert_id, 1);          /* approved: the alert was right (a learning example) */
	return 0;
}

int act_reject(uint32_t id, char *err, size_t errlen)
{
	struct decision *d = dec_get(id);

	if (!d || d->state != D_PROPOSED) {
		snprintf(err, errlen, "decision %u is not waiting for approval", id);
		return -1;
	}
	snprintf(d->note, sizeof(d->note), "rejected by you");
	set_state(d, D_REJECTED);
	fed_on_answer(d->alert_id, 0);          /* a false alarm */
	return 0;
}

int act_revert(uint32_t id, char *err, size_t errlen)
{
	struct decision *d = dec_get(id);

	if (!d || d->state != D_ACTIVE) {
		snprintf(err, errlen, "decision %u is not active", id);
		return -1;
	}
	d->state = D_REVERTED;          /* first, so rebuilt lists no longer have it */
	unactuate(d);
	snprintf(d->note, sizeof(d->note), "undone by you");
	set_state(d, D_REVERTED);
	if (d->automatic)
		fed_on_answer(d->alert_id, 0);  /* taken on its own, and you undid it: a false alarm */
	return 0;
}

/* your policies first, then skills' policies; a zone limits a policy to devices in it */
static const struct ag_policy *policy_for(const char *kind, const char *ip, char *origin, size_t n, int *cap)
{
	const struct device *dv = ip && *ip ? find_device(ip) : NULL;
	const char *zone = dv ? dv->zone : "";
	int i;

	*cap = AG_MODE_ENFORCE;
	for (i = 0; i < cfg.n_policies; i++)
		if (!strcmp(cfg.policies[i].alert, kind) && (!cfg.policies[i].zone[0] || !strcmp(cfg.policies[i].zone, zone))) {
			snprintf(origin, n, "policy %s", cfg.policies[i].name);
			return &cfg.policies[i];
		}
	return plugin_policy_for(kind, zone, origin, n, cap);
}

void act_on_alert(uint32_t alert_id, const char *kind, const char *ip, const char *reason)
{
	struct act_opts o = { 0 };
	struct decision g = { 0 }, *d;
	char note[160], err[200], origin[48];
	int mode, ttl, cap;
	const struct ag_policy *p = policy_for(kind, ip, origin, sizeof(origin), &cap);

	if (!p)
		return;
	mode = p->mode < cfg.act_mode ? p->mode : cfg.act_mode;
	if (mode > cap)
		mode = cap;
	if (mode == AG_MODE_OFF)
		return;
	ttl = p->ttl;
	o.rate_kbps = p->rate_kbps;
	if (guard(p->action, ip, &o, &ttl, &g, note, err, sizeof(err)) < 0) {
		AG_INFO("%s: not acting on alert %u: %s", origin, alert_id, err);
		return;
	}
	if (dec_find(p->action, g.target, g.scope, D_ACTIVE) || dec_find(p->action, g.target, g.scope, D_PROPOSED))
		return;     /* already handled */
	if (!strcmp(p->action, "channel")) {
		/* one channel decision per access point at a time, whichever channel */
		size_t n = (size_t)(strrchr(g.target, ':') - g.target) + 1;
		int i;

		for (i = 0; i < DEC_MAX; i++)
			if (decs[i].used && (decs[i].state == D_ACTIVE || decs[i].state == D_PROPOSED) &&
			    !strcmp(decs[i].action, "channel") && !strncmp(decs[i].target, g.target, n))
				return;
	}
	d = dec_new();
	if (!d)
		return;
	snprintf(d->action, sizeof(d->action), "%s", p->action);
	snprintf(d->target, sizeof(d->target), "%s", g.target);
	snprintf(d->scope, sizeof(d->scope), "%s", g.scope);
	snprintf(d->device, sizeof(d->device), "%s", g.device);
	snprintf(d->origin, sizeof(d->origin), "%s", origin);
	snprintf(d->reason, sizeof(d->reason), "%s", reason);
	snprintf(d->note, sizeof(d->note), "%s", note);
	d->alert_id = alert_id;
	d->ttl = ttl;
	d->rate = g.rate;

	if (mode == AG_MODE_ENFORCE) {
		if (auto_actions_last_hour() < cfg.act_max_auto) {
			apply(d, 1, err, sizeof(err));
			return;
		}
		snprintf(d->note, sizeof(d->note),
			 "enforce mode, but the hourly limit of %d automatic action%s is reached: waiting for you",
			 cfg.act_max_auto, cfg.act_max_auto == 1 ? "" : "s");
	}
	set_state(d, D_PROPOSED);
	d->decided = 0;
}

void act_init(void)
{
	char err[200];
	int i, n = 0;

	/* restored actions: put them back for the time they have left; lift
	 * Wi-Fi bans that ran out while agd was not running */
	for (i = 0; i < DEC_MAX; i++) {
		struct decision *d = &decs[i];

		if (!d->used || d->state != D_ACTIVE)
			continue;
		if (d->expires <= ag_wall_s()) {
			d->state = D_EXPIRED;
			if (!strcmp(d->action, "disconnect"))
				unactuate(d);
			continue;
		}
		if (is_plugin_action(d->action))
			continue;                /* sent to the plug-in once it has started */
		if (actuate(d, err, sizeof(err)) < 0)
			AG_WARN("could not restore decision %u (%s %s): %s", d->id, d->action, d->target, err);
		else
			n++;
	}
	if (dns_policy_write(err, sizeof(err)) < 0)
		AG_WARN("dns block list: %s", err);
	last_reassert = ag_wall_s();
	AG_INFO("acting in %s mode, %d policies, %d actions restored", ag_mode_str(cfg.act_mode),
		cfg.n_policies, n);
}

void act_tick(void)
{
	int64_t now = ag_wall_s();
	int i, reassert = now - last_reassert >= REASSERT_SECS;
	char err[200];

	if (reassert)
		last_reassert = now;
	for (i = 0; i < DEC_MAX; i++) {
		struct decision *d = &decs[i];

		if (!d->used)
			continue;
		if (d->state == D_ACTIVE && now >= d->expires) {
			d->state = D_EXPIRED;
			unactuate(d);
			snprintf(d->note, sizeof(d->note), "time limit reached");
			set_state(d, D_EXPIRED);
		} else if (d->state == D_ACTIVE && reassert && strcmp(d->action, "block") && !is_plugin_action(d->action)) {
			if (actuate(d, err, sizeof(err)) < 0)
				AG_WARN("decision %u: %s", d->id, err);
		} else if (d->state == D_PROPOSED && now - d->created >= PROPOSAL_SECS) {
			snprintf(d->note, sizeof(d->note), "not answered within a day");
			set_state(d, D_LAPSED);
		}
	}
}

int act_count(int active)
{
	int i, n = 0;

	for (i = 0; i < DEC_MAX; i++)
		if (decs[i].used && decs[i].state == (active ? D_ACTIVE : D_PROPOSED))
			n++;
	return n;
}

static void dec_blob(struct blob_buf *b, const struct decision *d)
{
	blobmsg_add_u32(b, "id", d->id);
	blobmsg_add_string(b, "state", state_names[d->state]);
	blobmsg_add_string(b, "action", d->action);
	blobmsg_add_string(b, "target", d->target);
	blobmsg_add_string(b, "scope", d->scope);
	blobmsg_add_u32(b, "rate", (uint32_t)d->rate);
	blobmsg_add_string(b, "device", d->device);
	blobmsg_add_string(b, "origin", d->origin);
	blobmsg_add_u32(b, "alert", d->alert_id);
	blobmsg_add_u32(b, "ttl", (uint32_t)d->ttl);
	blobmsg_add_u8(b, "automatic", (uint8_t)d->automatic);
	blobmsg_add_u64(b, "created", (uint64_t)d->created);
	blobmsg_add_u64(b, "decided", (uint64_t)d->decided);
	blobmsg_add_u64(b, "applied", (uint64_t)d->applied);
	blobmsg_add_u64(b, "expires", (uint64_t)d->expires);
	blobmsg_add_string(b, "reason", d->reason);
	blobmsg_add_string(b, "note", d->note);
}

static int live(const struct decision *d)
{
	return d->state == D_PROPOSED || d->state == D_ACTIVE;
}

static int by_live_then_newest(const void *a, const void *b)
{
	const struct decision *x = *(const struct decision *const *)a, *y = *(const struct decision *const *)b;

	if (live(x) != live(y))
		return live(y) - live(x);
	return x->id < y->id ? 1 : x->id > y->id ? -1 : 0;
}

static int by_newest(const void *a, const void *b)
{
	const struct decision *x = *(const struct decision *const *)a, *y = *(const struct decision *const *)b;

	return x->id < y->id ? 1 : x->id > y->id ? -1 : 0;
}

static void blob_sorted(struct blob_buf *b, unsigned limit, const char *name,
			int (*cmp)(const void *, const void *))
{
	const struct decision *list[DEC_MAX];
	unsigned i, n = 0;
	void *arr, *t;

	for (i = 0; i < DEC_MAX; i++)
		if (decs[i].used)
			list[n++] = &decs[i];
	qsort(list, n, sizeof(list[0]), cmp);
	arr = blobmsg_open_array(b, name);
	for (i = 0; i < n && i < limit; i++) {
		t = blobmsg_open_table(b, NULL);
		dec_blob(b, list[i]);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, arr);
}

/* live ones first (waiting for you, in effect), then history; newest first */
void act_blob(struct blob_buf *b, unsigned limit, const char *name)
{
	blob_sorted(b, limit, name, by_live_then_newest);
}

void act_save(struct blob_buf *b)
{
	blob_sorted(b, DEC_MAX, "decisions", by_newest);
}

static uint64_t num(struct blob_attr *a)
{
	switch (blobmsg_type(a)) {
	case BLOBMSG_TYPE_INT64: return blobmsg_get_u64(a);
	case BLOBMSG_TYPE_INT32: return blobmsg_get_u32(a);
	case BLOBMSG_TYPE_INT8:  return blobmsg_get_u8(a);
	default: return 0;
	}
}

void act_load(struct blob_attr *arr)
{
	struct blob_attr *e, *cur;
	size_t rem, r2;
	int n = 0;

	blobmsg_for_each_attr(e, arr, rem) {
		struct decision t = { 0 }, *d;
		int k;

		blobmsg_for_each_attr(cur, e, r2) {
			const char *key = blobmsg_name(cur);
			const char *s = blobmsg_type(cur) == BLOBMSG_TYPE_STRING ? blobmsg_get_string(cur) : "";

			if (!strcmp(key, "id")) t.id = (uint32_t)num(cur);
			else if (!strcmp(key, "state")) {
				for (k = 0; k < (int)(sizeof(state_names) / sizeof(state_names[0])); k++)
					if (!strcmp(s, state_names[k]))
						t.state = k;
			}
			else if (!strcmp(key, "action")) snprintf(t.action, sizeof(t.action), "%s", s);
			else if (!strcmp(key, "target")) snprintf(t.target, sizeof(t.target), "%s", s);
			else if (!strcmp(key, "scope")) snprintf(t.scope, sizeof(t.scope), "%s", s);
			else if (!strcmp(key, "rate")) t.rate = (int)num(cur);
			else if (!strcmp(key, "device")) snprintf(t.device, sizeof(t.device), "%s", s);
			else if (!strcmp(key, "origin")) snprintf(t.origin, sizeof(t.origin), "%s", s);
			else if (!strcmp(key, "alert")) t.alert_id = (uint32_t)num(cur);
			else if (!strcmp(key, "ttl")) t.ttl = (int)num(cur);
			else if (!strcmp(key, "automatic")) t.automatic = (int)num(cur);
			else if (!strcmp(key, "created")) t.created = (int64_t)num(cur);
			else if (!strcmp(key, "decided")) t.decided = (int64_t)num(cur);
			else if (!strcmp(key, "applied")) t.applied = (int64_t)num(cur);
			else if (!strcmp(key, "expires")) t.expires = (int64_t)num(cur);
			else if (!strcmp(key, "reason")) snprintf(t.reason, sizeof(t.reason), "%s", s);
			else if (!strcmp(key, "note")) snprintf(t.note, sizeof(t.note), "%s", s);
		}
		if (!t.id || !t.action[0] || !t.target[0] || n >= DEC_MAX)
			continue;
		d = &decs[n++];
		*d = t;
		d->used = 1;
		if (d->id >= next_id)
			next_id = d->id + 1;
	}
	/* saved newest first: reverse to keep oldest .. newest, then continue after the newest */
	for (int i = 0, j = n - 1; i < j; i++, j--) {
		struct decision tmp = decs[i];

		decs[i] = decs[j];
		decs[j] = tmp;
	}
	head = (unsigned)n % DEC_MAX;
	AG_INFO("restored %d decisions", n);
}

uint64_t act_applied_total(void)
{
	return applied_total;
}

struct device *act_find_device(const char *q)
{
	return q && *q ? find_device(q) : NULL;
}

/* A plug-in suggests an action: it always waits for a person. */
int act_propose(const char *action, const char *what, int ttl, const char *origin, const char *reason,
		uint32_t *id, char *err, size_t errlen)
{
	struct act_opts o = { 0 };
	struct decision g = { 0 }, *d;
	char note[160];

	if (cfg.act_mode == AG_MODE_OFF) {
		snprintf(err, errlen, "acting is off (act.mode)");
		return -1;
	}
	if (guard(action, what, &o, &ttl, &g, note, err, errlen) < 0)
		return -1;
	if ((d = dec_find(action, g.target, g.scope, D_ACTIVE)) || (d = dec_find(action, g.target, g.scope, D_PROPOSED))) {
		*id = d->id;
		return 0;                    /* already in effect or waiting */
	}
	if (!(d = dec_new())) {
		snprintf(err, errlen, "too many live decisions");
		return -1;
	}
	snprintf(d->action, sizeof(d->action), "%s", action);
	snprintf(d->target, sizeof(d->target), "%s", g.target);
	snprintf(d->scope, sizeof(d->scope), "%s", g.scope);
	snprintf(d->device, sizeof(d->device), "%s", g.device);
	snprintf(d->origin, sizeof(d->origin), "%s", origin);
	snprintf(d->reason, sizeof(d->reason), "%s", reason && *reason ? reason : "suggested by a plug-in");
	snprintf(d->note, sizeof(d->note), "%s", note);
	d->ttl = ttl;
	d->rate = g.rate;
	*id = d->id;
	set_state(d, D_PROPOSED);
	d->decided = 0;
	return 0;
}

static int owned_by(const struct decision *d, const char *plugin)
{
	size_t n = strlen(plugin);

	return !strncmp(d->action, plugin, n) && d->action[n] == '.';
}

/* An actuator plug-in reports how an action went. */
void act_plugin_result(uint32_t id, int ok, const char *error)
{
	struct decision *d = dec_get(id);

	if (!d || d->state != D_ACTIVE || !is_plugin_action(d->action))
		return;
	if (!ok) {
		snprintf(d->note, sizeof(d->note), "plug-in could not do it: %.130s", error && *error ? error : "no reason given");
		set_state(d, D_FAILED);
	} else {
		AG_DEBUG("decision %u: plug-in confirmed", id);
	}
}

/* A plug-in (re)started: give it the actions it should be carrying out. */
void act_plugin_started(const char *plugin)
{
	char err[200];
	int i;

	for (i = 0; i < DEC_MAX; i++) {
		struct decision *d = &decs[i];

		if (d->used && d->state == D_ACTIVE && owned_by(d, plugin) && d->expires > ag_wall_s() &&
		    actuate(d, err, sizeof(err)) < 0)
			AG_WARN("decision %u: %s", d->id, err);
	}
}

/* A plug-in was removed, turned off or failed: its actions can't be carried out or undone. */
void act_plugin_gone(const char *plugin)
{
	int i;

	for (i = 0; i < DEC_MAX; i++) {
		struct decision *d = &decs[i];

		if (!d->used || !owned_by(d, plugin))
			continue;
		if (d->state == D_ACTIVE) {
			snprintf(d->note, sizeof(d->note), "plug-in %s stopped; its action may still be in effect", plugin);
			set_state(d, D_FAILED);
		} else if (d->state == D_PROPOSED) {
			snprintf(d->note, sizeof(d->note), "plug-in %s stopped", plugin);
			set_state(d, D_LAPSED);
		}
	}
}
