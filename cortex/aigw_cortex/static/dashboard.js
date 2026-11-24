// SPDX-License-Identifier: Apache-2.0
// AIGW OS fleet dashboard. Everything a gateway sent is put on the page as text
// (textContent, never HTML), so a name or reason can't run as code.
"use strict";

const $ = (id) => document.getElementById(id);
let token = "";
let current = null;          // the gateway shown in detail

// ---------- small helpers ----------

function el(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === null || v === undefined || v === false) continue;
    if (k === "class") e.className = v;
    else if (k === "style") e.style.cssText = v;       // CSSOM: allowed by the page's CSP, unlike style=""
    else if (k === "on") for (const [ev, fn] of Object.entries(v)) e.addEventListener(ev, fn);
    else e.setAttribute(k, String(v));
  }
  for (const k of kids.flat(Infinity)) {
    if (k === null || k === undefined || k === false) continue;
    e.append(k instanceof Node ? k : document.createTextNode(String(k)));
  }
  return e;
}

function store(kind, key, value) {
  try {
    const s = kind === "session" ? sessionStorage : localStorage;
    if (value === undefined) return s.getItem(key) || "";
    if (value === null) s.removeItem(key); else s.setItem(key, value);
  } catch (e) { /* private window: fine without */ }
  return "";
}

const n = (v) => (v === null || v === undefined ? "–" : String(v));
const plural = (k, w) => `${k} ${w}${k === 1 ? "" : "s"}`;

function when(ts) {
  if (!ts) return "–";
  return new Date(ts * 1000).toLocaleString([], { dateStyle: "medium", timeStyle: "short" });
}

function ago(ts) {
  if (!ts) return "never";
  const s = Math.max(0, Math.floor(Date.now() / 1000 - ts));
  if (s < 120) return `${s} s ago`;
  if (s < 7200) return `${Math.floor(s / 60)} min ago`;
  if (s < 172800) return `${Math.floor(s / 3600)} h ago`;
  return `${Math.floor(s / 86400)} days ago`;
}

function scoreBadge(s) {
  if (s === null || s === undefined) return el("span", { class: "badge" }, "no data");
  const cls = s >= 95 ? "good" : s >= 80 ? "warn" : "bad";
  const word = s >= 95 ? "good" : s >= 80 ? "fair" : "poor";
  return el("span", { class: `badge ${cls}` }, `${s}% ${word}`);
}

function table(cols, rows, onRow) {
  if (!rows.length) return el("p", { class: "empty" }, "Nothing here.");
  const head = el("tr", {}, cols.map((c) => el("th", { class: c.num ? "num" : null, scope: "col" }, c.title)));
  const body = rows.map((r) => {
    const tr = el("tr", {}, cols.map((c) => el("td", { class: c.num ? "num" : null }, c.cell(r))));
    if (onRow) {
      tr.className = "click";
      tr.tabIndex = 0;
      tr.addEventListener("click", () => onRow(r));
      tr.addEventListener("keydown", (e) => { if (e.key === "Enter") onRow(r); });
    }
    return tr;
  });
  return el("div", { class: "scroll" }, el("table", {}, el("thead", {}, head), el("tbody", {}, body)));
}

// ---------- the API ----------

async function api(path, opts) {
  const o = Object.assign({ headers: {} }, opts || {});
  if (token) o.headers.Authorization = `Bearer ${token}`;
  const r = await fetch(path, o);
  let body = {};
  try { body = await r.json(); } catch (e) { /* not JSON */ }
  if (r.status === 401) {
    $("login").hidden = false;
    throw new Error(body.error || "this dashboard needs its token");
  }
  if (!r.ok) throw new Error(body.error || `${r.status} ${r.statusText}`);
  return body;
}

function showError(e) {
  const p = $("error");
  if (!e) { p.hidden = true; return; }
  p.textContent = String(e.message || e);
  p.hidden = false;
  ($("login").hidden ? p : $("login")).scrollIntoView({ behavior: "smooth", block: "center" });
}

// ---------- sections ----------

function tile(label, value, sub) {
  return el("div", { class: "tile" }, el("div", { class: "label" }, label), el("div", { class: "value" }, value),
    sub ? el("div", { class: "sub" }, sub) : null);
}

