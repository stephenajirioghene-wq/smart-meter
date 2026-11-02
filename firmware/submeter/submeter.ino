/*
 * submeter.ino
 * ---------------------------------------------------------------------------
 * Integrated sub-metering firmware, base build with the Wi-Fi serial monitor.
 *
 * Thesis: IoT-Based Smart Energy Sub-Metering System with Programmable Unit
 *         Allocation for Multi-Tenant Buildings.
 *
 * What this build does:
 *   - polls 4 PZEM-004T meters every 2 s (same read code and timing as Test 1)
 *   - deducts each tenant's energy from a local balance (Eq. 3.1, 3.2)
 *   - switches each tenant's relay through the MCP23017 (OFF at boot,
 *     OFF at B <= 0, ON while B > 0)
 *   - shows each tenant's balance on a red/green LED (RED at B <= 10% of U)
 *   - mirrors every Serial line to a web page on its own Wi-Fi access point
 *     "SubMeter-Debug", http://192.168.4.1/ (WebLog.h)
 *   - accepts firmware uploads over Wi-Fi (ArduinoOTA)
 *
 *   - syncs with Firebase (FirebaseSync.h): live readings, balances, relay
 *     states and history go up; dashboard top-ups come down and are applied
 *     in step 4 with the spec's multi-path handshake
 *   - keeps balances in flash (NVS) so a reset or power cut does not lose
 *     them, and the tenants keep running with no internet
 *   - landlord commands from the dashboard: disconnect one tenant (relay
 *     held OFF whatever the balance, kept in flash) and reset all units
 *     (every balance and allocation set to 0)
 *
 * Balance source:
 *   - FB_EMAIL / FB_PASSWORD set in secrets.h: Firebase mode. Balances come
 *     from NVS, or from Firebase on the very first boot. Top-ups come from
 *     the dashboard.
 *   - FB_EMAIL empty: bench test mode, as before. Every boot each tenant
 *     starts with TEST_ALLOCATION_KWH and nothing is saved.
 *
 * Firmware cycle (fixed order, every POLL_INTERVAL_MS):
 *   1 Initialize (setup)  2 Poll  3 Deduct  4 Check for new allocation
 *   5 Disconnect on zero / reconnect  6 Sync  7 Repeat
 *
 * Hardware: docs/hardware_setup/HARDWARE_AND_PINS.md
 *   Serial2 TX GPIO17 / RX GPIO16 -> PZEM bus (9600 8N1, addresses 0x01-0x04)
 *   I2C SDA GPIO21 / SCL GPIO22   -> MCP23017 at 0x20
 *   MCP23017 GPB0, GPB1, GPB5, GPB7 -> relay IN1..IN4 (tenant 1..4)
 *   Status LEDs (separate green and red pins, HIGH = lit): see
 *   LED_PIN_GREEN / LED_PIN_RED
 *
 * Serial log: docs/firmware/SERIAL_LOG_FORMAT.md, section 2 (metering log).
 * CSV rows, #EV and #H lines are printed exactly as the contract says.
 * Human messages start with a tag ([PZEM], [DEDUCT], [ALLOC], [RELAY],
 * [LED], [FB], [WIFI], [SYS]) and get a time prefix from WebLog.
 * The "buzzer" column is kept for the analysis; it now shows 1 while the
 * tenant's LED is RED (the low-balance warning that replaced the buzzer).
 *
 * Libraries and IDE settings: README.md in this folder.
 * ---------------------------------------------------------------------------
 */

#include <WiFi.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <PZEM004Tv30.h>
#include <esp_system.h>   // esp_reset_reason()
#include <time.h>
#include <Preferences.h>  // NVS
#include "WebLog.h"
#include "FirebaseSync.h"

// ============================ CONFIG =======================================
// Passwords: put real values in secrets.h (copy secrets_example.h). That file
// is git-ignored and is read first, so anything defined there wins. The
// values below are placeholders only.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#define FW_VERSION        "0.3.0-firebase"   // 0.3.0: landlord disconnect, reset all units
#define DEVICE_NAME       "SubMeter"

// Local access point: the ONLY way to reach the log page.
#ifndef AP_SSID
#define AP_SSID           "SubMeter-Debug"
#endif
#ifndef AP_PASS
#define AP_PASS           "change-me-ap"     // WPA2, at least 8 characters
#endif
#define AP_CHANNEL        6                  // used only while STA is not connected
#define AP_MAX_CLIENTS    3

// Login for the log page (HTTP basic auth on /, /ws and /dump).
#ifndef WEB_USER
#define WEB_USER          "admin"
#endif
#ifndef WEB_PASS
#define WEB_PASS          "change-me-web"
#endif

// Over-the-air updates.
#define OTA_HOSTNAME      "submeter"
#ifndef OTA_PASS
#define OTA_PASS          "change-me-ota"
#endif
// The Arduino IDE finds network ports through mDNS. 1 = ArduinoOTA announces
// itself so "submeter at 192.168.4.1" appears in the port list. 0 = no mDNS
// at all; upload with espota.py / arduino-cli to 192.168.4.1 instead.
#define OTA_MDNS          1

// Building Wi-Fi or phone hotspot, used for Firebase and NTP only.
// Empty SSID = AP only (the STA side never scans, so the AP is steadiest).
#ifndef STA_SSID
#define STA_SSID          "wally"
#endif
#ifndef STA_PASS
#define STA_PASS          "wally"
#endif
#define STA_RETRY_MS      30000UL            // retry a lost STA link every 30 s
#define TZ_OFFSET_S       3600               // WAT = UTC+1, for the log time prefix

// Firebase project (same values as dashboard/firebase-config.js; not secret,
// the database rules protect the data). The device login goes in secrets.h:
// a Firebase Auth user whose users/{uid}/role is "device".
#ifndef FB_API_KEY
#define FB_API_KEY        "AIzaSyBhujmeXC6gYgHddEksb90b9za9YdPY1U0"
#endif
#ifndef FB_DB_URL
#define FB_DB_URL         "https://submeter-44f13-default-rtdb.europe-west1.firebasedatabase.app"
#endif
#ifndef FB_EMAIL
#define FB_EMAIL          ""                 // set in secrets.h; empty = bench test mode, no Firebase
#endif
#ifndef FB_PASSWORD
#define FB_PASSWORD       ""
#endif
#define NVS_SAVE_MS       60000UL            // balances to flash at most once a minute

