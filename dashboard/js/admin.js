// Admin pages (one HTML file, hash routes):
//   #overview          status strip, requests panel, one card per channel, recent top-ups
//   #channel/ch1       channel detail: charts, time left, top-ups, tenant name
//   #topup  #topup/ch1 top up units (optionally pre-set to a channel)
//   #tenants           room names, accounts, create tenant login
//   #log               full top-up log with channel filter and CSV export
//
// The dashboard never writes a balance. A top-up is a topUpLog entry plus a
// pendingTopUps entry under the same key; the ESP32 applies it.
// Landlord commands go under control/: disconnect/chN (true = relay held
// OFF whatever the balance) and resetUnits (the ESP32 sets every balance to
// 0, then deletes the request).
// Energy is stored in kWh and shown in Wh (fmtWh / toWh in common.js).

import { firebaseConfig } from "../firebase-config.js";
import {
  db, auth, requireRole, wireSignOut, fmt, fmtWh, toWh, ago, dateTime, escapeHtml, isFresh, serverNow,
  balanceFraction, balanceLevel, topUpStatus, watchChannel, askConfirm
} from "./common.js";
import {
  ref, set, update, push, onValue, serverTimestamp, sendPasswordResetEmail,
  initializeApp, deleteApp, initializeAuth, inMemoryPersistence,
  createUserWithEmailAndPassword, signOut
} from "./sdk.js";
import { watchHistory, dailyKwh, avgPowerLastHour, hoursLeft, friendlyDuration, clockLabel, dayLabel } from "./usage.js";
import { powerLineChart, dailyBarChart, setChartData } from "./charts.js";

const $ = (id) => document.getElementById(id);

const { user } = await requireRole("admin");
wireSignOut();
$("who").textContent = user.email;

// ---------------------------------------------------------------------------
// State. Each listener writes its part, then asks for a redraw.
// ---------------------------------------------------------------------------
const S = {
  ids: [],          // channel ids, e.g. ["ch1", "ch2", "ch3", "ch4"]
  ch: {},           // ch -> { tenantName, remainingBalance, originalAllocation, relayStatus, live, adminOff }
  device: null,
  reset: null,      // control/resetUnits while a reset waits for the device
  log: {},          // topUpLog
  requests: {},     // topUpRequests
  users: {},
  mismatch: {},     // ch -> consecutive sync cycles with balance > 0 but relay OFF
  samples: [],      // history for the channel open in detail view
};
const cards = {};   // ch -> overview card element
const route = { view: "overview", arg: null };   // current hash route, set by applyRoute()

const name = (ch) => (S.ch[ch] && S.ch[ch].tenantName) || ch;
const deviceOnline = () => isFresh(S.device && S.device.lastSeen);

function showError(err) {
  console.error(err);
  const box = $("errorBox");
  box.hidden = false;
  box.textContent = err && err.code === "PERMISSION_DENIED"
    ? "Permission denied. Check that users/{your uid}/role is \"admin\"."
    : `Error: ${err && err.message ? err.message : err}`;
}

function flash(id, text, kind) {
  const box = $(id);
  box.className = `msg ${kind}`;
  box.textContent = text;
  box.hidden = !text;
}

// ---------------------------------------------------------------------------
// Channel discovery. A shallow REST read returns only the channel keys, so the
// page loops over whatever channels exist without downloading their history.
// ---------------------------------------------------------------------------
async function loadChannelIds() {
  const token = await user.getIdToken();
  const res = await fetch(`${firebaseConfig.databaseURL}/channels.json?shallow=true&auth=${encodeURIComponent(token)}`);
  if (!res.ok) throw new Error(`Could not list channels (HTTP ${res.status})`);
  const keys = Object.keys((await res.json()) || {});
  return keys.sort((a, b) => Number(a.replace(/\D/g, "")) - Number(b.replace(/\D/g, "")));
}

try {
  S.ids = await loadChannelIds();
} catch (e) {
  showError(e);
}

