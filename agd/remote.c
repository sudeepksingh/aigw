/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Remote access (phase 5): your phone reaches home over WireGuard, and its
 * lookups go through AIGW wherever it is.
 *
 *   agctl remote on <endpoint>   the address phones use to reach this home
 *                                (a DDNS name or public IP; forward UDP 51820
 *                                to the gateway if it sits behind a router)
 *   agctl remote add <name>      a peer: a QR code for the WireGuard app
 *   agctl remote revoke <name>
 *
 * A peer's private key is made, shown once in the QR code and forgotten:
 * only its public key stays (in <remote.dir>/peers). The gateway's key is
 * <remote.dir>/server.key, mode 600. aigw-remote does the kernel side (the
 * WireGuard interface, DNS redirect to agdns, forwarding and NAT); agd keeps
 * the peers and shows who is connected. Tunnel clients count as part of the
 * home: alerts, baselines and decisions apply to them like to any device.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <uci.h>

#include "agd.h"

#define PEER_MAX  64
#define KEY_LEN   45                /* base64 of 32 bytes + NUL */

struct peer {
	char name[32];
	char pub[KEY_LEN];
	char ip[16];
	int64_t created;
	/* from "wg show <iface> dump" */
	int64_t handshake;
	char endpoint[64];
	uint64_t rx, tx;
};

static struct peer peers[PEER_MAX];
static int npeers, running;
static char server_pub[KEY_LEN], last_error[200];
static int64_t last_poll;

static const char *tool(void)
{
	const char *t = getenv("AIGW_REMOTE");

	return t && *t ? t : "aigw-remote";
}

