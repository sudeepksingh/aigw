# Cortex: AIGW's optional services outside

Cortex runs wherever you like: the PC that is already the access point, a
NAS, a cloud VM. Gateways work fully without it. With it:

- **Fleet history:** each gateway reports its state (counters, devices,
  alerts, decisions, models in use), never traffic or DNS logs.
- **Training loop:** your answers to alerts become labels. Undoing or
  rejecting an action that came from a `dga` alert marks that name as a false
  alarm; approving it confirms the alert. `retrain` uses those labels.
- **Signed model updates:** a retrained model is stamped with a version,
  signed with cortex's key (usign format, Ed25519) and published. Each
  gateway's bridge fetches it. agd checks it and loads it without a restart.
- **Ask your home (MCP):** an MCP server for Claude Desktop, Claude Code or
  any MCP client. It can read everything above. It can only *suggest*
  actions, and each suggestion waits on the gateway for a person.

```
 gateway (agd)                   bridge plug-in (nobody, curl)          cortex
 status, devices, alerts,  ───►  POST /v1/report  ────────────────────►  cortex.db (SQLite)
 decisions, models                                                       labels from answers
 signature checked,        ◄───  GET /v1/models, /v1/models/<file>  ◄──  retrain → stamp → sign
 hot-loaded                                                              models/<name>-<v>.agm(.sig)
 proposal waits for you    ◄───  GET /v1/proposals  ◄──────────────────  MCP propose_action
                           ───►  POST /v1/proposals/<id>  ──────────────► how it was taken
```

## Run it

Python 3.9+ with the standard library is enough for the server, signing and
MCP. Retraining needs `numpy` and `scikit-learn` (see
`training/requirements.txt`). `cryptography` is used for signing when it is
installed; otherwise a built-in pure-Python Ed25519 signs (slower, same
result).

```bash
cd aigw/cortex
python3 -m aigw_cortex enroll home        # token for gateway "home", shown once
python3 -m aigw_cortex serve --listen 0.0.0.0 --port 8790 \
        --cert cert.pem --key key.pem     # HTTPS; leave both out on a trusted LAN
python3 -m aigw_cortex pubkey             # path of the public key gateways trust
```

Data lives in `~/.aigw-cortex` (or `--data DIR`, or `$AIGW_CORTEX_DATA`):
`cortex.db`, `keys/cortex.key` (mode 600, never leaves this machine),
`keys/cortex.pub`, `models/`.

On each gateway:

```bash
sudo agctl trust cortex.pub                  # models signed with it are accepted
sudo agctl bridge https://cortex-host:8790   # the bridge plug-in waits for these two
sudo agctl bridge token                      # paste the token from enroll
agctl bridge                                 # state, last check-in
agctl plugin config bridge devices anonymous # optional: no names, addresses or reasons
```

On OpenWRT: `opkg install aigw-bridge curl`.

## The training loop

```bash
python3 -m aigw_cortex labels                       # what people said
python3 -m aigw_cortex label example-shop.com benign  # add your own
python3 -m aigw_cortex retrain                      # Tranco top 200k + labels (a few minutes)
python3 -m aigw_cortex models                       # published versions
```

Each answer from a person counts like 50 synthetic examples
(`--feedback-weight` in `training/domain_risk/train.py`). The model records
how many answers it learned from and how far it agrees with them
(`feedback_agreement`). To publish a model you trained yourself:
`python3 -m aigw_cortex publish model.agm --notes "why"`.

On the gateway: `agctl models` shows the version in use, who signed it and
any refused offer. `agctl model rollback` goes back one step and holds the
newer version off, so the bridge doesn't fetch it again.
`agctl model install file.agm [file.sig] [--force]` installs one by hand.

## Shared threat intelligence

```bash
python3 -m aigw_cortex publish-intel               # default: --min-homes 2
```

This publishes a signed list (`threat-intel`) of names that people
confirmed as threats anywhere (an approved action from an alert, or `label
<name> risky`), plus names that looked machine-generated in at least
`--min-homes` homes. A name anyone called a false alarm never goes on it.
Gateways fetch the list like a model. A lookup of a listed name, or anything
under it, raises a `known-threat` alert there. Only names and a short reason
travel. On a gateway, `agctl models` shows the list's version, and
`agctl model rollback threat-intel` takes it back.

## Signed plug-ins

```bash
python3 -m aigw_cortex sign-plugin path/to/my-plugin   # writes my-plugin/plugin.sig
```

The signature covers every file in the folder. Gateways that trust cortex's
key run the plug-in with its privileged capabilities, without
`agctl plugin allow`. Changing any file takes that trust away.

## Ask your home (MCP)

```json
{ "mcpServers": { "aigw": {
    "command": "python3",
    "args": ["-m", "aigw_cortex", "mcp"],
    "cwd": "/path/to/aigw/cortex" } } }
```

| Tool | |
|------|--|
| `list_gateways`, `gateway_status` | which gateways report, version, mode, counters, models |
| `list_devices`, `list_alerts`, `list_decisions` | from the latest report |
| `model_status` | published models and what each gateway runs |
| `list_proposals` | suggestions sent and how each was taken |
| `propose_action` | **the only write tool.** quarantine, priority, deprioritize, throttle, disconnect or block, with a reason. It travels to the gateway with the next check-in and waits there as a proposal (`agctl decisions`, then `agctl approve <id>`), even in enforce mode. The gateway's guardrails (protected devices, time limits) apply as for any request. |

## HTTP API (for bridges)

Every request carries `Authorization: Bearer <token>`.

| | |
|-|-|
| `POST /v1/report` | `{status, models, alerts, decisions, devices}`; answers `{ok, labels}` |
| `GET /v1/models` | `{models: [{name, version, file, sig, sha256, size}]}`, the newest version of each (models and the threat list) |
| `GET /v1/models/<file>` | the `.agm` or `.list`, or its `.sig` |
| `GET /v1/proposals` | suggestions waiting for this gateway (each is sent once) |
| `POST /v1/proposals/<id>` | `{ok, decision, error}`: how the gateway took it |