for (const ch of S.ids) {
  S.ch[ch] = {};
  cards[ch] = buildCard(ch, false);
  $("cards").appendChild(cards[ch]);

  watchChannel(ch, (state, field) => {
    const prevTs = S.ch[ch].live && S.ch[ch].live.ts;
    S.ch[ch] = { ...state };
    trackMismatch(ch, field === "live" && state.live && state.live.ts !== prevTs);
    onChannelChange(ch, field);
  }, showError);
}
if (!S.ids.length) $("cards").innerHTML = `<p class="muted">No channels in the database yet.</p>`;

onValue(ref(db, "device"), (snap) => { S.device = snap.val(); renderStrip(); }, showError);
onValue(ref(db, "topUpLog"), (snap) => { S.log = snap.val() || {}; renderTables(); }, showError);
onValue(ref(db, "topUpRequests"), (snap) => { S.requests = snap.val() || {}; renderRequests(); }, showError);
onValue(ref(db, "users"), (snap) => { S.users = snap.val() || {}; renderAccounts(); renderTables(); }, showError);
onValue(ref(db, "control/resetUnits"), (snap) => { S.reset = snap.val(); renderReset(); }, showError);

// Relay mismatch: balance above 0 but relay OFF for more than 2 sync cycles,
// and the landlord has not disconnected the room on purpose.
// A sync cycle is counted each time the channel's live.ts changes.
function trackMismatch(ch, newCycle) {
  const c = S.ch[ch];
  const mismatch = typeof c.remainingBalance === "number" && c.remainingBalance > 0 &&
    c.relayStatus === "OFF" && c.adminOff !== true;
  if (!mismatch) S.mismatch[ch] = 0;
  else if (newCycle) S.mismatch[ch] = (S.mismatch[ch] || 0) + 1;
}

function onChannelChange(ch, field) {
  updateCard(cards[ch], ch);
  renderStrip();
  if (route.view === "channel" && route.arg === ch) {
    updateCard($("detailCard").firstElementChild, ch);
    renderDetailStats();
  }
  if (field === "tenantName") {
    fillChannelSelects();
    renderNames();
    renderRequests();
    renderAccounts();
    renderTables();
  }
}

// ---------------------------------------------------------------------------
// Status strip
// ---------------------------------------------------------------------------
function renderStrip() {
  const online = deviceOnline();
  const lastSeen = S.device && S.device.lastSeen;
  $("deviceStatus").innerHTML = online
    ? `<span class="dot on"></span>Online`
    : `<span class="dot"></span><span class="offline">Offline, last seen ${escapeHtml(ago(lastSeen))}</span>`;

  const freshChannels = S.ids.filter((ch) => online && S.ch[ch].live && isFresh(S.ch[ch].live.ts));
  const total = freshChannels.reduce((sum, ch) => sum + (S.ch[ch].live.power || 0), 0);
  $("totalPower").textContent = freshChannels.length ? `${fmt(total, 1)} W` : "–";
  const vch = freshChannels.find((ch) => typeof S.ch[ch].live.voltage === "number");
  $("supplyVoltage").textContent = vch ? `${fmt(S.ch[vch].live.voltage, 1)} V` : "–";

  let low = 0, empty = 0;
  for (const ch of S.ids) {
    const level = balanceLevel(S.ch[ch].remainingBalance, S.ch[ch].originalAllocation);
    if (level === "low") low++;
    if (level === "empty") empty++;
  }
  const parts = [];
  if (low) parts.push(`${low} low`);
  if (empty) parts.push(`${empty} exhausted`);
  $("alertTile").hidden = !parts.length;
  $("alertText").textContent = parts.join(", ");
}

