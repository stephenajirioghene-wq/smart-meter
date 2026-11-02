/*
 * FirebaseSync.cpp: see FirebaseSync.h for the design.
 */
#include "FirebaseSync.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "WebLog.h"

FirebaseSync Firebase;

// Values sent to Firebase: 4 decimals (1 Wh = 0.001 kWh), never below 0
// because the rules require remainingBalance >= 0 (OI-02). The local
// balance in loop() may still go slightly negative.
static float fbRound(float x) { return roundf(x * 10000.0f) / 10000.0f; }
static float fbBalance(float x) { return x < 0 ? 0.0f : fbRound(x); }

bool FirebaseSync::begin(const char *apiKey, const char *dbUrl, const char *email,
                         const char *password, const char *fwVersion) {
  if (!apiKey[0] || !dbUrl[0] || !email[0] || !password[0]) {
    Log.println("[FB] not configured (FB_EMAIL / FB_PASSWORD empty in secrets.h), local balances only");
    return false;
  }
  _apiKey = apiKey;
  _dbUrl = dbUrl;
  while (_dbUrl.endsWith("/")) _dbUrl.remove(_dbUrl.length() - 1);
  _email = email;
  _password = password;
  _fw = fwVersion;
  _topUps = xQueueCreate(8, sizeof(FbTopUp));
  _loaded = xQueueCreate(1, sizeof(FbLoaded));
  _enabled = true;
  // Core 0 (Wi-Fi core), loop() runs on core 1. TLS needs a big stack.
  xTaskCreatePinnedToCore(taskEntry, "firebase", 12288, this, 1, nullptr, 0);
  Log.printf("[FB] sync task started, %s, every %lu s\n", _dbUrl.c_str(), FB_SYNC_MS / 1000);
  return true;
}

// ---------------------------------------------------------------------------
// Called from loop()
// ---------------------------------------------------------------------------
void FirebaseSync::publish(const FbSnapshot &s) {
  portENTER_CRITICAL(&_mux);
  _snap = s;
  _haveSnap = true;
  portEXIT_CRITICAL(&_mux);
}

bool FirebaseSync::takeTopUp(FbTopUp &t) {
  return _topUps && xQueueReceive(_topUps, &t, 0) == pdTRUE;
}

void FirebaseSync::topUpDone(uint8_t ch) {
  if (ch < FB_CHANNELS) _inFlight[ch] = false;
}

void FirebaseSync::requestLoad() { _loadWanted = true; }

bool FirebaseSync::takeLoaded(FbLoaded &l) {
  return _loaded && xQueueReceive(_loaded, &l, 0) == pdTRUE;
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------
void FirebaseSync::taskEntry(void *arg) {
  static_cast<FirebaseSync *>(arg)->run();
}

void FirebaseSync::run() {
  unsigned long lastSync = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(200));
    if (_paused || WiFi.status() != WL_CONNECTED) continue;

    unsigned long now = millis();
    if (!signedIn || now - _tokenAt >= FB_TOKEN_MS) {
      if (_lastSignInTry && now - _lastSignInTry < FB_RETRY_MS) continue;
      _lastSignInTry = now;
      if (!signIn()) continue;
    }

    if (_loadWanted && loadBalances()) _loadWanted = false;

    if (now - lastSync < FB_SYNC_MS) continue;
    lastSync = now;

    FbSnapshot s;
    bool have;
    portENTER_CRITICAL(&_mux);
    s = _snap;
    have = _haveSnap;
    portEXIT_CRITICAL(&_mux);
    if (!have) continue;

    syncLive(s);          // step 6
    checkControl(s);      // landlord disconnect and reset, feeds step 4
    checkTopUps(s);       // feeds step 4 of the next cycle
    writeHistory(s);
  }
}

// One HTTPS request. Returns the HTTP code (negative = no connection).
// Certificates are not checked (setInsecure): a stated prototype limitation.
int FirebaseSync::request(const char *method, const String &url, const String &body, String *resp) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(FB_HTTP_TIMEOUT);
  http.setConnectTimeout(FB_HTTP_TIMEOUT);
  if (!http.begin(client, url)) return -100;
  if (body.length()) http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest(method, body);
  if (resp) *resp = (code > 0) ? http.getString() : String();
  http.end();
  if (code == 401) signedIn = false;     // token expired or revoked: sign in again
  return code;
}

String FirebaseSync::dbPath(const String &path) {
  return _dbUrl + "/" + path + ".json?auth=" + _token;
}