// Metering (same values as the Test 1 sketch).
#define PZEM_RX_PIN       16                 // ESP32 RX <- PZEM TX
#define PZEM_TX_PIN       17                 // ESP32 TX -> PZEM RX
#define POLL_INTERVAL_MS  2000UL
#define CT_TURNS          1                  // primary turns through each CT
#define MONITOR_BAUD      115200
#define NUM_CHANNELS      4

// Bench test mode only (FB_EMAIL empty): every tenant starts each boot with
// this allocation U_i in kWh. In Firebase mode it is not used.
const float TEST_ALLOCATION_KWH[NUM_CHANNELS] = {0.010, 0.010, 0.010, 0.010};

// Low-balance warning (Eq. 3.3): RED at B <= LOW_FRACTION x U.
#define LOW_FRACTION      0.10f

// Relays.
#define RELAY_ACTIVE_LOW  true               // input LOW = relay energised
#define I2C_SDA_PIN       21
#define I2C_SCL_PIN       22
#define I2C_FREQ_HZ       400000UL
#define I2C_TIMEOUT_MS    20
#define MCP_ADDR          0x20
#define I2C_RETRY_MS      1000UL             // retry a lost expander every 1 s

// Status LEDs. One GPIO per colour, HIGH = that colour lit, common leg to GND
// (same wiring as the relay control bench program). GPIO34/35 are input
// only, so they are not used here.
const uint8_t LED_PIN_GREEN[NUM_CHANNELS] = {32, 23, 33, 25};
const uint8_t LED_PIN_RED[NUM_CHANNELS]   = {26, 27,  2, 15};
#define LED_TEST_AT_BOOT  1    // 1 = each LED GREEN, RED, OFF once after boot (non-blocking)

// What goes in the log.
#define LOG_CSV_ROWS      1    // contract CSV row per channel per poll (analysis)
#define LOG_PZEM_LINES    1    // human [PZEM] line per channel per poll

#define HEAP_CHECK_MS     10000UL
#define HEAP_LOW_BYTES    40960              // below 40 KB: cut the log ring to 4 KB
#define HEALTH_MS         600000UL           // #H record every 10 min

// ===========================================================================


// ---------------------------------------------------------------------------
// MCP23017 (raw registers, same approach as the Test 3 precursor sketch)
// The relays are on port B. Port A is not used and is set to inputs.
// ---------------------------------------------------------------------------
#define REG_IODIRA    0x00
#define REG_IODIRB    0x01
#define REG_GPINTENB  0x05   // IOCON when BANK = 1, see safeInit()
#define REG_IOCON     0x0A
#define REG_OLATB     0x15
#define IOCON_VALUE   0x20   // BANK = 0, SEQOP = 1
#define IODIRA_VALUE  0xFF   // port A all inputs (unused)

// Tenant 1..4 = GPB0, GPB1, GPB5, GPB7 (Adafruit pin numbers 8, 9, 13, 15).
const uint8_t RELAY_BIT[NUM_CHANNELS] = {0, 1, 5, 7};


// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
const uint8_t PZEM_ADDRESS[NUM_CHANNELS] = {0x01, 0x02, 0x03, 0x04};

PZEM004Tv30 pzem[NUM_CHANNELS] = {
  PZEM004Tv30(Serial2, PZEM_RX_PIN, PZEM_TX_PIN, 0x01),
  PZEM004Tv30(Serial2, PZEM_RX_PIN, PZEM_TX_PIN, 0x02),
  PZEM004Tv30(Serial2, PZEM_RX_PIN, PZEM_TX_PIN, 0x03),
  PZEM004Tv30(Serial2, PZEM_RX_PIN, PZEM_TX_PIN, 0x04)
};

// Latest reading per channel.
float Voltage[NUM_CHANNELS], Current[NUM_CHANNELS], Power[NUM_CHANNELS], PF[NUM_CHANNELS];
float Frequency[NUM_CHANNELS];
float Energy[NUM_CHANNELS];              // E_i[k], kWh register
bool  Responded[NUM_CHANNELS];
unsigned long Read_Start[NUM_CHANNELS], Read_Ms[NUM_CHANNELS];

// Accounting.
float Last_Energy[NUM_CHANNELS];         // E_i[k-1]
bool  Baseline_Set[NUM_CHANNELS];        // first valid reading taken
float Delta_E[NUM_CHANNELS];             // deltaE_i[k] of this poll (NAN = no reading)
float Remaining_Balance[NUM_CHANNELS];   // B_i[k], may go below 0
float Original_Allocation[NUM_CHANNELS]; // U_i
bool  Balance_Known[NUM_CHANNELS];       // an allocation has been applied
bool  Admin_Off[NUM_CHANNELS];           // landlord disconnect: relay held OFF
uint32_t Reset_Gen_Applied = 0;          // last Firebase.resetGen() applied here
float Integrated[NUM_CHANNELS];          // sum of P x dt, kWh (cross-check)
unsigned long Last_Valid_Ms[NUM_CHANNELS];
bool  Meter_Up[NUM_CHANNELS];            // for NO_RESPONSE / METER_RECOVERED events
bool  Low_Flag[NUM_CHANNELS];            // LOW_BALANCE already logged
bool  Exhausted_Flag[NUM_CHANNELS];      // EXHAUSTED already logged
const char *Row_State[NUM_CHANNELS];     // state column of this poll

// Outputs.
bool Relay_Status[NUM_CHANNELS];         // true = ON, as accepted by the MCP23017
enum LedColour : uint8_t { LED_OFF, LED_GREEN, LED_RED };
LedColour Led_State[NUM_CHANNELS];       // colour the balance asks for
const char *LED_NAME[] = {"OFF", "GREEN", "RED"};

uint8_t olatShadow = 0;                  // what OLATB holds (written only by outputsWrite)
bool busOk = false;
unsigned long lastI2cRetry = 0;
uint8_t lastWireErr = 0;