/* run argv; feed `in` on stdin, the first line of stdout into out. 0 on exit status 0 */
static int run(char *const argv[], const char *in, char *out, size_t on)
{
	int pin[2], pout[2], status = -1;
	pid_t pid;

	if (pipe(pin))
		return -1;
	if (pipe(pout)) {
		close(pin[0]);
		close(pin[1]);
		return -1;
	}
	pid = fork();
	if (pid == 0) {
		dup2(pin[0], 0);
		dup2(pout[1], 1);
		close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pin[0]);
	close(pout[1]);
	if (in && pid > 0 && write(pin[1], in, strlen(in)) < 0)
		AG_DEBUG("remote: write to %s failed", argv[0]);
	close(pin[1]);
	if (out && on) {
		ssize_t n = pid > 0 ? read(pout[0], out, on - 1) : -1;

		out[n > 0 ? n : 0] = '\0';
		out[strcspn(out, "\r\n")] = '\0';
	}
	close(pout[0]);
	if (pid < 0)
		return -1;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int keypair(char priv[KEY_LEN], char pub[KEY_LEN])
{
	char *gen[] = { "wg", "genkey", NULL }, *pk[] = { "wg", "pubkey", NULL }, in[KEY_LEN + 2];

	if (run(gen, NULL, priv, KEY_LEN) || strlen(priv) != 44)
		return -1;
	snprintf(in, sizeof(in), "%s\n", priv);
	return run(pk, in, pub, KEY_LEN) || strlen(pub) != 44 ? -1 : 0;
}

static void path(const char *file, char *out, size_t n)
{
	snprintf(out, n, "%s/%s", cfg.remote_dir, file);
}

/* the tunnel network: base address, prefix, gateway (.1) */
static int subnet(struct in_addr *base, int *prefix, char gw[16])
{
	char net[32], *slash;
	uint32_t mask, b;

	snprintf(net, sizeof(net), "%s", cfg.remote_subnet);
	slash = strchr(net, '/');
	*prefix = slash ? atoi(slash + 1) : 24;
	if (slash)
		*slash = '\0';
	if (inet_pton(AF_INET, net, base) != 1 || *prefix < 16 || *prefix > 29)
		return -1;
	mask = htonl(0xffffffffu << (32 - *prefix));
	base->s_addr &= mask;
	b = ntohl(base->s_addr) + 1;
	snprintf(gw, 16, "%u.%u.%u.%u", b >> 24, (b >> 16) & 255, (b >> 8) & 255, b & 255);
	return 0;
}

static int server_key(char priv[KEY_LEN], char *err, size_t n)
{
	char file[300];
	FILE *f;

	path("server.key", file, sizeof(file));
	if ((f = fopen(file, "r"))) {
		if (!fgets(priv, KEY_LEN, f))
			priv[0] = '\0';
		fclose(f);
		priv[strcspn(priv, "\r\n")] = '\0';
	} else {
		char pub[KEY_LEN];
		int fd;

		if (keypair(priv, pub)) {
			snprintf(err, n, "cannot make keys: is wireguard-tools (wg) installed?");
			return -1;
		}
		fd = open(file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0 || write(fd, priv, strlen(priv)) < 0 || write(fd, "\n", 1) < 0) {
			snprintf(err, n, "cannot write %s: %s", file, strerror(errno));
			if (fd >= 0)
				close(fd);
			return -1;
		}
		close(fd);
	}
	if (strlen(priv) != 44) {
		snprintf(err, n, "%s is not a WireGuard key", file);
		return -1;
	}
	{
		char *pk[] = { "wg", "pubkey", NULL }, in[KEY_LEN + 2];

		snprintf(in, sizeof(in), "%s\n", priv);
		if (run(pk, in, server_pub, sizeof(server_pub))) {
			snprintf(err, n, "wg pubkey failed: is wireguard-tools (wg) installed?");
			return -1;
		}
	}
	return 0;
}

static void load_peers(void)
{
	char file[300], line[256];
	FILE *f;

	npeers = 0;
	path("peers", file, sizeof(file));
	if (!(f = fopen(file, "r")))
		return;
	while (npeers < PEER_MAX && fgets(line, sizeof(line), f)) {
		struct peer *p = &peers[npeers];
		char *name = strtok(line, "\t\n"), *pub = strtok(NULL, "\t\n"), *ip = strtok(NULL, "\t\n"),
		     *ts = strtok(NULL, "\t\n");

		if (!name || !pub || !ip || strlen(pub) != 44)
			continue;
		memset(p, 0, sizeof(*p));
		snprintf(p->name, sizeof(p->name), "%s", name);
		snprintf(p->pub, sizeof(p->pub), "%s", pub);
		snprintf(p->ip, sizeof(p->ip), "%s", ip);
		p->created = ts ? atoll(ts) : 0;
		npeers++;
	}
	fclose(f);
}

static int write_files(char *err, size_t n)
{
	char priv[KEY_LEN], file[300], tmp[320];
	FILE *f;
	int i, fd;

	if (server_key(priv, err, n))
		return -1;
	/* peers: names and public keys only */
	path("peers", file, sizeof(file));
	snprintf(tmp, sizeof(tmp), "%s.new", file);
	if (!(f = fopen(tmp, "w"))) {
		snprintf(err, n, "cannot write %s: %s", tmp, strerror(errno));
		return -1;
	}
	for (i = 0; i < npeers; i++)
		fprintf(f, "%s\t%s\t%s\t%lld\n", peers[i].name, peers[i].pub, peers[i].ip, (long long)peers[i].created);
	if (fclose(f) || rename(tmp, file)) {
		snprintf(err, n, "cannot write %s", file);
		return -1;
	}
	/* the interface configuration for "wg setconf" (has the private key: 600) */
	path("wg.conf", file, sizeof(file));
	snprintf(tmp, sizeof(tmp), "%s.new", file);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0 || !(f = fdopen(fd, "w"))) {
		if (fd >= 0)
			close(fd);
		snprintf(err, n, "cannot write %s", tmp);
		return -1;
	}
	fprintf(f, "[Interface]\nPrivateKey = %s\nListenPort = %d\n", priv, cfg.remote_port);
	for (i = 0; i < npeers; i++)
		fprintf(f, "\n[Peer]\nPublicKey = %s\nAllowedIPs = %s/32\n", peers[i].pub, peers[i].ip);
	memset(priv, 0, sizeof(priv));
	if (fclose(f) || rename(tmp, file)) {
		snprintf(err, n, "cannot write %s", file);
		return -1;
	}
	return 0;
}