// ---------------------------------------------------------------------------
// Channel cards (built once, then updated in place so buttons stay clickable)
// ---------------------------------------------------------------------------
function buildCard(ch, big) {
  const el = document.createElement("div");
  el.className = big ? "card big static" : "card";
  if (!big) {
    el.tabIndex = 0;
    el.setAttribute("role", "link");
    el.addEventListener("click", (ev) => { if (!ev.target.closest("button")) location.hash = `#channel/${ch}`; });
    el.addEventListener("keydown", (ev) => { if (ev.key === "Enter") location.hash = `#channel/${ch}`; });
  }
  el.innerHTML = `
    <div class="card-head">
      <div><h3 data-f="name"></h3><div class="id">${escapeHtml(ch)}</div></div>
      <span class="pill" data-f="relay"></span>
    </div>
    <div class="balance num"><span data-f="bal">–</span> <span class="unit">Wh</span></div>
    <div class="bar" data-f="bar"><span style="width:0"></span></div>
    <div class="muted small" data-f="pct"></div>
    <div class="live" data-f="live">
      <div><div class="k">Voltage</div><div class="v" data-f="v">–</div></div>
      <div><div class="k">Current</div><div class="v" data-f="a">–</div></div>
      <div><div class="k">Power</div><div class="v" data-f="w">–</div></div>
      <div><div class="k">PF</div><div class="v" data-f="pf">–</div></div>
    </div>
    <div class="stale-note" data-f="stale" hidden></div>
    <div class="warn-line" data-f="mismatch" hidden>Relay is OFF but units remain. Check the relay and MCP23017.</div>
    <div class="card-foot">
      <span class="muted small" data-f="updated"></span>
      <div class="row">
        <button type="button" data-f="landlord"></button>
        <button type="button" data-topup>Top up</button>
      </div>
    </div>`;
  el.querySelector("[data-topup]").addEventListener("click", () => { location.hash = `#topup/${ch}`; });
  el.querySelector('[data-f="landlord"]').addEventListener("click", () => setLandlordOff(ch, S.ch[ch].adminOff !== true));
  return el;
}

// Landlord disconnect. The ESP32 reads control/disconnect at each sync and
// holds the relay OFF while the flag is true. The balance is not touched.
async function setLandlordOff(ch, off) {
  const question = off
    ? `Disconnect ${name(ch)}? The supply goes off within about 15 s and stays off until you reconnect, even if units remain.`
    : `Reconnect ${name(ch)}? The supply comes back if the room still has units.`;
  if (!(await askConfirm(question, off ? "Disconnect" : "Reconnect"))) return;
  try {
    await set(ref(db, `control/disconnect/${ch}`), off);
  } catch (e) {
    showError(e);
  }
}

function updateCard(el, ch) {
  if (!el) return;
  const f = (k) => el.querySelector(`[data-f="${k}"]`);
  const c = S.ch[ch] || {};
  const live = c.live || {};
  const level = balanceLevel(c.remainingBalance, c.originalAllocation);
  const frac = balanceFraction(c.remainingBalance, c.originalAllocation);

  f("name").textContent = name(ch);
  const relay = f("relay");
  const landlordOff = c.adminOff === true;
  if (landlordOff && c.relayStatus === "ON") { relay.className = "pill off"; relay.textContent = "Disconnecting…"; }
  else if (landlordOff) { relay.className = "pill off"; relay.textContent = "Disconnected by landlord"; }
  else if (c.relayStatus === "ON") { relay.className = "pill on"; relay.textContent = "Supply ON"; }
  else if (c.relayStatus === "OFF") { relay.className = "pill off"; relay.textContent = "Disconnected"; }
  else { relay.className = "pill"; relay.textContent = "–"; }
  const btn = f("landlord");
  btn.textContent = landlordOff ? "Reconnect" : "Disconnect";
  btn.className = landlordOff ? "primary" : "danger";

  f("bal").textContent = fmtWh(c.remainingBalance, 1);
  f("bar").className = `bar ${level}`;
  f("bar").firstElementChild.style.width = `${Math.round(frac * 100)}%`;
  f("pct").textContent = typeof c.originalAllocation === "number" && c.originalAllocation > 0
    ? `${Math.round(frac * 100)}% of ${fmtWh(c.originalAllocation, 1)} Wh left`
    : "No allocation yet";

  f("v").textContent = `${fmt(live.voltage, 1)} V`;
  f("a").textContent = `${fmt(live.current, 3)} A`;
  f("w").textContent = `${fmt(live.power, 1)} W`;
  f("pf").textContent = fmt(live.pf, 2);

  // Grey out the live row: device offline, or (device online) this channel stale
  const online = deviceOnline();
  const fresh = isFresh(live.ts);
  f("live").classList.toggle("stale", !online || !fresh);
  const note = f("stale");
  note.hidden = online && fresh;
  note.textContent = !online ? "Device offline" : "No reading";

  f("mismatch").hidden = !((S.mismatch[ch] || 0) > 2);
  f("updated").textContent = typeof live.ts === "number" ? `Reading ${ago(live.ts)}` : "No reading yet";
}