// Network.
volatile bool staUp = false;             // set by the Wi-Fi event task
bool staWasUp = false, staEverUp = false, ntpStarted = false, ntpLogged = false;
unsigned long lastStaTry = 0;
volatile bool otaActive = false;

// LED test (command "ledtest").
bool ledTestActive = false;
uint8_t ledTestCh = 0, ledTestStep = 0;
unsigned long ledTestT = 0;

// Timers and counters.
unsigned long lastPoll = 0, lastHeapCheck = 0, lastHealth = 0, rebootAt = 0;
bool rebootPending = false;
unsigned long meterReadsOk = 0, meterReadsFail = 0, wifiReconnects = 0;
const char *bootReason = "";

// Persistence (Firebase mode only).
Preferences prefs;
bool useFirebase = false;
float Saved_Balance[NUM_CHANNELS], Saved_Original[NUM_CHANNELS];
unsigned long lastNvsSave = 0;


// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "RST_POWERON";
    case ESP_RST_EXT:       return "RST_EXT";
    case ESP_RST_SW:        return "RST_SW";
    case ESP_RST_PANIC:     return "RST_PANIC";
    case ESP_RST_INT_WDT:   return "RST_INT_WDT";
    case ESP_RST_TASK_WDT:  return "RST_TASK_WDT";
    case ESP_RST_WDT:       return "RST_WDT";
    case ESP_RST_DEEPSLEEP: return "RST_DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "RST_BROWNOUT";
    case ESP_RST_SDIO:      return "RST_SDIO";
    default:                return "RST_UNKNOWN";
  }
}

// #EV line (SERIAL_LOG_FORMAT.md section 3). detail may be "".
void logEvent(uint8_t channel, const char *event, const char *detail) {
  Log.printf("#EV,%lu,%u,%s,%s\n", millis(), channel, event, detail);
}

float percentOf(float b, float u) {
  return (u > 0) ? 100.0f * b / u : 0.0f;
}


// ---------------------------------------------------------------------------
// Status LEDs (ESP32 GPIOs, one pin per colour)
// ---------------------------------------------------------------------------

// Drive one LED. Never both colours at once.
void ledDrive(uint8_t ch, LedColour c) {
  digitalWrite(LED_PIN_GREEN[ch], c == LED_GREEN ? HIGH : LOW);
  digitalWrite(LED_PIN_RED[ch],   c == LED_RED   ? HIGH : LOW);
}

// Show the real balance colours, unless OTA or the LED test owns the LEDs.
void ledsShowState() {
  if (otaActive || ledTestActive) return;
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) ledDrive(ch, Led_State[ch]);
}

void ledsAllOff() {
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) ledDrive(ch, LED_OFF);
}

// Cycle 5: pick each LED's colour from the balance. [LED] only on a change.
void updateLeds() {
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    LedColour c = LED_OFF;
    if (Balance_Known[ch]) {
      c = (Remaining_Balance[ch] <= LOW_FRACTION * Original_Allocation[ch]) ? LED_RED : LED_GREEN;
    }
    if (c != Led_State[ch]) {
      Log.printf("[LED] ch%u %s->%s bal=%.4f orig=%.4f (%.1f%%)\n", ch + 1,
                 LED_NAME[Led_State[ch]], LED_NAME[c], Remaining_Balance[ch],
                 Original_Allocation[ch], percentOf(Remaining_Balance[ch], Original_Allocation[ch]));
      Led_State[ch] = c;
    }
  }
  ledsShowState();
}

// "ledtest": each LED in turn GREEN 1 s, RED 1 s, OFF 0.3 s, then the real
// colours come back. Stepped from loop(), never blocks.
void ledTestStart() {
  ledTestActive = true;
  ledTestCh = 0;
  ledTestStep = 0;
  ledTestT = millis();
  ledsAllOff();
  ledDrive(0, LED_GREEN);
  Log.println("[LED] test: each LED GREEN 1 s, RED 1 s, OFF");
}

void ledTestUpdate(unsigned long now) {
  if (!ledTestActive || otaActive) return;
  unsigned long stepMs = (ledTestStep == 2) ? 300 : 1000;
  if (now - ledTestT < stepMs) return;
  ledTestT = now;
  ledTestStep++;
  if (ledTestStep == 1) {
    ledDrive(ledTestCh, LED_RED);
  } else if (ledTestStep == 2) {
    ledDrive(ledTestCh, LED_OFF);
  } else {
    ledTestStep = 0;
    ledTestCh++;
    if (ledTestCh >= NUM_CHANNELS) {
      ledTestActive = false;
      ledsShowState();
      Log.println("[LED] test done, real colours restored");
      return;
    }
    ledDrive(ledTestCh, LED_GREEN);
  }
}


// ---------------------------------------------------------------------------
// MCP23017 and relays
// ---------------------------------------------------------------------------
bool mcpWriteReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MCP_ADDR);
  Wire.write(reg);
  Wire.write(value);
  lastWireErr = Wire.endTransmission();
  return lastWireErr == 0;
}

bool mcpReadReg(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(MCP_ADDR);
  Wire.write(reg);
  lastWireErr = Wire.endTransmission(false);
  if (lastWireErr != 0) return false;
  if (Wire.requestFrom((uint16_t)MCP_ADDR, (size_t)1, true) != 1) {
    lastWireErr = 6;
    return false;
  }
  value = Wire.read();
  return true;
}

// The only function that writes OLATB. The shadow changes only when the
// chip ACKs, so it always holds what the chip last accepted.
bool outputsWrite(uint8_t value) {
  if (!mcpWriteReg(REG_OLATB, value)) return false;
  olatShadow = value;
  return true;
}

bool relayBitHigh(bool on) {
  return RELAY_ACTIVE_LOW ? !on : on;
}

// Only the 4 relay pins become outputs. The other port B pins stay inputs,
// so a relay wired to the wrong pin can never be held ON by a LOW output.
uint8_t relayMask() {
  uint8_t m = 0;
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) m |= (1 << RELAY_BIT[ch]);
  return m;
}