bool FirebaseSync::signIn() {
  String url = "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" + _apiKey;
  JsonDocument req;
  req["email"] = _email;
  req["password"] = _password;
  req["returnSecureToken"] = true;
  String body, resp;
  serializeJson(req, body);
  int code = request("POST", url, body, &resp);
  if (code != 200) {
    Log.printf("[FB] ERR sign-in failed (HTTP %d), retry in %lu s\n", code, FB_RETRY_MS / 1000);
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp) || !doc["idToken"].is<const char *>()) {
    Log.println("[FB] ERR sign-in reply not understood");
    return false;
  }
  _token = doc["idToken"].as<String>();
  _tokenAt = millis();
  signedIn = true;
  Log.printf("[FB] signed in as %s\n", _email.c_str());
  return true;
}

// Multi-path update at the database root: all paths apply or none do.
bool FirebaseSync::patchRoot(const String &body, const char *what) {
  String resp;
  int code = request("PATCH", dbPath(""), body, &resp);
  if (code == 200) {
    writesOk = writesOk + 1;
    return true;
  }
  writesFail = writesFail + 1;
  if (resp.length() > 80) resp = resp.substring(0, 80);
  resp.replace(",", ";");                // keep the #EV line one CSV field
  resp.replace("\n", " ");
  Log.printf("#EV,%lu,0,FB_WRITE_FAIL,path=%s;err=%d %s\n", millis(), what, code, resp.c_str());
  return false;
}

// Step 6: device heartbeat, live readings, balances and relay states, one update.
void FirebaseSync::syncLive(const FbSnapshot &s) {
  JsonDocument doc;
  doc["device/lastSeen"][".sv"] = "timestamp";
  doc["device/firmware"] = _fw;
  doc["device/ip"] = WiFi.localIP().toString();
  bool resetSeen = (s.resetGen == _resetGen);   // false: snapshot is from before a reset
  for (uint8_t i = 0; i < FB_CHANNELS; i++) {
    const FbChannelSnap &c = s.ch[i];
    String p = "channels/ch" + String(i + 1) + "/";
    if (c.known && resetSeen) {          // never overwrite Firebase with an unknown or pre-reset balance
      doc[p + "remainingBalance"] = fbBalance(c.balance);
      doc[p + "originalAllocation"] = fbBalance(c.original);
    }
    doc[p + "relayStatus"] = c.relayOn ? "ON" : "OFF";
    if (c.valid) {                       // no reading: live.ts goes stale, dashboard shows "No reading"
      JsonObject live = doc[p + "live"].to<JsonObject>();
      live["voltage"] = fbRound(c.voltage);
      live["current"] = fbRound(c.current);
      live["power"] = fbRound(c.power);
      live["pf"] = fbRound(c.pf);
      live["energy"] = fbRound(c.energy);
      live["frequency"] = fbRound(c.frequency);
      live["ts"][".sv"] = "timestamp";
    }
  }
  String body;
  serializeJson(doc, body);
  patchRoot(body, "live");
}

// Top-up handshake (spec "Top-up handshake"). Per channel, the oldest entry
// in pendingTopUps is applied with one multi-path update. loop() adds the
// kWh locally only after this update succeeded. One entry per channel per
// round; the next waits until loop() has added the previous one.
void FirebaseSync::checkTopUps(const FbSnapshot &s) {
  if (s.resetGen != _resetGen) return;   // wait until loop() has applied the reset
  for (uint8_t i = 0; i < FB_CHANNELS; i++) {
    if (!s.ch[i].known || _inFlight[i]) continue;
    String chName = "ch" + String(i + 1);
    String resp;
    int code = request("GET", dbPath("channels/" + chName + "/pendingTopUps"), "", &resp);
    if (code != 200 || resp == "null") continue;

    JsonDocument doc;
    if (deserializeJson(doc, resp) || !doc.is<JsonObject>()) continue;
    const char *bestId = nullptr;
    double bestAt = 0;
    float bestKwh = 0;
    for (JsonPair kv : doc.as<JsonObject>()) {
      double at = kv.value()["requestedAt"] | 0.0;
      float kwh = kv.value()["kwh"] | 0.0f;
      if (kwh <= 0) continue;
      if (!bestId || at < bestAt) {
        bestId = kv.key().c_str();
        bestAt = at;
        bestKwh = kwh;
      }
    }
    if (!bestId) continue;

    float after = s.ch[i].balance + bestKwh;
    JsonDocument up;
    up["channels/" + chName + "/remainingBalance"] = fbBalance(after);
    up["channels/" + chName + "/originalAllocation"] = fbBalance(after);
    up["topUpLog/" + String(bestId) + "/appliedAt"][".sv"] = "timestamp";
    up["channels/" + chName + "/pendingTopUps/" + String(bestId)] = nullptr;
    String body;
    serializeJson(up, body);
    if (!patchRoot(body, "topup")) continue;

    FbTopUp t;
    t.ch = i;
    t.kwh = bestKwh;
    strlcpy(t.id, bestId, sizeof t.id);
    _inFlight[i] = true;
    if (xQueueSend(_topUps, &t, 0) != pdTRUE) _inFlight[i] = false;   // queue holds 8, not expected
  }
}

