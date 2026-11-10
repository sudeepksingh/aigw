#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""A small TR-069 ACS (CWMP over HTTP) for tests/cwmp-test.sh.

    acs.py serve DIR [--port 7557]        the ACS: every Inform is appended to
                                          DIR/informs.jsonl; queued jobs run
                                          in the next session
    acs.py job DIR JOB.json [--cr URL --user U --password P] [--timeout S]
                                          queue a job (a list of RPCs), wake the
                                          CPE with a connection request, print
                                          the results when the session ran it
    acs.py wait DIR EVENT [--after N] [--timeout S]
                                          wait for an Inform carrying EVENT
                                          (e.g. "4 VALUE CHANGE"), print it

A job: {"rpcs": [{"method": "GetParameterValues", "names": [...]},
                 {"method": "SetParameterValues", "params": [[name, value, xsd type]], "key": "k"},
                 {"method": "GetParameterNames", "path": "Device.", "next": true}]}
"""
import argparse
import http.server
import json
import os
import sys
import time
import urllib.request
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

SOAP = "http://schemas.xmlsoap.org/soap/envelope/"
ENC = "http://schemas.xmlsoap.org/soap/encoding/"


def local(tag):
    return tag.rsplit("}", 1)[-1]


def child(el, name):
    for c in el:
        if local(c.tag) == name:
            return c
    return None


def kids(el, name):
    c = child(el, name)
    return list(c) if c is not None else []


def text(el, name):
    c = child(el, name)
    return (c.text or "") if c is not None else ""


def envelope(cwmp_ns, cwmp_id, body):
    return ('<?xml version="1.0" encoding="UTF-8"?>'
            f'<soap-env:Envelope xmlns:soap-env="{SOAP}" xmlns:soap-enc="{ENC}" '
            f'xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" '
            f'xmlns:cwmp="{cwmp_ns}">'
            f'<soap-env:Header><cwmp:ID soap-env:mustUnderstand="1">{escape(cwmp_id)}</cwmp:ID></soap-env:Header>'
            f'<soap-env:Body>{body}</soap-env:Body></soap-env:Envelope>').encode()


def rpc_body(r):
    m = r["method"]
    if m == "GetParameterValues":
        names = "".join(f"<string>{escape(n)}</string>" for n in r["names"])
        return (f'<cwmp:GetParameterValues><ParameterNames soap-enc:arrayType="xsd:string[{len(r["names"])}]">'
                f"{names}</ParameterNames></cwmp:GetParameterValues>")
    if m == "SetParameterValues":
        ps = "".join(f'<ParameterValueStruct><Name>{escape(n)}</Name><Value xsi:type="{t}">{escape(v)}</Value>'
                     f"</ParameterValueStruct>" for n, v, t in r["params"])
        return (f'<cwmp:SetParameterValues><ParameterList soap-enc:arrayType="cwmp:ParameterValueStruct'
                f'[{len(r["params"])}]">{ps}</ParameterList><ParameterKey>{escape(r.get("key", ""))}</ParameterKey>'
                f"</cwmp:SetParameterValues>")
    if m == "GetParameterNames":
        return (f'<cwmp:GetParameterNames><ParameterPath>{escape(r["path"])}</ParameterPath>'
                f'<NextLevel>{"true" if r.get("next") else "false"}</NextLevel></cwmp:GetParameterNames>')
    raise ValueError(m)


def parse_response(el):
    """One CPE response (or Fault) -> a dict."""
    name = local(el.tag)
    if name == "Fault":
        detail = el.find(".//{*}Fault") if child(el, "detail") is not None else None
        out = {"fault": None, "string": "", "params": []}
        if detail is not None:
            out["fault"] = int(text(detail, "FaultCode") or 0)
            out["string"] = text(detail, "FaultString")
            for p in detail:
                if local(p.tag) == "SetParameterValuesFault":
                    out["params"].append({"name": text(p, "ParameterName"), "fault": int(text(p, "FaultCode") or 0),
                                          "string": text(p, "FaultString")})
        return out
    if name == "GetParameterValuesResponse":
        return {"values": {text(p, "Name"): text(p, "Value") for p in kids(el, "ParameterList")}}
    if name == "SetParameterValuesResponse":
        return {"status": int(text(el, "Status") or 0)}
    if name == "GetParameterNamesResponse":
        return {"names": {text(p, "Name"): text(p, "Writable") in ("1", "true") for p in kids(el, "ParameterList")}}
    return {"response": name}


class Session:
    def __init__(self):
        self.cwmp_ns = "urn:dslforum-org:cwmp-1-0"
        self.queue = []     # (job file, rpc list)
        self.current = None
        self.results = []


def make_handler(d):
    state = {"session": None}
    os.makedirs(os.path.join(d, "jobs"), exist_ok=True)
    os.makedirs(os.path.join(d, "done"), exist_ok=True)

    def next_rpc(s):
        """The next RPC of the session's jobs, or None to end the session."""
        while True:
            if s.current and s.current[1]:
                return s.current[1][0]
            if s.current:   # job finished
                name = s.current[0]
                tmp = os.path.join(d, "done", name + ".tmp")
                with open(tmp, "w") as f:
                    json.dump({"results": s.results}, f)
                os.rename(tmp, os.path.join(d, "done", name))
                s.current, s.results = None, []
            jobs = sorted(f for f in os.listdir(os.path.join(d, "jobs")) if f.endswith(".json"))
            if not jobs:
                return None
            path = os.path.join(d, "jobs", jobs[0])
            with open(path) as f:
                job = json.load(f)
            os.unlink(path)
            s.current = (jobs[0], list(job["rpcs"]))

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *a):
            sys.stderr.write("acs: " + fmt % a + "\n")

        def reply(self, body, code=200):
            self.send_response(code)
            if body:
                self.send_header("Content-Type", 'text/xml; charset="utf-8"')
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if body:
                self.wfile.write(body)

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(n) if n else b""
            s = state["session"]
            el, cwmp_id = None, "1"
            if raw.strip():
                root = ET.fromstring(raw)
                hdr = root.find("{%s}Header" % SOAP)
                if hdr is not None:
                    for c in hdr:
                        if local(c.tag) == "ID":
                            cwmp_id = c.text or "1"
                body = root.find("{%s}Body" % SOAP)
                el = body[0] if body is not None and len(body) else None
            if el is not None and local(el.tag) == "Inform":
                s = state["session"] = Session()
                s.cwmp_ns = el.tag[1:].split("}")[0]
                dev = child(el, "DeviceId")
                events = [text(e, "EventCode") for e in kids(el, "Event")]
                params = {text(p, "Name"): text(p, "Value") for p in kids(el, "ParameterList")}
                rec = {"time": time.time(), "events": events, "params": params,
                       "device": {local(c.tag): c.text or "" for c in dev} if dev is not None else {}}
                with open(os.path.join(d, "informs.jsonl"), "a") as f:
                    f.write(json.dumps(rec) + "\n")
                self.reply(envelope(s.cwmp_ns, cwmp_id, "<cwmp:InformResponse><MaxEnvelopes>1</MaxEnvelopes>"
                                    "</cwmp:InformResponse>"))
                return
            if s is None:
                self.reply(b"", 204)
                return
            if el is not None and local(el.tag) == "GetRPCMethods":
                methods = ["Inform", "GetRPCMethods", "TransferComplete"]
                ms = "".join(f"<string>{m}</string>" for m in methods)
                self.reply(envelope(s.cwmp_ns, cwmp_id, f'<cwmp:GetRPCMethodsResponse><MethodList soap-enc:arrayType='
                                    f'"xsd:string[{len(methods)}]">{ms}</MethodList></cwmp:GetRPCMethodsResponse>'))
                return
            if el is not None and local(el.tag) in ("TransferComplete", "AutonomousTransferComplete"):
                self.reply(envelope(s.cwmp_ns, cwmp_id, f"<cwmp:{local(el.tag)}Response/>"))
                return
            if el is not None and s.current and s.current[1]:
                # the answer to the RPC we sent
                s.results.append({"method": s.current[1][0]["method"], **parse_response(el)})
                s.current[1].pop(0)
            r = next_rpc(s)
            if r is None:
                state["session"] = None
                self.reply(b"", 204)
                return
            self.reply(envelope(s.cwmp_ns, "aigw-test", rpc_body(r)))

    return H


