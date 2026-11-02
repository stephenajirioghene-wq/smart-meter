/*
 * FirebaseSync.h
 * ---------------------------------------------------------------------------
 * Firebase Realtime Database link for submeter.ino, over the REST API.
 *
 * All network work runs in its own FreeRTOS task on core 0, so a slow or
 * dead internet link never holds up polling, deduction or relay switching
 * in loop() (core 1). The two sides only swap small structs:
 *
 *   loop() step 6  -> publish(snapshot)     latest readings and balances
 *   loop() step 4  <- takeTopUp(t)          a top-up the task has applied
 *   loop() step 4  <- takeLoaded(l)         balances read at first boot
 *   loop() step 4  <- adminOff(ch)          landlord disconnect flags
 *   loop() step 4  <- resetGen()            count of "reset all units" applied
 *
 * Data contract: "Sub-meter dashboard spec.md" and firebase/database.rules.json.
 *   device/{lastSeen, firmware, ip}                  every FB_SYNC_MS
 *   channels/chN/{remainingBalance, originalAllocation, relayStatus, live}
 *   channels/chN/history/YYYY-MM-DD/HHMM {power, balance}   every 5 min
 *   Top-up handshake: one multi-path update per pendingTopUps entry
 *   (balance, originalAllocation, topUpLog/{id}/appliedAt, delete entry).
 *   control/disconnect/chN (true = landlord holds the relay OFF), read
 *   every FB_SYNC_MS together with control/resetUnits. A reset request is
 *   applied with one multi-path update (every balance and allocation 0,
 *   delete the request); loop() then zeroes its local balances.
 *
 * The device signs in as its own Firebase user (role "device"), email and
 * password from secrets.h, through the Firebase Auth REST API.
 * ---------------------------------------------------------------------------
 */
#pragma once
#include <Arduino.h>

#define FB_CHANNELS      4
#define FB_SYNC_MS       10000UL      // live + balances (dashboard assumes 10 s, OI-26)
#define FB_HISTORY_MIN   5            // one history sample per channel every 5 min
#define FB_TOKEN_MS      (50UL * 60UL * 1000UL)   // re-sign-in before the 60 min token expires
#define FB_RETRY_MS      30000UL      // wait after a failed sign-in
#define FB_HTTP_TIMEOUT  6000         // ms per request

// One channel as loop() last saw it.
struct FbChannelSnap {
  bool  valid;                 // meter answered this poll
  float voltage, current, power, pf, energy, frequency;
  bool  known;                 // balance is known (never write an unknown balance)
  float balance, original;     // kWh, balance may be below 0 locally
  bool  relayOn;
};

struct FbSnapshot {
  FbChannelSnap ch[FB_CHANNELS];
  uint32_t resetGen;           // last reset loop() has applied (see resetGen())
};

// A top-up already written to Firebase. loop() adds kwh to the local balance.
struct FbTopUp {
  uint8_t ch;                  // 0-based
  float   kwh;
  char    id[32];
};

// Balances read from Firebase at first boot (when NVS had none).
struct FbLoaded {
  float balance[FB_CHANNELS];
  float original[FB_CHANNELS];
};

class FirebaseSync {
public:
  // Starts the task. Returns false if any setting is empty (then nothing runs).
  bool begin(const char *apiKey, const char *dbUrl, const char *email,
             const char *password, const char *fwVersion);
  bool enabled() const { return _enabled; }

  void publish(const FbSnapshot &s);   // loop() step 6
  bool takeTopUp(FbTopUp &t);          // loop() step 4, call until false
  void topUpDone(uint8_t ch);          // loop() step 4, after the local add
  void requestLoad();                  // ask the task to read balances once
  bool takeLoaded(FbLoaded &l);        // loop() step 4
  void setPaused(bool p) { _paused = p; }   // OTA

  // Landlord commands. controlKnown() is false until control/ has been read
  // once; until then loop() keeps the flags it restored from flash.
  bool controlKnown() const { return _controlKnown; }
  bool adminOff(uint8_t ch) const { return ch < FB_CHANNELS && _adminOff[ch]; }
  // Goes up by 1 each time a reset is written to Firebase. loop() zeroes its
  // balances when this differs from the last value it applied, and reports
  // that value back in FbSnapshot.resetGen. Until the two match, the task
  // sends no balances and applies no top-ups (they would use old balances).
  uint32_t resetGen() const { return _resetGen; }

  volatile bool     signedIn = false;
  volatile uint32_t writesOk = 0, writesFail = 0;

private:
  static void taskEntry(void *arg);
  void run();
  bool signIn();
  int  request(const char *method, const String &url, const String &body, String *resp);
  bool patchRoot(const String &body, const char *what);
  void syncLive(const FbSnapshot &s);
  void checkTopUps(const FbSnapshot &s);
  void checkControl(const FbSnapshot &s);
  void writeHistory(const FbSnapshot &s);
  bool loadBalances();
  String dbPath(const String &path);

  bool _enabled = false;
  volatile bool _paused = false;
  String _apiKey, _dbUrl, _email, _password, _fw, _token;
  unsigned long _tokenAt = 0, _lastSignInTry = 0;
  bool _haveSnap = false;
  FbSnapshot _snap;                       // guarded by _mux
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
  volatile bool _inFlight[FB_CHANNELS] = {false, false, false, false};
  volatile bool _loadWanted = false;
  volatile bool _controlKnown = false;
  volatile bool _adminOff[FB_CHANNELS] = {false, false, false, false};
  volatile uint32_t _resetGen = 0;
  QueueHandle_t _topUps = nullptr;        // FbTopUp
  QueueHandle_t _loaded = nullptr;        // FbLoaded, depth 1
  String _lastHistKey;
};

extern FirebaseSync Firebase;