// Landlord commands, one GET of the whole control/ node:
//   control/disconnect/chN  true = relay held OFF (loop() reads adminOff())
//   control/resetUnits      present = set every balance to 0
// The reset waits until every balance is known, no top-up is between
// Firebase and loop(), and loop() has applied any earlier reset. Then one
// multi-path update writes 0 to every balance and allocation and deletes the
// request, so the reset is applied once. loop() zeroes locally on the next
// cycle. Pending top-ups are left alone and apply after the reset.
void FirebaseSync::checkControl(const FbSnapshot &s) {
  String resp;
  if (request("GET", dbPath("control"), "", &resp) != 200) return;
  JsonDocument doc;
  if (resp != "null" && (deserializeJson(doc, resp) || !doc.is<JsonObject>())) return;

  for (uint8_t i = 0; i < FB_CHANNELS; i++) {
    _adminOff[i] = doc["disconnect"]["ch" + String(i + 1)] | false;
  }
  _controlKnown = true;

  if (!doc["resetUnits"].is<JsonObject>()) return;
  if (s.resetGen != _resetGen) return;
  for (uint8_t i = 0; i < FB_CHANNELS; i++) {
    if (!s.ch[i].known || _inFlight[i]) return;
  }
  JsonDocument up;
  for (uint8_t i = 0; i < FB_CHANNELS; i++) {
    String p = "channels/ch" + String(i + 1) + "/";
    up[p + "remainingBalance"] = 0;
    up[p + "originalAllocation"] = 0;
  }
  up["control/resetUnits"] = nullptr;
  String body;
  serializeJson(up, body);
  if (!patchRoot(body, "reset")) return;
  _resetGen = _resetGen + 1;
  Log.println("[FB] reset all units written to Firebase");
}

// One sample per channel every FB_HISTORY_MIN minutes, keyed in local time
// (WAT, set by configTime in submeter.ino). Needs NTP time.
void FirebaseSync::writeHistory(const FbSnapshot &s) {
  time_t t = time(nullptr);
  if (t < 1700000000) return;            // clock not set yet
  if (s.resetGen != _resetGen) return;   // snapshot still holds pre-reset balances
  struct tm lt;
  localtime_r(&t, &lt);
  char day[12], hhmm[6];
  strftime(day, sizeof day, "%Y-%m-%d", &lt);
  snprintf(hhmm, sizeof hhmm, "%02d%02d", lt.tm_hour, lt.tm_min - lt.tm_min % FB_HISTORY_MIN);
  String key = String(day) + "/" + hhmm;
  if (key == _lastHistKey) return;

  JsonDocument doc;
  for (uint8_t i = 0; i < FB_CHANNELS; i++) {
    if (!s.ch[i].known) continue;
    String p = "channels/ch" + String(i + 1) + "/history/" + key + "/";
    doc[p + "power"] = fbRound(s.ch[i].valid ? s.ch[i].power : 0);
    doc[p + "balance"] = fbBalance(s.ch[i].balance);
  }
  if (doc.size() == 0) return;
  String body;
  serializeJson(doc, body);
  if (patchRoot(body, "history")) _lastHistKey = key;
}

// First boot with nothing in NVS: read each channel's balance from Firebase.
// A channel with no balance in Firebase starts at 0 (relay stays OFF until
// the admin tops it up).
bool FirebaseSync::loadBalances() {
  FbLoaded l;
  for (uint8_t i = 0; i < FB_CHANNELS; i++) {
    String p = "channels/ch" + String(i + 1) + "/";
    String rb, ro;
    if (request("GET", dbPath(p + "remainingBalance"), "", &rb) != 200) return false;
    if (request("GET", dbPath(p + "originalAllocation"), "", &ro) != 200) return false;
    l.balance[i] = (rb == "null") ? 0.0f : rb.toFloat();
    l.original[i] = (ro == "null") ? 0.0f : ro.toFloat();
  }
  xQueueOverwrite(_loaded, &l);
  Log.println("[FB] balances read from Firebase");
  return true;
}