uint8_t safeLatch() {                    // all relays OFF, unused bits LOW
  uint8_t v = 0;
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    if (relayBitHigh(false)) v |= (1 << RELAY_BIT[ch]);
  }
  return v;
}

// Safe-init order (do not change, see the precursor sketch for the reasons):
// force BANK = 0, IOCON, OLATB = all OFF, read back, only then IODIRB.
bool safeInit() {
  busOk = false;
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) Relay_Status[ch] = false;
  uint8_t rb = 0;
  if (!mcpWriteReg(REG_GPINTENB, 0x00) || !mcpWriteReg(REG_IOCON, IOCON_VALUE) ||
      !mcpWriteReg(REG_IODIRA, IODIRA_VALUE) ||
      !outputsWrite(safeLatch()) || !mcpReadReg(REG_OLATB, rb) || rb != safeLatch() ||
      !mcpWriteReg(REG_IODIRB, (uint8_t)~relayMask())) {
    Log.printf("[RELAY] ERR expander safe init failed (wire_err %u), retrying every 1 s\n", lastWireErr);
    lastI2cRetry = millis();
    return false;
  }
  busOk = true;
  Log.printf("[RELAY] safe init OK, all relays OFF (OLATB=0x%02X)\n", olatShadow);
  return true;
}

void startI2C() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);
  Wire.setTimeOut(I2C_TIMEOUT_MS);
}

// Bus down: retry the expander every I2C_RETRY_MS (called from loop()).
// While it is down the relays keep whatever the expander last latched.
void i2cRecover(unsigned long now) {
  if (busOk || now - lastI2cRetry < I2C_RETRY_MS) return;
  lastI2cRetry = now;
  Wire.end();
  startI2C();
  Wire.beginTransmission(MCP_ADDR);
  if (Wire.endTransmission() == 0) {
    Log.println("[RELAY] expander answering again");
    safeInit();                          // relays OFF; cycle 5 turns them back on
  }
}

// Switch one relay, read OLATB back. Logs [RELAY] and #EV RELAY_ON/OFF.
bool commandRelay(uint8_t ch, bool on) {
  if (!busOk) return false;
  uint8_t next = olatShadow;
  uint8_t bit = 1 << RELAY_BIT[ch];
  if (relayBitHigh(on)) next |= bit;
  else                  next &= ~bit;

  uint8_t rb = 0;
  if (!outputsWrite(next) || !mcpReadReg(REG_OLATB, rb)) {
    busOk = false;
    lastI2cRetry = millis();
    Log.printf("[RELAY] ERR ch%u write FAIL (wire_err %u), expander down\n", ch + 1, lastWireErr);
    return false;
  }
  bool ok = (rb == next);
  Relay_Status[ch] = on;
  char d[8];
  snprintf(d, sizeof d, "ok=%d", ok ? 1 : 0);
  logEvent(ch + 1, on ? "RELAY_ON" : "RELAY_OFF", d);
  Log.printf("[RELAY] ch%u %s bal=%.4f%s\n", ch + 1, on ? "ON" : "OFF",
             Remaining_Balance[ch], ok ? "" : " readback FAIL");
  return ok;
}


// ---------------------------------------------------------------------------
// Cycle 2: poll (same read logic as Test 1)
// ---------------------------------------------------------------------------

// Voltage first: the library reads all registers in one Modbus transaction
// and caches them, so the other getters cost nothing. On NAN the module did
// not answer and the other getters are skipped (each would time out again).
void readChannel(uint8_t ch) {
  Read_Start[ch] = millis();
  Voltage[ch] = pzem[ch].voltage();
  if (isnan(Voltage[ch])) {
    Responded[ch] = false;
    meterReadsFail++;
  } else {
    Responded[ch] = true;
    meterReadsOk++;
    Current[ch] = pzem[ch].current() / CT_TURNS;
    Power[ch]   = pzem[ch].power() / CT_TURNS;
    Energy[ch]  = pzem[ch].energy();
    PF[ch]      = pzem[ch].pf();
    Frequency[ch] = pzem[ch].frequency();
  }
  Read_Ms[ch] = millis() - Read_Start[ch];

#if LOG_PZEM_LINES
  if (Responded[ch]) {
    Log.printf("[PZEM] ch%u V=%.1f I=%.3f P=%.1f E=%.4f PF=%.2f\n", ch + 1,
               Voltage[ch], Current[ch], Power[ch], Energy[ch], PF[ch]);
  } else {
    Log.printf("[PZEM] ch%u NO_RESPONSE (%lu ms)\n", ch + 1, Read_Ms[ch]);
  }
#endif
}


// ---------------------------------------------------------------------------
// Cycle 3: deduct (Eq. 3.1, 3.2)
// ---------------------------------------------------------------------------
void deduct(uint8_t ch) {
  char d[48];
  Row_State[ch] = nullptr;               // decided in cycle 5 unless set here
  Delta_E[ch] = NAN;

  if (!Responded[ch]) {
    // No reading: skip this poll. Energy is cumulative, so the next valid
    // read catches up.
    Row_State[ch] = "NO_RESPONSE";
    if (Meter_Up[ch]) {
      Meter_Up[ch] = false;
      logEvent(ch + 1, "NO_RESPONSE", "");
    }
    return;
  }
  if (!Meter_Up[ch]) {
    Meter_Up[ch] = true;
    if (Baseline_Set[ch]) logEvent(ch + 1, "METER_RECOVERED", "");
  }

  if (!Baseline_Set[ch]) {
    // First valid reading is the baseline. Registers are never reset.
    Baseline_Set[ch] = true;
    Last_Energy[ch] = Energy[ch];
    Last_Valid_Ms[ch] = Read_Start[ch];
    Delta_E[ch] = 0;
    snprintf(d, sizeof d, "e=%.4f", Energy[ch]);
    logEvent(ch + 1, "BASELINE", d);
    return;
  }

  float dE = Energy[ch] - Last_Energy[ch];
  if (dE < 0) {
    // Register went down (module reset): clamp so the tenant is not credited.
    snprintf(d, sizeof d, "reg=%.4f", Energy[ch]);
    logEvent(ch + 1, "COUNTER_RESET", d);
    Row_State[ch] = "COUNTER_RESET";
    dE = 0;
  }
  Last_Energy[ch] = Energy[ch];
  Delta_E[ch] = dE;
  Remaining_Balance[ch] -= dE;
  Integrated[ch] += Power[ch] * (Read_Start[ch] - Last_Valid_Ms[ch]) / 3.6e9f;
  Last_Valid_Ms[ch] = Read_Start[ch];

  if (dE > 0) {
    Log.printf("[DEDUCT] ch%u dE=%.4f bal=%.4f kWh (%.1f%%)\n", ch + 1, dE,
               Remaining_Balance[ch], percentOf(Remaining_Balance[ch], Original_Allocation[ch]));
  }
}


