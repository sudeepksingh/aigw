#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Train AIGW's domain-risk model and export it as an .agm file.

    python3 cortex/training/domain_risk/train.py            # writes models/domain-risk.agm
    python3 cortex/training/domain_risk/train.py --top 100000 --trees 120

Benign examples: the Tranco list of popular sites (https://tranco-list.eu).
Risky examples: synthetic algorithmically generated domains (dga.py).
The model scores how machine-generated a domain's registrable label looks.
"""
import argparse
import csv
import io
import json
import os
import random
import struct
import sys
import time
import urllib.request
import zipfile

import numpy as np
from sklearn.ensemble import GradientBoostingClassifier
from sklearn.metrics import roc_auc_score

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dga import FAMILIES, generate  # noqa: E402
from features import (BIGRAM_DIM, FEATURE_NAMES, N_FEATURES, SCHEMA,  # noqa: E402
                      bigram_table, domain_features, domain_label)

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
TRANCO_URL = "https://tranco-list.eu/top-1m.csv.zip"
CACHE = os.path.join(os.path.expanduser("~"), ".cache", "aigw-train")
THRESHOLD = 0.98   # alert level: chosen for few false alarms on everyday traffic

# Hosts under a registrable domain. Generated names are trained under these as
# often as popular ones, so the number of labels says nothing about risk: a
# model that learned "more labels = safe" would miss www.<generated name>.
SUBDOMAINS = ["www", "mail", "cdn", "api", "m", "app", "static", "img", "login", "portal", "news", "shop",
              "blog", "dev", "files", "update", "secure", "ns1", "smtp", "web"]


def with_subdomain(q, r):
    host = r.choice(SUBDOMAINS)
    if r.random() < 0.2:
        host = r.choice(SUBDOMAINS) + "." + host
    return host + "." + q


def augment(items, share, r):
    """(qname, label) pairs, a share of them moved under a subdomain (same label)."""
    return [(with_subdomain(q, r), lab) if r.random() < share else (q, lab) for q, lab in items]


def read_agm(path):
    """(base, trees, bigram table) of an existing .agm, to compare a new model with it."""
    with open(path, "rb") as f:
        data = f.read()
    n_sec = struct.unpack_from("<I", data, 48)[0]
    off, base, trees, table = 52, 0.0, [], None
    for _ in range(n_sec):
        t, n = struct.unpack_from("<II", data, off)
        off += 8
        sec = data[off:off + n]
        off += n
        if t == 1:
            base, nt = struct.unpack_from("<dI", sec, 0)
            p = 12
            for _ in range(nt):
                (nn,) = struct.unpack_from("<I", sec, p)
                p += 4
                nodes = []
                for _ in range(nn):
                    f_, v, l, r = struct.unpack_from("<idii", sec, p)
                    p += 20
                    nodes.append((f_, v, l, r))
                trees.append(nodes)
        elif t == 2:
            (dim,) = struct.unpack_from("<I", sec, 0)
            table = np.frombuffer(sec[4:4 + dim * dim * 4], dtype="<f4").reshape(dim, dim).astype(np.float64)
    return base, trees, table


def report(name, score, b_test, d_test, r):
    """False alarms and catch rate, for names as they are and under www. (a share of real lookups)."""
    www = random.Random(r)
    rows = []
    for variant in ("as listed", "under a subdomain"):
        sub = variant != "as listed"
        pb = np.array([score(with_subdomain(q, www) if sub else q, lab) for q, lab in b_test])
        pd = np.array([score(with_subdomain(q, www) if sub else q, lab) for _, lab, q in d_test])
        rows.append((variant, float((pb >= THRESHOLD).mean()), float((pd >= THRESHOLD).mean())))
    print(f"{name}: at score >= {THRESHOLD}")
    for variant, fa, caught in rows:
        print(f"  {variant:<18} false alarms {fa * 100:6.3f}%   caught {caught * 100:6.2f}%")
    return rows


def fnv1a(s):
    h = 2166136261
    for b in s.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def load_tranco(top):
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, "tranco-top-1m.csv.zip")
    if not os.path.exists(path):
        print("downloading", TRANCO_URL)
        req = urllib.request.Request(TRANCO_URL, headers={"User-Agent": "aigw-trainer"})
        with urllib.request.urlopen(req, timeout=120) as r, open(path, "wb") as f:
            f.write(r.read())
    with zipfile.ZipFile(path) as z:
        name = z.namelist()[0]
        with z.open(name) as f:
            rows = csv.reader(io.TextIOWrapper(f, "utf-8"))
            domains = [row[1] for _, row in zip(range(top), rows)]
    return domains


def featurize(items, table):
    return np.vstack([domain_features(q, lab, table) for q, lab in items])


def tree_eval(trees, base, x):
    s = base
    for nodes in trees:
        i = 0
        while nodes[i][0] >= 0:
            f, thr, left, right = nodes[i]
            i = left if float(x[f]) <= thr else right
        s += nodes[i][1]
    return s


def export_trees(clf):
    lr = clf.learning_rate
    trees = []
    for est in clf.estimators_[:, 0]:
        t = est.tree_
        nodes = []
        for i in range(t.node_count):
            if t.children_left[i] == -1:
                nodes.append((-1, float(t.value[i][0][0]) * lr, -1, -1))
            else:
                nodes.append((int(t.feature[i]), float(t.threshold[i]),
                              int(t.children_left[i]), int(t.children_right[i])))
        trees.append(nodes)
    return trees


def write_agm(path, name, base, trees, table, meta):
    gbdt = struct.pack("<dI", base, len(trees))
    for nodes in trees:
        gbdt += struct.pack("<I", len(nodes))
        for f, v, l, r in nodes:
            gbdt += struct.pack("<idii", f, v, l, r)
    bigram = struct.pack("<I", BIGRAM_DIM) + table.astype("<f4").tobytes()
    metab = json.dumps(meta, sort_keys=True).encode()
    sections = [(1, gbdt), (2, bigram), (3, metab)]
    head = b"AGM1" + struct.pack("<HHII", 1, 2, N_FEATURES, fnv1a(SCHEMA))
    head += name.encode()[:32].ljust(32, b"\0") + struct.pack("<I", len(sections))
    body = b"".join(struct.pack("<II", t, len(d)) + d for t, d in sections)
    with open(path, "wb") as f:
        f.write(head + body)
    return len(head + body)


GOLDEN_NAMES = [
    "google.com", "www.wikipedia.org", "r3---sn-ci5gup-qxas.googlevideo.com", "bbc.co.uk",
    "www.irctc.co.in", "mail.google.com", "graph.facebook.com", "d1a2b3c4d5e6f7.cloudfront.net",
    "e1234.dscb.akamaiedge.net", "tunnel.googlezip.net", "conn-service-in-05.allawnos.com",
    "pagead2.googlesyndication.com", "xn--80ak6aa92e.com", "my-very-long-company-name.com",
    "a.io", "1.2.3.4", "printer.local", "205.0.42.10.in-addr.arpa",
    "xkqjzvbwptmr.com", "q8f3k2l9z7x1v.net", "9f86d081884c7d659a2feaa0c55ad015.info",
    "mfrggzdfmztwq2lk.biz", "kofamuveliza.top", "zbqxwvkdpt4821.ru",
    "www.xkqjzvbwptmr.com", "cdn.q8f3k2l9z7x1v.net", "www.bbc.co.uk", "mail.wikipedia.org",
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--top", type=int, default=200000, help="popular domains to learn from")
    ap.add_argument("--trees", type=int, default=150)
    ap.add_argument("--depth", type=int, default=3)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--out", default=os.path.join(REPO, "models", "domain-risk.agm"))
    ap.add_argument("--golden", default=os.path.join(REPO, "tests", "data", "domain-risk-golden.tsv"),
                    help="golden test file to write ('' = none)")
    ap.add_argument("--benign-file", help="popular domains, one per line, instead of downloading Tranco")
    ap.add_argument("--feedback", help="people's answers: <domain>\\t<0 benign | 1 risky> per line")
    ap.add_argument("--feedback-weight", type=float, default=50.0,
                    help="how much one answer counts against one generated example")
    ap.add_argument("--subdomain-share", type=float, default=0.35,
                    help="share of training names (both kinds) put under a subdomain such as www.")
    ap.add_argument("--compare", help="an existing .agm to compare with on the same held-out names")
    args = ap.parse_args()
    rng = random.Random(args.seed)

    if args.benign_file:
        with open(args.benign_file) as f:
            domains = [ln.strip() for ln in f if ln.strip() and not ln.startswith("#")]
    else:
        domains = load_tranco(args.top)
    fb_items, fb_y = [], []
    if args.feedback:
        with open(args.feedback) as f:
            for ln in f:
                parts = ln.rstrip("\n").split("\t")
                lab = domain_label(parts[0]) if len(parts) == 2 and parts[1] in ("0", "1") else None
                if lab:
                    fb_items.append((parts[0], lab))
                    fb_y.append(int(parts[1]))
    seen, benign = set(), []
    for d in domains:
        lab = domain_label(d)
        if lab and lab not in seen:
            seen.add(lab)
            benign.append((d, lab))
    rng.shuffle(benign)
    cut = int(len(benign) * 0.8)
    b_train, b_test = benign[:cut], benign[cut:]
    table = bigram_table([lab for _, lab in b_train])

    per_fam = len(b_train) // len(FAMILIES)
    dga = generate(per_fam + per_fam // 4, args.seed)
    rng.shuffle(dga)
    dcut = int(len(dga) * 0.8)
    d_train, d_test = dga[:dcut], dga[dcut:]

    aug = random.Random(args.seed + 1)
    b_fit = augment(b_train, args.subdomain_share, aug)
    d_fit = augment([(q, l) for _, l, q in d_train], args.subdomain_share, aug)
    X = np.vstack([featurize(b_fit, table), featurize(d_fit, table)])
    y = np.array([0] * len(b_fit) + [1] * len(d_fit))
    w = np.ones(len(y))
    if fb_items:
        # a person's answer about a real lookup outweighs many synthetic examples
        X = np.vstack([X, featurize(fb_items, table)])
        y = np.concatenate([y, fb_y])
        w = np.concatenate([w, np.full(len(fb_y), args.feedback_weight)])
    print(f"training on {len(b_train)} popular + {len(d_train)} generated domains"
          f" + {len(fb_items)} answers from people, {N_FEATURES} features")
    t0 = time.time()
    clf = GradientBoostingClassifier(n_estimators=args.trees, max_depth=args.depth, learning_rate=0.1,
                                     subsample=0.8, random_state=args.seed)
    clf.fit(X, y, sample_weight=w)
    print(f"trained in {time.time() - t0:.0f}s")

    Xb = featurize(b_test, table)
    Xd = featurize([(q, l) for _, l, q in d_test], table)
    pb, pd = clf.predict_proba(Xb)[:, 1], clf.predict_proba(Xd)[:, 1]
    auc = roc_auc_score([0] * len(pb) + [1] * len(pd), np.concatenate([pb, pd]))
    fpr = float((pb >= THRESHOLD).mean())
    fams = {}
    for (fam, _, _), p in zip(d_test, pd):
        fams.setdefault(fam, []).append(p >= THRESHOLD)
    recall = {f: round(float(np.mean(v)), 4) for f, v in fams.items()}
    print("threshold  false-alarms(popular)  caught(generated)")
    for thr in (0.9, 0.95, 0.98, 0.99, 0.995):
        print(f"  {thr:<8} {float((pb >= thr).mean()) * 100:8.3f}%  {float((pd >= thr).mean()) * 100:12.2f}%")
    print(f"held-out AUC {auc:.4f}; at score >= {THRESHOLD}: false alarms on popular sites {fpr * 100:.2f}%")
    for f in FAMILIES:
        print(f"  caught {recall.get(f, 0) * 100:6.2f}%  {f}")
    worst = sorted(zip(pb, b_test), key=lambda t: -t[0])[:10]
    print("most suspicious popular sites:", ", ".join(f"{d}({p:.2f})" for p, (d, _) in worst))

    trees = export_trees(clf)
    base = float(clf._raw_predict_init(X[:1])[0, 0])
    new_rows = report("this model", lambda q, lab: 1 / (1 + np.exp(-tree_eval(trees, base, domain_features(q, lab, table)))),
                      b_test, d_test, args.seed + 2)
    old_rows = None
    if args.compare:
        ob, ot, otab = read_agm(args.compare)
        old_rows = report(f"compared with {os.path.basename(args.compare)}",
                          lambda q, lab: 1 / (1 + np.exp(-tree_eval(ot, ob, domain_features(q, lab, otab)))),
                          b_test, d_test, args.seed + 2)
    sample = np.vstack([Xb[:500], Xd[:500]])
    ours = np.array([tree_eval(trees, base, x) for x in sample])
    diff = float(np.max(np.abs(ours - clf.decision_function(sample))))
    assert diff < 1e-9, f"exported trees disagree with sklearn: {diff}"

    importance = {n: round(float(v), 4) for n, v in zip(FEATURE_NAMES, clf.feature_importances_)}
    fb_ok = None
    if fb_items:
        p = clf.predict_proba(featurize(fb_items, table))[:, 1]
        fb_ok = round(float(np.mean([(pi >= THRESHOLD) == bool(yi) for pi, yi in zip(p, fb_y)])), 4)
        print(f"agrees with {fb_ok * 100:.1f}% of people's answers at score >= {THRESHOLD}")
    src = f"{os.path.basename(args.benign_file)}" if args.benign_file else f"Tranco top {args.top}"
    meta = {
        "schema": SCHEMA, "features": FEATURE_NAMES, "trained": time.strftime("%Y-%m-%d"),
        "benign_source": f"{src} ({len(benign)} unique labels)",
        "feedback_labels": len(fb_items), "feedback_agreement": fb_ok,
        "risky_source": "synthetic DGA families: " + ", ".join(FAMILIES),
        "trees": args.trees, "depth": args.depth, "threshold": THRESHOLD,
        "heldout_auc": round(auc, 4), "heldout_false_alarm_rate": round(fpr, 5),
        "heldout_recall_by_family": recall, "feature_importance": importance,
        "subdomain_share": args.subdomain_share,
        "heldout_under_subdomain": {"false_alarm_rate": round(new_rows[1][1], 5), "caught": round(new_rows[1][2], 4)},
    }
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    size = write_agm(args.out, "domain-risk", base, trees, table, meta)
    print(f"wrote {args.out} ({size} bytes, {len(trees)} trees)")

    if not args.golden:
        return
    os.makedirs(os.path.dirname(args.golden), exist_ok=True)
    with open(args.golden, "w") as f:
        f.write("# qname\tlabel\t" + "\t".join(FEATURE_NAMES) + "\tprob\n")
        for q in GOLDEN_NAMES:
            lab = domain_label(q)
            if lab is None:
                f.write(f"{q}\t-\n")
                continue
            x = domain_features(q, lab, table)
            p = float(1.0 / (1.0 + np.exp(-tree_eval(trees, base, x))))
            f.write(f"{q}\t{lab}\t" + "\t".join(repr(float(v)) for v in x) + f"\t{p!r}\n")
    print(f"wrote {args.golden}")


if __name__ == "__main__":
    main()
