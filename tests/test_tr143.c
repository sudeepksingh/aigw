/* SPDX-License-Identifier: Apache-2.0 */
/* TR-143 diagnostics: URLs, HTTP answers, times and names (the pure parts) */
#include <stdio.h>
#include <string.h>

#include "ag/tr143.h"

static int failed;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); failed++; } \
			      else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
	char host[64], path[128], t[48];
	unsigned port = 0, u = 0;
	int code = 0;
	long long clen = 0;
	size_t head = 0;
	struct tr143_in in;

	/* URLs: http only, host, port, path */
	CHECK(tr143_url("http://speed.example.net/10MB.bin", host, sizeof(host), &port, path, sizeof(path)) == 0 &&
	      !strcmp(host, "speed.example.net") && port == 80 && !strcmp(path, "/10MB.bin"), "http URL: host, port 80, path");
	CHECK(tr143_url("HTTP://10.0.0.1:8080/up?x=1", host, sizeof(host), &port, path, sizeof(path)) == 0 &&
	      !strcmp(host, "10.0.0.1") && port == 8080 && !strcmp(path, "/up?x=1"), "port and query kept");
	CHECK(tr143_url("http://example.com", host, sizeof(host), &port, path, sizeof(path)) == 0 && !strcmp(path, "/"),
	      "no path: /");
	CHECK(tr143_url("https://example.com/", host, sizeof(host), &port, path, sizeof(path)) < 0, "https refused (HTTP only)");
	CHECK(tr143_url("ftp://example.com/f", host, sizeof(host), &port, path, sizeof(path)) < 0, "FTP refused");
	CHECK(tr143_url("http://user@example.com/", host, sizeof(host), &port, path, sizeof(path)) < 0, "user@ refused");
	CHECK(tr143_url("http://example.com:0/", host, sizeof(host), &port, path, sizeof(path)) < 0 &&
	      tr143_url("http://example.com:99999/", host, sizeof(host), &port, path, sizeof(path)) < 0, "bad ports refused");
	CHECK(tr143_url("http:///x", host, sizeof(host), &port, path, sizeof(path)) < 0, "no host refused");
	CHECK(tr143_url("http://a/b c", host, sizeof(host), &port, path, sizeof(path)) < 0, "a space in the path refused");

	/* HTTP answers */
	{
		const char *a = "HTTP/1.1 200 OK\r\nServer: x\r\ncontent-length:  1234\r\n\r\nBODY";
		const char *b = "HTTP/1.0 404 Not Found\r\n\r\n";
		const char *c = "HTTP/1.1 200 OK\r\nContent-";

		CHECK(tr143_http_head(a, strlen(a), &code, &clen, &head) == 1 && code == 200 && clen == 1234 &&
		      head == strlen(a) - 4, "200 with Content-Length (any case), head length");
		CHECK(tr143_http_head(b, strlen(b), &code, &clen, &head) == 1 && code == 404 && clen == -1,
		      "404, no length");
		CHECK(tr143_http_head(c, strlen(c), &code, &clen, &head) == 0, "a head not all there yet");
		CHECK(tr143_http_head("SSH-2.0-x\r\n\r\n", 13, &code, &clen, &head) < 0, "not HTTP");
	}

	/* times with microseconds */
	tr143_time(1794700800LL * 1000000 + 123456, t, sizeof(t));
	CHECK(!strcmp(t, "2026-11-15T00:00:00.123456Z"), "dateTime with microseconds: %s", t);
	tr143_time(0, t, sizeof(t));
	CHECK(!strcmp(t, "0001-01-01T00:00:00Z"), "unknown time");
	CHECK(tr143_kbps(1250000, 0, 1000000) == 10000, "1.25 MB in 1 s = 10000 kbit/s");
	CHECK(tr143_kbps(0, 0, 10) == 0 && tr143_kbps(5, 10, 10) == 0, "no bytes or no time: 0");

	/* numbers, names, defaults */
	CHECK(tr143_uint("3", 1, 100, &u) == 0 && u == 3, "a number in range");
	CHECK(tr143_uint("0", 1, 100, &u) < 0 && tr143_uint("-1", 0, 100, &u) < 0 && tr143_uint("5x", 0, 100, &u) < 0 &&
	      tr143_uint("", 0, 100, &u) < 0, "out of range, negative, not a number, empty: refused");
	CHECK(!strcmp(tr143_status_name(TS_COMPLETE), "Complete") &&
	      !strcmp(tr143_status_name(TS_CANNOT_RESOLVE), "Error_CannotResolveHostName") &&
	      !strcmp(tr143_status_name(TS_MAX_HOPS), "Error_MaxHopCountExceeded") &&
	      !strcmp(tr143_status_name(TS_NONE), "None") && !strcmp(tr143_status_name(99), "Error_Other"),
	      "status names as TR-181 has them");
	CHECK(!strcmp(tr143_ns_status_name(NS_NOT_RESOLVED), "Error_HostNameNotResolved"), "NSLookup result names");
	tr143_defaults(TK_PING, &in);
	CHECK(in.reps == 3 && in.timeout_ms == 1000 && in.size == 64 && in.nconn == 1 && !strcmp(in.proto, "Any"),
	      "IPPing defaults");
	tr143_defaults(TK_TRACE, &in);
	CHECK(in.max_hops == 30 && in.reps == 3, "TraceRoute defaults");
	tr143_defaults(TK_UPLOAD, &in);
	CHECK(in.file_len == 1048576, "Upload: 1 MiB");
	CHECK(!strcmp(tr143_kind_name(TK_NSLOOKUP), "NSLookupDiagnostics"), "kind names");

	printf(failed ? "\n%d FAILED\n" : "\nall passed\n", failed);
	return failed ? 1 : 0;
}