// ---------------------------------------------------------------------------
// Cycle 4: check for new allocation
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// NVS: balances in flash, so a reset does not lose them (Firebase mode only).
// The ESP32 is the only writer of balances, so flash is never older than
// Firebase except for at most NVS_SAVE_MS of unsaved deduction.
// ---------------------------------------------------------------------------
void nvsSave(bool force) {
  bool changed = false;
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    if (!Balance_Known[ch]) return;      // save only a complete set
    if (fabsf(Remaining_Balance[ch] - Saved_Balance[ch]) >= 0.0001f ||
        fabsf(Original_Allocation[ch] - Saved_Original[ch]) >= 0.0001f) changed = true;
  }
  if (!changed && !force) return;
  prefs.putBytes("bal", Remaining_Balance, sizeof Remaining_Balance);
  prefs.putBytes("orig", Original_Allocation, sizeof Original_Allocation);
  memcpy(Saved_Balance, Remaining_Balance, sizeof Saved_Balance);
  memcpy(Saved_Original, Original_Allocation, sizeof Saved_Original);
  Log.printf("#EV,%lu,0,NVS_SAVE,b1=%.4f;b2=%.4f;b3=%.4f;b4=%.4f\n", millis(),
             Remaining_Balance[0], Remaining_Balance[1], Remaining_Balance[2], Remaining_Balance[3]);
}

// Landlord disconnect flags, saved on every change so a reboot with no
// internet keeps a disconnected tenant OFF.
void nvsSaveAdminOff() {
  prefs.putBytes("off", Admin_Off, sizeof Admin_Off);
}

void nvsRestoreAdminOff() {
  if (prefs.getBytesLength("off") != sizeof Admin_Off) return;
  prefs.getBytes("off", Admin_Off, sizeof Admin_Off);
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    if (Admin_Off[ch]) Log.printf("[ALLOC] ch%u landlord disconnect restored from flash\n", ch + 1);
  }
}

bool nvsRestore() {
  if (prefs.getBytesLength("bal") != sizeof Remaining_Balance ||
      prefs.getBytesLength("orig") != sizeof Original_Allocation) return false;
  prefs.getBytes("bal", Remaining_Balance, sizeof Remaining_Balance);
  prefs.getBytes("orig", Original_Allocation, sizeof Original_Allocation);
  memcpy(Saved_Balance, Remaining_Balance, sizeof Saved_Balance);
  memcpy(Saved_Original, Original_Allocation, sizeof Saved_Original);
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) Balance_Known[ch] = true;
  Log.printf("#EV,%lu,0,NVS_RESTORE,b1=%.4f;b2=%.4f;b3=%.4f;b4=%.4f\n", millis(),
             Remaining_Balance[0], Remaining_Balance[1], Remaining_Balance[2], Remaining_Balance[3]);
  return true;
}

// Firebase mode: first-boot balances, then queued top-ups. A top-up is
// already written to Firebase (multi-path update succeeded) when it gets
// here; only now is it added locally.
void applyFirebaseAllocations() {
  FbLoaded l;
  if (Firebase.takeLoaded(l)) {
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
      if (Balance_Known[ch]) continue;
      Remaining_Balance[ch] = l.balance[ch];
      Original_Allocation[ch] = l.original[ch];
      Balance_Known[ch] = true;
      Log.printf("[ALLOC] ch%u balance %.4f of %.4f kWh (from Firebase, first boot)\n",
                 ch + 1, Remaining_Balance[ch], Original_Allocation[ch]);
    }
    nvsSave(true);
  }

  FbTopUp t;
  while (Firebase.takeTopUp(t)) {
    uint8_t ch = t.ch;
    float before = Remaining_Balance[ch];
    Remaining_Balance[ch] += t.kwh;
    Original_Allocation[ch] = Remaining_Balance[ch];   // spec: U = balance right after a top-up
    char d[96];
    snprintf(d, sizeof d, "id=%s;kwh=%.4f;before=%.4f;after=%.4f;source=firebase",
             t.id, t.kwh, before, Remaining_Balance[ch]);
    logEvent(ch + 1, "TOPUP_APPLIED", d);
    Log.printf("[ALLOC] ch%u top-up %.4f kWh, balance %.4f -> %.4f\n", ch + 1, t.kwh,
               before, Remaining_Balance[ch]);
    Firebase.topUpDone(ch);
    nvsSave(true);                       // a paid top-up goes to flash at once
  }

  // Reset all units: Firebase already holds 0 for every channel.
  uint32_t gen = Firebase.resetGen();
  if (gen != Reset_Gen_Applied) {
    Reset_Gen_Applied = gen;
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
      char d[48];
      snprintf(d, sizeof d, "before=%.4f;orig=%.4f", Remaining_Balance[ch], Original_Allocation[ch]);
      logEvent(ch + 1, "UNITS_RESET", d);
      Remaining_Balance[ch] = 0;
      Original_Allocation[ch] = 0;
    }
    Log.println("[ALLOC] all units reset to 0 by the landlord");
    nvsSave(true);
  }

  // Landlord disconnect flags (cycle 5 uses them).
  if (Firebase.controlKnown()) {
    bool changed = false;
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
      bool off = Firebase.adminOff(ch);
      if (off == Admin_Off[ch]) continue;
      Admin_Off[ch] = off;
      changed = true;
      logEvent(ch + 1, off ? "ADMIN_OFF" : "ADMIN_ON", "");
      Log.printf("[ALLOC] ch%u landlord %s\n", ch + 1, off ? "disconnect" : "reconnect");
    }
    if (changed) nvsSaveAdminOff();
  }
}

