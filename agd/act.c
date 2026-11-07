/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Decisions (phase 3, "act"): the brain turns alerts into actions -- safely.
 *
 *   alert ──► policy ──► proposal ──(you approve, or enforce mode)──► guardrails ──► actuator
 *                                                                       │
 *                                         every action has a TTL and an undo ◄──┘
 *
 * Actions and their actuators:
 *   quarantine <device>   MAC into nft set "inet ag quarantine": the device can
 *                         still reach the gateway (DHCP, DNS) but nothing beyond it
 *   priority <device>     IP into nft set "priority4/6": its traffic is marked
 *                         DSCP CS5, which Wi-Fi (WMM) sends in the video queue
 *   block <domain>        agdns answers "no such name" for it and its subdomains
 *
 * Guardrails, checked for every action whoever asks (a policy, you, later the cloud):
 *   - never the gateway itself, never a device outside the LAN, never a protected device
 *   - always a TTL (10 s .. max_ttl); the kernel and agdns expire actions on their own,
 *     so an action ends on time even if agd stops
 *   - automatic actions only in enforce mode, at most max_auto_per_hour, and never "block"
 *   - DNS and DHCP to the gateway are never affected (quarantine is a forwarding rule)
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
#define REASSERT_SECS    60          /* re-add active actions in case the nft table was reloaded */
#define MIN_TTL          10

enum dec_state { D_PROPOSED, D_ACTIVE, D_REVERTED, D_EXPIRED, D_REJECTED, D_FAILED, D_LAPSED };
static const char *const state_names[] = {
	"proposed", "active", "reverted", "expired", "rejected", "failed", "lapsed",
};

struct decision {
	int used;
	uint32_t id;
	int state;
	char action[16];
	char target[NAME_LEN];      /* MAC (quarantine), IP (priority) or domain (block) */
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

static struct decision *dec_get(uint32_t id)
{
	int i;

	for (i = 0; i < DEC_MAX; i++)
		if (decs[i].used && decs[i].id == id)
			return &decs[i];
	return NULL;
}

static struct decision *dec_find(const char *action, const char *target, int state)
{
	int i;