static int bring_up(char *err, size_t n)
{
	struct in_addr base;
	char gw[16], addr[24], net[32], port[8], dns[8], conf[300];
	int prefix;
	char *argv[10];

	if (subnet(&base, &prefix, gw)) {
		snprintf(err, n, "remote.subnet must be an IPv4 network /16 to /29, e.g. 10.77.0.0/24");
		return -1;
	}
	if (mkdir(cfg.remote_dir, 0700) && errno != EEXIST) {
		snprintf(err, n, "cannot create %s: %s", cfg.remote_dir, strerror(errno));
		return -1;
	}
	chmod(cfg.remote_dir, 0700);
	load_peers();
	if (write_files(err, n))
		return -1;
	snprintf(addr, sizeof(addr), "%s/%d", gw, prefix);
	snprintf(net, sizeof(net), "%s/%d", inet_ntoa(base), prefix);
	snprintf(port, sizeof(port), "%d", cfg.remote_port);
	snprintf(dns, sizeof(dns), "%d", cfg.dns_port);
	path("wg.conf", conf, sizeof(conf));
	argv[0] = (char *)tool(); argv[1] = "up"; argv[2] = cfg.remote_iface; argv[3] = addr; argv[4] = net;
	argv[5] = port; argv[6] = conf; argv[7] = dns; argv[8] = NULL;
	if (run(argv, NULL, NULL, 0)) {
		snprintf(err, n, "%s up failed (installed? on OpenWRT: package aigw-remote; WireGuard in the kernel?)",
			 tool());
		return -1;
	}
	running = 1;
	/* tunnel clients are part of the home for every sensor */
	if (!strstr(cfg.lan_ifaces, cfg.remote_iface)) {
		char both[sizeof(cfg.lan_ifaces)];

		if ((size_t)snprintf(both, sizeof(both), "%s%s%s", cfg.lan_ifaces, cfg.lan_ifaces[0] ? " " : "",
				     cfg.remote_iface) < sizeof(both))
			memcpy(cfg.lan_ifaces, both, sizeof(both));
	}
	AG_INFO("remote access on: %s %s, UDP %d, %d peer%s, endpoint %s", cfg.remote_iface, addr, cfg.remote_port,
		npeers, npeers == 1 ? "" : "s", cfg.remote_endpoint[0] ? cfg.remote_endpoint : "(not set)");
	return 0;
}

static int sync_peers(char *err, size_t n)
{
	char conf[300], *argv[5];

	if (write_files(err, n))
		return -1;
	if (!running)
		return 0;
	path("wg.conf", conf, sizeof(conf));
	argv[0] = (char *)tool(); argv[1] = "sync"; argv[2] = cfg.remote_iface; argv[3] = conf; argv[4] = NULL;
	if (run(argv, NULL, NULL, 0)) {
		snprintf(err, n, "%s sync failed", tool());
		return -1;
	}
	return 0;
}

void remote_init(void)
{
	if (!cfg.remote_enabled)
		return;
	if (bring_up(last_error, sizeof(last_error)))
		AG_WARN("remote access: %s", last_error);
	else
		last_error[0] = '\0';
}