void checkNewAllocation() {
  if (useFirebase) {
    applyFirebaseAllocations();
    return;
  }
  // Bench test mode: apply the local test allocation on the first cycle.
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    if (Balance_Known[ch]) continue;
    Original_Allocation[ch] = TEST_ALLOCATION_KWH[ch];
    Remaining_Balance[ch]   = TEST_ALLOCATION_KWH[ch];
    Balance_Known[ch] = true;
    Log.printf("[ALLOC] ch%u allocation %.4f kWh (local test value, bench test mode)\n",
               ch + 1, Original_Allocation[ch]);
  }
}


// ---------------------------------------------------------------------------
// Cycle 5: thresholds, relays, LEDs
// ---------------------------------------------------------------------------
void updateOutputs() {
  char d[48];
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    if (!Balance_Known[ch]) continue;
    float b = Remaining_Balance[ch], u = Original_Allocation[ch];
    float p = Responded[ch] ? Power[ch] : 0;

    // Eq. 3.3: low balance, logged once per crossing.
    if (b <= LOW_FRACTION * u && b > 0 && !Low_Flag[ch]) {
      Low_Flag[ch] = true;
      if (!Row_State[ch]) Row_State[ch] = "LOW_BALANCE";
      snprintf(d, sizeof d, "b=%.4f;p=%.1f", b, p);
      logEvent(ch + 1, "LOW_BALANCE", d);
    }
    // Eq. 3.4: exhausted, logged once per crossing.
    if (b <= 0 && !Exhausted_Flag[ch]) {
      Exhausted_Flag[ch] = true;
      Row_State[ch] = "EXHAUSTED";
      snprintf(d, sizeof d, "b=%.4f;p=%.1f", b, p);
      logEvent(ch + 1, "EXHAUSTED", d);
    }
    if (b > LOW_FRACTION * u) Low_Flag[ch] = false;      // re-armed by a top-up
    if (b > 0)                Exhausted_Flag[ch] = false;

    // Relay: OFF at B <= 0, ON while B > 0, and OFF while the landlord has
    // disconnected the tenant. Only that tenant is switched.
    bool want = (b > 0) && !Admin_Off[ch];
    if (want != Relay_Status[ch]) commandRelay(ch, want);

    if (!Row_State[ch]) Row_State[ch] = (b <= 0) ? "EXHAUSTED" : "OK";
  }
  updateLeds();
}


// ---------------------------------------------------------------------------
// Cycle 6: sync
// ---------------------------------------------------------------------------

// Hands the latest values to the Firebase task, which sends them every
// FB_SYNC_MS when the internet is up. Never blocks. Also saves balances to
// flash once a minute.
void syncToFirebase() {
  if (!useFirebase || otaActive) return;
  FbSnapshot s;
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    FbChannelSnap &c = s.ch[ch];
    c.valid     = Responded[ch];
    c.voltage   = Voltage[ch];
    c.current   = Current[ch];
    c.power     = Power[ch];
    c.pf        = PF[ch];
    c.energy    = Energy[ch];
    c.frequency = Frequency[ch];
    c.known     = Balance_Known[ch];
    c.balance   = Remaining_Balance[ch];
    c.original  = Original_Allocation[ch];
    c.relayOn   = Relay_Status[ch];
  }
  s.resetGen = Reset_Gen_Applied;
  Firebase.publish(s);

  if (millis() - lastNvsSave >= NVS_SAVE_MS) {
    lastNvsSave = millis();
    nvsSave(false);
  }
}

// Firebase mode needs a device login and the building Wi-Fi. Balances come
// from flash; on the very first boot (flash empty) from Firebase. Until then
// the relays stay OFF.
void firebaseBegin() {
  if (!STA_SSID[0]) {
    Log.println("[FB] no building Wi-Fi (STA_SSID empty), bench test mode");
    return;
  }
  useFirebase = Firebase.begin(FB_API_KEY, FB_DB_URL, FB_EMAIL, FB_PASSWORD, FW_VERSION);
  if (!useFirebase) return;
  prefs.begin("submeter", false);
  lastNvsSave = millis();
  nvsRestoreAdminOff();
  if (!nvsRestore()) {
    Log.println("[ALLOC] no balances in flash, reading them from Firebase (relays OFF until then)");
    Firebase.requestLoad();
  }
}


// ---------------------------------------------------------------------------
// CSV row (SERIAL_LOG_FORMAT.md section 2). One printf per row.
// ---------------------------------------------------------------------------
void printRow(uint8_t ch, unsigned long cycleMs) {
  char meas[96] = ",,,,,";                // 6 empty fields on NO_RESPONSE
  if (Responded[ch]) {
    snprintf(meas, sizeof meas, "%.1f,%.3f,%.1f,%.2f,%.4f,%.4f",
             Voltage[ch], Current[ch], Power[ch], PF[ch], Energy[ch],
             isnan(Delta_E[ch]) ? 0.0f : Delta_E[ch]);
  }
  Log.printf("%lu,0,%u,0x%02X,%s,%.4f,%.4f,%d,%s,%d,%lu,%lu\n",
             Read_Start[ch], ch + 1, PZEM_ADDRESS[ch], meas,
             Remaining_Balance[ch], Integrated[ch], Relay_Status[ch] ? 1 : 0,
             Row_State[ch] ? Row_State[ch] : "OK",
             Led_State[ch] == LED_RED ? 1 : 0, Read_Ms[ch], cycleMs);
}


// ---------------------------------------------------------------------------
// The cycle
// ---------------------------------------------------------------------------
void runCycle() {
  unsigned long cycleStart = millis();
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) readChannel(ch);   // 2 poll
  unsigned long cycleMs = millis() - cycleStart;

  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) deduct(ch);        // 3 deduct
  checkNewAllocation();                                            // 4 allocation
  updateOutputs();                                                 // 5 relays + LEDs

#if LOG_CSV_ROWS
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) printRow(ch, cycleMs);
#endif

  syncToFirebase();                                                // 6 sync
}


