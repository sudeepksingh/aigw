/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Traffic meter (phase 7): a small eBPF program on each LAN interface counts
 * every device's bytes and packets in the kernel, per MAC address, as they
 * pass. agd reads the counts every second: live rates per device without
 * waiting for connections to end (conntrack counts arrive when they do), and
 * they include connections in the fast path, which skip netfilter (traffic the
 * switch chip forwards by itself, with hardware offload, is not seen).
 *
 *   LAN interface ── tcx ingress: from a device (up), key = source MAC
 *                └── tcx egress:  to a device (down), key = destination MAC
 *   LRU hash map:  MAC -> { up bytes, up packets, down bytes, down packets }
 *
 * The program is written out as BPF instructions below, so building AIGW needs
 * no BPF compiler; agd loads it and attaches it with bpf() alone (tcx, Linux
 * 6.6 or newer). The links belong to agd: when agd stops, the meter is gone
 * and nothing is left in the data path. The program only counts and always
 * passes the packet on.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <net/if.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/bpf.h>
#include <libubox/uloop.h>

#include "agd.h"

#define METER_IFACES   4
#define METER_ENTRIES  1024
#define ATTACH_INGRESS 46            /* BPF_TCX_INGRESS (enum bpf_attach_type, Linux 6.6) */
#define ATTACH_EGRESS  47            /* BPF_TCX_EGRESS */
#define MAP_LRU_HASH   9             /* BPF_MAP_TYPE_LRU_HASH */

struct counts { uint64_t up_bytes, up_pkts, down_bytes, down_pkts; };

static int map_fd = -1, prog_in = -1, prog_out = -1;
static int links[METER_IFACES * 2], n_links;
static char attached[METER_IFACES][IFNAMSIZ];
static int n_attached;
static char why[200];
static int64_t last_read_ms, last_try;

static long sys_bpf(int cmd, union bpf_attr *a)
{
	return syscall(__NR_bpf, cmd, a, sizeof(*a));
}

/* ---------- the program ---------- */

#define INSN(c, d, s, o, i) ((struct bpf_insn){ .code = (c), .dst_reg = (d), .src_reg = (s), .off = (o), .imm = (i) })
#define MOV_REG(d, s)        INSN(BPF_ALU64 | BPF_MOV | BPF_X, d, s, 0, 0)
#define MOV_IMM(d, i)        INSN(BPF_ALU64 | BPF_MOV | BPF_K, d, 0, 0, i)
#define ADD_IMM(d, i)        INSN(BPF_ALU64 | BPF_ADD | BPF_K, d, 0, 0, i)
#define LDX(sz, d, s, o)     INSN(BPF_LDX | (sz) | BPF_MEM, d, s, o, 0)
#define STX(sz, d, s, o)     INSN(BPF_STX | (sz) | BPF_MEM, d, s, o, 0)
#define ST(sz, d, o, i)      INSN(BPF_ST | (sz) | BPF_MEM, d, 0, o, i)
#define XADD(d, s, o)        INSN(BPF_STX | BPF_DW | 0xc0 /* BPF_ATOMIC */, d, s, o, 0 /* BPF_ADD */)
#define JGT_REG(d, s, o)     INSN(BPF_JMP | BPF_JGT | BPF_X, d, s, o, 0)
#define JEQ_IMM(d, i, o)     INSN(BPF_JMP | BPF_JEQ | BPF_K, d, 0, o, i)
#define JA(o)                INSN(BPF_JMP | BPF_JA, 0, 0, o, 0)
#define CALL(f)              INSN(BPF_JMP | BPF_CALL, 0, 0, 0, f)
#define EXIT()               INSN(BPF_JMP | BPF_EXIT, 0, 0, 0, 0)
#define LD_MAP(d, fd)        INSN(BPF_LD | BPF_DW | BPF_IMM, d, BPF_PSEUDO_MAP_FD, 0, fd), INSN(0, 0, 0, 0, 0)

/* mac_off: where the device's MAC is in the Ethernet header (6 = source, on
 * ingress; 0 = destination, on egress); val_off: its byte counter (packets next) */
