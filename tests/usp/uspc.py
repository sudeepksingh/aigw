#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""A small USP (TR-369) controller over MQTT 5, for tests.

    uspc.py get <path>...                     values, as JSON {path: value}
    uspc.py set <path>=<value>...             {"ok": bool, "err_code", "err_msg"}
    uspc.py operate <command()> [Arg=value]...  {"ok", "out" | "err_code", "err_msg"}
    uspc.py supported <object path>           commands and events the agent lists under it
    uspc.py watch <event> <seconds> -- <shell command>
                                              subscribe to the event, run the command, print
                                              the first notification {"event", "params"}

Environment: USP_PB_DIR (the generated *_pb2.py), USP_BROKER, USP_PORT,
USP_AGENT_ID, USP_AGENT_TOPIC; the controller is self::usp-controller on
/usp/controller, as the agent's settings name it.
"""
import json
import os
import subprocess
import sys
import time
import uuid

sys.path.insert(0, os.environ["USP_PB_DIR"])
import usp_msg_1_5_pb2 as M      # noqa: E402
import usp_record_1_5_pb2 as R   # noqa: E402
import paho.mqtt.client as mqtt  # noqa: E402
from paho.mqtt.packettypes import PacketTypes  # noqa: E402
from paho.mqtt.properties import Properties  # noqa: E402

BROKER = os.environ.get("USP_BROKER", "127.0.0.1")
PORT = int(os.environ.get("USP_PORT", "1883"))
AGENT_ID = os.environ["USP_AGENT_ID"]
AGENT_TOPIC = os.environ["USP_AGENT_TOPIC"]
CTRL_ID = "self::usp-controller"
CTRL_TOPIC = "/usp/controller"


class Controller:
    def __init__(self):
        self.inbox = []
        self.c = mqtt.Client(client_id="aigw-test-" + uuid.uuid4().hex[:8], protocol=mqtt.MQTTv5)
        self.c.on_message = lambda c, u, m: self.inbox.append(m.payload)
        self.c.connect(BROKER, PORT)
        self.c.subscribe(CTRL_TOPIC, qos=1)
        self.c.loop_start()
        time.sleep(0.3)

    def close(self):
        self.c.loop_stop()
        self.c.disconnect()

    def send(self, msg):
        rec = R.Record(version="1.3", to_id=AGENT_ID, from_id=CTRL_ID,
                       payload_security=R.Record.PLAINTEXT)
        rec.no_session_context.payload = msg.SerializeToString()
        props = Properties(PacketTypes.PUBLISH)
        props.ResponseTopic = CTRL_TOPIC
        self.c.publish(AGENT_TOPIC, rec.SerializeToString(), qos=1, properties=props)

    def wait(self, pred, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            while self.inbox:
                rec = R.Record()
                rec.ParseFromString(self.inbox.pop(0))
                if not rec.HasField("no_session_context"):
                    continue
                m = M.Msg()
                m.ParseFromString(rec.no_session_context.payload)
                if m.header.msg_type == M.Header.NOTIFY:
                    self.ack(m)
                if pred(m):
                    return m
            time.sleep(0.05)
        return None

    def ack(self, m):
        n = m.body.request.notify
        if not n.send_resp:
            return
        r = M.Msg()
        r.header.msg_id = m.header.msg_id
        r.header.msg_type = M.Header.NOTIFY_RESP
        r.body.response.notify_resp.subscription_id = n.subscription_id
        self.send(r)

    def request(self, msg_type, fill, timeout=10.0):
        m = M.Msg()
        m.header.msg_id = uuid.uuid4().hex
        m.header.msg_type = msg_type
        fill(m.body.request)
        self.send(m)
        r = self.wait(lambda x: x.header.msg_id == m.header.msg_id, timeout)
        if r is None:
            raise SystemExit("no answer from the agent")
        if r.body.HasField("error"):
            e = r.body.error
            return {"ok": False, "err_code": e.err_code, "err_msg": e.err_msg,
                    "param_errs": {p.param_path: p.err_code for p in e.param_errs}}
        return r.body.response


def split(path):
    obj, _, param = path.rpartition(".")
    return obj + ".", param


def cmd_get(c, paths):
    def fill(req):
        req.get.param_paths.extend(paths)
    r = c.request(M.Header.GET, fill)
    if isinstance(r, dict):
        return r
    out, errs = {}, {}
    for res in r.get_resp.req_path_results:
        if res.err_code:
            errs[res.requested_path] = res.err_code
        for rp in res.resolved_path_results:
            for k, v in rp.result_params.items():
                out[rp.resolved_path + k] = v
    if errs:
        out["_errors"] = errs
    return out


def cmd_set(c, items):
    objs = {}
    for it in items:
        path, _, value = it.partition("=")
        obj, param = split(path)
        objs.setdefault(obj, []).append((param, value))

    def fill(req):
        req.set.allow_partial = False
        for obj, ps in objs.items():
            u = req.set.update_objs.add()
            u.obj_path = obj
            for p, v in ps:
                s = u.param_settings.add()
                s.param, s.value, s.required = p, v, True
    r = c.request(M.Header.SET, fill)
    if isinstance(r, dict):
        return r
    for res in r.set_resp.updated_obj_results:
        st = res.oper_status
        if st.HasField("oper_failure"):
            return {"ok": False, "err_code": st.oper_failure.err_code, "err_msg": st.oper_failure.err_msg}
    return {"ok": True}


def cmd_operate(c, command, args):
    def fill(req):
        req.operate.command = command
        req.operate.command_key = "k" + uuid.uuid4().hex[:6]
        req.operate.send_resp = True
        for a in args:
            k, _, v = a.partition("=")
            req.operate.input_args[k] = v
    r = c.request(M.Header.OPERATE, fill)
    if isinstance(r, dict):
        return r
    for res in r.operate_resp.operation_results:
        if res.HasField("cmd_failure"):
            return {"ok": False, "err_code": res.cmd_failure.err_code, "err_msg": res.cmd_failure.err_msg}
        return {"ok": True, "out": dict(res.req_output_args.output_args)}
    return {"ok": False, "err_msg": "no result"}


def cmd_supported(c, obj):
    def fill(req):
        g = req.get_supported_dm
        g.obj_paths.append(obj)
        g.first_level_only = False
        g.return_commands = g.return_events = g.return_params = True
    r = c.request(M.Header.GET_SUPPORTED_DM, fill)
    if isinstance(r, dict):
        return r
    out = {"commands": [], "events": [], "params": []}
    for res in r.get_supported_dm_resp.req_obj_results:
        for so in res.supported_objs:
            out["commands"] += [so.supported_obj_path + x.command_name for x in so.supported_commands]
            out["events"] += [so.supported_obj_path + x.event_name for x in so.supported_events]
            out["params"] += [so.supported_obj_path + x.param_name for x in so.supported_params]
    return out


def cmd_watch(c, event, seconds, trigger):
    sub_id = "aigw-test-" + uuid.uuid4().hex[:6]

    def fill(req):
        req.add.allow_partial = False
        o = req.add.create_objs.add()
        o.obj_path = "Device.LocalAgent.Subscription."
        for p, v in (("Enable", "true"), ("ID", sub_id), ("NotifType", "Event"),
                     ("ReferenceList", event), ("NotifRetry", "false")):
            s = o.param_settings.add()
            s.param, s.value, s.required = p, v, True
    r = c.request(M.Header.ADD, fill)
    if isinstance(r, dict):
        return {"subscribe": r}
    if trigger:
        subprocess.run(trigger, shell=True, check=False)
    obj, _, name = event.rpartition(".")
    n = c.wait(lambda m: m.header.msg_type == M.Header.NOTIFY and
               m.body.request.notify.subscription_id == sub_id and
               m.body.request.notify.HasField("event"), float(seconds))
    if n is None:
        return {"event": None}
    ev = n.body.request.notify.event
    return {"event": ev.obj_path + ev.event_name, "params": dict(ev.params)}


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    cmd, rest = sys.argv[1], sys.argv[2:]
    c = Controller()
    try:
        if cmd == "get":
            out = cmd_get(c, rest)
        elif cmd == "set":
            out = cmd_set(c, rest)
        elif cmd == "operate":
            out = cmd_operate(c, rest[0], rest[1:])
        elif cmd == "supported":
            out = cmd_supported(c, rest[0])
        elif cmd == "watch":
            trigger = " ".join(rest[3:]) if len(rest) > 3 and rest[2] == "--" else ""
            out = cmd_watch(c, rest[0], rest[1], trigger)
        else:
            raise SystemExit(__doc__)
    finally:
        c.close()
    print(json.dumps(out, indent=1, sort_keys=True))


if __name__ == "__main__":
    main()