// ---------------------------------------------------------------------------
// Wi-Fi: AP always on, STA for Firebase only
// ---------------------------------------------------------------------------

// Runs in the Wi-Fi event task. Log is thread-safe; one printf per line.
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
      const uint8_t *m = info.wifi_ap_staconnected.mac;
      Log.printf("[WIFI] AP client joined %02X:%02X:%02X:%02X:%02X:%02X\n",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: {
      const uint8_t *m = info.wifi_ap_stadisconnected.mac;
      Log.printf("[WIFI] AP client left %02X:%02X:%02X:%02X:%02X:%02X\n",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
      break;
    }
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      staUp = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      staUp = false;
      break;
    default:
      break;
  }
}

// Note: in WIFI_AP_STA mode the radio has one channel. Once STA connects,
// the AP moves to the router's channel, so phones on "SubMeter-Debug" may
// drop for a few seconds when STA (re)connects on a different channel. They
// rejoin by themselves and the page reconnects every 3 s. While STA is down
// it scans all channels on each retry, which also disturbs the AP briefly;
// that is why retries are 30 s apart and auto-reconnect is off.
void startNetwork() {
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);                  // steadier AP for the log page
  WiFi.setAutoReconnect(false);          // retries are paced by handleWifi()

  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  if (WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL, 0, AP_MAX_CLIENTS)) {
    Log.printf("[WIFI] AP \"%s\" up at %s, max %u clients\n", AP_SSID,
               WiFi.softAPIP().toString().c_str(), AP_MAX_CLIENTS);
  } else {
    Log.println("[WIFI] ERR AP failed to start (password under 8 characters?)");
  }

  if (STA_SSID[0]) {
    Log.printf("[WIFI] STA connecting to \"%s\"\n", STA_SSID);
    WiFi.begin(STA_SSID, STA_PASS);      // returns at once
    lastStaTry = millis();
  } else {
    Log.println("[WIFI] STA not configured, AP only");
  }
}

// STA link changes and paced retries. Never blocks.
void handleWifi(unsigned long now) {
  if (!STA_SSID[0]) return;
  bool up = staUp;
  if (up != staWasUp) {
    staWasUp = up;
    if (up) {
      if (staEverUp) wifiReconnects++;
      staEverUp = true;
      Log.printf("[WIFI] STA up, ip %s, RSSI %d dBm, channel %d\n",
                 WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), (int)WiFi.channel());
      logEvent(0, "WIFI_UP", "");
      if (!ntpStarted) {
        ntpStarted = true;
        configTime(TZ_OFFSET_S, 0, "pool.ntp.org", "time.google.com");   // runs in the background
      }
    } else {
      Log.println("[WIFI] STA lost, retrying");
      logEvent(0, "WIFI_DOWN", "");
      lastStaTry = now;
    }
  }
  if (!up && now - lastStaTry >= STA_RETRY_MS) {
    lastStaTry = now;
    Log.println("[WIFI] STA lost, retrying");
    WiFi.disconnect(false, false);       // keep the AP running
    WiFi.begin(STA_SSID, STA_PASS);
  }
}


// ---------------------------------------------------------------------------
// OTA
// ---------------------------------------------------------------------------

// During an upload ArduinoOTA.handle() holds loop() until the transfer ends,
// so no poll, relay or Firebase work happens. The MCP23017 keeps its latch,
// so tenants stay as they are. After the upload the ESP32 reboots and the
// normal boot rule applies (relays OFF, then ON in the first cycle while
// the balance is above zero).
void setupOta() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.setMdnsEnabled(OTA_MDNS);
  ArduinoOTA.onStart([]() {
    otaActive = true;
    Firebase.setPaused(true);
    ledTestActive = false;
    ledsAllOff();
    Log.println("[SYS] OTA start (relays held, LEDs off, Firebase paused)");
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    static unsigned int lastPct = 0;
    unsigned int pct = total ? done * 100 / total : 0;
    if (pct >= lastPct + 10) {
      lastPct = pct;
      Log.printf("[SYS] OTA %u%%\n", pct);
    }
  });
  ArduinoOTA.onEnd([]() {
    Log.println("[SYS] OTA done, rebooting");
  });
  ArduinoOTA.onError([](ota_error_t err) {
    Log.printf("[SYS] OTA FAIL error %u, back to normal\n", (unsigned)err);
    otaActive = false;
    Firebase.setPaused(false);
    ledsShowState();                     // restore the LED colours
  });
  ArduinoOTA.begin();
  Log.printf("[SYS] OTA ready, hostname \"%s\"%s\n", OTA_HOSTNAME,
             OTA_MDNS ? " (mDNS on for the IDE port list)" : " (no mDNS)");
}


// ---------------------------------------------------------------------------
// Web page hooks
// ---------------------------------------------------------------------------

// Per-channel part of the 5 s status JSON: [{"led":"G","relay":1},...]
void statusJson(char *buf, size_t len) {
  size_t k = snprintf(buf, len, "[");
  for (uint8_t ch = 0; ch < NUM_CHANNELS && k < len; ch++) {
    char c = Led_State[ch] == LED_GREEN ? 'G' : (Led_State[ch] == LED_RED ? 'R' : 'O');
    k += snprintf(buf + k, len - k, "%s{\"led\":\"%c\",\"relay\":%d}",
                  ch ? "," : "", c, Relay_Status[ch] ? 1 : 0);
  }
  if (k < len) snprintf(buf + k, len - k, "]");
}