// ---------------------------------------------------------------------------
// Top-up rows (shared by overview, top-up page, detail page and log page)
// ---------------------------------------------------------------------------
function adminName(uid) {
  if (S.users[uid] && S.users[uid].email) return S.users[uid].email;
  if (uid === user.uid) return user.email;
  return uid ? `${uid.slice(0, 6)}…` : "–";
}

function logEntries(ch) {
  return Object.entries(S.log)
    .filter(([, e]) => !ch || e.channel === ch)
    .sort((a, b) => (b[1].requestedAt || 0) - (a[1].requestedAt || 0));
}

function topUpTable(entries, cols) {
  const online = deviceOnline();
  const head = [
    "<th>Date and time</th>",
    cols.channel ? "<th>Channel</th><th>Tenant</th>" : "",
    `<th class="num">Wh</th>`,
    cols.admin ? "<th>Admin</th>" : "",
    "<th>Status</th>",
    cols.applied ? "<th>Applied</th>" : ""
  ].join("");
  const span = 3 + (cols.channel ? 2 : 0) + (cols.admin ? 1 : 0) + (cols.applied ? 1 : 0);
  const rows = entries.map(([, e]) => {
    const st = topUpStatus(e, online);
    return `<tr>
      <td>${escapeHtml(dateTime(e.requestedAt))}</td>
      ${cols.channel ? `<td>${escapeHtml(e.channel)}</td><td>${escapeHtml(name(e.channel))}</td>` : ""}
      <td class="num">${fmtWh(e.kwh, 0)}</td>
      ${cols.admin ? `<td>${escapeHtml(adminName(e.by))}</td>` : ""}
      <td class="status ${st.key}">${escapeHtml(st.text)}</td>
      ${cols.applied ? `<td>${escapeHtml(dateTime(e.appliedAt))}</td>` : ""}
    </tr>`;
  }).join("");
  return `<table><thead><tr>${head}</tr></thead><tbody>${rows ||
    `<tr class="empty-row"><td colspan="${span}">No top-ups yet</td></tr>`}</tbody></table>`;
}

function renderTables() {
  const recent = logEntries().slice(0, 8);
  $("recentTopUps").innerHTML = topUpTable(recent, { channel: true });
  $("topUpRecent").innerHTML = topUpTable(logEntries().slice(0, 15), { channel: true, admin: true });
  if (route.view === "channel") {
    $("detailTopUps").innerHTML = topUpTable(logEntries(route.arg), { admin: true, applied: true });
    renderDetailStats();
  }
  if (route.view === "log") {
    $("logTable").innerHTML = topUpTable(logEntries($("logFilter").value), { channel: true, admin: true, applied: true });
  }
}

// ---------------------------------------------------------------------------
// Top up units
// ---------------------------------------------------------------------------
function topUpPaths(id, ch, kwh) {
  return {
    [`topUpLog/${id}`]: { channel: ch, kwh, by: user.uid, requestedAt: serverTimestamp() },
    [`channels/${ch}/pendingTopUps/${id}`]: { kwh, requestedAt: serverTimestamp() }
  };
}

function balanceSentence(ch, kwh) {
  const now = S.ch[ch] && S.ch[ch].remainingBalance;
  const before = typeof now === "number" ? now : 0;
  return `Add ${fmtWh(kwh, 0)} Wh to ${name(ch)}? Balance goes from ${fmtWh(before, 1)} to ${fmtWh(before + kwh, 1)} Wh.`;
}