/* save remote.enabled / remote.endpoint in /etc/config/aigw */
static int save_uci(const char *option, const char *value)
{
	struct uci_context *ctx = uci_alloc_context();
	struct uci_ptr ptr;
	const char *confdir = getenv("AIGW_UCI_CONFDIR");
	char tuple[64];
	int rc = -1;

	if (!ctx)
		return -1;
	if (confdir)
		uci_set_confdir(ctx, confdir);
	snprintf(tuple, sizeof(tuple), "aigw.remote=remote");
	if (uci_lookup_ptr(ctx, &ptr, tuple, true) == UCI_OK && uci_set(ctx, &ptr) == UCI_OK) {
		memset(&ptr, 0, sizeof(ptr));
		ptr.package = "aigw";
		ptr.section = "remote";
		ptr.option = option;
		if (uci_lookup_ptr(ctx, &ptr, NULL, true) == UCI_OK) {
			ptr.value = value;
			if (uci_set(ctx, &ptr) == UCI_OK && uci_save(ctx, ptr.p) == UCI_OK &&
			    uci_commit(ctx, &ptr.p, false) == UCI_OK)
				rc = 0;
		}
	}
	uci_free_context(ctx);
	return rc;
}

int remote_enable(int on, const char *endpoint, char *err, size_t n)
{
	if (on) {
		if (endpoint && *endpoint) {
			size_t i;

			for (i = 0; endpoint[i]; i++)
				if (!(isalnum((unsigned char)endpoint[i]) || strchr(".-:[]", endpoint[i])))
					break;
			if (endpoint[i] || i >= sizeof(cfg.remote_endpoint)) {
				snprintf(err, n, "endpoint: a host name or address (optionally :port)");
				return -1;
			}
			snprintf(cfg.remote_endpoint, sizeof(cfg.remote_endpoint), "%s", endpoint);
		}
		if (!cfg.remote_endpoint[0]) {
			snprintf(err, n, "say where phones reach this home: agctl remote on <public address or DDNS name>");
			return -1;
		}
		if (bring_up(err, n)) {
			snprintf(last_error, sizeof(last_error), "%s", err);
			return -1;
		}
		last_error[0] = '\0';
		cfg.remote_enabled = 1;
		if (save_uci("enabled", "1") || save_uci("endpoint", cfg.remote_endpoint))
			AG_WARN("remote access: could not save the setting in /etc/config/aigw");
		return 0;
	}
	{
		char *argv[] = { (char *)tool(), "down", cfg.remote_iface, NULL };

		run(argv, NULL, NULL, 0);
	}
	running = 0;
	cfg.remote_enabled = 0;
	save_uci("enabled", "0");
	AG_INFO("remote access off");
	return 0;
}

static int name_ok(const char *s)
{
	size_t i;

	for (i = 0; s[i]; i++)
		if (!(isalnum((unsigned char)s[i]) || strchr(" _.-", s[i])))
			return 0;
	return i > 0 && i < 32;
}

static struct peer *find_peer(const char *name)
{
	int i;

	for (i = 0; i < npeers; i++)
		if (!strcasecmp(peers[i].name, name))
			return &peers[i];
	return NULL;
}

/* the home's own IPv4 networks (lan.iface), for a split tunnel */
static void home_nets(char *out, size_t n)
{
	struct ifaddrs *ifa, *i;

	if (getifaddrs(&ifa))
		return;
	for (i = ifa; i; i = i->ifa_next) {
		char a[16], word[24];
		uint32_t ip, mask;

		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask || !strstr(cfg.lan_ifaces, i->ifa_name) ||
		    !strcmp(i->ifa_name, cfg.remote_iface))
			continue;
		ip = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
		mask = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
		ip &= mask;
		snprintf(a, sizeof(a), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
		snprintf(word, sizeof(word), ",%s/%d", a, __builtin_popcount(mask));
		if (strlen(out) + strlen(word) + 1 < n && !strstr(out, word + 1))
			strcat(out, word);
	}
	freeifaddrs(ifa);
}