function renderHealth(r) {
  const e = r.experience, p = r.problems, w = r.early_warnings, h = r.homes;
  let change = "";
  if (e.mean !== null && e.mean_before !== null) {
    const d = e.mean - e.mean_before;
    change = `${d >= 0 ? "+" : ""}${d.toFixed(1)} from the ${r.period.days} days before`;
  }
  $("tiles").replaceChildren(
    tile("Homes", `${h.reporting} of ${h.total}`, h.silent ? `reporting; ${h.silent} silent` : "reporting"),
    tile("Experience", e.mean === null ? "–" : `${e.mean}%`,
      e.mean === null ? "no care data yet (AIGW OS 0.29 on)" : `of the time without a problem; ${change || `median ${e.median}%`}`),
    tile("Problems found", n(p.found), `${p.open} open now; ${p.hours} h in all`),
    tile("Fixed by AIGW", n(p.fixed_by_aigw), `no call needed; ${p.cleared_by_itself || 0} cleared by themselves`),
    tile("Told to the ISP first", n(p.reported_to_isp), "reported before anyone called"),
    tile("Early warnings", n(w.warnings), `${w.problems_seen_coming} problems seen coming` +
      (w.mean_lead_hours !== null ? `, ${w.mean_lead_hours} h ahead` : "") + `; ${w.open} open`),
  );
  const total = e.good + e.fair + e.poor;
  if (!total) { $("mix").replaceChildren(); return; }
  const seg = (cls, k) => k ? el("span", { class: cls, style: `width:${(100 * k / total).toFixed(2)}%`,
    title: `${plural(k, "home")} ${cls}` }) : null;
  const key = (cls, label, k) => el("span", {}, el("i", { class: cls, style: `background:var(--${cls === "fair" ? "warn-fill" : cls === "poor" ? "bad" : "good"})` }),
    `${label}: ${k}`);
  $("mix").replaceChildren(
    el("div", { class: "mix", role: "img", "aria-label": `${e.good} good, ${e.fair} fair, ${e.poor} poor homes` },
      seg("good", e.good), seg("fair", e.fair), seg("poor", e.poor)),
    el("div", { class: "legend" }, key("good", "Good (95% or more)", e.good), key("fair", "Fair (80 to 95%)", e.fair),
      key("poor", "Poor (under 80%)", e.poor)));
}

const KIND = { DOWN: "down", LOSS: "losing packets", SLOW: "slow", DNS: "failing to look up names" };

function renderOutages(r) {
  $("outages").replaceChildren(...r.outages.map((o) => {
    const codes = Object.entries(o.codes).map(([k, v]) => `${k} ${v}`).join(", ");
    const open = o.state === "open";
    const state = open ? `${o.still_open} still affected` : `over at ${when(o.end)}`;
    const homes = (o.affected || []).map((h) => el("li", {},
      el("a", { href: "#", on: { click: (ev) => { ev.preventDefault(); openGateway(h.gateway); } } }, h.home),
      ` ${h.code} from ${when(h.since)}${h.until ? ` to ${when(h.until)}` : ", still affected"}`));
    return el("div", { class: open ? "outage" : "outage over", role: "status" },
      el("strong", {}, `${open ? "Area outage" : "Area outage, over"} ${o.id}: `),
      `${o.homes} of ${o.homes_in_group} homes${o.group ? ` on ${o.group}` : ""} ${KIND[o.kind] || o.kind} from ` +
      `${when(o.start)} (within ${Math.max(1, Math.floor(o.spread_s / 60))} min: ${codes}; ${o.confidence} confidence). ` +
      `The ISP's network, not the homes; ${state}.` + (open ? " Their help-desk briefs say so, and fixes in those homes are held." : ""),
      o.isp_ticket ? el("div", { class: "muted" }, `ISP ticket: ${ispWords(o.isp_ticket)}`) : null,
      homes.length ? el("details", {}, el("summary", {}, `The ${plural(homes.length, "home")}`), el("ul", {}, homes)) : null);
  }));
}

const WHY = { problem: "bad", "early warning": "warn", "poor score": "warn", silent: "" };

function renderAttention(r) {
  $("attention").replaceChildren(table([
    { title: "Home", cell: (a) => a.home },
    { title: "Group", cell: (a) => a.group || "–" },
    { title: "Why", cell: (a) => el("span", { class: `badge ${WHY[a.why] || ""}` }, a.why) },
    { title: "In words", cell: (a) => a.words },
  ], r.attention, (a) => openGateway(a.home)));
}