// Entered in whole Wh, stored in kWh (1 Wh = 0.001 kWh, the PZEM resolution)
$("topUpForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const ch = $("topUpChannel").value;
  const wh = Math.round(Number($("topUpWh").value));
  const kwh = wh / 1000;
  if (!ch) return flash("topUpMsg", "Pick a room.", "error");
  if (!(wh >= 1 && wh <= 500000)) return flash("topUpMsg", "Enter between 1 and 500000 Wh.", "error");
  if (!(await askConfirm(balanceSentence(ch, kwh), "Add units"))) return;

  const offline = !deviceOnline();
  $("topUpBtn").disabled = true;
  try {
    // One multi-path update: log entry and queue entry under the same key
    const id = push(ref(db, "topUpLog")).key;
    await update(ref(db), topUpPaths(id, ch, kwh));
    $("topUpWh").value = "";
    if (offline) flash("topUpMsg", "Device offline. Units will apply when it reconnects.", "warn");
    else flash("topUpMsg", `Sent ${wh} Wh to ${name(ch)}. It shows Pending until the device applies it.`, "ok");
  } catch (e) {
    console.error(e);
    flash("topUpMsg", `Top-up failed: ${e.message}`, "error");
  } finally {
    $("topUpBtn").disabled = false;
  }
});

// ---------------------------------------------------------------------------
// Tenant top-up requests (overview panel)
// ---------------------------------------------------------------------------
function renderRequests() {
  const waiting = Object.entries(S.requests)
    .filter(([, r]) => r.status === "waiting")
    .sort((a, b) => (a[1].requestedAt || 0) - (b[1].requestedAt || 0));
  $("requestsPanel").hidden = !waiting.length;
  $("requestsBody").innerHTML = waiting.map(([id, r]) => `<tr>
      <td>${escapeHtml(dateTime(r.requestedAt))}</td>
      <td>${escapeHtml(name(r.channel))} <span class="muted">${escapeHtml(r.channel)}</span></td>
      <td class="num">${fmtWh(r.kwh, 0)}</td>
      <td><div class="row" style="justify-content:flex-end">
        <button class="primary" data-approve="${escapeHtml(id)}">Approve</button>
        <button class="danger" data-decline="${escapeHtml(id)}">Decline</button>
      </div></td></tr>`).join("");
}

$("requestsBody").addEventListener("click", async (ev) => {
  const approveId = ev.target.dataset.approve;
  const declineId = ev.target.dataset.decline;
  const id = approveId || declineId;
  if (!id || !S.requests[id]) return;
  const r = S.requests[id];
  try {
    if (approveId) {
      if (!(await askConfirm(balanceSentence(r.channel, r.kwh), "Approve"))) return;
      // Approve = status change + normal top-up, all in one multi-path update
      const topUpId = push(ref(db, "topUpLog")).key;
      await update(ref(db), {
        [`topUpRequests/${id}/status`]: "approved",
        [`topUpRequests/${id}/decidedAt`]: serverTimestamp(),
        ...topUpPaths(topUpId, r.channel, r.kwh)
      });
    } else {
      if (!(await askConfirm(`Decline ${fmtWh(r.kwh, 0)} Wh for ${name(r.channel)}?`, "Decline"))) return;
      await update(ref(db, `topUpRequests/${id}`), { status: "declined", decidedAt: serverTimestamp() });
    }
  } catch (e) {
    showError(e);
  }
});

// ---------------------------------------------------------------------------
// Reset all units (overview panel). The dashboard only asks; the ESP32 sets
// every balance and allocation to 0 in one multi-path update and deletes
// control/resetUnits. Pending top-ups are not cancelled and still apply.
// ---------------------------------------------------------------------------
function renderReset() {
  const waiting = !!S.reset;
  $("resetBtn").disabled = waiting;
  flash("resetMsg", waiting
    ? `Reset sent ${ago(S.reset.requestedAt)}, waiting for the device${deviceOnline() ? "" : " (device offline)"}.`
    : "", "warn");
}

$("resetBtn").addEventListener("click", async () => {
  const question = "Reset all units? Every room's balance goes to 0 Wh and every room is disconnected " +
    "until it is topped up. Top-ups still pending will apply after the reset.";
  if (!(await askConfirm(question, "Reset all units"))) return;
  try {
    await set(ref(db, "control/resetUnits"), { requestedAt: serverTimestamp(), by: user.uid });
  } catch (e) {
    showError(e);
  }
});

// ---------------------------------------------------------------------------
// Channel detail
// ---------------------------------------------------------------------------
let stopHistory = null;
let powerChart = null;
let dailyChart = null;