int remote_add(const char *name, struct blob_buf *reply, char *err, size_t n)
{
	struct in_addr base;
	char gw[16], priv[KEY_LEN], pub[KEY_LEN], ip[16], conf[600], allowed[200], endpoint[160];
	int prefix, host, i;
	uint32_t b;
	struct peer *p;

	if (!cfg.remote_enabled || !running) {
		snprintf(err, n, "remote access is off: agctl remote on <public address or DDNS name>");
		return -1;
	}
	if (!name_ok(name)) {
		snprintf(err, n, "name: 1-31 letters, digits, spaces, '.', '_' or '-'");
		return -1;
	}
	if (find_peer(name)) {
		snprintf(err, n, "'%s' already has remote access (agctl remote revoke it first)", name);
		return -1;
	}
	if (npeers >= PEER_MAX || subnet(&base, &prefix, gw)) {
		snprintf(err, n, "no room for another peer");
		return -1;
	}
	b = ntohl(base.s_addr);
	for (host = 2; host < (1 << (32 - prefix)) - 1; host++) {
		uint32_t a = b + (uint32_t)host;

		snprintf(ip, sizeof(ip), "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
		for (i = 0; i < npeers && strcmp(peers[i].ip, ip); i++)
			;
		if (i == npeers)
			break;
	}
	if (host >= (1 << (32 - prefix)) - 1) {
		snprintf(err, n, "the tunnel network %s is full", cfg.remote_subnet);
		return -1;
	}
	if (keypair(priv, pub)) {
		snprintf(err, n, "cannot make keys: is wireguard-tools (wg) installed?");
		return -1;
	}
	p = &peers[npeers++];
	memset(p, 0, sizeof(*p));
	snprintf(p->name, sizeof(p->name), "%s", name);
	snprintf(p->pub, sizeof(p->pub), "%s", pub);
	snprintf(p->ip, sizeof(p->ip), "%s", ip);
	p->created = ag_wall_s();
	if (sync_peers(err, n)) {
		npeers--;
		memset(priv, 0, sizeof(priv));
		return -1;
	}
	if (cfg.remote_full) {
		snprintf(allowed, sizeof(allowed), "0.0.0.0/0");
	} else {
		snprintf(allowed, sizeof(allowed), "%s", cfg.remote_subnet);
		home_nets(allowed, sizeof(allowed));
	}
	if (strchr(cfg.remote_endpoint, ']') ? !strstr(cfg.remote_endpoint, "]:") :
	    (strchr(cfg.remote_endpoint, ':') == NULL))
		snprintf(endpoint, sizeof(endpoint), "%s:%d", cfg.remote_endpoint, cfg.remote_port);
	else
		snprintf(endpoint, sizeof(endpoint), "%s", cfg.remote_endpoint);
	/* compact (no spaces) so it fits a QR code; the WireGuard apps read it as is */
	snprintf(conf, sizeof(conf),
		 "[Interface]\nPrivateKey=%s\nAddress=%s/32\nDNS=%s\n[Peer]\nPublicKey=%s\nEndpoint=%s\n"
		 "AllowedIPs=%s\nPersistentKeepalive=25\n", priv, ip, gw, server_pub, endpoint, allowed);
	memset(priv, 0, sizeof(priv));
	blobmsg_add_string(reply, "name", p->name);
	blobmsg_add_string(reply, "ip", p->ip);
	blobmsg_add_string(reply, "config", conf);
	memset(conf, 0, sizeof(conf));
	AG_INFO("remote access: added %s (%s)", p->name, p->ip);
	return 0;
}

int remote_revoke(const char *name, char *err, size_t n)
{
	struct peer *p = find_peer(name);
	char gone[32];

	if (!p) {
		snprintf(err, n, "no remote peer '%s' (agctl remote)", name);
		return -1;
	}
	snprintf(gone, sizeof(gone), "%.31s", p->name);
	*p = peers[--npeers];
	if (sync_peers(err, n))
		return -1;
	AG_INFO("remote access: revoked %s", gone);
	return 0;
}

/* who is connected: latest handshakes from the kernel */
void remote_tick(void)
{
	char buf[8192], *line, *save = NULL;
	char *argv[] = { "wg", "show", cfg.remote_iface, "dump", NULL };
	int pout[2], status, i;
	ssize_t len = 0, r;
	pid_t pid;

	if (!running || ag_wall_s() - last_poll < 10)
		return;
	last_poll = ag_wall_s();
	if (pipe(pout))
		return;
	pid = fork();
	if (pid == 0) {
		dup2(pout[1], 1);
		close(pout[0]);
		close(pout[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pout[1]);
	while (pid > 0 && (r = read(pout[0], buf + len, sizeof(buf) - 1 - (size_t)len)) > 0)
		len += r;
	close(pout[0]);
	if (pid > 0)
		waitpid(pid, &status, 0);
	buf[len > 0 ? len : 0] = '\0';
	/* first line: the interface; then: pub psk endpoint allowed-ips handshake rx tx keepalive */
	for (line = strtok_r(buf, "\n", &save), line = line ? strtok_r(NULL, "\n", &save) : NULL; line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *f[8], *s2 = NULL;
		int k = 0;

		for (char *t = strtok_r(line, "\t", &s2); t && k < 8; t = strtok_r(NULL, "\t", &s2))
			f[k++] = t;
		if (k < 7)
			continue;
		for (i = 0; i < npeers; i++) {
			struct peer *p = &peers[i];
			struct device *d;

			if (strcmp(p->pub, f[0]))
				continue;
			p->handshake = atoll(f[4]);
			snprintf(p->endpoint, sizeof(p->endpoint), "%s", strcmp(f[2], "(none)") ? f[2] : "");
			p->rx = strtoull(f[5], NULL, 10);
			p->tx = strtoull(f[6], NULL, 10);
			/* the tunnel address is this peer: give the device its name */
			if (p->handshake && (d = dev_find_ip(p->ip, 1)) && !d->hostname[0])
				snprintf(d->hostname, sizeof(d->hostname), "%s", p->name);
		}
	}
}

int remote_count(int online)
{
	int i, c = 0;

	for (i = 0; i < npeers; i++)
		if (!online || (peers[i].handshake && ag_wall_s() - peers[i].handshake < 180))
			c++;
	return c;
}

void remote_blob(struct blob_buf *b)
{
	void *arr, *t;
	int i;

	if (!running && cfg.remote_enabled)
		load_peers();
	blobmsg_add_u8(b, "enabled", (uint8_t)cfg.remote_enabled);
	blobmsg_add_u8(b, "running", (uint8_t)running);
	blobmsg_add_string(b, "endpoint", cfg.remote_endpoint);
	blobmsg_add_u32(b, "port", (uint32_t)cfg.remote_port);
	blobmsg_add_string(b, "subnet", cfg.remote_subnet);
	blobmsg_add_string(b, "iface", cfg.remote_iface);
	blobmsg_add_u8(b, "full_tunnel", (uint8_t)cfg.remote_full);
	blobmsg_add_string(b, "public_key", running ? server_pub : "");
	blobmsg_add_string(b, "error", last_error);
	arr = blobmsg_open_array(b, "peers");
	for (i = 0; i < npeers; i++) {
		const struct peer *p = &peers[i];

		t = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "name", p->name);
		blobmsg_add_string(b, "ip", p->ip);
		blobmsg_add_u64(b, "created", (uint64_t)p->created);
		blobmsg_add_u64(b, "handshake", (uint64_t)p->handshake);
		blobmsg_add_u8(b, "online", (uint8_t)(p->handshake && ag_wall_s() - p->handshake < 180));
		blobmsg_add_string(b, "endpoint", p->endpoint);
		blobmsg_add_u64(b, "rx", p->rx);
		blobmsg_add_u64(b, "tx", p->tx);
		blobmsg_close_table(b, t);
	}
	blobmsg_close_array(b, arr);
}