	for (i = 0; i < DEC_MAX; i++)
		if (decs[i].used && decs[i].state == state && !strcmp(decs[i].action, action) &&
		    !strcasecmp(decs[i].target, target))
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
	AG_INFO("decision %u %s %s: %s%s%s", d->id, d->action, d->target, state_names[state],
		d->note[0] ? " -- " : "", d->note);
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

/* Targets come from DHCP/ARP data; only well-formed addresses ever reach nft. */
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

static const char *prio_set(const char *ip)
{
	return strchr(ip, ':') ? "priority6" : "priority4";
}

/* Writes the active block list for agdns: "name expires" per line. agdns
 * re-reads it when it changes and ignores expired lines by itself. */
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
	fprintf(f, "# AIGW blocked names, written by agd: name expires-epoch\n");
	for (i = 0; i < DEC_MAX; i++)
		if (decs[i].used && decs[i].state == D_ACTIVE && !strcmp(decs[i].action, "block"))
			fprintf(f, "%s %lld\n", decs[i].target, (long long)decs[i].expires);
	if (fclose(f) != 0 || rename(tmp, cfg.dns_policy) != 0) {
		snprintf(err, errlen, "%s: %s", cfg.dns_policy, strerror(errno));
		return -1;
	}
	return 0;
}

/* Puts the action into effect for its remaining lifetime. */
static int actuate(struct decision *d, char *err, size_t errlen)
{
	char cmd[1024];
	long left = (long)(d->expires - ag_wall_s());

	err[0] = '\0';
	if (left < 1)
		left = 1;
	/* one nft transaction: (re)set the element with exactly the time it has left
	 * (a plain "add" would keep an existing element's old timeout) */
	if (!strcmp(d->action, "quarantine") || !strcmp(d->action, "priority")) {
		const char *set = !strcmp(d->action, "quarantine") ? "quarantine" : prio_set(d->target);

		if (!strcmp(d->action, "quarantine") ? !valid_mac(d->target) : !valid_ip(d->target)) {
			snprintf(err, errlen, "malformed target '%.60s'", d->target);
			return -1;
		}
		snprintf(cmd, sizeof(cmd),
			 "add element inet ag %s { %s }; delete element inet ag %s { %s }; "
			 "add element inet ag %s { %s timeout %lds }",
			 set, d->target, set, d->target, set, d->target, left);
	} else if (!strcmp(d->action, "block"))
		return dns_policy_write(err, errlen);
	else {
		snprintf(err, errlen, "unknown action");
		return -1;
	}
	if (nft(cmd, err, errlen) != 0) {
		if (!err[0])
			snprintf(err, errlen, "nft failed");
		return -1;
	}
	return 0;
}

static void unactuate(struct decision *d)
{
	char cmd[300], err[200];

	if ((!strcmp(d->action, "quarantine") && !valid_mac(d->target)) ||
	    (!strcmp(d->action, "priority") && !valid_ip(d->target)))
		return;
	if (!strcmp(d->action, "quarantine"))
		snprintf(cmd, sizeof(cmd), "delete element inet ag quarantine { %s }", d->target);
	else if (!strcmp(d->action, "priority"))
		snprintf(cmd, sizeof(cmd), "delete element inet ag %s { %s }", prio_set(d->target), d->target);
	else {
		if (!strcmp(d->action, "block") && dns_policy_write(err, sizeof(err)))
			AG_WARN("undo block %s: %s", d->target, err);
		return;
	}
	/* fails harmlessly when the kernel already expired it */
	if (nft(cmd, err, sizeof(err)) != 0)
		AG_DEBUG("undo %s %s: %s", d->action, d->target, err);
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

/*
 * Checks a requested action and fills in its target. Returns 0 when allowed,
 * -1 with a plain-words reason when refused.
 */
static int guard(const char *action, const char *what, int *ttl, char *target, char *device,
		 char *note, char *err, size_t errlen)
{
	struct device *dv;

	note[0] = '\0';
	if (*ttl <= 0)
		*ttl = cfg.act_ttl;
	if (*ttl < MIN_TTL)
		*ttl = MIN_TTL;
	if (*ttl > cfg.act_max_ttl) {
		snprintf(note, 160, "time limit capped at %ds (max_ttl)", cfg.act_max_ttl);
		*ttl = cfg.act_max_ttl;
	}

	if (!strcmp(action, "block")) {
		if (normalize_domain(what, target, NAME_LEN) < 0) {
			snprintf(err, errlen, "'%s' is not a domain AIGW can block (needs a registrable name)", what);
			return -1;
		}
		snprintf(device, 64, "every device");
		return 0;
	}
	if (strcmp(action, "quarantine") && strcmp(action, "priority")) {
		snprintf(err, errlen, "unknown action '%s' (quarantine, priority or block)", action);
		return -1;
	}

	dv = find_device(what);
	if (!dv) {
		if (is_local(what)) {
			snprintf(err, errlen, "%s is the gateway itself; AIGW never acts on the gateway", what);
			return -1;
		}
		snprintf(err, errlen, "no device '%s' (use its IP, MAC or name from 'agctl devices')", what);
		return -1;
	}
	snprintf(device, 64, "%s", dv->hostname[0] ? dv->hostname : dv->mac[0] ? dv->mac : dv->ip);
	if (dv->ip[0] && is_local(dv->ip)) {
		snprintf(err, errlen, "%s is the gateway itself; AIGW never acts on the gateway", device);
		return -1;
	}
	if (in_words(cfg.act_protect, dv->mac) || in_words(cfg.act_protect, dv->ip) ||
	    in_words(cfg.act_protect, dv->hostname)) {
		snprintf(err, errlen, "%s is protected (act.protect); AIGW never acts on it", device);
		return -1;
	}
	if (!dv->ip[0] || !is_lan_client(dv->ip)) {
		snprintf(err, errlen, "%s is not on a LAN network AIGW looks after", device);
		return -1;
	}
	if (!strcmp(action, "quarantine")) {
		if (!dv->mac[0]) {
			snprintf(err, errlen, "%s has no known MAC address yet, so it can't be quarantined", device);
			return -1;
		}
		snprintf(target, NAME_LEN, "%s", dv->mac);
	} else {
		snprintf(target, NAME_LEN, "%s", dv->ip);
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
	d->state = D_ACTIVE;            /* the block list is written from active decisions */
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

int act_request(const char *action, const char *what, int ttl, const char *reason,
		uint32_t *id, char *err, size_t errlen)
{
	char target[NAME_LEN], device[64], note[160];
	struct decision *d;

	if (guard(action, what, &ttl, target, device, note, err, errlen) < 0) {
		AG_INFO("refused %s %s: %s", action, what, err);
		return -1;
	}
	if ((d = dec_find(action, target, D_ACTIVE))) {
		/* asking again restarts its time limit */
		*id = d->id;
		d->ttl = ttl;
		d->expires = ag_wall_s() + ttl;
		if (actuate(d, err, errlen) < 0)
			return -1;
		snprintf(d->note, sizeof(d->note), "time limit restarted by you%s%.120s", note[0] ? "; " : "", note);
		set_state(d, D_ACTIVE);
		return 0;
	}
	if ((d = dec_find(action, target, D_PROPOSED))) {
		/* asking for what AIGW proposed is approving it */
		d->ttl = ttl;
		snprintf(d->origin + strlen(d->origin), sizeof(d->origin) - strlen(d->origin), ", approved by you");
	} else {
		d = dec_new();
		if (!d) {
			snprintf(err, errlen, "too many live decisions; undo or reject some first");
			return -1;
		}
		snprintf(d->action, sizeof(d->action), "%s", action);
		snprintf(d->target, sizeof(d->target), "%s", target);
		snprintf(d->device, sizeof(d->device), "%s", device);
		snprintf(d->origin, sizeof(d->origin), "you");
		snprintf(d->reason, sizeof(d->reason), "%s", reason && *reason ? reason : "asked for by you");
		d->ttl = ttl;
	}
	snprintf(d->note, sizeof(d->note), "%s", note);
	*id = d->id;
	return apply(d, 0, err, errlen);
}

int act_approve(uint32_t id, int ttl, char *err, size_t errlen)
{
	struct decision *d = dec_get(id);
	char target[NAME_LEN], device[64], note[160];

	if (!d || d->state != D_PROPOSED) {
		snprintf(err, errlen, "decision %u is not waiting for approval", id);
		return -1;
	}
	if (ttl <= 0)
		ttl = d->ttl;
	/* the device may have changed since the proposal: check again */
	if (guard(d->action, d->target, &ttl, target, device, note, err, errlen) < 0) {
		snprintf(d->note, sizeof(d->note), "refused: %s", err);
		set_state(d, D_REJECTED);
		return -1;
	}
	d->ttl = ttl;
	snprintf(d->origin + strlen(d->origin), sizeof(d->origin) - strlen(d->origin), ", approved by you");
	snprintf(d->note, sizeof(d->note), "%s", note);
	return apply(d, 0, err, errlen);
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
	return 0;
}

int act_revert(uint32_t id, char *err, size_t errlen)
{
	struct decision *d = dec_get(id);

	if (!d || d->state != D_ACTIVE) {
		snprintf(err, errlen, "decision %u is not active", id);
		return -1;
	}
	d->state = D_REVERTED;          /* first, so the block list no longer has it */
	unactuate(d);
	snprintf(d->note, sizeof(d->note), "undone by you");
	set_state(d, D_REVERTED);
	return 0;
}

static const struct ag_policy *policy_for(const char *kind)
{
	int i;

	for (i = 0; i < cfg.n_policies; i++)
		if (!strcmp(cfg.policies[i].alert, kind))
			return &cfg.policies[i];
	return NULL;
}

void act_on_alert(uint32_t alert_id, const char *kind, const char *ip, const char *reason)
{
	const struct ag_policy *p = policy_for(kind);
	char target[NAME_LEN], device[64], note[160], err[200];
	int mode, ttl;
	struct decision *d;

	if (!p)
		return;
	mode = p->mode < cfg.act_mode ? p->mode : cfg.act_mode;
	if (mode == AG_MODE_OFF)
		return;
	ttl = p->ttl;
	if (guard(p->action, ip, &ttl, target, device, note, err, sizeof(err)) < 0) {
		AG_INFO("policy %s: not acting on alert %u: %s", p->name, alert_id, err);
		return;
	}
	if (dec_find(p->action, target, D_ACTIVE) || dec_find(p->action, target, D_PROPOSED))
		return;     /* already handled */
	d = dec_new();
	if (!d)
		return;
	snprintf(d->action, sizeof(d->action), "%s", p->action);
	snprintf(d->target, sizeof(d->target), "%s", target);
	snprintf(d->device, sizeof(d->device), "%s", device);
	snprintf(d->origin, sizeof(d->origin), "policy %s", p->name);
	snprintf(d->reason, sizeof(d->reason), "%s", reason);
	snprintf(d->note, sizeof(d->note), "%s", note);
	d->alert_id = alert_id;
	d->ttl = ttl;

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

	/* restored actions: put them back for the time they have left */
	for (i = 0; i < DEC_MAX; i++) {
		struct decision *d = &decs[i];

		if (!d->used || d->state != D_ACTIVE)
			continue;
		if (d->expires <= ag_wall_s()) {
			d->state = D_EXPIRED;
			continue;
		}
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
		} else if (d->state == D_ACTIVE && reassert && strcmp(d->action, "block")) {
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
	/* saved newest first, so the slot after them holds the oldest-to-reuse order:
	 * reverse to keep oldest .. newest, then continue after the newest */
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
