/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "ag/ag.h"

static const char *log_ident = "aigw";
static int log_level = AG_LOG_INFO;

void ag_log_init(const char *ident, int level)
{
	log_ident = ident;
	log_level = level;
}

int ag_log_level_parse(const char *s)
{
	if (!s)
		return AG_LOG_INFO;
	if (!strcasecmp(s, "error") || !strcasecmp(s, "err"))
		return AG_LOG_ERR;
	if (!strcasecmp(s, "warn") || !strcasecmp(s, "warning"))
		return AG_LOG_WARN;
	if (!strcasecmp(s, "debug"))
		return AG_LOG_DEBUG;
	return AG_LOG_INFO;
}

void ag_log(int level, const char *fmt, ...)
{
	static const char *names[] = { "error", "warn", "info", "debug" };
	va_list ap;

	if (level > log_level)
		return;
	/* stderr is captured by procd (logd) on OpenWRT and journald on Linux */
	fprintf(stderr, "%s[%d] %s: ", log_ident, (int)getpid(), names[level]);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

uint64_t ag_mono_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

int64_t ag_wall_s(void)
{
	return (int64_t)time(NULL);
}

int ag_is_openwrt(void)
{
	return access("/etc/openwrt_release", F_OK) == 0;
}
