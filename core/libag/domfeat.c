/* SPDX-License-Identifier: Apache-2.0 */
/* Domain features, schema "domain-v1". Keep in sync with
 * cortex/training/domain_risk/features.py (checked by tests/test_agm.c). */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ag/domfeat.h"

/* Public suffixes with two labels that are common enough to matter.
 * A full Public Suffix List is a later improvement. */
static const char *const multi_suffix[] = {
	"co.uk", "org.uk", "ac.uk", "gov.uk", "ltd.uk", "plc.uk", "me.uk", "net.uk",
	"co.in", "net.in", "org.in", "gov.in", "ac.in", "edu.in", "nic.in", "res.in", "firm.in", "gen.in", "ind.in",
	"co.jp", "ne.jp", "or.jp", "ac.jp", "go.jp",
	"com.au", "net.au", "org.au", "edu.au", "gov.au",
	"com.br", "net.br", "org.br", "gov.br",
	"com.cn", "net.cn", "org.cn", "gov.cn", "edu.cn",
	"co.nz", "net.nz", "org.nz", "co.za", "org.za", "gov.za",
	"com.sg", "edu.sg", "gov.sg", "com.my", "co.kr", "or.kr", "com.tw", "com.hk",
	"com.mx", "com.tr", "com.ar", "com.co", "com.pk", "com.bd", "com.np", "com.lk",
	"co.id", "or.id", "ac.id", "go.id", "co.th", "in.th", "com.vn", "com.ph",
	"com.sa", "com.eg", "com.ng", "co.ke", "com.ua", "co.il", "org.il", "ac.il",
	"github.io", "blogspot.com", "herokuapp.com", "azurewebsites.net", "cloudfront.net",
	"amazonaws.com", "appspot.com", "firebaseapp.com", "netlify.app", "vercel.app",
	NULL
};

/* Names that are not internet domains. */
static const char *const skip_tld[] = {
	"arpa", "local", "lan", "home", "internal", "localdomain", "localhost", "test",
	"invalid", "example", "onion", "intranet", "corp", "private", NULL
};

static int in_list(const char *s, const char *const *list)
{
	for (; *list; list++)
		if (!strcmp(s, *list))
			return 1;
	return 0;
}

int ag_domain_registrable(const char *qname, char *out, size_t n)
{
	char label[256], name[256];
	size_t len, i, ll;

	if (ag_domain_label(qname, label, sizeof(label)) < 0)
		return -1;
	snprintf(name, sizeof(name), "%s", qname);
	for (i = 0; name[i]; i++)
		name[i] = (char)tolower((unsigned char)name[i]);
	len = strlen(name);
	while (len && name[len - 1] == '.')
		name[--len] = '\0';
	/* the label is the registrable part: find it as a whole label, scanning
	 * from the right because a subdomain may repeat it */
	ll = strlen(label);
	for (i = len - ll + 1; i-- > 0;) {
		if (!strncmp(name + i, label, ll) && (i == 0 || name[i - 1] == '.') && name[i + ll] == '.') {
			snprintf(out, n, "%s", name + i);
			return 0;
		}
	}
	return -1;
}

int ag_domain_label(const char *qname, char *label, size_t n)
{
	char name[256], *labels[128];
	int nl = 0, i, numeric = 1;
	size_t len;
	char two[300];
	const char *pick;

	if (!qname || !n)
		return -1;
	snprintf(name, sizeof(name), "%s", qname);
	for (i = 0; name[i]; i++)
		name[i] = (char)tolower((unsigned char)name[i]);
	len = strlen(name);
	while (len && name[len - 1] == '.')
		name[--len] = '\0';
	if (!len || !strchr(name, '.'))
		return -1;

	labels[nl++] = name;
	for (i = 0; name[i] && nl < 128; i++) {
		if (name[i] == '.') {
			name[i] = '\0';
			labels[nl++] = &name[i + 1];
		}
	}
	for (i = 0; i < nl; i++) {
		const char *p;

		for (p = labels[i]; *p; p++)
			if (!isdigit((unsigned char)*p))
				numeric = 0;
	}
	if (numeric || in_list(labels[nl - 1], skip_tld))
		return -1;

	snprintf(two, sizeof(two), "%s.%s", labels[nl - 2], labels[nl - 1]);
	if (nl >= 3 && in_list(two, multi_suffix))
		pick = labels[nl - 3];
	else
		pick = labels[nl - 2];
	if (!*pick)
		return -1;
	snprintf(label, n, "%s", pick);
	return 0;
}

