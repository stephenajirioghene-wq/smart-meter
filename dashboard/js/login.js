// Login page: sign in, route by users/{uid}/role, forgot password,
// and the first-login password change (mustChangePassword).

import { auth, db, configMissing } from "./common.js";
import {
  ref, get, set, onAuthStateChanged, signInWithEmailAndPassword, signOut,
  setPersistence, browserLocalPersistence, browserSessionPersistence,
  sendPasswordResetEmail, updatePassword
} from "./sdk.js";

const $ = (id) => document.getElementById(id);
const loading = $("loading");
const loginForm = $("loginForm");
const changeForm = $("changeForm");
const msgBox = $("msg");

const NOT_LINKED = "Your account is not linked to a room yet. Ask the admin.";

function showMsg(text, kind = "error") {
  msgBox.textContent = text;
  msgBox.className = `msg ${kind}`;
  msgBox.hidden = !text;
}

function showOnly(el) {
  for (const e of [loading, loginForm, changeForm]) e.hidden = e !== el;
}

// Where each role goes. Anything else is "not linked".
function destination(profile) {
  if (!profile) return null;
  if (profile.role === "admin") return "admin.html";
  if (profile.role === "tenant" && profile.channel) return "tenant.html";
  return null;
}

// Runs whenever someone is signed in (fresh sign-in or remembered session).
async function route(user) {
  showOnly(loading);
  let profile = null;
  try {
    profile = (await get(ref(db, `users/${user.uid}`))).val();
  } catch (e) {
    console.error(e);
    showMsg("Could not load your account. Check your connection and try again.");
    await signOut(auth);
    return;
  }

  const target = destination(profile);
  if (!target) {
    // No record, tenant without a channel, or the ESP32 device account
    showMsg(NOT_LINKED);
    await signOut(auth);
    return;
  }

  if (profile.mustChangePassword === true) {
    showMsg("");
    changeForm.dataset.target = target;
    showOnly(changeForm);
    $("newPw").focus();
    return;
  }
  location.replace(target);
}

onAuthStateChanged(auth, (user) => {
  if (user) {
    route(user);
  } else {
    showOnly(loginForm);
    if (configMissing) {
      showMsg("firebase-config.js still has the PASTE_... placeholders. Paste your Firebase web config into it, then reload.");
      $("signInBtn").disabled = true;
    }
  }
});

// ---------- sign in ----------
$("togglePw").addEventListener("click", () => {
  const pw = $("password");
  const show = pw.type === "password";
  pw.type = show ? "text" : "password";
  $("togglePw").textContent = show ? "Hide" : "Show";
  $("togglePw").setAttribute("aria-label", show ? "Hide password" : "Show password");
});

loginForm.addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const email = $("email").value.trim();
  const password = $("password").value;
  if (!email || !password) return showMsg("Enter your email and password.");

  $("signInBtn").disabled = true;
  showMsg("");
  try {
    await setPersistence(auth, $("keep").checked ? browserLocalPersistence : browserSessionPersistence);
    await signInWithEmailAndPassword(auth, email, password);
    // onAuthStateChanged takes it from here
  } catch (e) {
    showMsg(signInError(e.code));
  } finally {
    $("signInBtn").disabled = false;
  }
});

// One generic message for wrong password and unknown email, so the page
// doesn't reveal which emails have accounts. Any other failure (setup or
// config problems) shows its error code so it isn't mistaken for a bad password.
const CREDENTIAL_ERRORS = [
  "auth/invalid-credential", "auth/wrong-password", "auth/user-not-found",
  "auth/invalid-email", "auth/missing-password", "auth/user-disabled"
];

function signInError(code) {
  if (CREDENTIAL_ERRORS.includes(code)) return "Email or password is wrong";
  if (code === "auth/too-many-requests") return "Too many attempts. Wait a few minutes or reset your password.";
  if (code === "auth/network-request-failed") return "No connection. Check your internet and try again.";
  return `Sign-in failed (${code || "unknown error"}). Check the Firebase setup.`;
}

// ---------- forgot password ----------
$("forgot").addEventListener("click", async () => {
  const email = $("email").value.trim();
  if (!email) {
    showMsg("Type your email above first, then tap Forgot password.", "info");
    $("email").focus();
    return;
  }
  try {
    await sendPasswordResetEmail(auth, email);
  } catch (e) {
    console.warn(e.code); // same message either way
  }
  showMsg("If that email has an account, a reset link is on its way", "ok");
});

// ---------- first-login password change ----------
changeForm.addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const pw = $("newPw").value;
  if (pw.length < 8) return showMsg("Use at least 8 characters.");
  if (pw !== $("newPw2").value) return showMsg("The two passwords don't match.");

  const user = auth.currentUser;
  $("changeBtn").disabled = true;
  try {
    await updatePassword(user, pw);
    // The rules let a user write only this field on their own record, only to false
    await set(ref(db, `users/${user.uid}/mustChangePassword`), false);
    location.replace(changeForm.dataset.target);
  } catch (e) {
    console.error(e);
    if (e.code === "auth/requires-recent-login") {
      showMsg("For security, sign in again with your temporary password, then choose a new one.");
      await signOut(auth);
    } else if (e.code === "auth/weak-password") {
      showMsg("That password is too weak. Try a longer one.");
    } else {
      showMsg("Could not save the new password. Try again.");
    }
  } finally {
    $("changeBtn").disabled = false;
  }
});