function renderSignals(r) {
  const s = r.signals || {};
  if (!s.homes) {
    $("signals").replaceChildren(el("p", { class: "empty" },
      "No signals. Gateways send them from AIGW OS 0.32 on, when the home allows it (offers.share)."));
    return;
  }
  const prods = Object.entries(s.by_product);
  $("signals").replaceChildren(
    el("div", { class: "tiles" }, prods.map(([k, v]) => tile(k, v, s.high[k] ? `${s.high[k]} with high confidence` : "homes"))),
    el("h3", { style: "margin-top:0.8rem" }, "In the order to call"),
    table([
      { title: "Home", cell: (o) => o.home },
      { title: "Signal", cell: (o) => `${o.product || o.code}` },
      { title: "Confidence", cell: (o) => `${o.confidence}, ${o.days} of ${o.of_days} days` },
      { title: "Evidence", cell: (o) => o.evidence },
    ], s.list, (o) => openGateway(o.home)));
}

function renderProactive(r) {
  const p = r.proactive || {};
  if (!p.homes) {
    $("proactive").replaceChildren(el("p", { class: "empty" },
      "No tickets before the call yet. Gateways raise them from AIGW OS 0.40 on (proactive.enabled)."));
    return;
  }
  const c = p.counts || {};
  $("proactive").replaceChildren(
    el("div", { class: "tiles" },
      tile("Calls likely avoided", n(c.calls_likely_avoided || 0),
        `fixed, or the customer heard first; ${c.called_anyway || 0} called anyway`),
      tile("Fixed by AIGW", n(c.fixed_by_aigw || 0), `${c.proposed || 0} fixes proposed (advise mode)`),
      tile("Told to the ISP", n(c.told_isp || 0), `${c.suppressed || 0} kept at home, ${c.deduplicated || 0} repeats not told again`),
      tile("Open now", n(p.open), plural(p.homes_with_open, "home"))),
    p.open_tickets.length ? table([
      { title: "Home", cell: (t) => t.home },
      { title: "Status", cell: (t) => el("span", { class: `badge ${t.status === "Raised" ? "bad" : "warn"}` }, t.status) },
      { title: "Fault", cell: (t) => `${t.code} (${t.fault})` },
      { title: "Since", cell: (t) => ago(t.since) },
      { title: "Next step", cell: (t) => t.next_step || "" },
      { title: "For the customer", cell: (t) => t.customer_note || "" },
      { title: "ISP ticket", cell: (t) => ispBadge(t.isp_ticket) },
    ], p.open_tickets, (t) => openGateway(t.home)) : el("p", { class: "muted" }, "No open tickets."));
}

// v0.44: mesh Wi-Fi (mesh.py): the homes whose extenders hurt more than they help
function renderMesh(r) {
  const m = r.mesh || {};
  if (!m.homes) {
    $("mesh").replaceChildren(el("p", { class: "empty" },
      "No mesh Wi-Fi reported. Gateways report it from AIGW OS 0.44 on, where an EasyMesh controller runs."));
    return;
  }
  const c = m.counts || {};
  $("mesh").replaceChildren(
    el("div", { class: "tiles" },
      tile("Extenders hurting", n(m.homes_hurting), `homes, of ${plural(m.homes_with_extenders, "home")} with extenders`),
      tile("Extenders", n(m.extenders), `${m.wireless_backhauls || 0} on a wireless backhaul, ${m.weak_backhauls || 0} weak`),
      tile("Devices moved", n(c.steers || 0),
        `${c.kept || 0} kept, ${c.undone || 0} moved back` + (m.kept_pct !== null && m.kept_pct !== undefined ? ` (${m.kept_pct}% kept)` : "")),
      tile("Channel changes", n(c.channel_changes || 0), `${c.channels_kept || 0} kept, ${c.channels_undone || 0} switched back`)),
    (m.hurting || []).length ? table([
      { title: "Home", cell: (h) => h.home },
      { title: "Backhaul", cell: (h) => el("span", { class: "badge bad" }, h.kind || "Weak") },
      { title: "Signal", cell: (h) => (h.signal !== null && h.signal !== undefined ? `${h.signal} dBm` : "–") },
      { title: "Rate", cell: (h) => (h.rate_mbps !== null && h.rate_mbps !== undefined ? `${h.rate_mbps} Mbit/s` : "–") },
      { title: "Devices behind it", cell: (h) => String(h.stations) },
    ], m.hurting, (h) => openGateway(h.home)) : el("p", { class: "muted" }, "No extender hurts more than it helps."));
}

