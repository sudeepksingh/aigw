# SPDX-License-Identifier: Apache-2.0
"""AIGW cortex command line.

    python3 -m aigw_cortex serve [--listen 0.0.0.0] [--port 8790] [--cert c --key k]
    python3 -m aigw_cortex enroll <gateway-name>      prints the gateway's token, once
    python3 -m aigw_cortex gateways | remove <name>
    python3 -m aigw_cortex pubkey                     the key gateways trust (agctl trust)
    python3 -m aigw_cortex publish <model.agm> [--notes text]
    python3 -m aigw_cortex retrain [--top N | --benign-file F] [--trees N]
    python3 -m aigw_cortex label <domain> benign|risky
    python3 -m aigw_cortex labels | models | proposals [gateway]
    python3 -m aigw_cortex propose <gateway> <action> <target> <reason...>
    python3 -m aigw_cortex mcp                        MCP server on stdio
    python3 -m aigw_cortex sign-plugin <folder>       sign a plug-in folder (plugin.sig)
    python3 -m aigw_cortex publish-intel [--min-homes N]  signed list of known threats
    python3 -m aigw_cortex fed [status]               federated learning: rounds, homes, privacy
    python3 -m aigw_cortex fed aggregate              close the open round now (with what came in)

serve takes the round settings: --fed-min-homes 3 --fed-gap 86400 (seconds
between rounds) --fed-noise 1.0 --fed-clip 1.0 --fed-lr 0.5 --fed-epochs 50,
or --no-fed.

Data (database, signing key, published models) lives in --data, default
$AIGW_CORTEX_DATA or ~/.aigw-cortex.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time

from . import agm, usign
from .store import Store

HERE = os.path.dirname(os.path.abspath(__file__))
TRAINER = os.path.join(HERE, "..", "training", "domain_risk", "train.py")


def key_paths(data):
    d = os.path.join(data, "keys")
    return os.path.join(d, "cortex.key"), os.path.join(d, "cortex.pub")


def signing_key(data):
    """The cortex's usign key pair, made on first use. Returns (secret text, public path)."""
    sk, pk = key_paths(data)
    if not os.path.exists(sk):
        os.makedirs(os.path.dirname(sk), mode=0o700, exist_ok=True)
        sec, pub, fp = usign.generate()
        fd = os.open(sk, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(sec)
        with open(pk, "w") as f:
            f.write(pub)
        print(f"made a new signing key {fp}; gateways trust it with:  agctl trust {pk}", file=sys.stderr)
    with open(sk) as f:
        return f.read(), pk


def publish(store, path, notes=""):
    with open(path, "rb") as f:
        data = f.read()
    head, _ = agm.read(data)
    name = head["name"]
    version = store.next_version(name)
    data = agm.stamp(data, version=version, published=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    sec, _ = signing_key(store.dir)
    fname = f"{name}-{version}.agm"
    out = os.path.join(store.models_dir(), fname)
    with open(out + ".tmp", "wb") as f:
        f.write(data)
    with open(out + ".sig.tmp", "w") as f:
        f.write(usign.sign(sec, data))
    os.replace(out + ".sig.tmp", out + ".sig")
    os.replace(out + ".tmp", out)
    sha = hashlib.sha256(data).hexdigest()
    store.add_model(name, version, fname, sha, len(data), notes)
    return name, version, fname


def publish_intel(store, min_homes=2):
    entries = store.intel(min_homes)
    version = store.next_version("threat-intel")
    stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    lines = ["# aigw-list threat-intel", f"# version {version}", f"# published {stamp}",
             f"# names confirmed by people, or flagged in at least {min_homes} homes"]
    lines += [f"{name}\t{reason}" for name, reason in entries]
    data = ("\n".join(lines) + "\n").encode()
    sec, _ = signing_key(store.dir)
    fname = f"threat-intel-{version}.list"
    out = os.path.join(store.models_dir(), fname)
    with open(out + ".sig.tmp", "w") as f:
        f.write(usign.sign(sec, data))
    with open(out + ".tmp", "wb") as f:
        f.write(data)
    os.replace(out + ".sig.tmp", out + ".sig")
    os.replace(out + ".tmp", out)
    store.add_model("threat-intel", version, fname, hashlib.sha256(data).hexdigest(), len(data),
                    f"{len(entries)} names")
    return version, len(entries)


def plugin_digest(folder):
    """The text a plug-in signature covers (same as agd/plugin.c check_signature)."""
    paths = []
    for root, dirs, files in os.walk(folder):
        for n in dirs + files:
            full = os.path.join(root, n)
            rel = os.path.relpath(full, folder).replace(os.sep, "/")
            if os.path.islink(full):
                raise ValueError(f"{rel}: links can't be signed")
            if n in files and rel != "plugin.sig":
                paths.append(rel)
    out = []
    for rel in sorted(paths, key=lambda s: s.encode()):
        with open(os.path.join(folder, rel), "rb") as f:
            out.append(f"{hashlib.sha512(f.read()).hexdigest()}  {rel}\n")
    return "".join(out).encode()


def sign_plugin(store, folder):
    if not os.path.isfile(os.path.join(folder, "manifest.json")):
        sys.exit(f"{folder} has no manifest.json")
    sec, pub = signing_key(store.dir)
    sig = usign.sign(sec, plugin_digest(folder))
    with open(os.path.join(folder, "plugin.sig"), "w") as f:
        f.write(sig)
    return pub


def retrain(store, args):
    labels = store.labels("dga")
    with tempfile.TemporaryDirectory() as tmp:
        fb = os.path.join(tmp, "feedback.tsv")
        with open(fb, "w") as f:
            for r in labels:
                f.write(f"{r['subject']}\t{r['label']}\n")
        out = os.path.join(tmp, "domain-risk.agm")
        cmd = [args.python or sys.executable, args.trainer, "--feedback", fb, "--out", out, "--golden", "",
               "--trees", str(args.trees)]
        if args.benign_file:
            cmd += ["--benign-file", args.benign_file]
        else:
            cmd += ["--top", str(args.top)]
        print(f"retraining domain-risk with {len(labels)} label(s) from people's answers", file=sys.stderr)
        r = subprocess.run(cmd)
        if r.returncode:
            sys.exit(f"training failed ({r.returncode})")
        benign = sum(1 for x in labels if x["label"] == 0)
        notes = f"retrained with {len(labels)} label(s): {benign} false alarm(s), {len(labels) - benign} confirmed"
        return publish(store, out, notes)


def show(rows):
    print(json.dumps(rows, indent=1))


def fed_args(p):
    p.add_argument("--no-fed", action="store_true", help="no federated learning rounds")
    p.add_argument("--fed-min-homes", type=int, default=3, help="changes needed to close a round")
    p.add_argument("--fed-gap", type=int, default=86400, help="seconds a round stays open at least")
    p.add_argument("--fed-noise", type=float, default=1.0, help="noise multiplier homes are asked for")
    p.add_argument("--fed-clip", type=float, default=1.0)
    p.add_argument("--fed-lr", type=float, default=0.5)
    p.add_argument("--fed-epochs", type=int, default=50)


def fed_settings(a):
    from .fed import Settings
    return Settings(enabled=not a.no_fed, min_homes=a.fed_min_homes, gap=a.fed_gap, lr=a.fed_lr,
                    epochs=a.fed_epochs, clip=a.fed_clip, noise=a.fed_noise)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="aigw_cortex", description="AIGW cortex")
    ap.add_argument("--data", default=os.environ.get("AIGW_CORTEX_DATA", os.path.expanduser("~/.aigw-cortex")))
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("serve")
    p.add_argument("--listen", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8790)
    p.add_argument("--cert")
    p.add_argument("--key")
    p.add_argument("--quiet", action="store_true")
    fed_args(p)
    sub.add_parser("enroll").add_argument("name")
    sub.add_parser("gateways")
    sub.add_parser("remove").add_argument("name")
    sub.add_parser("pubkey")
    p = sub.add_parser("publish")
    p.add_argument("model")
    p.add_argument("--notes", default="")
    p = sub.add_parser("retrain")
    p.add_argument("--top", type=int, default=200000)
    p.add_argument("--benign-file")
    p.add_argument("--trees", type=int, default=150)
    p.add_argument("--trainer", default=TRAINER)
    p.add_argument("--python")
    p = sub.add_parser("label")
    p.add_argument("domain")
    p.add_argument("verdict", choices=("benign", "risky"))
    sub.add_parser("labels")
    sub.add_parser("models")
    sub.add_parser("proposals").add_argument("gateway", nargs="?")
    p = sub.add_parser("propose")
    p.add_argument("gateway")
    p.add_argument("action")
    p.add_argument("target")
    p.add_argument("reason", nargs="+")
    p = sub.add_parser("history")
    p.add_argument("gateway", nargs="?")
    sub.add_parser("mcp")
    sub.add_parser("sign-plugin").add_argument("folder")
    sub.add_parser("publish-intel").add_argument("--min-homes", type=int, default=2)
    p = sub.add_parser("fed")
    p.add_argument("what", nargs="?", default="status", choices=("status", "aggregate"))
    fed_args(p)
    a = ap.parse_args(argv)

    store = Store(a.data)
    if a.cmd == "serve":
        from .server import serve
        serve(store, a.listen, a.port, a.cert, a.key, log=not a.quiet, fed_settings=fed_settings(a))
    elif a.cmd == "fed":
        from . import fed
        s = fed_settings(a)
        if a.what == "aggregate":
            r = fed.aggregate(store, s, force=True)
            if not r:
                print("no changes in the open round yet")
            elif r.get("refused"):
                print(f"round {r['round']} ({r['homes']} homes) not published: {r['refused']}")
            else:
                print(f"published domain-adapter version {r['version']}: {r['note']}")
        else:
            show(fed.status(store, s))
    elif a.cmd == "enroll":
        gid, token = store.enroll(a.name)
        print(f"gateway {a.name} ({gid}) enrolled. On the gateway, as root:\n"
              f"  agctl bridge <this cortex's URL>\n"
              f"  agctl bridge token   (then paste the token)\n"
              f"Token (shown only now):", file=sys.stderr)
        print(token)
    elif a.cmd == "gateways":
        show(store.gateways())
    elif a.cmd == "remove":
        print("removed" if store.remove(a.name) else "no such gateway")
    elif a.cmd == "pubkey":
        _, pk = signing_key(a.data)
        print(pk)
    elif a.cmd == "publish":
        name, version, fname = publish(store, a.model, a.notes)
        print(f"published {name} version {version} ({fname}); gateways pick it up at their next check-in")
    elif a.cmd == "retrain":
        name, version, fname = retrain(store, a)
        print(f"published {name} version {version} ({fname}); gateways pick it up at their next check-in")
    elif a.cmd == "label":
        store.label(a.domain, "dga", 0 if a.verdict == "benign" else 1)
        print(f"{a.domain}: {a.verdict} (used by the next retrain)")
    elif a.cmd == "labels":
        show(store.labels())
    elif a.cmd == "models":
        show(store.latest_models())
    elif a.cmd == "proposals":
        g = store.gateway(a.gateway) if a.gateway else None
        show(store.proposals(g["id"] if g else None))
    elif a.cmd == "propose":
        g = store.gateway(a.gateway)
        if not g:
            sys.exit("no such gateway")
        print(store.propose(g["id"], a.action, a.target, 0, " ".join(a.reason), "you (cortex)"))
    elif a.cmd == "history":
        g = store.gateway(a.gateway)
        show(store.history(g["id"]) if g else [])
    elif a.cmd == "mcp":
        from .mcp import run
        run(store)
    elif a.cmd == "publish-intel":
        version, n = publish_intel(store, a.min_homes)
        print(f"published threat-intel version {version} with {n} names; gateways pick it up at their next check-in")
    elif a.cmd == "sign-plugin":
        pub = sign_plugin(store, a.folder)
        print(f"signed {a.folder}/plugin.sig; gateways that trust {pub} run it with its privileges "
              "(any later change to the folder breaks the signature)")


if __name__ == "__main__":
    main()
