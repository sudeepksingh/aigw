# SPDX-License-Identifier: Apache-2.0
"""AIGW cortex command line.

    python3 -m aigw_cortex serve [--listen 0.0.0.0] [--port 8790] [--cert c --key k]
    python3 -m aigw_cortex enroll <gateway-name> [--group G]  prints the gateway's token, once
    python3 -m aigw_cortex gateways | remove <name>
    python3 -m aigw_cortex group <gateway> <group>    its area or uplink (outages are looked for within one)
    python3 -m aigw_cortex fleet [--days 7] [--group G] [--json]   the ISP's fleet report
    python3 -m aigw_cortex outages [--all] [--json]   area outages: many homes of one uplink at once
    python3 -m aigw_cortex outages settings [--window S --min-homes N --share F --recovered F]
    python3 -m aigw_cortex outages declare <uplink> [--kind DOWN] [--note text] | end <number>
                                                      an outage the NOC knows of: its homes are tagged
    python3 -m aigw_cortex rollout new|list|status|pause|resume|abort|homes|result ...
                                                      staged firmware rollouts that stop themselves
                                                      (rollout.py; --help for each)
    python3 -m aigw_cortex tickets [status|sync|outbox|retry|config|hub] [--json]
                                                      area outages and proactive tickets in the ISP's
                                                      ticketing: TMF621 and signed webhooks (tickets.py)
    python3 -m aigw_cortex dashboard [--listen 127.0.0.1:8090] [--token-file F] [--cert c --key k]
                                                      the operator dashboard, a web page (dashboard.py)
    python3 -m aigw_cortex demo [--listen 127.0.0.1:8090]   a simulated fleet and its dashboard, to try it
                                                      (demo.py; its own data, never --data)
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


def rollout_args(p):
    rs = p.add_subparsers(dest="what", required=True)
    n = rs.add_parser("new", help="plan a rollout; its first wave starts at once")
    n.add_argument("name")
    n.add_argument("--version", required=True, help="the firmware version the homes will report")
    n.add_argument("--url", default="", help="the image (https://, http:// or file://); needed with --via bridge")
    n.add_argument("--sha256", default="", help="the image's SHA-256, checked by the firmware handler")
    n.add_argument("--size", default="", help="the image's size in bytes")
    n.add_argument("--waves", default="1,10,50,100", help="rising percentages of the homes (default 1,10,50,100)")
    n.add_argument("--wave", action="append", help="a wave as a list of homes: --wave a,b --wave c,d,e")
    n.add_argument("--group", help="only the homes of this group")
    n.add_argument("--via", choices=("acs", "bridge"), default="acs",
                   help="who gives the homes the firmware: the ISP's ACS (default) or the bridge")
    n.add_argument("--max-bad", type=float, default=5.0, help="%% of a wave that may get worse (default 5)")
    n.add_argument("--max-score-drop", type=float, default=2.0,
                   help="points the score may fall on average in a wave (default 2)")
    n.add_argument("--min-reports", type=float, default=80.0, help="%% of a wave that must report (default 80)")
    n.add_argument("--timeout-h", type=float, default=72.0, help="hours a wave has to report (default 72)")
    n.add_argument("--paused", action="store_true", help="planned, not started (rollout resume)")
    rs.add_parser("list")
    for what in ("status", "step"):
        s = rs.add_parser(what)
        s.add_argument("name")
        s.add_argument("--homes", action="store_true", help="each home's state (names: for the ISP only)")
        s.add_argument("--json", action="store_true")
    for what in ("pause", "resume", "abort"):
        s = rs.add_parser(what)
        s.add_argument("name")
        s.add_argument("why", nargs="*")
    s = rs.add_parser("homes", help="the homes cleared now, for the ACS to push to")
    s.add_argument("name")
    s.add_argument("--json", action="store_true")
    s = rs.add_parser("result", help="a result the ACS heard (UpgradeChecked!), for homes without a bridge")
    s.add_argument("name")
    s.add_argument("home")
    s.add_argument("result")
    s.add_argument("--score-before", type=int)
    s.add_argument("--score-after", type=int)


def rollout_cmd(store, a):
    from . import rollout as ro
    if a.what == "new":
        lists = [[x.strip() for x in w.split(",") if x.strip()] for w in a.wave] if a.wave else None
        waves = [float(x) for x in a.waves.split(",") if x.strip()]
        rules = ro.Rules(a.max_bad, a.max_score_drop, a.min_reports, int(a.timeout_h * 3600))
        rid = ro.create(store, a.name, a.version, a.url, a.sha256, a.size, waves, lists, a.group, a.via, rules,
                        a.paused)
        print(ro.text(ro.status(store, rid)))
    elif a.what == "list":
        show([{k: s[k] for k in ("name", "version", "via", "state", "wave", "waves", "homes", "reason")}
              for s in ro.summaries(store, since=0)])
    elif a.what in ("status", "step"):
        ro.step(store, a.name)
        s = ro.status(store, a.name, homes=a.homes)
        show(s) if a.json else print(ro.text(s))
    elif a.what in ("pause", "resume", "abort"):
        s = ro.control(store, a.name, a.what, " ".join(a.why), "you (cortex)")
        print(ro.text(s))
    elif a.what == "homes":
        rows = ro.cleared(store, a.name)
        if a.json:
            show(rows)
        else:
            for r in rows:
                print(r["name"])
    elif a.what == "result":
        print(ro.text(ro.acs_result(store, a.name, a.home, a.result, a.score_before, a.score_after)))


def tickets_cmd(store, a):
    from . import tickets as tk
    try:
        cfg = tk.load_config(store.dir, a.config)
    except ValueError as e:
        sys.exit(f"tickets: {e}")
    if a.what == "config":              # where secrets come from, never the secrets: they aren't in it
        return show(cfg)
    if a.what == "sync":
        if not cfg["enabled"]:
            sys.exit("ISP ticketing is off: set enabled true in " + tk.config_path(store.dir, a.config))
        from . import outage
        outage.update(store)
        r = tk.sync(store, cfg)
        return show(r) if a.json else print(f"found {r['found'] or 'nothing new'}; sent {r['sent'] or 'nothing'}")
    if a.what == "outbox":
        return show(tk.outbox(store))
    if a.what == "list":
        return show(tk.listing(store, a.open))
    if a.what == "retry":
        return print(f"{tk.retry(store, a.ref)} to send again (with the next sync)")
    if a.what == "hub":
        try:
            if a.ref == "register":
                return print(f"registered with the ISP's hub: {tk.hub_register(store, cfg)}")
            if a.ref == "unregister":
                return print(f"unregistered {tk.hub_unregister(store, cfg)}")
        except ValueError as e:
            sys.exit(f"tickets hub: {e}")
        sys.exit("tickets hub register | unregister")
    s = tk.summary(store, cfg)
    show(s) if a.json else print(tk.text(s))


def main(argv=None):
    ap = argparse.ArgumentParser(prog="aigw_cortex", description="AIGW OS cortex")
    ap.add_argument("--data", default=os.environ.get("AIGW_CORTEX_DATA", os.path.expanduser("~/.aigw-cortex")))
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("serve")
    p.add_argument("--listen", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8790)
    p.add_argument("--cert")
    p.add_argument("--key")
    p.add_argument("--quiet", action="store_true")
    p.add_argument("--tickets", help="ISP ticketing configuration (default $AIGW_CORTEX_TICKETS, else "
                                     "<data>/tickets.json; none: off)")
    fed_args(p)
    p = sub.add_parser("tickets", help="area outages and proactive tickets in the ISP's ticketing (tickets.py)")
    p.add_argument("what", nargs="?", default="status",
                   choices=("status", "list", "sync", "outbox", "retry", "config", "hub"))
    p.add_argument("ref", nargs="?", help="retry: one ticket's key; hub: register or unregister")
    p.add_argument("--config", help="the configuration file (default $AIGW_CORTEX_TICKETS, else <data>/tickets.json)")
    p.add_argument("--open", action="store_true", help="list: only the open ones")
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("enroll")
    p.add_argument("name")
    p.add_argument("--group", default="", help="its area, e.g. an exchange or a town")
    sub.add_parser("gateways")
    p = sub.add_parser("group")
    p.add_argument("gateway")
    p.add_argument("group")
    p = sub.add_parser("outages", help="area outages: many homes of one uplink at once (outage.py)")
    p.add_argument("what", nargs="?", default="list", choices=("list", "settings", "declare", "end"))
    p.add_argument("ref", nargs="?", help="declare: the uplink; end: the incident's number")
    p.add_argument("--kind", default="DOWN", choices=("DOWN", "LOSS", "SLOW", "DNS"))
    p.add_argument("--note", default="", help="declare: why, in a few words")
    p.add_argument("--all", action="store_true", help="also those over in the last --days")
    p.add_argument("--days", type=int, default=7)
    p.add_argument("--json", action="store_true")
    p.add_argument("--window", type=int, help="s: faults starting this close together belong together (900)")
    p.add_argument("--min-homes", type=int, help="never fewer homes than this (3)")
    p.add_argument("--share", type=float, help="nor fewer than this share of the uplink's homes (0.1)")
    p.add_argument("--recovered", type=float, help="over when this share of its homes recovered (0.8)")
    p.add_argument("--silent-after", type=int, help="s without a report before a home counts as dropped (900)")
    p.add_argument("--hold", type=int, help="s a gateway holds its fixes after the last notice (1800)")
    p = sub.add_parser("fleet")
    p.add_argument("--days", type=int, default=7)
    p.add_argument("--group")
    p.add_argument("--json", action="store_true")
    rollout_args(sub.add_parser("rollout", help="staged firmware rollouts that stop themselves"))
    p = sub.add_parser("brief", help="the help desk's answer about one home")
    p.add_argument("home", nargs="?", help="gateway name, id or TR-069 serial number")
    p.add_argument("--period", help="what the customer means (\"tonight 20:00-22:00\"); cortex has the last hour")
    p.add_argument("--propose", action="store_true", help="send the remote fix as a proposal (waits for a yes)")
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("proactive", help="proactive care before the call: tickets the gateways raised or fixed")
    p.add_argument("home", nargs="?", help="one home (gateway name or id)")
    p.add_argument("--open", action="store_true", help="only the open tickets")
    p.add_argument("--json", action="store_true")
    p = sub.add_parser("dashboard", help="the operator dashboard, a web page")
    p.add_argument("--listen", default="127.0.0.1:8090",
                   help="address:port (default 127.0.0.1:8090; elsewhere, every request needs the token)")
    p.add_argument("--token-file", help="the dashboard's token (default <data>/dashboard.token, made on first use)")
    p.add_argument("--cert")
    p.add_argument("--key")
    p.add_argument("--quiet", action="store_true")
    p = sub.add_parser("demo", help="a simulated fleet and its dashboard, to try cortex (demo.py)")
    p.add_argument("what", nargs="?", default="run", choices=("run", "seed"))
    p.add_argument("--dir", help="the demo's own data directory (default: a new one, removed at the end)")
    p.add_argument("--listen", default="127.0.0.1:8090")
    p.add_argument("--home", help="seed: also enroll this gateway (a real one, e.g. the demo image's) and print "
                                  "its token")
    p.add_argument("--home-group", default="riverside")
    p.add_argument("--quiet", action="store_true")
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

    if a.cmd == "demo":                     # never in the real fleet's directory (--data)
        from . import demo
        return demo.main(a)
    store = Store(a.data)
    if a.cmd == "serve":
        from . import tickets
        from .server import serve
        try:
            tc = tickets.load_config(a.data, a.tickets)
        except ValueError as e:
            sys.exit(f"tickets: {e}")
        serve(store, a.listen, a.port, a.cert, a.key, log=not a.quiet, fed_settings=fed_settings(a), tickets_cfg=tc)
    elif a.cmd == "tickets":
        tickets_cmd(store, a)
    elif a.cmd == "dashboard":
        from .dashboard import serve as dashboard
        try:
            dashboard(store, a.listen, a.token_file, a.cert, a.key, log=not a.quiet)
        except (OSError, ValueError) as e:
            sys.exit(f"dashboard: {e}")
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
    elif a.cmd == "fleet":
        from . import fleet
        r = fleet.report(store, max(1, a.days), a.group)
        if a.json:
            show(r)
        else:
            print(fleet.text(r))
    elif a.cmd == "outages":
        from . import outage
        if a.what == "settings":
            try:
                show(outage.set_settings(store, window=a.window, min_homes=a.min_homes, share=a.share,
                                         recovered=a.recovered, silent_after=a.silent_after, hold_s=a.hold))
            except ValueError as e:
                sys.exit(str(e))
        elif a.what in ("declare", "end"):
            try:
                o = (outage.declare(store, a.ref, a.kind, a.note) if a.what == "declare" else
                     outage.end(store, int(a.ref or 0)))
            except ValueError as e:
                sys.exit(str(e))
            show(o) if a.json else print(o["words"])
        else:
            outage.update(store)
            rows = outage.incidents(store, open_only=not a.all, since=int(time.time()) - max(1, a.days) * 86400)
            show(rows) if a.json else print(outage.text(rows))
    elif a.cmd == "brief":
        from . import brief
        try:
            b = brief.answer(store, a.home, a.period, a.propose, who="you (cortex)")
        except ValueError as e:
            sys.exit(str(e))
        show(b) if a.json else print(brief.text(b))
    elif a.cmd == "proactive":
        from . import proactive
        try:
            r = proactive.summary(store, a.home, a.open)
        except ValueError as e:
            sys.exit(str(e))
        show(r) if a.json else print(proactive.text(r))
    elif a.cmd == "rollout":
        try:
            rollout_cmd(store, a)
        except ValueError as e:
            sys.exit(str(e))
    elif a.cmd == "group":
        print("set" if store.set_group(a.gateway, a.group) else "no such gateway")
    elif a.cmd == "enroll":
        gid, token = store.enroll(a.name, a.group)
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
