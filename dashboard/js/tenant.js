// Tenant page: read-only view of the tenant's own channel, plus top-up requests.
// The channel always comes from users/{uid}/channel, never from the URL.

import {
  db, requireRole, wireSignOut, fmt, fmtWh, toWh, ago, dateTime, escapeHtml, isFresh,
  balanceFraction, balanceLevel, watchChannel, askConfirm
} from "./common.js";
import { ref, push, query, orderByChild, equalTo, onValue, serverTimestamp } from "./sdk.js";
import { watchHistory, dailyKwh, avgPowerLastHour, hoursLeft, friendlyDuration, dayLabel } from "./usage.js";
import { dailyBarChart, setChartData } from "./charts.js";

const $ = (id) => document.getElementById(id);
const RING_LENGTH = 2 * Math.PI * 52;   // circumference of the SVG ring

const { profile } = await requireRole("tenant");
wireSignOut();
const ch = profile.channel;

// Everything the page shows lives here; render() redraws from it.
const S = { channel: {}, samples: [], topUps: {}, requests: {} };

// ---------- subscriptions (one onValue per readable child) ----------
watchChannel(ch, (state) => { S.channel = state; render(); }, showPermissionError);

// Tenant reads of topUpLog / topUpRequests must use this exact query
onValue(query(ref(db, "topUpLog"), orderByChild("channel"), equalTo(ch)),
  (snap) => { S.topUps = snap.val() || {}; render(); }, showPermissionError);
onValue(query(ref(db, "topUpRequests"), orderByChild("channel"), equalTo(ch)),
  (snap) => { S.requests = snap.val() || {}; render(); }, showPermissionError);

watchHistory(ch, 7, (samples) => { S.samples = samples; render(); });

function showPermissionError(err) {
  const box = $("errorBox");
  box.hidden = false;
  box.textContent = err && err.code === "PERMISSION_DENIED"
    ? `Permission denied reading room ${ch}. Ask the admin to check your account.`
    : "Could not load your data. Check your connection.";
}

// ---------- rendering ----------
// Energy is stored in kWh and shown in Wh
const chart = dailyBarChart($("dailyChart"), "Wh");

