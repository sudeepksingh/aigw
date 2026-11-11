/* SPDX-License-Identifier: Apache-2.0 */
/*
 * What a management system (or agctl) changes on the gateway itself:
 *
 *   Wi-Fi settings   name, passphrase, security mode, channel, width. Checked
 *                    here; saved and applied by the Wi-Fi handler, one call
 *                    per access point with everything one request changed.
 *                    Until the access point comes back with them, the data
 *                    model shows the new values.
 *   Firmware         the images the firmware handler knows (a running one and
 *                    a slot for the next), download into a slot, activate it.
 *
 * Both handlers are replaceable (handler.c), so an OEM's own Wi-Fi stack or
 * dual-bank flash fits without changing agd.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libubox/uloop.h>

#include "agd.h"

/* ---------- Wi-Fi settings ---------- */

#define WS_HOLD 90                      /* seconds a staged value shows before the AP must have it */

static const char *const ws_env[WS_N] = {
	"AIGW_WIFI_SSID", "AIGW_WIFI_KEY", "AIGW_WIFI_MODE", "AIGW_WIFI_CHANNEL", "AIGW_WIFI_WIDTH",
};

static struct {
	char iface[24];
	char val[WS_N][72];
	unsigned set;                   /* staged in this request, not yet given to the handler */
	unsigned shown;                 /* given to the handler: shown until the AP has them */
	time_t since;
} ws[WIFI_AP_MAX];

static int ws_slot(const char *iface, int create)
{
	int i, free_i = -1;

	for (i = 0; i < WIFI_AP_MAX; i++) {
		if (!strcmp(ws[i].iface, iface))
			return i;
		if (!ws[i].iface[0] && free_i < 0)
			free_i = i;
	}
	if (!create || free_i < 0)
		return -1;
	snprintf(ws[free_i].iface, sizeof(ws[free_i].iface), "%s", iface);
	return free_i;
}

const char *const wifi_modes[] = { "None", "WPA2-Personal", "WPA3-Personal", "WPA2-WPA3-Personal", NULL };

/* the security mode hostapd runs with, as TR-181 names it */
void wifi_mode(const char *iface, char *buf, size_t n)
{
	char reply[1024], *km;
	int sae, psk;

	snprintf(buf, n, "%s", "");
	if (wifi_cmd(iface, "GET_CONFIG", reply, sizeof(reply)) < 0)
		return;
	km = strstr(reply, "key_mgmt=");
	sae = km && strstr(km, "SAE") != NULL;
	psk = km && strstr(km, "WPA-PSK") != NULL;
	snprintf(buf, n, "%s", sae && psk ? "WPA2-WPA3-Personal" : sae ? "WPA3-Personal" : psk ? "WPA2-Personal" : "None");
}

static int printable(const char *v, size_t min, size_t max)
{
	size_t i, l = strlen(v);

	if (l < min || l > max)
		return 0;
	for (i = 0; i < l; i++)
		if ((unsigned char)v[i] < 0x20 || (unsigned char)v[i] == 0x7f)
			return 0;
	return 1;
}

int wifiset_check(const char *iface, int what, const char *v, char *err, size_t en)
{
	const struct wifi_ap *ap = wifi_ap_find(iface);
	size_t i, l = strlen(v);
	char *end;
	long ch;

	if (!ap) {
		snprintf(err, en, "no access point %s", iface);
		return -1;
	}
	switch (what) {
	case WS_SSID:
		if (!printable(v, 1, 32)) {
			snprintf(err, en, "SSID is 1 to 32 characters");
			return -1;
		}
		return 0;
	case WS_KEY:
		if (l == 64) {
			for (i = 0; i < l; i++)
				if (!isxdigit((unsigned char)v[i]))
					break;
			if (i == l)
				return 0;
		}
		for (i = 0; i < l; i++)
			if ((unsigned char)v[i] < 0x20 || (unsigned char)v[i] > 0x7e)
				break;
		if (l < 8 || l > 63 || i < l) {
			snprintf(err, en, "KeyPassphrase is 8 to 63 printable ASCII characters (or 64 hex digits)");
			return -1;
		}
		return 0;
	case WS_MODE:
		for (i = 0; wifi_modes[i]; i++)
			if (!strcmp(v, wifi_modes[i]))
				return 0;
		snprintf(err, en, "ModeEnabled is None, WPA2-Personal, WPA3-Personal or WPA2-WPA3-Personal");
		return -1;
	case WS_CHANNEL:
		ch = strtol(v, &end, 10);
		if (*end || !chan_valid(ap->band, (int)ch)) {
			snprintf(err, en, "channel %s is not one AIGW uses on %s GHz", v, ap->band == 5 ? "5" : "2.4");
			return -1;
		}
		return 0;
	case WS_WIDTH:
		if (!strcmp(v, "20MHz") || !strcmp(v, "40MHz") || !strcmp(v, "Auto") ||
		    (!strcmp(v, "80MHz") && ap->band == 5))
			return 0;
		snprintf(err, en, "OperatingChannelBandwidth is 20MHz, 40MHz, %sor Auto", ap->band == 5 ? "80MHz " : "");
		return -1;
	}
	snprintf(err, en, "not a Wi-Fi setting");
	return -1;
}

