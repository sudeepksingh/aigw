# SPDX-License-Identifier: Apache-2.0
"""Domain features, schema "domain-v1".

Python twin of core/libag/domfeat.c. Both must produce the same numbers;
tests/test_agm.c checks the C side against golden data written here.
"""
import math

import numpy as np

SCHEMA = "domain-v1"
N_FEATURES = 12
BIGRAM_DIM = 38
BOUNDARY = 37

FEATURE_NAMES = [
    "length", "entropy", "digit_ratio", "vowel_ratio", "max_consonant_run",
    "hyphens", "labels", "bigram_mean", "bigram_min", "digit_letter_switches",
    "unique_ratio", "hex_like",
]

MULTI_SUFFIX = {
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
}

SKIP_TLD = {
    "arpa", "local", "lan", "home", "internal", "localdomain", "localhost", "test",
    "invalid", "example", "onion", "intranet", "corp", "private",
}


def domain_label(qname):
    """Registrable label of a DNS name, or None when there is nothing to judge."""
    name = qname.lower().rstrip(".")
    if not name or "." not in name:
        return None
    labels = name.split(".")[:128]
    if all(c.isdigit() for lab in labels for c in lab) or labels[-1] in SKIP_TLD:
        return None
    two = labels[-2] + "." + labels[-1]
    pick = labels[-3] if len(labels) >= 3 and two in MULTI_SUFFIX else labels[-2]
    return pick or None


def bigram_index(c):
    if "a" <= c <= "z":
        return ord(c) - ord("a")
    if "0" <= c <= "9":
        return 26 + ord(c) - ord("0")
    return 36


def bigram_table(labels):
    """Add-one smoothed character bigram log-probabilities (natural log), float32."""
    counts = np.ones((BIGRAM_DIM, BIGRAM_DIM), dtype=np.float64)
    for lab in labels:
        prev = BOUNDARY
        for c in lab:
            cur = bigram_index(c)
            counts[prev, cur] += 1
            prev = cur
        counts[prev, BOUNDARY] += 1
    return np.log(counts / counts.sum(axis=1, keepdims=True)).astype(np.float32)


VOWELS = set("aeiou")


def domain_features(qname, label, table):
    out = [0.0] * N_FEATURES
    L = len(label)
    if L == 0:
        return np.array(out, dtype=np.float32)
    digits = letters = vowels = hyphens = run = maxrun = transitions = 0
    hexok = True
    counts = {}
    for i, c in enumerate(label):
        counts[c] = counts.get(c, 0) + 1
        isd, isl = "0" <= c <= "9", "a" <= c <= "z"
        digits += isd
        letters += isl
        vowels += c in VOWELS
        hyphens += c == "-"
        if isl and c not in VOWELS:
            run += 1
            maxrun = max(maxrun, run)
        else:
            run = 0
        if not (isd or "a" <= c <= "f"):
            hexok = False
        if i > 0:
            p = label[i - 1]
            d0, l0 = "0" <= p <= "9", "a" <= p <= "z"
            if (d0 and isl) or (l0 and isd):
                transitions += 1
    entropy = 0.0
    for ch in sorted(counts, key=lambda ch: ord(ch) if ord(ch) < 256 else 255):
        p = counts[ch] / L
        entropy -= p * math.log2(p)
    s, mn, nb, prev = 0.0, 0.0, 0, BOUNDARY
    for i in range(L + 1):
        cur = bigram_index(label[i]) if i < L else BOUNDARY
        lp = float(table[prev, cur])
        s += lp
        if nb == 0 or lp < mn:
            mn = lp
        nb += 1
        prev = cur
    nlabels = qname.rstrip(".").count(".") + 1
    out[0] = L
    out[1] = entropy
    out[2] = digits / L
    out[3] = vowels / (letters if letters > 0 else 1)
    out[4] = maxrun
    out[5] = hyphens
    out[6] = nlabels
    out[7] = s / nb
    out[8] = mn
    out[9] = transitions
    out[10] = len(counts) / L
    out[11] = 1.0 if (L >= 8 and hexok) else 0.0
    return np.array(out, dtype=np.float32)
