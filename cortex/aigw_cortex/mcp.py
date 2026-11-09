# SPDX-License-Identifier: Apache-2.0
"""MCP server: ask your home from any MCP client (Claude Desktop, Claude Code, ...).

    python3 -m aigw_cortex mcp          (stdio; add it to your client's MCP servers)

Read tools answer from the fleet history the gateways report. There is one
write tool, propose_action, and it only proposes: the suggestion travels to
the gateway through its bridge and waits there, like any plug-in's proposal,
until a person approves it (agctl approve, Home Assistant, ...). An AI
assistant can never change the network on its own.
"""
import json
import sys
import time

from . import __version__

PROTOCOL = "2025-06-18"
ACTIONS = ("quarantine", "priority", "deprioritize", "throttle", "disconnect", "block")

GW = {"gateway": {"type": "string", "description": "Gateway name or id; omit when there is only one"}}


def tool(name, description, props=None, required=(), read_only=True):
    return {
        "name": name,
        "description": description,
        "inputSchema": {"type": "object", "properties": dict(GW, **(props or {})), "required": list(required)},
        "annotations": {"readOnlyHint": read_only, "destructiveHint": False},
    }


TOOLS = [
    tool("list_gateways", "Gateways reporting to this cortex, with when each was last heard from."),
    tool("gateway_status", "A gateway's latest state: version, mode, device and alert counts, loaded models."),
    tool("list_devices", "Devices on a gateway's network: name, kind, zone, address, online state."),
    tool("list_alerts", "Recent alerts on a gateway, newest first, with the reason in plain words.",
         {"limit": {"type": "integer", "description": "How many (default 20)"}}),
    tool("list_decisions", "Actions on a gateway: waiting for approval, active, and past ones, with who asked."),
    tool("list_proposals", "Suggestions sent from here to a gateway, and how each was taken."),
    tool("model_status", "Signed models published here, and which version each gateway runs."),
    tool("propose_action",
         "Suggest an action on a gateway. It is never applied directly: it waits on the gateway until a "
         "person approves it. Actions: quarantine, priority, deprioritize, throttle, disconnect (target: a "
         "device name, MAC or IP) and block (target: a domain).",
         {"action": {"type": "string", "enum": list(ACTIONS)},
          "target": {"type": "string"},
          "ttl": {"type": "integer", "description": "Seconds the action lasts once approved (gateway default if 0)"},
          "reason": {"type": "string", "description": "Why, in plain words, shown to the person deciding"}},
         required=("action", "target", "reason"), read_only=False),
]


def ago(ts):
    if not ts:
        return "never"
    s = int(time.time()) - int(ts)
    if s < 120:
        return f"{s}s ago"
    if s < 7200:
        return f"{s // 60} min ago"
    if s < 172800:
        return f"{s // 3600} h ago"
    return f"{s // 86400} days ago"


class Server:
    def __init__(self, store):
        self.store = store

    def gw(self, args):
        g = self.store.gateway(args.get("gateway"))
        if not g:
            raise ValueError("no such gateway" if args.get("gateway") else "no gateway has reported yet")
        return g

    def call(self, name, args):
        s = self.store
        if name == "list_gateways":
            return [{"name": g["name"], "id": g["id"], "aigw": g["aigw"], "host": g["host"],
                     "last_seen": ago(g["last_seen"])} for g in s.gateways()]
        if name == "model_status":
            gws = []
            for g in s.gateways():
                snap = s.snapshot(g["id"]) or {}
                gws.append({"gateway": g["name"], "models": snap.get("models", [])})
            return {"published": [{k: m[k] for k in ("name", "version", "sha256", "notes")}
                                  for m in s.latest_models()], "gateways": gws}
        g = self.gw(args)
        snap = s.snapshot(g["id"]) or {}
        if name == "gateway_status":
            return {"gateway": g["name"], "last_seen": ago(g["last_seen"]), "status": snap.get("status", {}),
                    "models": snap.get("models", [])}
        if name == "list_devices":
            return [{k: d.get(k) for k in ("hostname", "mac", "ip", "kind", "vendor", "zone", "status", "wifi_key")
                     if d.get(k) not in (None, "")} for d in snap.get("devices", [])]
        if name == "list_alerts":
            limit = int(args.get("limit") or 20)
            return [{k: a.get(k) for k in ("id", "kind", "device", "subject", "count", "reason")}
                    for a in snap.get("alerts", [])[:limit]]
        if name == "list_decisions":
            return [{k: d.get(k) for k in ("id", "state", "action", "target", "device", "origin", "reason", "note")}
                    for d in snap.get("decisions", [])]
        if name == "list_proposals":
            return s.proposals(g["id"])
        if name == "propose_action":
            action, target = str(args.get("action", "")), str(args.get("target", "")).strip()
            reason = str(args.get("reason", "")).strip()
            if action not in ACTIONS:
                raise ValueError("action must be one of " + ", ".join(ACTIONS))
            if not target or len(target) > 200 or not reason:
                raise ValueError("needs a target and a reason")
            pid = s.propose(g["id"], action, target, int(args.get("ttl") or 0),
                            ("AI assistant: " + reason)[:300], "mcp")
            return {"proposal": pid, "state": "queued",
                    "note": f"Sent to {g['name']} with its next check-in. It waits there for a person to approve "
                            "it; nothing changes on the network until then."}
        raise ValueError(f"unknown tool {name}")

    def handle(self, msg):
        method, mid = msg.get("method"), msg.get("id")
        if mid is None:
            return None                          # notification (e.g. notifications/initialized)
        try:
            if method == "initialize":
                result = {"protocolVersion": PROTOCOL, "capabilities": {"tools": {"listChanged": False}},
                          "serverInfo": {"name": "aigw-cortex", "version": __version__},
                          "instructions": "AIGW OS home gateways. Read tools are safe. propose_action only "
                                          "suggests; a person approves on the gateway."}
            elif method == "ping":
                result = {}
            elif method == "tools/list":
                result = {"tools": TOOLS}
            elif method == "tools/call":
                p = msg.get("params") or {}
                try:
                    out = self.call(p.get("name"), p.get("arguments") or {})
                    result = {"content": [{"type": "text", "text": json.dumps(out, indent=1)}], "isError": False}
                except (ValueError, TypeError) as e:
                    result = {"content": [{"type": "text", "text": str(e)}], "isError": True}
            else:
                return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": f"no method {method}"}}
        except Exception as e:  # never kill the session over one request
            return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32603, "message": str(e)}}
        return {"jsonrpc": "2.0", "id": mid, "result": result}


def run(store, inp=sys.stdin, out=sys.stdout):
    srv = Server(store)
    for line in inp:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            resp = {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "parse error"}}
        else:
            resp = srv.handle(msg) if isinstance(msg, dict) else None
        if resp is not None:
            out.write(json.dumps(resp) + "\n")
            out.flush()