static int load_prog(int mac_off, int val_off, char *err, size_t n)
{
	enum { R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10 };
	struct bpf_insn p[] = {
		/* 0 */  MOV_REG(R6, R1),
		/* 1 */  LDX(BPF_W, R2, R6, offsetof(struct __sk_buff, data)),
		/* 2 */  LDX(BPF_W, R3, R6, offsetof(struct __sk_buff, data_end)),
		/* 3 */  MOV_REG(R4, R2),
		/* 4 */  ADD_IMM(R4, 14),
		/* 5 */  JGT_REG(R4, R3, 38),                 /* shorter than an Ethernet header: out */
		/* 6 */  LDX(BPF_B, R5, R2, mac_off + 0), STX(BPF_B, R10, R5, -8),
		/* 8 */  LDX(BPF_B, R5, R2, mac_off + 1), STX(BPF_B, R10, R5, -7),
		/* 10 */ LDX(BPF_B, R5, R2, mac_off + 2), STX(BPF_B, R10, R5, -6),
		/* 12 */ LDX(BPF_B, R5, R2, mac_off + 3), STX(BPF_B, R10, R5, -5),
		/* 14 */ LDX(BPF_B, R5, R2, mac_off + 4), STX(BPF_B, R10, R5, -4),
		/* 16 */ LDX(BPF_B, R5, R2, mac_off + 5), STX(BPF_B, R10, R5, -3),
		/* 18 */ ST(BPF_H, R10, -2, 0),               /* key: MAC + 2 zero bytes */
		/* 19 */ LDX(BPF_W, R7, R6, offsetof(struct __sk_buff, len)),
		/* 20 */ LD_MAP(R1, map_fd),
		/* 22 */ MOV_REG(R2, R10),
		/* 23 */ ADD_IMM(R2, -8),
		/* 24 */ CALL(BPF_FUNC_map_lookup_elem),
		/* 25 */ JEQ_IMM(R0, 0, 4),                   /* new device: insert */
		/* 26 */ XADD(R0, R7, val_off),
		/* 27 */ MOV_IMM(R1, 1),
		/* 28 */ XADD(R0, R1, val_off + 8),
		/* 29 */ JA(14),
		/* 30 */ ST(BPF_DW, R10, -40, 0), ST(BPF_DW, R10, -32, 0),
		/* 32 */ ST(BPF_DW, R10, -24, 0), ST(BPF_DW, R10, -16, 0),
		/* 34 */ STX(BPF_DW, R10, R7, -40 + val_off),
		/* 35 */ ST(BPF_DW, R10, -40 + val_off + 8, 1),
		/* 36 */ LD_MAP(R1, map_fd),
		/* 38 */ MOV_REG(R2, R10),
		/* 39 */ ADD_IMM(R2, -8),
		/* 40 */ MOV_REG(R3, R10),
		/* 41 */ ADD_IMM(R3, -40),
		/* 42 */ MOV_IMM(R4, BPF_NOEXIST),
		/* 43 */ CALL(BPF_FUNC_map_update_elem),
		/* 44 */ MOV_IMM(R0, -1),                     /* TCX_NEXT: the packet goes on, untouched */
		/* 45 */ EXIT(),
	};
	static char log[4096];
	union bpf_attr a;
	int fd;

	memset(&a, 0, sizeof(a));
	a.prog_type = BPF_PROG_TYPE_SCHED_CLS;
	a.insn_cnt = sizeof(p) / sizeof(p[0]);
	a.insns = (uint64_t)(uintptr_t)p;
	a.license = (uint64_t)(uintptr_t)"Apache-2.0";
	a.log_buf = (uint64_t)(uintptr_t)log;
	a.log_size = sizeof(log);
	a.log_level = 1;
	log[0] = '\0';
	fd = (int)sys_bpf(BPF_PROG_LOAD, &a);
	if (fd < 0) {
		char *last = strrchr(log, '\n');

		while (last && last > log && last[1] == '\0') {
			*last = '\0';
			last = strrchr(log, '\n');
		}
		snprintf(err, n, "the kernel refused the meter program: %s%s%.120s", strerror(errno),
			 log[0] ? ": " : "", last ? last + 1 : log);
	}
	return fd;
}

static void detach_all(void)
{
	int i;

	for (i = 0; i < n_links; i++)
		close(links[i]);
	n_links = n_attached = 0;
}

static int attach(const char *ifname, char *err, size_t n)
{
	union bpf_attr a;
	unsigned idx = if_nametoindex(ifname);
	int k, fd[2];

	if (!idx) {
		snprintf(err, n, "no interface %s", ifname);
		return -1;
	}
	for (k = 0; k < 2; k++) {
		memset(&a, 0, sizeof(a));
		a.link_create.prog_fd = (uint32_t)(k ? prog_out : prog_in);
		a.link_create.target_ifindex = idx;
		a.link_create.attach_type = k ? ATTACH_EGRESS : ATTACH_INGRESS;
		fd[k] = (int)sys_bpf(BPF_LINK_CREATE, &a);
		if (fd[k] < 0) {
			snprintf(err, n, "%s: can't attach (tcx needs Linux 6.6 or newer): %s", ifname, strerror(errno));
			if (k)
				close(fd[0]);
			return -1;
		}
	}
	links[n_links++] = fd[0];
	links[n_links++] = fd[1];
	snprintf(attached[n_attached++], IFNAMSIZ, "%s", ifname);
	return 0;
}