function render() {
  const c = S.channel;
  const live = c.live || {};
  const fresh = isFresh(live.ts);
  const rem = c.remainingBalance;
  const level = balanceLevel(rem, c.originalAllocation);
  const frac = balanceFraction(rem, c.originalAllocation);

  // 1. Greeting
  $("greeting").textContent = c.tenantName ? `Hello, ${c.tenantName}` : "Hello";

  // 2. Balance ring and relay pill
  $("units").textContent = fmtWh(rem, 0);
  $("pct").textContent = typeof rem === "number" ? `${Math.round(frac * 100)}%` : "";
  $("ring").className = `ring ${level}`;
  $("ringFill").setAttribute("stroke-dashoffset", String(RING_LENGTH * (1 - frac)));
  const pill = $("relayPill");
  const landlordOff = c.adminOff === true;
  if (landlordOff) {
    pill.className = "pill off"; pill.textContent = "Disconnected by landlord";
  } else if (c.relayStatus === "ON") {
    pill.className = "pill on"; pill.textContent = "Supply ON";
  } else if (c.relayStatus === "OFF") {
    pill.className = "pill off";
    pill.textContent = level === "empty" ? "Disconnected, units exhausted" : "Disconnected";
  } else {
    pill.className = "pill off"; pill.textContent = "–";
  }

  // 3. Low / exhausted banner (same 10 % threshold as the buzzer)
  const banner = $("lowBanner");
  if (landlordOff) {
    banner.className = "banner empty"; banner.hidden = false;
    banner.textContent = "The landlord has disconnected your supply. Your units are kept. Contact the landlord to reconnect.";
  } else if (level === "low") {
    banner.className = "banner low"; banner.hidden = false;
    banner.textContent = `You have ${fmtWh(rem, 0)} Wh left (${Math.round(frac * 100)}%). Request a top-up below.`;
  } else if (level === "empty" && typeof rem === "number") {
    banner.className = "banner empty"; banner.hidden = false;
    banner.textContent = "Your units are used up and the supply is disconnected. Request a top-up below.";
  } else {
    banner.hidden = true;
  }

  // Device offline / readings paused (the tenant can't read device/, so the
  // channel's own live.ts is the freshness signal)
  const paused = $("pausedBanner");
  paused.hidden = fresh || typeof live.ts !== "number";
  paused.textContent = `Readings paused. Last update ${ago(live.ts)}.`;

  // 4. Live now
  $("power").textContent = fresh ? `${fmt(live.power, 1)} W` : "–";
  const hours = fresh ? hoursLeft(rem, avgPowerLastHour(S.samples, live.power)) : null;
  $("timeLeft").textContent = level === "empty" ? "0" : (friendlyDuration(hours) ?? "–");
  $("voltage").textContent = fresh ? `${fmt(live.voltage, 1)} V` : "–";
  $("current").textContent = fresh ? `${fmt(live.current, 3)} A` : "–";
  $("pf").textContent = fresh ? fmt(live.pf, 2) : "–";

  // 5. Usage: today + 7-day chart
  const days = dailyKwh(S.samples, Object.values(S.topUps), rem, 7);
  const today = days[days.length - 1];
  $("today").textContent = today && today.kwh !== null ? `${fmtWh(today.kwh, 0)} Wh` : "–";
  setChartData(chart, days.map((d) => dayLabel(d.key)), days.map((d) => toWh(d.kwh ?? 0)));

  // 6. My top-ups: applied/pending top-ups, plus requests still waiting or declined
  // (an approved request shows up as its top-up entry)
  const rows = [
    ...Object.values(S.topUps).map((u) => ({
      t: u.requestedAt, kwh: u.kwh,
      status: typeof u.appliedAt === "number" ? ["applied", "Applied"] : ["pending", "Pending"]
    })),
    ...Object.values(S.requests).filter((r) => r.status !== "approved").map((r) => ({
      t: r.requestedAt, kwh: r.kwh,
      status: r.status === "declined" ? ["declined", "Request declined"] : ["waiting", "Waiting for approval"]
    }))
  ].sort((a, b) => (b.t || 0) - (a.t || 0));

  $("myTopUps").innerHTML = rows.length
    ? rows.map((r) => `<li>
        <span><strong class="num">${fmtWh(r.kwh, 0)} Wh</strong><br><span class="muted small">${escapeHtml(dateTime(r.t))}</span></span>
        <span class="status ${r.status[0]}">${r.status[1]}</span></li>`).join("")
    : `<li class="muted">No top-ups yet</li>`;

  // Footer
  $("footer").textContent = typeof live.ts === "number" ? `Updated ${ago(live.ts)}` : "Waiting for the first reading…";
}

// A 1 s timer only re-computes "last seen" text and staleness (no DB polling)
setInterval(render, 1000);

// ---------- show / hide meter details ----------
$("toggleDetails").addEventListener("click", () => {
  const d = $("details");
  d.hidden = !d.hidden;
  $("toggleDetails").textContent = d.hidden ? "Show meter details" : "Hide meter details";
});

// ---------- request a top-up ----------
$("requestForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const msg = $("reqMsg");
  // Entered in whole Wh, stored in kWh
  const wh = Math.round(Number($("reqUnits").value));
  const kwh = wh / 1000;
  if (!(wh >= 1 && wh <= 500000)) {
    msg.className = "msg error"; msg.hidden = false;
    msg.textContent = "Enter between 1 and 500000 Wh.";
    return;
  }
  if (!(await askConfirm(`Ask the admin for ${wh} Wh?`, "Send request"))) return;

  $("reqBtn").disabled = true;
  try {
    await push(ref(db, "topUpRequests"), {
      channel: ch, kwh, requestedAt: serverTimestamp(), status: "waiting"
    });
    msg.className = "msg ok";
    msg.textContent = "Request sent. You'll see it here once the admin approves it.";
    $("reqUnits").value = "";
  } catch (e) {
    console.error(e);
    msg.className = "msg error";
    msg.textContent = "Could not send the request. Try again.";
  } finally {
    msg.hidden = false;
    $("reqBtn").disabled = false;
  }
});