function enterDetail(ch) {
  $("detailCard").innerHTML = "";
  $("detailCard").appendChild(buildCard(ch, true));
  updateCard($("detailCard").firstElementChild, ch);
  $("detailName").value = (S.ch[ch] && S.ch[ch].tenantName) || "";
  flash("detailNameMsg", "", "ok");
  if (!powerChart) powerChart = powerLineChart($("powerChart"));
  if (!dailyChart) dailyChart = dailyBarChart($("dailyChart"), "Wh");
  S.samples = [];
  if (stopHistory) stopHistory();
  stopHistory = watchHistory(ch, 7, (samples) => { S.samples = samples; renderDetailCharts(); });
  renderTables();
}

function leaveDetail() {
  if (stopHistory) stopHistory();
  stopHistory = null;
}

function renderDetailCharts() {
  if (route.view !== "channel" || !powerChart) return;
  const since = serverNow() - 24 * 3600 * 1000;
  const day = S.samples.filter((s) => s.t > since);
  setChartData(powerChart, day.map((s) => clockLabel(s.t)), day.map((s) => s.power));
  renderDetailStats();
}

function renderDetailStats() {
  if (route.view !== "channel" || !dailyChart) return;
  const ch = route.arg;
  const c = S.ch[ch] || {};
  const live = c.live || {};
  const topUps = logEntries(ch).map(([, e]) => e);
  const days = dailyKwh(S.samples, topUps, c.remainingBalance, 7);
  setChartData(dailyChart, days.map((d) => dayLabel(d.key)), days.map((d) => toWh(d.kwh ?? 0)));

  const avg = avgPowerLastHour(S.samples, isFresh(live.ts) ? live.power : null);
  const hours = hoursLeft(c.remainingBalance, avg);
  $("detailHours").textContent = hours === null
    ? "–"
    : `${fmt(hours, 1)} h (${friendlyDuration(hours).toLowerCase()})`;
}

$("detailNameForm").addEventListener("submit", (ev) => {
  ev.preventDefault();
  saveName(route.arg, $("detailName").value, "detailNameMsg");
});

async function saveName(ch, value, msgId) {
  const text = value.trim();
  if (text.length < 1 || text.length > 40) return flash(msgId, "Name must be 1 to 40 characters.", "error");
  try {
    await set(ref(db, `channels/${ch}/tenantName`), text);
    flash(msgId, `Saved name for ${ch}.`, "ok");
  } catch (e) {
    flash(msgId, `Could not save: ${e.message}`, "error");
  }
}

// ---------------------------------------------------------------------------
// Tenants page
// ---------------------------------------------------------------------------
function fillChannelSelects() {
  for (const id of ["topUpChannel", "newChannel"]) {
    const sel = $(id);
    const keep = sel.value;
    sel.innerHTML = S.ids.map((ch) => `<option value="${escapeHtml(ch)}">${escapeHtml(name(ch))} (${escapeHtml(ch)})</option>`).join("");
    if (keep) sel.value = keep;
  }
  const filter = $("logFilter");
  const keep = filter.value;
  filter.innerHTML = `<option value="">All rooms</option>` +
    S.ids.map((ch) => `<option value="${escapeHtml(ch)}">${escapeHtml(name(ch))} (${escapeHtml(ch)})</option>`).join("");
  filter.value = keep;
}

function renderNames() {
  const body = $("namesBody");
  if (!body.children.length) {
    body.innerHTML = S.ids.map((ch) => `<tr>
      <td>${escapeHtml(ch)}</td>
      <td><input data-name="${escapeHtml(ch)}" maxlength="40"></td>
      <td><button data-savename="${escapeHtml(ch)}">Save</button></td></tr>`).join("");
    body.querySelectorAll("input").forEach((inp) => inp.addEventListener("input", () => { inp.dataset.dirty = "1"; }));
  }
  // Refresh values without clobbering a name being typed
  body.querySelectorAll("input[data-name]").forEach((inp) => {
    if (document.activeElement !== inp && !inp.dataset.dirty) inp.value = S.ch[inp.dataset.name].tenantName || "";
  });
}

