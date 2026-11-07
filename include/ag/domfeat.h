/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Domain features, schema "domain-v1": the inputs of the domain-risk model.
 * The Python training code (cortex/training/domain_risk/features.py) must
 * compute exactly the same values; tests/test_agm.c checks this against
 * golden data written by the trainer.
 *
 * Features describe the registrable label ("example" in www.example.co.uk),
 * because malware generates registrable domains; random-looking CDN
 * subdomains (r3---sn-x.googlevideo.com) are not judged.
 */
#ifndef AG_DOMFEAT_H
#define AG_DOMFEAT_H

#include <stddef.h>

#define AG_DOMFEAT_SCHEMA "domain-v1"
#define AG_DOMFEAT_N      12
#define AG_BIGRAM_DIM     38   /* a-z, 0-9, '-', boundary */

/* The registrable label of a DNS name, lower-case. Returns 0, or -1 when the
 * name has none worth judging (no dot, IP literal, .arpa, .local, .lan, ...). */
int ag_domain_label(const char *qname, char *label, size_t n);

/* Fills out[AG_DOMFEAT_N]. bigram is the model's dim x dim log-prob table. */
void ag_domain_features(const char *qname, const char *label, const float *bigram, int dim,
			float *out);

#endif