// v0.46: Wi-Fi coverage (coverage.py): the homes that need an extender (a sales lead), and those that don't
function renderCoverage(r) {
  const c = r.coverage || {};
  if (!c.homes) {
    $("coverage").replaceChildren(el("p", { class: "empty" },
      "No coverage reported yet. Gateways report it from AIGW OS 0.46 on, after an hour or so of learning."));
    return;
  }
  $("coverage").replaceChildren(
    el("div", { class: "tiles" },
      tile("Need an extender", n(c.need_extender), `of ${plural(c.homes, "home")} judged: a weak spot devices share`),
      tile("Use their extender", n(c.use_extender), "it hears the weak spot well"),
      tile("Move one device", n(c.move_device), `${c.devices_to_move || 0} devices in far spots`),
      tile("Weak radios", n(c.weak_device_only), `${c.weak_transmitters || 0} devices weak on their own`)),
    (c.leads || []).length ? table([
      { title: "Home", cell: (h) => h.home },
      { title: "Devices", cell: (h) => String(h.devices) },
      { title: "Where", cell: (h) => h.where || "" },
      { title: "Mesh", cell: (h) => (h.mesh ? "yes" : "no") },
      { title: "Advice", cell: (h) => h.advice || "" },
    ], c.leads, (h) => openGateway(h.home)) : el("p", { class: "muted" }, "No home needs an extender."));
}

// v0.42: the ISP's own tickets (tickets.py)
const SYNC = { synced: "good", "dry-run": "", pending: "warn", retrying: "warn", failed: "bad", none: "" };

function ispWords(i) {
  if (!i) return "–";
  const id = i.remote_id ? `#${i.remote_id}` : "not created yet";
  return `${id}${i.remote_status ? ` (${i.remote_status})` : ""}, ${i.sync}` +
    (i.isp_closed ? ", closed by the ISP" : "") + (i.error ? `: ${i.error}` : "");
}

function ispBadge(i) {
  if (!i || i.sync === "none") return "–";
  return el("span", { class: `badge ${SYNC[i.sync] || ""}`, title: ispWords(i) },
    i.remote_id ? `#${i.remote_id} ${i.sync}` : i.sync);
}

function renderIspTickets(r) {
  const s = r.isp_tickets || {};
  if (s.config_error) {
    $("isp-tickets").replaceChildren(el("p", { class: "error" }, `The ticketing configuration: ${s.config_error}`));
    return;
  }
  if (!s.enabled) {
    $("isp-tickets").replaceChildren(el("p", { class: "empty" },
      "Off. Turn it on in the cortex's tickets.json: a TMF621 Trouble Ticket API, signed webhooks, or both " +
      "(docs/isp-ticketing.md)."));
    return;
  }
  const t = s.tickets, o = s.outbox;
  const targets = s.targets.map((x) => `${x.kind} ${x.host}`).join(", ") || "no targets";
  const kids = [
    s.dry_run ? el("p", { class: "demo", role: "note" }, el("strong", {}, "Dry run. "),
      "Tickets are worked out and kept, nothing is sent.") : null,
    el("div", { class: "tiles" },
      tile("Open", n(t.open), `${t.resolved} resolved; ${t.isp_closed} closed by the ISP`),
      tile("Synced", n(s.sync.synced || 0), `to ${targets}`),
      tile("To send", n(o.queued), (s.sync.retrying ? `${s.sync.retrying} retrying; ` : "") +
        (s.last_ok ? `last sent ${ago(s.last_ok)}` : "nothing sent yet")),
      tile("Failed", n(o.failed), o.failed ? "python3 -m aigw_cortex tickets retry, once fixed" : "none")),
  ];
  if (s.last_error && (o.failed || s.sync.retrying)) {
    kids.push(el("p", { class: "reason" }, `Last error (${ago(s.last_error.time)}, ${s.last_error.target}): ` +
      s.last_error.text));
  }
  if (s.problems && s.problems.length) {
    kids.push(table([
      { title: "Ticket", cell: (x) => x.external_id },
      { title: "Sync", cell: (x) => ispBadge(x) },
      { title: "Error", cell: (x) => x.error || "" },
      { title: "Since", cell: (x) => ago(x.updated) },
    ], s.problems));
  }
  $("isp-tickets").replaceChildren(...kids.filter(Boolean));
}

