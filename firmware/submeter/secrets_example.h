/*
 * secrets_example.h
 * ---------------------------------------------------------------------------
 * Copy this file to secrets.h in the same folder and put the real values in
 * the copy. secrets.h is git-ignored and must never be committed or shared.
 * submeter.ino reads secrets.h first; anything not defined here falls back
 * to the placeholder in the sketch's CONFIG section.
 *
 * Every value below is a placeholder. Choose your own, and do not reuse one
 * password for several fields.
 * ---------------------------------------------------------------------------
 */
#pragma once

#define AP_PASS   "at-least-8-chars"   // Wi-Fi password of "SubMeter-Debug"
#define WEB_USER  "admin"              // log page login
#define WEB_PASS  "pick-a-password"
#define OTA_PASS  "pick-another-one"   // Arduino IDE asks for this on upload

#define STA_SSID  "building-wifi"      // leave "" for AP only
#define STA_PASS  "its-password"

// Firebase device login: a user made in Firebase Console > Authentication,
// with users/{its uid}/role = "device" in the database. Empty = bench test
// mode (no Firebase, every boot starts with TEST_ALLOCATION_KWH).
#define FB_EMAIL     "device@example.com"
#define FB_PASSWORD  "device-password"
