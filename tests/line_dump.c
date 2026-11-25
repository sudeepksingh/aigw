/* SPDX-License-Identifier: Apache-2.0 */
/*
 * For tests/test_areapredict.py: the gateway's own rules (core/libag/line.c) on
 * series it is given, so the test can check cortex's Python copy gives the same.
 *
 *   stdin    one series a line: <metric key> <today> <day>:<value> <day>:<value> ...
 *   stdout   one line each: <state> <days_left> <conf> <level> <slope> <onset> <tau7> <slope7>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag/line.h"

int main(void)
{
	static char line[8192];
	struct line_params p;

	line_params_default(&p);
	while (fgets(line, sizeof(line), stdin)) {
		struct line_day d[LINE_DAYS];
		struct line_trend t;
		char *tok, *save = NULL;
		long today;
		int m = -1, k, n = 0;

		memset(d, 0, sizeof(d));
		tok = strtok_r(line, " \n", &save);
		for (k = 0; tok && k < LINE_METRICS; k++)
			if (!strcmp(tok, line_metric_key(k)))
				m = k;
		tok = strtok_r(NULL, " \n", &save);
		today = tok ? atol(tok) : 0;
		while ((tok = strtok_r(NULL, " \n", &save)) && n < LINE_DAYS) {
			char *c = strchr(tok, ':');

			if (!c)
				continue;
			d[n].day = (int32_t)atol(tok);
			d[n].v = strtof(c + 1, NULL);
			d[n].n = 1;
			n++;
		}
		line_analyse(d, n, today, m, &p, &t);
		printf("%s %d %d %.4f %.5f %ld %.4f %.5f\n", t.state ? line_state_name(t.state) : "-", t.days_left, t.conf,
		       (double)t.level, (double)t.slope, (long)t.onset, (double)t.tau7, (double)t.slope7);
	}
	return 0;
}