def serve(a):
    srv = http.server.HTTPServer(("0.0.0.0", a.port), make_handler(a.dir))
    srv.serve_forever()


def connection_request(url, user, password):
    pm = urllib.request.HTTPPasswordMgrWithDefaultRealm()
    pm.add_password(None, url, user, password)
    op = urllib.request.build_opener(urllib.request.HTTPDigestAuthHandler(pm), urllib.request.HTTPBasicAuthHandler(pm))
    try:
        op.open(url, timeout=10).read()
    except urllib.error.HTTPError as e:
        if e.code >= 300:
            raise


def job(a):
    with open(a.job) as f:
        rpcs = json.load(f)
    name = "%d-%d.json" % (int(time.time() * 1000), os.getpid())
    tmp = os.path.join(a.dir, "jobs", name + ".tmp")
    os.makedirs(os.path.join(a.dir, "jobs"), exist_ok=True)
    with open(tmp, "w") as f:
        json.dump(rpcs, f)
    os.rename(tmp, os.path.join(a.dir, "jobs", name))
    done = os.path.join(a.dir, "done", name)
    end = time.time() + a.timeout
    last_cr = 0
    while time.time() < end:
        if os.path.exists(done):
            with open(done) as f:
                print(f.read())
            return 0
        if a.cr and time.time() - last_cr > 10:     # wake the CPE (again, if a session was already running)
            last_cr = time.time()
            try:
                connection_request(a.cr, a.user, a.password)
            except Exception as e:  # noqa: BLE001 - reported, retried
                sys.stderr.write(f"acs: connection request: {e}\n")
        time.sleep(0.3)
    sys.stderr.write("acs: the job did not run\n")
    return 1


def wait(a):
    end = time.time() + a.timeout
    path = os.path.join(a.dir, "informs.jsonl")
    while time.time() < end:
        if os.path.exists(path):
            with open(path) as f:
                lines = f.read().splitlines()
            for ln in lines[a.after:]:
                rec = json.loads(ln)
                if a.event in rec["events"]:
                    print(json.dumps(rec))
                    return 0
        time.sleep(0.3)
    sys.stderr.write(f"acs: no Inform with {a.event}\n")
    return 1


def main():
    p = argparse.ArgumentParser()
    sp = p.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("serve")
    s.add_argument("dir")
    s.add_argument("--port", type=int, default=7557)
    j = sp.add_parser("job")
    j.add_argument("dir")
    j.add_argument("job")
    j.add_argument("--cr")
    j.add_argument("--user", default="")
    j.add_argument("--password", default="")
    j.add_argument("--timeout", type=float, default=40)
    w = sp.add_parser("wait")
    w.add_argument("dir")
    w.add_argument("event")
    w.add_argument("--after", type=int, default=0)
    w.add_argument("--timeout", type=float, default=40)
    a = p.parse_args()
    if a.cmd == "serve":
        serve(a)
        return 0
    return job(a) if a.cmd == "job" else wait(a)


if __name__ == "__main__":
    sys.exit(main())