function renderQueueing(r) {
  const q = r.queueing, s = q.sqm, c = q.calls;
  const kids = [el("div", { class: "tiles" },
    tile("Smart queueing on", n(s.on), "homes (kept or on trial)"),
    tile("Waiting for approval", n(s.waiting), "homes asked to turn it on"),
    tile("Tried and undone", n(s.tried), "it didn't help those lines"))];
  if (c.reported) {
    kids.push(el("div", { class: "tiles", style: "margin-top:0.6rem" },
      tile("Calls and games", n(c.total), `${c.calls} calls, ${c.games} games in ${plural(c.homes, "home")}`),
      tile("On a full line", n(c.on_full_line), `${c.smooth_on_full_line} of them smooth`),
      tile("Put first", n(c.prioritized), "calls given priority")));
  } else {
    kids.push(el("p", { class: "muted" },
      "Calls-first results: no bridge reports them yet. Each home's own result is on the gateway " +
      "(agctl calls) and, when the home shares it, in TR-181 Device.X_AIGW_.CallsFirst."));
  }
  $("queueing").replaceChildren(...kids);
}

function renderRollouts(list) {
  if (!list.length) {
    $("rollouts").replaceChildren(el("p", { class: "empty" },
      "No rollouts. Start one with: python3 -m aigw_cortex rollout new <name> --version V --url U"));
    return;
  }
  $("rollouts").replaceChildren(...list.map((ro) => {
    const st = ro.state, cls = st === "done" ? "good" : st === "stopped" || st === "aborted" ? "bad" : st === "paused" ? "warn" : "";
    const states = Object.entries(ro.homes.states).map(([k, v]) => `${v} ${ro.words[k] || k}`).join(", ");
    const waves = ro.per_wave.map((w) => el("div", { class: `wave ${w.verdict || "later"}` },
      el("strong", {}, `Wave ${w.wave}`), ` · ${plural(w.homes, "home")}`, el("br"),
      w.verdict ? `${w.good} kept, ${w.bad} worse` : "not started",
      w.score_before !== undefined ? el("div", { class: "muted" }, `score ${w.score_before}% → ${w.score_after}%`) : null));
    const buttons = el("div", { class: "row", style: "justify-content:flex-start" },
      el("button", { type: "button", disabled: !ro.can.pause, on: { click: () => hold(ro, "pause") } }, "Pause"),
      el("button", { type: "button", class: "danger", disabled: !ro.can.abort, on: { click: () => hold(ro, "abort") } }, "Abort"));
    return el("div", { class: "card rollout" },
      el("div", { class: "head" },
        el("div", {}, el("strong", {}, ro.name), ` · ${ro.version} to ${plural(ro.homes.homes, "home")}` +
          `${ro.group ? ` in ${ro.group}` : ""}, through the ${ro.via === "bridge" ? "bridge" : "ACS"}`),
        el("div", {}, el("span", { class: `badge ${cls}` }, st), ` wave ${ro.wave} of ${ro.waves}`)),
      ro.reason ? el("p", { class: "reason" }, ro.reason) : null,
      el("div", { class: "waves" }, waves),
      el("div", { class: "muted" }, states || "no homes yet"),
      el("details", {}, el("summary", {}, "What happened"),
        el("ul", {}, ro.log.map((x) => el("li", {}, `${when(x.ts)}: ${x.text}`)))),
      buttons);
  }));
}

function renderGateways(list) {
  $("gateways").replaceChildren(table([
    { title: "Gateway", cell: (g) => g.name },
    { title: "Group", cell: (g) => g.group || "–" },
    { title: "AIGW OS", cell: (g) => g.aigw || "–" },
    { title: "Last heard", cell: (g) => g.silent ? el("span", { class: "badge bad" }, ago(g.last_seen)) : ago(g.last_seen) },
    { title: "Experience (7 days)", cell: (g) => scoreBadge(g.score) },
    { title: "Devices", num: true, cell: (g) => n(g.devices) },
    { title: "Alerts", num: true, cell: (g) => n(g.alerts) },
    { title: "Waiting for approval", num: true, cell: (g) => n(g.waiting) },
  ], list, (g) => openGateway(g.id)));
}

// ---------- one gateway ----------