$("namesBody").addEventListener("click", async (ev) => {
  const ch = ev.target.dataset.savename;
  if (!ch) return;
  const inp = $("namesBody").querySelector(`input[data-name="${ch}"]`);
  await saveName(ch, inp.value, "namesMsg");
  delete inp.dataset.dirty;
});

function renderAccounts() {
  const rows = Object.entries(S.users).sort((a, b) =>
    (a[1].role || "").localeCompare(b[1].role || "") || (a[1].channel || "").localeCompare(b[1].channel || ""));
  const chOptions = (current) => `<option value="">Not linked</option>` + S.ids.map((ch) =>
    `<option value="${escapeHtml(ch)}" ${ch === current ? "selected" : ""}>${escapeHtml(name(ch))} (${escapeHtml(ch)})</option>`).join("");

  $("accountsBody").innerHTML = rows.map(([uid, u]) => {
    const label = u.email || `${uid}${uid === user.uid ? " (you)" : ""}`;
    if (u.role !== "tenant") {
      return `<tr><td>${escapeHtml(label)}</td><td>${escapeHtml(u.role)}</td><td class="muted">–</td><td></td></tr>`;
    }
    return `<tr>
      <td>${escapeHtml(label)}${u.mustChangePassword ? ` <span class="muted small">(temporary password)</span>` : ""}</td>
      <td>tenant</td>
      <td><select data-link="${escapeHtml(uid)}" style="width:auto">${chOptions(u.channel)}</select></td>
      <td>${u.email ? `<button data-reset="${escapeHtml(uid)}">Send reset email</button>` : ""}</td></tr>`;
  }).join("") || `<tr class="empty-row"><td colspan="4">No accounts</td></tr>`;
}

// Link / unlink a tenant account. Unlinking keeps the login and the record
// (so it can be re-linked later) but the rules then give it no access.
$("accountsBody").addEventListener("change", async (ev) => {
  const uid = ev.target.dataset.link;
  if (!uid) return;
  const u = S.users[uid] || {};
  const ch = ev.target.value;
  const who = u.email || uid;
  const question = ch
    ? `Link ${who} to ${name(ch)} (${ch})? They will see that room only.`
    : `Unlink ${who}? They keep their login but can no longer see any room.`;
  if (!(await askConfirm(question, ch ? "Link" : "Unlink"))) { renderAccounts(); return; }
  try {
    await update(ref(db, `users/${uid}`), { channel: ch || null });
    flash("accountsMsg", ch ? `Linked ${who} to ${ch}.` : `Unlinked ${who}.`, "ok");
  } catch (e) {
    flash("accountsMsg", `Could not update: ${e.message}`, "error");
    renderAccounts();
  }
});

$("accountsBody").addEventListener("click", async (ev) => {
  const uid = ev.target.dataset.reset;
  if (!uid || !S.users[uid] || !S.users[uid].email) return;
  const email = S.users[uid].email;
  try {
    await sendPasswordResetEmail(auth, email);
    flash("accountsMsg", `Password reset email sent to ${email}.`, "ok");
  } catch (e) {
    flash("accountsMsg", `Could not send reset email: ${e.message}`, "error");
  }
});

// Create a tenant login with a second Firebase app instance, so the admin's
// own session is untouched. The second instance keeps its session in memory
// only and is deleted afterwards.
async function createLogin(email, password) {
  const secondary = initializeApp(firebaseConfig, `tenant-creator-${Date.now()}`);
  try {
    const secondaryAuth = initializeAuth(secondary, { persistence: inMemoryPersistence });
    const cred = await createUserWithEmailAndPassword(secondaryAuth, email, password);
    await signOut(secondaryAuth);
    return cred.user.uid;
  } finally {
    await deleteApp(secondary);
  }
}

