/* SPDX-License-Identifier: Apache-2.0 */
/*
 * TR-143 diagnostics, the tests every ISP's management system already runs
 * from a gateway: IPPing, TraceRoute, DownloadDiagnostics, UploadDiagnostics
 * (Device.IP.Diagnostics.) and NSLookupDiagnostics (Device.DNS.Diagnostics.).
 * agd (agd/tr143.c) runs one of each at a time in a child process; this is
 * what both sides share, and the pure parts (URLs, HTTP answers, times, the
 * names of the results), unit-tested in tests/test_tr143.c.
 *
 * IPv4 only; HTTP only (no FTP); one connection per test.
 */
#ifndef AG_TR143_H
#define AG_TR143_H

#include <stddef.h>
#include <stdint.h>

enum tr143_kind { TK_PING, TK_TRACE, TK_DOWNLOAD, TK_UPLOAD, TK_NSLOOKUP, TK_N };

/* DiagnosticsState / Status */
enum tr143_status {
	TS_NONE,
	TS_REQUESTED,           /* asked for, or running */
	TS_COMPLETE,
	TS_CANNOT_RESOLVE,      /* Error_CannotResolveHostName */
	TS_NO_ROUTE,            /* Error_NoRouteToHost */
	TS_INIT_CONN,           /* Error_InitConnectionFailed */
	TS_NO_RESPONSE,         /* Error_NoResponse */
	TS_TRANSFER,            /* Error_TransferFailed */
	TS_INCORRECT_SIZE,      /* Error_IncorrectSize */
	TS_TIMEOUT,             /* Error_Timeout */
	TS_MAX_HOPS,            /* Error_MaxHopCountExceeded */
	TS_DNS_SERVER,          /* Error_DNSServerNotResolved */
	TS_INTERNAL,            /* Error_Internal */
	TS_OTHER,               /* Error_Other */
	TS_N
};

/* NSLookupDiagnostics.Result.{i}.Status */
enum tr143_ns_status { NS_SUCCESS, NS_SERVER_NA, NS_NOT_RESOLVED, NS_TIMEOUT, NS_OTHER };

#define TR143_HOPS  64
#define TR143_NSRES 16

/* what a test is asked to do (every kind; what it doesn't use is 0 or "") */
struct tr143_in {
	char host[256];                 /* IPPing, TraceRoute: Host; NSLookup: HostName */
	char url[256];                  /* DownloadURL, UploadURL */
	char server[64];                /* NSLookup: DNSServer ("" the gateway's own) */
	unsigned reps;                  /* NumberOfRepetitions, NumberOfTries */
	unsigned timeout_ms;            /* Timeout (per echo, try or lookup) */
	unsigned size;                  /* DataBlockSize */
	unsigned dscp;
	unsigned max_hops;              /* TraceRoute: MaxHopCount */
	unsigned duration_s;            /* TimeBasedTestDuration (0: the whole file) */
	unsigned long long file_len;    /* Upload: TestFileLength */
	char dns_ip[48];                /* the gateway's own DNS (its listen address) */
	unsigned dns_port;
	/* taken and shown, not acted on: */
	char iface[64];                 /* Interface: the route decides */
	char proto[8];                  /* ProtocolVersion: Any or IPv4 (IPv6 is refused) */
	unsigned eprio;                 /* EthernetPriority */
	unsigned nconn;                 /* NumberOfConnections: 1 */
	int perconn;                    /* EnablePerConnectionResults */
};

struct tr143_hop {
	char addr[48];
	int code;                       /* the ICMP code of its answer */
	char rtts[64];                  /* RTTimes: ms per try, comma-separated */
};

struct tr143_ns {
	int status;                     /* enum tr143_ns_status */
	int authoritative;              /* 1 Authoritative, 0 NonAuthoritative, -1 None */
	char name[128];                 /* HostNameReturned */
	char ips[200];                  /* IPAddresses, comma-separated */
	char server[48];                /* DNSServerIP */
	unsigned ms;                    /* ResponseTime */
};

struct tr143_out {
	int status;                     /* enum tr143_status */
	char ip_used[48];               /* IPAddressUsed: the gateway's source address */
	char why[160];                  /* in words, for the log and agctl */
	/* IPPing */
	unsigned ok, fail;
	unsigned avg_ms, min_ms, max_ms, avg_us, min_us, max_us;
	/* TraceRoute */
	unsigned resp_ms;
	int nhops;
	struct tr143_hop hop[TR143_HOPS];
	/* Download, Upload: wall times in microseconds */
	int64_t rom_us, bom_us, eom_us, tcp_req_us, tcp_resp_us;
	unsigned long long test_rx, test_tx, total_rx, total_tx;
	/* NSLookup */
	unsigned ns_ok;
	int nns;
	struct tr143_ns ns[TR143_NSRES];
};

const char *tr143_kind_name(int kind);          /* "IPPing", ... */
const char *tr143_status_name(int status);      /* "None", "Requested", "Complete", "Error_..." */
const char *tr143_ns_status_name(int s);
/* the default inputs of a kind */
void tr143_defaults(int kind, struct tr143_in *in);
/* an unsigned number within [min, max]; -1 when it isn't one */
int tr143_uint(const char *v, unsigned min, unsigned max, unsigned *out);

/* http://host[:port]/path; 0 ok, -1 not an http URL (https and FTP aren't supported) */
int tr143_url(const char *url, char *host, size_t hn, unsigned *port, char *path, size_t pn);
/* an HTTP answer's head in buf: 1 complete (*code, *clen: Content-Length or -1,
   *head: its length with the blank line), 0 not all there yet, -1 not HTTP */
int tr143_http_head(const char *buf, size_t n, int *code, long long *clen, size_t *head);
/* a TR-106 dateTime with microseconds: 2026-11-15T03:00:00.123456Z (0: unknown time) */
void tr143_time(int64_t wall_us, char *buf, size_t n);
/* kbit/s of a transfer: bytes between two times (0 when none) */
unsigned long long tr143_kbps(unsigned long long bytes, int64_t from_us, int64_t to_us);

#endif
