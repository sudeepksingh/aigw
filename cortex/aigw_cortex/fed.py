# SPDX-License-Identifier: Apache-2.0
"""Federated learning rounds: homes improve the domain-risk judgement together
without sending the names they answered about.

    round r ── GET /v1/fed: the shared layer (14 numbers) and how to train
       │
    homes ──── each trains on its own answers and sends only a clipped,
       │       noised change: POST /v1/fed/update {round, delta, n, noise, clip}
       ▼
    average ── once min_homes changes are in (and the round is at least gap
       │       seconds old): new layer = layer + mean(change), every home
       │       weighing the same
       ▼
    publish ── signed "domain-adapter" file through /v1/models; each gateway
               checks it (anchor names) before using it; round r+1 starts

The layer (agd/fed.c): score = sigmoid(w0*logit(trees) + sum wi*(xi-ci)/si + b).
"""
import hashlib
import json
import math
import os
import threading
import time

from . import usign

_lock = threading.RLock()     # one round change at a time (the server answers in threads)

NAME = "domain-adapter"
N_FEAT = 12
N_W = N_FEAT + 2

# feature order: length, entropy, digit_ratio, vowel_ratio, max_consonant_run, hyphens,
# labels, bigram_mean, bigram_min, digit_letter_switches, unique_ratio, hex_like
CENTER = [10.0, 3.0, 0.05, 0.35, 3.0, 0.1, 2.2, -3.0, -4.5, 0.5, 0.85, 0.05]
SCALE = [4.0, 0.6, 0.15, 0.15, 2.5, 0.5, 0.5, 0.6, 1.2, 2.0, 0.12, 0.25]
IDENTITY = [1.0] + [0.0] * (N_W - 1)

DELTA = 1e-5          # tables: fed_rounds, fed_updates (store.py)


class Settings:
    def __init__(self, enabled=True, min_homes=3, gap=86400, lr=0.5, epochs=50, clip=1.0, noise=1.0):
        self.enabled, self.min_homes, self.gap = enabled, int(min_homes), int(gap)
        self.lr, self.epochs, self.clip, self.noise = float(lr), int(epochs), float(clip), float(noise)


def epsilon(sigma, rounds=1, delta=DELTA):
    """Per-home privacy after `rounds` rounds at noise multiplier sigma (zCDP -> (eps, delta))."""
    if sigma <= 0:
        return math.inf
    rho = rounds / (2 * sigma * sigma)
    return rho + 2 * math.sqrt(rho * math.log(1 / delta))


def current(store, s):
    """The open round, made (round 1 from the identity layer) when there is none."""
    with _lock:
        return _current(store)


def _current(store):
    r = store.q("SELECT * FROM fed_rounds WHERE state = 'open' ORDER BY round DESC LIMIT 1")
    if r:
        return r[0]
    last = store.q("SELECT * FROM fed_rounds ORDER BY round DESC LIMIT 1")
    weights = json.loads(last[0]["weights"]) if last else IDENTITY
    n = (last[0]["round"] + 1) if last else 1
    store.x("INSERT INTO fed_rounds (round, started, weights, state) VALUES (?, ?, ?, 'open')",
            (n, int(time.time()), json.dumps(weights)))
    return store.q("SELECT * FROM fed_rounds WHERE round = ?", (n,))[0]


def round_for_gateway(store, s):
    if not s.enabled:
        return {"round": 0, "why": "federated learning is off on this cortex"}
    r = current(store, s)
    return {"round": r["round"], "model": NAME, "center": CENTER, "scale": SCALE,
            "weights": json.loads(r["weights"]), "lr": s.lr, "epochs": s.epochs, "clip": s.clip,
            "noise": s.noise, "min_homes": s.min_homes}


def _finite(v, limit):
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) and abs(v) <= limit


def accept(store, s, gw, body):
    """A home's change for the open round. Returns (http code, answer)."""
    with _lock:
        return _accept(store, s, gw, body)