$("createForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const email = $("newEmail").value.trim();
  const password = $("newPassword").value;
  const ch = $("newChannel").value;
  if (!email) return flash("createMsg", "Enter the tenant's email.", "error");
  if (password.length < 8) return flash("createMsg", "Temporary password needs at least 8 characters.", "error");
  if (!ch) return flash("createMsg", "Pick a room.", "error");

  $("createBtn").disabled = true;
  let uid = null;
  try {
    uid = await createLogin(email, password);
    // The admin's own session writes the role record
    await set(ref(db, `users/${uid}`), { role: "tenant", channel: ch, email, mustChangePassword: true });
    flash("createMsg", `Created ${email} for ${name(ch)}. Give them the temporary password; they must change it at first sign-in.`, "ok");
    $("newEmail").value = "";
    $("newPassword").value = "";
  } catch (e) {
    console.error(e);
    const messages = {
      "auth/email-already-in-use": "That email already has a login. If it was unlinked, pick a room for it in Accounts above.",
      "auth/invalid-email": "That email address is not valid.",
      "auth/weak-password": "That password is too weak.",
      "auth/operation-not-allowed": "Email/password sign-up is disabled in Firebase Authentication settings."
    };
    flash("createMsg", uid
      ? `Login created (UID ${uid}) but linking it failed: ${e.message}`
      : (messages[e.code] || `Could not create the login: ${e.message}`), "error");
  } finally {
    $("createBtn").disabled = false;
  }
});

// ---------------------------------------------------------------------------
// Top-up log: filter and CSV export
// ---------------------------------------------------------------------------
$("logFilter").addEventListener("change", renderTables);

function csvTime(ms) {
  if (typeof ms !== "number") return "";
  const d = new Date(ms);
  const p = (n) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

$("csvBtn").addEventListener("click", () => {
  const q = (v) => {
    const s = String(v ?? "");
    return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
  };
  const online = deviceOnline();
  const lines = [["requested_at", "channel", "tenant", "wh", "admin", "status", "applied_at", "id"].join(",")];
  for (const [id, e] of logEntries($("logFilter").value)) {
    lines.push([csvTime(e.requestedAt), e.channel, name(e.channel), fmtWh(e.kwh, 0),
      adminName(e.by), topUpStatus(e, online).text, csvTime(e.appliedAt), id].map(q).join(","));
  }
  const blob = new Blob([lines.join("\r\n") + "\r\n"], { type: "text/csv" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = `topup-log-${csvTime(serverNow()).slice(0, 10)}.csv`;
  a.click();
  URL.revokeObjectURL(a.href);
});

// ---------------------------------------------------------------------------
// Hash router
// ---------------------------------------------------------------------------
function applyRoute() {
  const [view, arg] = (location.hash.slice(1) || "overview").split("/");
  const known = ["overview", "channel", "topup", "tenants", "log"];
  const next = known.includes(view) ? view : "overview";
  if (next === "channel" && !S.ids.includes(arg)) { location.hash = "#overview"; return; }

  if (route.view === "channel" && (next !== "channel" || arg !== route.arg)) leaveDetail();
  route.view = next;
  route.arg = arg || null;

  document.querySelectorAll("[data-view]").forEach((s) => { s.hidden = s.dataset.view !== next; });
  document.querySelectorAll("[data-nav]").forEach((a) => {
    a.classList.toggle("active", a.dataset.nav === next || (next === "channel" && a.dataset.nav === "overview"));
  });
  $("strip").hidden = !(next === "overview" || next === "channel");

  if (next === "channel") enterDetail(arg);
  if (next === "topup" && arg && S.ids.includes(arg)) {
    $("topUpChannel").value = arg;
    flash("topUpMsg", "", "ok");
    $("topUpWh").focus();
  }
  renderTables();
  window.scrollTo(0, 0);
}

window.addEventListener("hashchange", applyRoute);

// ---------------------------------------------------------------------------
// Start-up and the 1 s timer (only recomputes "last seen" and staleness)
// ---------------------------------------------------------------------------
fillChannelSelects();
renderNames();
renderStrip();
applyRoute();

setInterval(() => {
  renderStrip();
  if (S.reset) renderReset();
  for (const ch of S.ids) updateCard(cards[ch], ch);
  if (route.view === "channel") updateCard($("detailCard").firstElementChild, route.arg);
  // Pending rows change to "Not yet applied" after 60 s, so redraw while any are pending
  if (Object.values(S.log).some((e) => typeof e.appliedAt !== "number")) renderTables();
}, 1000);