void wifiset_stage(const char *iface, int what, const char *v)
{
	int s = ws_slot(iface, 1);

	if (s < 0 || what < 0 || what >= WS_N)
		return;
	snprintf(ws[s].val[what], sizeof(ws[s].val[what]), "%s", v);
	ws[s].set |= 1u << what;
}

struct ws_job {
	char iface[24];
	char who[48];
};

static void ws_done(void *ctx, int code, const char *msg)
{
	struct ws_job *j = ctx;

	if (code == 0)
		AG_INFO("wifi: %s settings applied (asked by %s)%s%s", j->iface, j->who, msg[0] ? ": " : "", msg);
	else
		AG_WARN("wifi: %s settings not applied (exit %d)%s%s", j->iface, code, msg[0] ? ": " : "", msg);
	if (code != 0) {
		int s = ws_slot(j->iface, 0);

		if (s >= 0)
			ws[s].shown = 0;        /* show what really runs */
	}
	free(j);
}

const char *wifi_handler(void)
{
	return handler_path(cfg.wifi_handler, "wifi-config");
}

void wifiset_flush(const char *who)
{
	const char *prog = wifi_handler();
	int s, w;

	for (s = 0; s < WIFI_AP_MAX; s++) {
		char envs[WS_N][100], *env[WS_N + 1], *argv[4];
		struct ws_job *j;
		int ne = 0;

		if (!ws[s].set)
			continue;
		for (w = 0; w < WS_N; w++)
			if (ws[s].set & (1u << w)) {
				snprintf(envs[ne], sizeof(envs[ne]), "%s=%s", ws_env[w], ws[s].val[w]);
				env[ne] = envs[ne];
				ne++;
			}
		env[ne] = NULL;
		argv[0] = (char *)prog;
		argv[1] = "set";
		argv[2] = ws[s].iface;
		argv[3] = NULL;
		ws[s].shown |= ws[s].set;
		ws[s].since = time(NULL);
		ws[s].set = 0;
		if (!prog || !(j = calloc(1, sizeof(*j)))) {
			AG_WARN("wifi: no Wi-Fi handler: %s settings not applied", ws[s].iface);
			ws[s].shown = 0;
			continue;
		}
		snprintf(j->iface, sizeof(j->iface), "%.23s", ws[s].iface);
		snprintf(j->who, sizeof(j->who), "%.47s", who);
		AG_INFO("wifi: applying new settings to %s (asked by %s)", ws[s].iface, who);
		if (handler_start(prog, argv, env, ws_done, j) < 0) {
			AG_WARN("wifi: cannot run %s", prog);
			ws[s].shown = 0;
			free(j);
		}
	}
}

void wifiset_drop(void)
{
	int s;

	for (s = 0; s < WIFI_AP_MAX; s++)
		ws[s].set = 0;
}

/* a value given to the handler and not yet seen running (for WS_HOLD seconds), else NULL */
const char *wifiset_pending(const char *iface, int what)
{
	int s = ws_slot(iface, 0);

	if (s < 0 || !(ws[s].shown & (1u << what)))
		return NULL;
	if (time(NULL) - ws[s].since > WS_HOLD) {
		ws[s].shown = 0;
		return NULL;
	}
	return ws[s].val[what];
}

/* the access point now runs with v: stop showing the staged value */
void wifiset_seen(const char *iface, int what, const char *v)
{
	int s = ws_slot(iface, 0);

	if (s >= 0 && (ws[s].shown & (1u << what)) && !strcmp(ws[s].val[what], v))
		ws[s].shown &= ~(1u << what);
}

/* ---------- firmware ---------- */

#define FW_CACHE 3                      /* seconds a listing is reused */

static struct fw_image fw_cache[FW_MAX];
static int fw_n = -1;
static time_t fw_when;
static uint32_t fw_busy;                /* the image being downloaded or activated */
static char fw_busy_state[24];

const char *fw_handler(void)
{
	return handler_path(cfg.fw_handler, "firmware");
}

/*
 * "firmware list": one line per image,
 *   <id> <status> <active 0|1> <boot 0|1> <version> [name...]
 */