function kv(pairs) {
  return el("dl", { class: "kv" }, pairs.filter((p) => p[1] !== undefined && p[1] !== null && p[1] !== "")
    .map(([k, v]) => [el("dt", {}, k), el("dd", {}, v)]));
}

async function openGateway(ref, quiet) {
  current = ref;
  try {
    const g = await api(`/api/gateways/${encodeURIComponent(ref)}`);
    const d = $("detail");
    const share = { names: "device names shared", anonymous: "devices anonymous", none: "no devices shared" }[g.sharing];
    const waiting = g.decisions.filter((x) => x.state === "proposed");
    d.replaceChildren(...[
      el("div", { class: "rollout" }, el("div", { class: "head" },
        el("h2", { style: "margin:0" }, g.name),
        el("button", { type: "button", on: { click: () => { d.hidden = true; current = null; } } }, "Close"))),
      kv([["Group", g.group || "–"], ["AIGW OS", g.aigw || "–"], ["Last heard", `${ago(g.last_seen)} (${when(g.last_seen)})`],
        ["Mode", g.status.mode], ["Devices", n(g.status.devices)], ["Alerts", n(g.status.alerts)],
        ["Experience this week", g.score !== undefined && g.score !== null ? `${g.score}%` : "–"],
        ["Privacy", share]]),
      g.area_outage ? el("div", { class: "outage", role: "status" },
        el("strong", {}, `Known outage in the area (area outage ${g.area_outage.id}): `),
        `${g.area_outage.homes} of ${g.area_outage.homes_in_area} homes on ${g.area_outage.uplink} since ` +
        `${when(g.area_outage.start)}. The network's fault, not this home's; fixes in the home are held until it is over.`)
        : null,
      g.problem ? el("div", { class: "outage" }, el("strong", {}, `Problem: ${g.problem.code} (${g.problem.area})`),
        ` since ${when(g.problem.since)}${g.problem.reported ? ", told to the ISP" : ""}.`,
        g.problem.summary ? el("div", {}, g.problem.summary) : null,
        g.problem.advice ? el("div", { class: "muted" }, g.problem.advice) : null) : null,
      g.early_warning ? el("div", { class: "card" }, el("strong", {}, `Early warning: ${g.early_warning.metric}`),
        g.early_warning.predicted ? ` · a fault likely in about ${g.early_warning.predicted_fault_days} days` : "",
        el("div", {}, g.early_warning.summary || ""),
        g.early_warning.advice ? el("div", { class: "muted" }, g.early_warning.advice) : null) : null,
      el("h3", { style: "margin-top:1rem" }, `Waiting for approval (${waiting.length})`),
      el("p", { class: "muted" }, "Approving stays on the gateway: agctl approve, the home's app, or Home Assistant."),
      table([
        { title: "Action", cell: (x) => x.action },
        { title: "Target", cell: (x) => n(x.target) },
        { title: "Asked by", cell: (x) => n(x.origin) },
        { title: "Why", cell: (x) => n(x.reason) },
        { title: "Since", cell: (x) => when(x.created) },
      ], waiting),
      el("h3", { style: "margin-top:1rem" }, "Alerts"),
      table([
        { title: "Kind", cell: (x) => n(x.kind) },
        { title: "About", cell: (x) => n(x.subject) },
        { title: "Device", cell: (x) => n(x.device) },
        { title: "Times", num: true, cell: (x) => n(x.count) },
        { title: "Why", cell: (x) => n(x.reason) },
        { title: "Last", cell: (x) => when(x.last) },
      ], g.alerts),
      el("h3", { style: "margin-top:1rem" }, "Other decisions"),
      table([
        { title: "Action", cell: (x) => x.action },
        { title: "Target", cell: (x) => n(x.target) },
        { title: "State", cell: (x) => n(x.state) },
        { title: "Asked by", cell: (x) => n(x.origin) },
        { title: "When", cell: (x) => when(x.created) },
      ], g.decisions.filter((x) => x.state !== "proposed")),
      el("h3", { style: "margin-top:1rem" }, "Suggestions sent from cortex"),
      table([
        { title: "Action", cell: (x) => x.action },
        { title: "Target", cell: (x) => n(x.target) },
        { title: "State", cell: (x) => n(x.state) },
        { title: "From", cell: (x) => n(x.origin) },
        { title: "When", cell: (x) => when(x.created) },
      ], g.proposals)].filter(Boolean));
    d.hidden = false;
    if (!quiet) d.scrollIntoView({ behavior: "smooth", block: "start" });
  } catch (e) { showError(e); }
}