static void start(void)
{
	union bpf_attr a;
	char ifs[256], *tok, *save = NULL, err[200] = "";

	memset(&a, 0, sizeof(a));
	a.map_type = MAP_LRU_HASH;
	a.key_size = 8;
	a.value_size = sizeof(struct counts);
	a.max_entries = METER_ENTRIES;
	map_fd = (int)sys_bpf(BPF_MAP_CREATE, &a);
	if (map_fd < 0) {
		snprintf(why, sizeof(why), "no eBPF here (bpf map: %s)", strerror(errno));
		goto fail;
	}
	prog_in = load_prog(6, offsetof(struct counts, up_bytes), err, sizeof(err));
	prog_out = prog_in < 0 ? -1 : load_prog(0, offsetof(struct counts, down_bytes), err, sizeof(err));
	if (prog_in < 0 || prog_out < 0) {
		snprintf(why, sizeof(why), "%s", err);
		goto fail;
	}
	snprintf(ifs, sizeof(ifs), "%s", cfg.lan_ifaces);
	for (tok = strtok_r(ifs, " ", &save); tok && n_attached < METER_IFACES; tok = strtok_r(NULL, " ", &save))
		if (attach(tok, err, sizeof(err)))
			AG_INFO("traffic meter: %s", err);
	if (!n_attached) {
		snprintf(why, sizeof(why), "%s", err[0] ? err : "no LAN interface to attach to");
		goto fail;
	}
	why[0] = '\0';
	AG_INFO("traffic meter: counting per device in the kernel (eBPF) on %d interface%s", n_attached,
		n_attached == 1 ? "" : "s");
	return;
fail:
	AG_INFO("traffic meter off: %s", why);
	detach_all();
	if (prog_in >= 0)
		close(prog_in);
	if (prog_out >= 0)
		close(prog_out);
	if (map_fd >= 0)
		close(map_fd);
	map_fd = prog_in = prog_out = -1;
}

static void tick_cb(struct uloop_timeout *t)
{
	meter_tick();
	uloop_timeout_set(t, 1000);
}

void meter_init(void)
{
	static struct uloop_timeout timer = { .cb = tick_cb };

	if (!cfg.meter_enabled) {
		snprintf(why, sizeof(why), "off in the config (meter.enabled)");
		return;
	}
	last_try = ag_wall_s();
	start();
	uloop_timeout_set(&timer, 1000);
}

/* every second: totals per MAC -> each device's rate */
void meter_tick(void)
{
	uint8_t key[8], next[8];
	struct counts c;
	union bpf_attr a;
	int64_t now = (int64_t)(ag_mono_us() / 1000);
	double dt;
	int first = 1, n = 0;

	if (map_fd < 0) {
		/* LAN interfaces come and go (the access point restarts): try again now and then */
		if (cfg.meter_enabled && n_attached == 0 && ag_wall_s() - last_try >= 60) {
			last_try = ag_wall_s();
			start();
		}
		return;
	}
	if (now - last_read_ms < 900)
		return;
	dt = last_read_ms ? (now - last_read_ms) / 1000.0 : 0;
	last_read_ms = now;
	memset(key, 0, sizeof(key));
	for (;;) {
		char mac[18];
		struct device *d;

		memset(&a, 0, sizeof(a));
		a.map_fd = (uint32_t)map_fd;
		a.key = first ? 0 : (uint64_t)(uintptr_t)key;
		a.next_key = (uint64_t)(uintptr_t)next;
		if (sys_bpf(BPF_MAP_GET_NEXT_KEY, &a))
			break;
		first = 0;
		memcpy(key, next, sizeof(key));
		memset(&a, 0, sizeof(a));
		a.map_fd = (uint32_t)map_fd;
		a.key = (uint64_t)(uintptr_t)key;
		a.value = (uint64_t)(uintptr_t)&c;
		if (sys_bpf(BPF_MAP_LOOKUP_ELEM, &a))
			continue;
		if (++n > METER_ENTRIES)
			break;
		snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", key[0], key[1], key[2], key[3], key[4], key[5]);
		if (!(d = dev_find_mac(mac, 0)))
			continue;               /* the gateway's own, broadcast, a device not known yet */
		if (dt > 0 && d->meter_at) {
			uint64_t du = c.up_bytes >= d->meter_up ? c.up_bytes - d->meter_up : 0;
			uint64_t dd = c.down_bytes >= d->meter_down ? c.down_bytes - d->meter_down : 0;

			d->rate_up = (uint64_t)(du * 8 / dt);
			d->rate_down = (uint64_t)(dd * 8 / dt);
		}
		d->meter_up = c.up_bytes;
		d->meter_down = c.down_bytes;
		d->meter_pkts = c.up_pkts + c.down_pkts;
		d->meter_at = ag_wall_s();
	}
	/* a device that sent nothing in the last read: idle */
	{
		int i;

		for (i = 0; i < DEV_MAX; i++)
			if (devs[i].used && devs[i].meter_at && ag_wall_s() - devs[i].meter_at > 2)
				devs[i].rate_up = devs[i].rate_down = 0;
	}
}

int meter_active(void)
{
	return n_attached > 0;
}

void meter_blob(struct blob_buf *b)
{
	void *t = blobmsg_open_table(b, "meter"), *arr;
	int i;

	blobmsg_add_u8(b, "active", (uint8_t)(n_attached > 0));
	arr = blobmsg_open_array(b, "ifaces");
	for (i = 0; i < n_attached; i++)
		blobmsg_add_string(b, NULL, attached[i]);
	blobmsg_close_array(b, arr);
	if (why[0])
		blobmsg_add_string(b, "why", why);
	blobmsg_close_table(b, t);
}