def _accept(store, s, gw, body):
    if not s.enabled:
        return 409, {"ok": False, "error": "federated learning is off"}
    r = current(store, s)
    if body.get("round") != r["round"]:
        return 409, {"ok": False, "error": f"round {body.get('round')} is not the open round ({r['round']})"}
    delta, n = body.get("delta"), body.get("n")
    noise, clip = body.get("noise", 0), body.get("clip", s.clip)
    bound = 20 * s.clip * (1 + (noise if _finite(noise, 100) else 0))
    if not isinstance(delta, list) or len(delta) != N_W or not all(_finite(x, bound) for x in delta):
        return 400, {"ok": False, "error": f"delta must be {N_W} finite numbers"}
    if not isinstance(n, int) or isinstance(n, bool) or not 1 <= n <= 100000:
        return 400, {"ok": False, "error": "n must be a whole number of examples"}
    if not _finite(noise, 100) or noise < 0 or not _finite(clip, 10) or clip <= 0:
        return 400, {"ok": False, "error": "noise and clip must be sane numbers"}
    if store.q("SELECT 1 FROM fed_updates WHERE round = ? AND gateway = ?", (r["round"], gw)):
        return 409, {"ok": False, "error": "this gateway already sent its change for this round"}
    store.x("INSERT INTO fed_updates VALUES (?, ?, ?, ?, ?, ?, ?)",
            (r["round"], gw, json.dumps([float(x) for x in delta]), n, float(noise), float(clip), int(time.time())))
    result = _aggregate(store, s, False)
    return 200, {"ok": True, "round": r["round"], "published": result.get("version", 0) if result else 0}


def aggregate(store, s, force=False):
    """Average the open round's changes and publish the new layer, when it is time."""
    with _lock:
        return _aggregate(store, s, force)


def _aggregate(store, s, force):
    r = current(store, s)
    ups = store.q("SELECT * FROM fed_updates WHERE round = ?", (r["round"],))
    if not ups:
        return None
    if not force and (len(ups) < s.min_homes or time.time() - r["started"] < s.gap):
        return None
    w = json.loads(r["weights"])
    deltas = [json.loads(u["delta"]) for u in ups]
    mean = [sum(d[i] for d in deltas) / len(deltas) for i in range(N_W)]
    new = [w[i] + mean[i] for i in range(N_W)]
    noise = min(u["noise"] for u in ups)
    now = int(time.time())
    why = ""
    if not 0.2 <= new[0] <= 5:
        why = f"weight of the trees' score would be {new[0]:.2f} (outside 0.2 .. 5)"
    elif not all(math.isfinite(x) and abs(x) <= 50 for x in new):
        why = "a weight would be out of range"
    if why:
        store.x("UPDATE fed_rounds SET state = 'refused', closed = ?, homes = ?, note = ? WHERE round = ?",
                (now, len(ups), why, r["round"]))
        current(store, s)
        return {"round": r["round"], "homes": len(ups), "refused": why}
    eps = epsilon(noise)
    note = (f"round {r['round']}: {len(ups)} homes, {sum(u['n'] for u in ups)} answers, "
            f"noise {noise:.2f} (per-home eps {eps:.2f} at delta {DELTA:g} for this round)")
    version = publish(store, new, note)
    store.x("UPDATE fed_rounds SET state = 'published', closed = ?, homes = ?, version = ?, note = ? WHERE round = ?",
            (now, len(ups), version, note, r["round"]))
    store.x("INSERT INTO fed_rounds (round, started, weights, state) VALUES (?, ?, ?, 'open')",
            (r["round"] + 1, now, json.dumps(new)))
    return {"round": r["round"], "homes": len(ups), "version": version, "weights": new, "note": note}


def publish(store, weights, note):
    from .__main__ import signing_key        # one key for models, lists and the layer
    version = store.next_version(NAME)
    stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    fmt = lambda xs: " ".join(f"{x:.6g}" for x in xs)
    data = (f"# aigw-adapter {NAME}\n# version {version}\n# published {stamp}\n# {note}\n"
            f"center {fmt(CENTER)}\nscale {fmt(SCALE)}\nweights {fmt(weights)}\n").encode()
    sec, _ = signing_key(store.dir)
    fname = f"{NAME}-{version}.adapter"
    out = os.path.join(store.models_dir(), fname)
    with open(out + ".sig.tmp", "w") as f:
        f.write(usign.sign(sec, data))
    with open(out + ".tmp", "wb") as f:
        f.write(data)
    os.replace(out + ".sig.tmp", out + ".sig")
    os.replace(out + ".tmp", out)
    store.add_model(NAME, version, fname, hashlib.sha256(data).hexdigest(), len(data), note)
    return version


def status(store, s):
    rounds = store.q("SELECT round, started, state, closed, homes, version, note FROM fed_rounds ORDER BY round DESC "
                     "LIMIT 20")
    if s.enabled:
        r = current(store, s)
        waiting = store.q("SELECT COUNT(*) n FROM fed_updates WHERE round = ?", (r["round"],))[0]["n"]
    else:
        r, waiting = None, 0
    return {"enabled": s.enabled, "min_homes": s.min_homes, "gap": s.gap, "noise": s.noise, "clip": s.clip,
            "lr": s.lr, "epochs": s.epochs, "per_round_epsilon": epsilon(s.noise) if s.noise > 0 else None,
            "open_round": r["round"] if r else 0, "changes_in": waiting, "rounds": rounds}