// ---------- pause and abort ----------

let pending = null;

function hold(ro, what) {
  pending = { name: ro.name, what };
  $("hold-title").textContent = `${what === "pause" ? "Pause" : "Abort"} rollout ${ro.name}`;
  $("hold-what").textContent = what === "pause"
    ? "No new home starts; results still count, and can still stop it. Resume at the command line."
    : "No more homes get this firmware. An aborted rollout stays so: fix the cause and start another.";
  $("hold-ok").textContent = what === "pause" ? "Pause" : "Abort";
  $("hold-reason").value = "";
  $("hold").showModal();
}

async function holdSubmit(ev) {
  ev.preventDefault();
  const reason = $("hold-reason").value.trim();
  if (!reason || !pending) return;
  $("hold").close();
  try {
    const r = await api(`/api/rollouts/${encodeURIComponent(pending.name)}/${pending.what}`, {
      method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ reason }) });
    renderRollouts(r.rollouts);
    showError(null);
  } catch (e) { showError(e); }
  pending = null;
}

// ---------- loading ----------

async function load() {
  const days = $("days").value, group = $("group").value;
  const q = `?days=${encodeURIComponent(days)}` + (group ? `&group=${encodeURIComponent(group)}` : "");
  try {
    const [r, ro, gw] = await Promise.all([api(`/api/overview${q}`), api("/api/rollouts"), api("/api/gateways")]);
    $("login").hidden = true;
    showError(null);
    $("version").textContent = `cortex ${r.version}`;
    $("demo").hidden = !r.demo;
    $("period").textContent = `${r.period.from} to ${r.period.to}` + (r.period.group ? `, group ${r.period.group}` : "") +
      `. Updated ${new Date(r.now * 1000).toLocaleTimeString()}.`;
    const sel = $("group");
    if (sel.options.length - 1 !== r.groups.length) {
      sel.replaceChildren(el("option", { value: "" }, "All homes"), ...r.groups.map((g) => el("option", { value: g }, g)));
      sel.value = group;
    }
    renderOutages(r);
    renderHealth(r);
    renderAttention(r);
    renderProactive(r);
    renderMesh(r);
    renderCoverage(r);
    renderIspTickets(r);
    renderSignals(r);
    renderQueueing(r);
    renderRollouts(group ? ro.rollouts.filter((x) => x.group === group || !x.group) : ro.rollouts);
    renderGateways(group ? gw.gateways.filter((g) => g.group === group) : gw.gateways);
    if (current) openGateway(current, true);
  } catch (e) { showError(e); }
}

function setTheme(t) {
  if (t) document.documentElement.setAttribute("data-theme", t);
  else document.documentElement.removeAttribute("data-theme");
}

function takeToken() {
  // the link's #token= never reaches a server; keep it for this tab and take it off the address bar
  const m = /(?:^#|&)token=([^&]+)/.exec(location.hash);
  if (!m) return false;
  token = decodeURIComponent(m[1]);
  store("session", "aigw-token", token);
  history.replaceState(null, "", location.pathname + location.search);
  return true;
}

function init() {
  if (!takeToken()) token = store("session", "aigw-token");
  window.addEventListener("hashchange", () => { if (takeToken()) load(); });
  setTheme(store("local", "aigw-theme"));
  $("theme").addEventListener("click", () => {
    const dark = document.documentElement.getAttribute("data-theme") === "dark" ||
      (!document.documentElement.getAttribute("data-theme") && matchMedia("(prefers-color-scheme: dark)").matches);
    const t = dark ? "light" : "dark";
    setTheme(t);
    store("local", "aigw-theme", t);
  });
  $("refresh").addEventListener("click", load);
  $("days").addEventListener("change", load);
  $("group").addEventListener("change", load);
  $("login-form").addEventListener("submit", (ev) => {
    ev.preventDefault();
    token = $("token").value.trim();
    store("session", "aigw-token", token);
    $("token").value = "";
    load();
  });
  $("hold-form").addEventListener("submit", holdSubmit);
  $("hold-cancel").addEventListener("click", () => { $("hold").close(); pending = null; });
  load();
  setInterval(() => { if (!document.hidden) load(); }, 60000);
}

document.addEventListener("DOMContentLoaded", init);