int fw_list(struct fw_image *out, int max)
{
	const char *prog = fw_handler();
	char buf[2048], *line, *save;
	char *argv[] = { (char *)prog, "list", NULL };
	int i;

	if (fw_n >= 0 && time(NULL) - fw_when < FW_CACHE)
		goto copy;
	fw_n = 0;
	fw_when = time(NULL);
	if (prog && handler_run(prog, argv, buf, sizeof(buf), 5000) == 0)
		for (line = strtok_r(buf, "\n", &save); line && fw_n < FW_MAX; line = strtok_r(NULL, "\n", &save)) {
			struct fw_image *f = &fw_cache[fw_n];
			int off = 0;

			memset(f, 0, sizeof(*f));
			if (sscanf(line, "%u %23s %d %d %63s %n", &f->id, f->status, &f->active, &f->boot, f->version, &off) < 5 ||
			    !f->id)
				continue;
			if (!strcmp(f->version, "-"))
				f->version[0] = '\0';
			snprintf(f->name, sizeof(f->name), "%s", off ? line + off : "");
			fw_n++;
		}
copy:
	for (i = 0; i < fw_n && i < max; i++) {
		out[i] = fw_cache[i];
		if (fw_busy && out[i].id == fw_busy)
			snprintf(out[i].status, sizeof(out[i].status), "%s", fw_busy_state);
	}
	return i;
}

int fw_find(uint32_t id, struct fw_image *f)
{
	struct fw_image all[FW_MAX];
	int i, n = fw_list(all, FW_MAX);

	for (i = 0; i < n; i++)
		if (all[i].id == id) {
			if (f)
				*f = all[i];
			return 0;
		}
	return -1;
}

int fw_ids(uint32_t *out, int max)
{
	struct fw_image all[FW_MAX];
	int i, n = fw_list(all, FW_MAX);

	for (i = 0; i < n && i < max; i++)
		out[i] = all[i].id;
	return i;
}

static char fw_ver[96];
static time_t fw_ver_when;

/* the running firmware's version (the handler's "version"), "" when there is no handler */
void fw_version(char *buf, size_t n)
{
	const char *prog = fw_handler();
	char *argv[] = { (char *)prog, "version", NULL };
	struct fw_image all[FW_MAX];
	int i, k = fw_list(all, FW_MAX);

	/* the running image's, as listed (follows a switch at once) */
	for (i = 0; i < k; i++)
		if (all[i].active && all[i].version[0]) {
			snprintf(buf, n, "%s", all[i].version);
			return;
		}
	if (!fw_ver[0] || time(NULL) - fw_ver_when > 60) {
		fw_ver[0] = '\0';
		fw_ver_when = time(NULL);
		if (!prog || handler_run(prog, argv, fw_ver, sizeof(fw_ver), 5000) != 0)
			fw_ver[0] = '\0';
		fw_ver[strcspn(fw_ver, "\r\n")] = '\0';
	}
	snprintf(buf, n, "%s", fw_ver);
}

/* after an activation or a reboot: ask the handler again */
static void fw_forget(void)
{
	fw_n = -1;
	fw_ver[0] = '\0';
}

struct fw_job {
	uint32_t id;
	int activate, reboot;
	char who[48];
	fw_done_t done;
	void *ctx;
};

static struct uloop_timeout reboot_timer;

static void rebooted(void *ctx, int code, const char *msg)
{
	(void)ctx;
	fw_forget();                    /* a handler may "reboot" without stopping agd (tests) */
	if (code)
		AG_WARN("firmware: reboot failed (exit %d): %s", code, msg);
}

static void do_reboot(struct uloop_timeout *t)
{
	const char *prog = fw_handler();
	char *argv[] = { (char *)prog, "reboot", NULL };

	(void)t;
	AG_INFO("firmware: rebooting into the new image");
	if (prog && handler_start(prog, argv, NULL, rebooted, NULL) < 0)
		AG_WARN("firmware: cannot run %s reboot", prog);
}

static void fw_finish(struct fw_job *j, int ok, const char *msg)
{
	fw_busy = 0;
	fw_forget();                    /* list again */
	if (j->done)
		j->done(j->ctx, ok, msg);
	if (ok && j->activate && j->reboot) {
		/* a moment for the answer to reach whoever asked */
		reboot_timer.cb = do_reboot;
		uloop_timeout_set(&reboot_timer, 3000);
	}
	free(j);
}

static void fw_activated(void *ctx, int code, const char *msg)
{
	struct fw_job *j = ctx;

	if (code == 0)
		AG_INFO("firmware: image %u activated (asked by %s)", j->id, j->who);
	else
		AG_WARN("firmware: image %u not activated (exit %d): %s", j->id, code, msg);
	fw_finish(j, code == 0, code == 0 ? "" : msg[0] ? msg : "activation failed");
}

static int fw_start_activate(struct fw_job *j)
{
	const char *prog = fw_handler();
	char id[16], *argv[] = { (char *)prog, "activate", id, NULL };

	snprintf(id, sizeof(id), "%u", j->id);
	fw_busy = j->id;
	snprintf(fw_busy_state, sizeof(fw_busy_state), "Available");
	return prog ? handler_start(prog, argv, NULL, fw_activated, j) : -1;
}

