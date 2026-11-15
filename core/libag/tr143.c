/* SPDX-License-Identifier: Apache-2.0 */
/* TR-143 diagnostics: the pure parts (see ag/tr143.h) */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "ag/tr143.h"

const char *tr143_kind_name(int kind)
{
	static const char *const n[TK_N] = { "IPPing", "TraceRoute", "DownloadDiagnostics", "UploadDiagnostics",
					     "NSLookupDiagnostics" };

	return kind >= 0 && kind < TK_N ? n[kind] : "";
}

const char *tr143_status_name(int status)
{
	static const char *const n[TS_N] = {
		"None", "Requested", "Complete", "Error_CannotResolveHostName", "Error_NoRouteToHost",
		"Error_InitConnectionFailed", "Error_NoResponse", "Error_TransferFailed", "Error_IncorrectSize",
		"Error_Timeout", "Error_MaxHopCountExceeded", "Error_DNSServerNotResolved", "Error_Internal",
		"Error_Other",
	};

	return status >= 0 && status < TS_N ? n[status] : "Error_Other";
}

const char *tr143_ns_status_name(int s)
{
	static const char *const n[] = { "Success", "Error_DNSServerNotAvailable", "Error_HostNameNotResolved",
					 "Error_Timeout", "Error_Other" };

	return s >= 0 && s <= NS_OTHER ? n[s] : "Error_Other";
}

void tr143_defaults(int kind, struct tr143_in *in)
{
	memset(in, 0, sizeof(*in));
	snprintf(in->proto, sizeof(in->proto), "Any");
	in->nconn = 1;
	switch (kind) {
	case TK_PING:
		in->reps = 3;
		in->timeout_ms = 1000;
		in->size = 64;
		break;
	case TK_TRACE:
		in->reps = 3;
		in->timeout_ms = 5000;
		in->size = 38;
		in->max_hops = 30;
		break;
	case TK_UPLOAD:
		in->file_len = 1048576;
		break;
	case TK_NSLOOKUP:
		in->reps = 1;
		in->timeout_ms = 5000;
		break;
	}
}

int tr143_uint(const char *v, unsigned min, unsigned max, unsigned *out)
{
	unsigned long x;
	char *end;

	if (!v || !isdigit((unsigned char)*v))
		return -1;
	x = strtoul(v, &end, 10);
	if (*end || x < min || x > max)
		return -1;
	*out = (unsigned)x;
	return 0;
}

int tr143_url(const char *url, char *host, size_t hn, unsigned *port, char *path, size_t pn)
{
	const char *h, *e, *p;
	size_t l;

	if (!url || strncasecmp(url, "http://", 7))
		return -1;
	h = url + 7;
	l = strcspn(h, ":/?#");
	if (!l || l >= hn || memchr(h, '@', l) || memchr(h, '[', l))
		return -1;
	memcpy(host, h, l);
	host[l] = '\0';
	e = h + l;
	*port = 80;
	if (*e == ':') {
		char num[8];
		size_t k = strcspn(e + 1, "/?#");

		if (!k || k >= sizeof(num))
			return -1;
		memcpy(num, e + 1, k);
		num[k] = '\0';
		if (tr143_uint(num, 1, 65535, port) < 0)
			return -1;
		e += 1 + k;
	}
	p = *e == '/' ? e : "/";
	if (*e && *e != '/')
		return -1;              /* "?x" right after the host: not a path we can ask for */
	if (strlen(p) >= pn || strpbrk(p, " \r\n"))
		return -1;
	snprintf(path, pn, "%s", p);
	return 0;
}

int tr143_http_head(const char *buf, size_t n, int *code, long long *clen, size_t *head)
{
	const char *end = NULL, *line;
	size_t i;

	for (i = 0; i + 3 < n; i++)
		if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
			end = buf + i;
			break;
		}
	if (n >= 5 && strncmp(buf, "HTTP/", 5))
		return -1;
	if (!end)
		return n > 16384 ? -1 : 0;      /* a head that never ends */
	if (end - buf < 12 || buf[8] != ' ' || !isdigit((unsigned char)buf[9]) || !isdigit((unsigned char)buf[10]) ||
	    !isdigit((unsigned char)buf[11]))
		return -1;
	*code = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
	*clen = -1;
	*head = (size_t)(end - buf) + 4;
	for (line = memchr(buf, '\n', (size_t)(end - buf)); line && line < end; line = memchr(line, '\n', (size_t)(end - line))) {
		line++;
		if ((size_t)(end - line) > 15 && !strncasecmp(line, "Content-Length:", 15)) {
			const char *v = line + 15;

			while (*v == ' ' || *v == '\t')
				v++;
			if (isdigit((unsigned char)*v))
				*clen = strtoll(v, NULL, 10);
		}
	}
	return 1;
}

void tr143_time(int64_t wall_us, char *buf, size_t n)
{
	time_t t = (time_t)(wall_us / 1000000);
	struct tm tm;
	char s[32];

	if (wall_us <= 0 || !gmtime_r(&t, &tm)) {
		snprintf(buf, n, "0001-01-01T00:00:00Z");
		return;
	}
	strftime(s, sizeof(s), "%Y-%m-%dT%H:%M:%S", &tm);
	snprintf(buf, n, "%s.%06dZ", s, (int)(wall_us % 1000000));
}

unsigned long long tr143_kbps(unsigned long long bytes, int64_t from_us, int64_t to_us)
{
	if (to_us <= from_us || !bytes)
		return 0;
	return bytes * 8000ULL / (unsigned long long)(to_us - from_us);
}