static int bigram_index(unsigned char c)
{
	if (c >= 'a' && c <= 'z')
		return c - 'a';
	if (c >= '0' && c <= '9')
		return 26 + (c - '0');
	return 36; /* '-' and anything unusual */
}

static int is_vowel(unsigned char c)
{
	return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}

void ag_domain_features(const char *qname, const char *label, const float *bigram, int dim, float *out)
{
	int counts[256] = { 0 };
	size_t L = strlen(label), i;
	int digits = 0, letters = 0, vowels = 0, hyphens = 0, run = 0, maxrun = 0;
	int transitions = 0, uniq = 0, hex = 1, nlabels = 1;
	double entropy = 0, sum = 0, minlp = 0;
	size_t nb = 0, qlen;
	int prev = 37, cur;

	memset(out, 0, AG_DOMFEAT_N * sizeof(float));
	if (L == 0)
		return;

	for (i = 0; i < L; i++) {
		unsigned char c = (unsigned char)label[i];

		counts[c]++;
		if (c >= '0' && c <= '9')
			digits++;
		if (c >= 'a' && c <= 'z')
			letters++;
		if (is_vowel(c))
			vowels++;
		if (c == '-')
			hyphens++;
		if (c >= 'a' && c <= 'z' && !is_vowel(c)) {
			run++;
			if (run > maxrun)
				maxrun = run;
		} else {
			run = 0;
		}
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			hex = 0;
		if (i > 0) {
			unsigned char p = (unsigned char)label[i - 1];
			int d0 = p >= '0' && p <= '9', d1 = c >= '0' && c <= '9';
			int l0 = p >= 'a' && p <= 'z', l1 = c >= 'a' && c <= 'z';

			if ((d0 && l1) || (l0 && d1))
				transitions++;
		}
	}
	for (i = 0; i < 256; i++) {
		if (counts[i]) {
			double p = (double)counts[i] / (double)L;

			entropy -= p * log2(p);
			uniq++;
		}
	}

	/* character bigram log-probabilities under the benign-domain model */
	if (bigram && dim == AG_BIGRAM_DIM) {
		for (i = 0; i <= L; i++) {
			cur = i < L ? bigram_index((unsigned char)label[i]) : 37;
			{
				double lp = (double)bigram[prev * dim + cur];

				sum += lp;
				if (nb == 0 || lp < minlp)
					minlp = lp;
				nb++;
			}
			prev = cur;
		}
	}

	qlen = strlen(qname);
	while (qlen && qname[qlen - 1] == '.')
		qlen--;
	for (i = 0; i < qlen; i++)
		if (qname[i] == '.')
			nlabels++;

	out[0] = (float)L;
	out[1] = (float)entropy;
	out[2] = (float)((double)digits / (double)L);
	out[3] = (float)((double)vowels / (double)(letters > 0 ? letters : 1));
	out[4] = (float)maxrun;
	out[5] = (float)hyphens;
	out[6] = (float)nlabels;
	out[7] = nb ? (float)(sum / (double)nb) : 0.0f;
	out[8] = nb ? (float)minlp : 0.0f;
	out[9] = (float)transitions;
	out[10] = (float)((double)uniq / (double)L);
	out[11] = (L >= 8 && hex) ? 1.0f : 0.0f;
}