static void fw_downloaded(void *ctx, int code, const char *msg)
{
	struct fw_job *j = ctx;

	if (code != 0) {
		AG_WARN("firmware: download into image %u failed (exit %d): %s", j->id, code, msg);
		fw_finish(j, 0, msg[0] ? msg : code == 3 ? "the image did not pass its checks" : "download failed");
		return;
	}
	AG_INFO("firmware: image %u downloaded and checked (asked by %s)", j->id, j->who);
	if (j->activate) {
		if (fw_start_activate(j) == 0)
			return;
		fw_finish(j, 0, "cannot run the firmware handler");
		return;
	}
	fw_finish(j, 1, "");
}

int fw_download(uint32_t id, const struct fw_request *r, const char *who, fw_done_t done, void *ctx,
		char *err, size_t en)
{
	const char *prog = fw_handler();
	char sid[16], e_user[160], e_pass[160], e_size[48], e_alg[48], e_sum[160];
	char *argv[] = { (char *)prog, "download", sid, (char *)r->url, NULL };
	char *env[] = { e_user, e_pass, e_size, e_alg, e_sum, NULL };
	struct fw_image f;
	struct fw_job *j;

	if (!prog) {
		snprintf(err, en, "no firmware handler on this gateway (mgmt.firmware_handler)");
		return -1;
	}
	if (fw_busy) {
		snprintf(err, en, "image %u is busy (a download or activation is under way)", fw_busy);
		return -1;
	}
	if (fw_find(id, &f) < 0) {
		snprintf(err, en, "no firmware image %u", id);
		return -1;
	}
	if (f.active) {
		snprintf(err, en, "image %u is the one running: download into another", id);
		return -1;
	}
	if (!r->url || (strncmp(r->url, "http://", 7) && strncmp(r->url, "https://", 8) && strncmp(r->url, "file://", 7))) {
		snprintf(err, en, "URL is http://, https:// or file://");
		return -1;
	}
	if (!(j = calloc(1, sizeof(*j))))
		return -1;
	j->id = id;
	j->activate = r->activate;
	j->reboot = r->reboot;
	j->done = done;
	j->ctx = ctx;
	snprintf(j->who, sizeof(j->who), "%s", who);
	snprintf(sid, sizeof(sid), "%u", id);
	snprintf(e_user, sizeof(e_user), "AIGW_FW_USER=%s", r->user ? r->user : "");
	snprintf(e_pass, sizeof(e_pass), "AIGW_FW_PASSWORD=%s", r->password ? r->password : "");
	snprintf(e_size, sizeof(e_size), "AIGW_FW_SIZE=%s", r->size ? r->size : "");
	snprintf(e_alg, sizeof(e_alg), "AIGW_FW_CHECKSUM_ALGORITHM=%s", r->alg ? r->alg : "");
	snprintf(e_sum, sizeof(e_sum), "AIGW_FW_CHECKSUM=%s", r->sum ? r->sum : "");
	fw_busy = id;
	snprintf(fw_busy_state, sizeof(fw_busy_state), "Downloading");
	AG_INFO("firmware: downloading into image %u (asked by %s)", id, who);
	if (handler_start(prog, argv, env, fw_downloaded, j) < 0) {
		fw_busy = 0;
		free(j);
		snprintf(err, en, "cannot run %s", prog);
		return -1;
	}
	return 0;
}

int fw_activate(uint32_t id, int reboot, const char *who, fw_done_t done, void *ctx, char *err, size_t en)
{
	struct fw_image f;
	struct fw_job *j;

	if (!fw_handler()) {
		snprintf(err, en, "no firmware handler on this gateway (mgmt.firmware_handler)");
		return -1;
	}
	if (fw_busy) {
		snprintf(err, en, "image %u is busy (a download or activation is under way)", fw_busy);
		return -1;
	}
	if (fw_find(id, &f) < 0) {
		snprintf(err, en, "no firmware image %u", id);
		return -1;
	}
	if (strcmp(f.status, "Available") && strcmp(f.status, "Active")) {
		snprintf(err, en, "image %u is %s: nothing to activate", id, f.status);
		return -1;
	}
	if (!(j = calloc(1, sizeof(*j))))
		return -1;
	j->id = id;
	j->activate = 1;
	j->reboot = reboot;
	j->done = done;
	j->ctx = ctx;
	snprintf(j->who, sizeof(j->who), "%s", who);
	AG_INFO("firmware: activating image %u (asked by %s)", id, who);
	if (fw_start_activate(j) < 0) {
		fw_busy = 0;
		free(j);
		snprintf(err, en, "cannot run the firmware handler");
		return -1;
	}
	return 0;
}
