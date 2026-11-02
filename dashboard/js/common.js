// Shared helpers for every page: Firebase handles, server clock, page guard,
// number formatting, balance thresholds and the per-channel listeners.

import { firebaseConfig, STALE_MS } from "../firebase-config.js";
import {
  initializeApp, getAuth, getDatabase, ref, get, onValue, onAuthStateChanged, signOut
} from "./sdk.js";

// True while firebase-config.js still holds the PASTE_... placeholders
export const configMissing = Object.values(firebaseConfig).some((v) => String(v).startsWith("PASTE_"));

export const app = initializeApp(firebaseConfig);
export const auth = getAuth(app);
export const db = getDatabase(app);

// ---------------------------------------------------------------------------
// Server clock. Staleness is judged against Firebase's clock, not the
// browser's, so a phone with a wrong clock doesn't show everything offline.
// ---------------------------------------------------------------------------
let serverOffset = 0;
onValue(ref(db, ".info/serverTimeOffset"), (snap) => { serverOffset = snap.val() || 0; });
export const serverNow = () => Date.now() + serverOffset;

export const isFresh = (ts) => typeof ts === "number" && serverNow() - ts < STALE_MS;

// ---------------------------------------------------------------------------
// Page guard. Every page except login calls this on load. No user, the wrong
// role, an unlinked tenant or a pending password change goes back to login,
// which shows the right message.
// ---------------------------------------------------------------------------
export function requireRole(role) {
  return new Promise((resolve) => {
    const stop = onAuthStateChanged(auth, async (user) => {
      stop();
      if (!user) return location.replace("index.html");
      let profile = null;
      try {
        profile = (await get(ref(db, `users/${user.uid}`))).val();
      } catch (e) {
        console.error("Could not read users record", e);
      }
      const ok = profile && profile.role === role &&
        profile.mustChangePassword !== true &&
        (role !== "tenant" || !!profile.channel);
      if (!ok) return location.replace("index.html");

      // Signing out in another tab sends this tab back to login too.
      onAuthStateChanged(auth, (u) => { if (!u) location.replace("index.html"); });
      resolve({ user, profile });
    });
  });
}

// Any element with data-signout becomes a sign-out button.
export function wireSignOut() {
  document.querySelectorAll("[data-signout]").forEach((btn) => {
    btn.addEventListener("click", async () => {
      await signOut(auth);
      location.replace("index.html");
    });
  });
}

// ---------------------------------------------------------------------------
// Formatting (decimals follow the spec's formatting table)
// ---------------------------------------------------------------------------
export function fmt(value, decimals) {
  return typeof value === "number" && isFinite(value) ? value.toFixed(decimals) : "–";
}

// Firebase stores energy in kWh (the firmware and the serial log work in kWh).
// The pages show and accept Wh, so convert only at the edge.
export const toWh = (kwh) => (typeof kwh === "number" ? kwh * 1000 : kwh);
export const fmtWh = (kwh, decimals) => fmt(toWh(kwh), decimals);

export function ago(ms) {
  if (typeof ms !== "number") return "never";
  const s = Math.max(0, Math.round((serverNow() - ms) / 1000));
  if (s < 60) return `${s} s ago`;
  const m = Math.round(s / 60);
  if (m < 60) return `${m} min ago`;
  const h = Math.round(m / 60);
  if (h < 48) return `${h} h ago`;
  return `${Math.round(h / 24)} days ago`;
}

export function dateTime(ms) {
  if (typeof ms !== "number") return "–";
  return new Date(ms).toLocaleString(undefined, {
    day: "numeric", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit"
  });
}

export function escapeHtml(text) {
  return String(text ?? "").replace(/[&<>"']/g, (c) => (
    { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]
  ));
}

// ---------------------------------------------------------------------------
// Balance thresholds (same 10 % as the buzzer)
// ---------------------------------------------------------------------------
export const LOW_FRACTION = 0.10;

export function balanceFraction(remaining, original) {
  if (typeof remaining !== "number") return 0;
  if (typeof original !== "number" || original <= 0) return remaining > 0 ? 1 : 0;
  return Math.min(1, Math.max(0, remaining / original));
}

// "empty" at 0, "low" at 10 % or below, else "ok"
export function balanceLevel(remaining, original) {
  if (typeof remaining !== "number" || remaining <= 0) return "empty";
  if (typeof original === "number" && original > 0 && remaining / original <= LOW_FRACTION) return "low";
  return "ok";
}

// ---------------------------------------------------------------------------
// Top-up status. Pending until the ESP32 sets appliedAt. Still pending after
// 60 s with the device online means something is wrong on the device side.
// ---------------------------------------------------------------------------
export function topUpStatus(entry, deviceOnline) {
  if (typeof entry.appliedAt === "number") return { key: "applied", text: "Applied" };
  if (deviceOnline && typeof entry.requestedAt === "number" && serverNow() - entry.requestedAt > 60000) {
    return { key: "late", text: "Not yet applied, check the device" };
  }
  return { key: "pending", text: "Pending" };
}

// ---------------------------------------------------------------------------
// Per-channel listeners. A tenant may read only the individual children of
// channels/{ch}, never the whole node, so each field gets its own onValue.
// The admin uses the same function so the overview never downloads history.
// ---------------------------------------------------------------------------
// adminOff is the landlord disconnect flag. It lives under control/disconnect
// (not channels/) so the ESP32 reads every command in one request.
export const CHANNEL_FIELDS = ["tenantName", "remainingBalance", "originalAllocation", "relayStatus", "live", "adminOff"];
const fieldPath = (ch, field) => (field === "adminOff" ? `control/disconnect/${ch}` : `channels/${ch}/${field}`);

export function watchChannel(ch, onChange, onError) {
  const state = {};
  const stops = CHANNEL_FIELDS.map((field) =>
    onValue(ref(db, fieldPath(ch, field)),
      (snap) => { state[field] = snap.val(); onChange(state, field); },
      (err) => { console.error(fieldPath(ch, field), err); if (onError) onError(err, field); })
  );
  return () => stops.forEach((stop) => stop());
}

// Reusable confirm step built on <dialog id="confirmDialog"> (present on admin
// and tenant pages). Resolves true on Confirm, false on Cancel or Escape.
export function askConfirm(message, confirmLabel = "Confirm") {
  const dlg = document.getElementById("confirmDialog");
  dlg.querySelector("[data-msg]").textContent = message;
  dlg.querySelector("[data-ok]").textContent = confirmLabel;
  dlg.returnValue = "";
  dlg.showModal();
  return new Promise((resolve) => {
    dlg.addEventListener("close", () => resolve(dlg.returnValue === "ok"), { once: true });
  });
}
