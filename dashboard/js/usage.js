// Usage maths shared by the admin detail page and the tenant page:
// history listeners, daily kWh, average power and estimated time left.
//
// History layout: channels/{ch}/history/{YYYY-MM-DD}/{HHMM} = { power, balance }
// with one sample every 5 minutes, keyed in building local time.

import { HISTORY_UTC_OFFSET_MIN } from "../firebase-config.js";
import { db, serverNow } from "./common.js";
import { ref, onValue } from "./sdk.js";

const DAY_MS = 24 * 3600 * 1000;
const OFFSET_MS = HISTORY_UTC_OFFSET_MIN * 60 * 1000;

// "2026-10-14" for the building-local day containing time ms
export function dayKey(ms) {
  return new Date(ms + OFFSET_MS).toISOString().slice(0, 10);
}

// Epoch ms of the building-local midnight that starts the given day key
export function dayStart(key) {
  return Date.parse(`${key}T00:00:00Z`) - OFFSET_MS;
}

// Epoch ms for a history sample key pair ("2026-10-14", "1310")
function sampleTime(key, hhmm) {
  return dayStart(key) + (Number(hhmm.slice(0, 2)) * 60 + Number(hhmm.slice(2, 4))) * 60000;
}

// "13:10" in building local time, for chart labels
export function clockLabel(ms) {
  return new Date(ms + OFFSET_MS).toISOString().slice(11, 16);
}

// Short day label for the 7-day chart ("Tue 14")
export function dayLabel(key) {
  const d = new Date(`${key}T12:00:00Z`);
  return d.toLocaleDateString(undefined, { weekday: "short", day: "numeric", timeZone: "UTC" });
}

// The last n building-local day keys, oldest first, ending today
export function lastDayKeys(n) {
  const today = dayStart(dayKey(serverNow()));
  const keys = [];
  for (let i = n - 1; i >= 0; i--) keys.push(dayKey(today - i * DAY_MS + 3600000));
  return keys;
}

// Listen to the last `days` days of history plus the day before (needed as
// the starting balance for the oldest day). Calls onSamples with a sorted
// array of { t, power, balance }. Re-subscribes after local midnight so a page
// left open keeps rolling. Returns a stop function.
export function watchHistory(ch, days, onSamples) {
  let stops = [];
  let timer = null;
  const byDay = {};

  function emit() {
    const samples = [];
    for (const [key, rows] of Object.entries(byDay)) {
      if (!rows) continue;
      for (const [hhmm, row] of Object.entries(rows)) {
        if (row && typeof row.balance === "number") {
          samples.push({ t: sampleTime(key, hhmm), power: row.power, balance: row.balance });
        }
      }
    }
    samples.sort((a, b) => a.t - b.t);
    onSamples(samples);
  }

  function subscribe() {
    stops.forEach((stop) => stop());
    for (const k of Object.keys(byDay)) delete byDay[k];
    stops = lastDayKeys(days + 1).map((key) =>
      onValue(ref(db, `channels/${ch}/history/${key}`),
        (snap) => { byDay[key] = snap.val(); emit(); },
        (err) => console.error(`history ${key}`, err))
    );
    const nextMidnight = dayStart(dayKey(serverNow())) + DAY_MS;
    timer = setTimeout(subscribe, nextMidnight - serverNow() + 5000);
  }

  subscribe();
  return () => { stops.forEach((stop) => stop()); clearTimeout(timer); };
}

// kWh used per day for the last `days` days:
//   used = balance at start - balance at end + top-ups applied in between
// Start balance is the last sample at or before local midnight (or the
// day's first sample). For today the end is the live balance right now.
// topUps: array of { kwh, appliedAt }.
export function dailyKwh(samples, topUps, liveBalance, days) {
  const now = serverNow();
  return lastDayKeys(days).map((key) => {
    const t0 = dayStart(key);
    const t1 = t0 + DAY_MS;
    const before = samples.filter((s) => s.t <= t0);
    const inside = samples.filter((s) => s.t > t0 && s.t <= t1);
    const start = before.length ? before[before.length - 1] : inside[0];
    let end = inside.length ? inside[inside.length - 1] : null;
    if (now < t1 && typeof liveBalance === "number") end = { t: now, balance: liveBalance };
    if (!start || !end || end.t <= start.t) return { key, kwh: start || end ? 0 : null };

    const added = topUps
      .filter((u) => typeof u.appliedAt === "number" && u.appliedAt > start.t && u.appliedAt <= end.t)
      .reduce((sum, u) => sum + u.kwh, 0);
    const used = start.balance - end.balance + added;
    return { key, kwh: Math.max(0, Math.round(used * 1000) / 1000) };
  });
}

// Average power over the last hour of history samples. Falls back to the
// live reading when no sample exists yet (first 5 minutes after start-up).
export function avgPowerLastHour(samples, livePower) {
  const since = serverNow() - 3600000;
  const recent = samples.filter((s) => s.t > since && typeof s.power === "number");
  if (!recent.length) return typeof livePower === "number" ? livePower : null;
  return recent.reduce((sum, s) => sum + s.power, 0) / recent.length;
}

// Hours left = balance (kWh) / average power (kW). Hidden under 5 W.
export function hoursLeft(balance, avgWatts) {
  if (typeof balance !== "number" || typeof avgWatts !== "number" || avgWatts < 5) return null;
  return (balance * 1000) / avgWatts;
}

// "About 17 days" / "About 6 hours" / "Less than 1 hour"
export function friendlyDuration(hours) {
  if (hours === null) return null;
  if (hours < 1) return "Less than 1 hour";
  if (hours < 48) return `About ${Math.round(hours)} hours`;
  return `About ${Math.round(hours / 24)} days`;
}
