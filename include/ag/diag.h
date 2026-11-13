/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AIGW Diagnose: "my internet is slow", answered by the gateway. Pure (no
 * I/O): agd (agd/diag.c) measures the line, DNS, Wi-Fi and one device, this
 * names what is wrong, most important first, in plain words with a code a
 * help desk can count; the unit test (tests/test_diag.c) feeds it cases.
 */
#ifndef AG_DIAG_H
#define AG_DIAG_H

#define DIAG_RADIOS   8
#define DIAG_FINDINGS 16

enum diag_sev { DS_OK, DS_INFO, DS_WARN, DS_PROBLEM };

/* echo requests to one place: sent, unanswered, median delay (-1: no answer) */
struct diag_ping {
	int sent, lost, rtt_ms;
};

struct diag_radio {
	char name[24];                  /* interface */
	int band, channel;              /* 2 or 5 */
	int status, cause;              /* enum heal_status / heal_cause (ag/heal.h) */
	int loss_pct, rtt_ms;
	char explanation[300];          /* Self-Healing Wi-Fi's own words */
	int kept_today, tried_today;    /* channel trials today (the healing report) */
};

/* the device a person asked about (optional) */
struct diag_host {
	int given, found;               /* asked about; known to the gateway */
	char name[64];                  /* name, else MAC, else what was asked */
	int connected;                  /* on Wi-Fi now */
	int seen_ago;                   /* seconds since it was last seen (any link); -1 never */
	int radio;                      /* its access point in radios[], -1 none */
	int signal;                     /* dBm on Wi-Fi, 0 unknown */
	int loss_pct, rtt_ms, samples;  /* what it feels (Self-Healing Wi-Fi's probes); samples 0: not measured */
	int blocked;                    /* AIGW keeps it off the network (quarantine, disconnect) */
	int limited;                    /* AIGW slows it (throttle, deprioritize) */
	char actions[160];              /* those actions, in words ("quarantine, 2h left") */
	int on24;                       /* kept on 2.4 GHz (stay24): information only */
};

struct diag_input {
	int route;                      /* a default route exists */
	char gw[48];                    /* its next hop ("" when none) */
	struct diag_ping gw_ping;       /* to the next hop (modem, ISP's router) */
	struct diag_ping inet;          /* to the internet (the best of the targets) */
	int dns;                        /* 1 answered, 0 no answer, -1 not checked */
	int dns_rcode, dns_ms;          /* its answer code (0 ok, 2 SERVFAIL, 3 NXDOMAIN) and time */
	struct diag_radio radios[DIAG_RADIOS];
	int nradios;
	struct diag_host host;
	int weak_signal, loss_pct;      /* wifi.heal_weak and heal_loss */
};

struct diag_finding {
	const char *code;               /* e.g. "WIFI_COVERAGE" */
	const char *area;               /* Internet, DNS, WiFi, Device, AIGW */
	int sev;                        /* enum diag_sev */
	char text[400];
};

struct diag_result {
	int n;
	struct diag_finding f[DIAG_FINDINGS];   /* f[0] is the verdict: the most important */
};

void diag_judge(const struct diag_input *in, struct diag_result *out);
/* what to do about a code: for the help desk, and for the person at home */
const char *diag_advice(const char *code);
const char *diag_sev_name(int sev);
/* the median of n delays (-1 values ignored); -1 when none */
int diag_median(int *v, int n);

#endif