#if WEBLOG_COMMANDS
// Whitelist only. No relay or balance command exists here on purpose:
// allocations stay on Firebase and the dashboard.
void runCommand(const char *cmd) {
  if (!strcmp(cmd, "status")) {
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
      Log.printf("[SYS] ch%u bal=%.4f orig=%.4f (%.1f%%) LED=%s relay=%s%s meter=%s\n", ch + 1,
                 Remaining_Balance[ch], Original_Allocation[ch],
                 percentOf(Remaining_Balance[ch], Original_Allocation[ch]),
                 LED_NAME[Led_State[ch]], Relay_Status[ch] ? "ON" : "OFF",
                 Admin_Off[ch] ? " (landlord)" : "", Responded[ch] ? "OK" : "NO_RESPONSE");
    }
  } else if (!strcmp(cmd, "heap")) {
    Log.printf("[SYS] heap free=%u min=%u largest=%u log_ring=%u\n",
               (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
               (unsigned)ESP.getMaxAllocHeap(), (unsigned)WebLogCtl.capacity());
  } else if (!strcmp(cmd, "wifi")) {
    Log.printf("[WIFI] AP \"%s\" %s clients=%u channel=%d\n", AP_SSID,
               WiFi.softAPIP().toString().c_str(), WiFi.softAPgetStationNum(), (int)WiFi.channel());
    if (staUp) {
      Log.printf("[WIFI] STA \"%s\" ip %s RSSI %d dBm reconnects=%lu\n", STA_SSID,
                 WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), wifiReconnects);
    } else {
      Log.printf("[WIFI] STA down (%s)\n", STA_SSID[0] ? "retrying" : "not configured");
    }
  } else if (!strcmp(cmd, "ledtest")) {
    ledTestStart();
  } else if (!strcmp(cmd, "reboot")) {
    Log.println("[SYS] reboot in 1 s");
    rebootPending = true;
    rebootAt = millis() + 1000;          // let the line reach the page first
  } else {
    Log.println("[SYS] unknown command");
  }
}
#endif


// ---------------------------------------------------------------------------
// Housekeeping
// ---------------------------------------------------------------------------
void logHeap(const char *when) {
  Log.printf("[SYS] free heap %s: %u B (min %u B, largest block %u B)\n", when,
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
             (unsigned)ESP.getMaxAllocHeap());
}

void housekeeping(unsigned long now) {
  if (now - lastHeapCheck >= HEAP_CHECK_MS) {
    lastHeapCheck = now;
    if (ESP.getFreeHeap() < HEAP_LOW_BYTES && WebLogCtl.shrink()) {
      Log.printf("[SYS] WARNING free heap %u B < %u B: log ring cut to 4 KB\n",
                 (unsigned)ESP.getFreeHeap(), HEAP_LOW_BYTES);
    }
    if (ntpStarted && !ntpLogged && time(nullptr) > 1700000000) {
      ntpLogged = true;
      Log.println("[SYS] NTP time set, log prefix is now wall clock");
    }
  }
  if (now - lastHealth >= HEALTH_MS) {
    lastHealth = now;
    Log.printf("#H,%lu,%lu,%u,%u,%lu,%lu,%lu,%lu,%lu,%s\n", now, now / 1000,
               (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), wifiReconnects,
               meterReadsOk, meterReadsFail, (unsigned long)Firebase.writesOk,
               (unsigned long)Firebase.writesFail, bootReason);
  }
  if (rebootPending && (long)(now - rebootAt) >= 0) {
    ESP.restart();
  }
}


// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
  // LEDs off and relays OFF before anything else.
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    pinMode(LED_PIN_GREEN[ch], OUTPUT);
    pinMode(LED_PIN_RED[ch], OUTPUT);
    ledDrive(ch, LED_OFF);
    Led_State[ch] = LED_OFF;
    Meter_Up[ch] = true;                 // so a dead meter logs NO_RESPONSE once
  }

  Serial.setTxBufferSize(1024);          // fewer stalls when lines come in bursts
  Serial.begin(MONITOR_BAUD);
  WebLogCtl.begin(true);                 // Log works from here on
  bootReason = resetReasonName(esp_reset_reason());

  startI2C();
  safeInit();                            // relays OFF as early as possible

  // Boot banner, header, BOOT event (SERIAL_LOG_FORMAT.md sections 2, 3, 5).
  Log.println();
  Log.println("# submeter.ino (integrated firmware, base build with Wi-Fi log)");
  Log.printf("# fw=%s poll_ms=%lu CT_TURNS=%d test_allocation_kWh=%.4f,%.4f,%.4f,%.4f\n",
             FW_VERSION, POLL_INTERVAL_MS, CT_TURNS, TEST_ALLOCATION_KWH[0],
             TEST_ALLOCATION_KWH[1], TEST_ALLOCATION_KWH[2], TEST_ALLOCATION_KWH[3]);
  Log.println("millis,run,channel,address,voltage_V,current_A,power_W,pf,energy_kWh,deltaE_kWh,"
              "remaining_kWh,integrated_kWh,relay_status,state,buzzer,read_ms,cycle_ms");
  char d[64];
  snprintf(d, sizeof d, "reason=%s;fw=%s", bootReason, FW_VERSION);
  logEvent(0, "BOOT", d);
  logHeap("at boot");

  startNetwork();
  WebLogCtl.onStatus(statusJson);
#if WEBLOG_COMMANDS
  WebLogCtl.onCommand(runCommand);
#endif
  WebLogCtl.beginServer(DEVICE_NAME, WEB_USER, WEB_PASS);
  setupOta();
  logHeap("after network start");

  firebaseBegin();                       // Firebase task + NVS, or bench test mode
  logHeap("after Firebase start");
  if (ESP.getFreeHeap() < HEAP_LOW_BYTES && WebLogCtl.shrink()) {
    Log.println("[SYS] WARNING free heap under 40 KB: log ring cut to 4 KB");
  }

  // First poll one full interval after boot, NOT straight away. The PZEM
  // library skips the bus and returns its cached values (all 0) when called
  // in the first 200 ms after start-up. A poll at that moment would take
  // E = 0 as the baseline, and the first real reading would then deduct the
  // meter's whole register from the tenant.
  lastPoll = millis();
  lastHealth = millis();

#if LED_TEST_AT_BOOT
  ledTestStart();                        // lamp check; stepped from loop(), relays unaffected
#endif
}

void loop() {
  ArduinoOTA.handle();                   // holds loop() only while an upload runs
  if (otaActive) return;

  unsigned long now = millis();
  WebLogCtl.loop();                      // push log lines, status, commands
  handleWifi(now);
  i2cRecover(now);
  ledTestUpdate(now);

  if (now - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = now;
    runCycle();
  }

  housekeeping(now);
}
