/*
 * sensor_read.ino
 * ---------------------------------------------------------------------------
 * Bench sketch: switch the tenant relays ON and read all 4 PZEM-004T meters
 * every 2 s. Results go to the Serial Monitor at 115200 baud.
 *
 * Thesis: IoT-Based Smart Energy Sub-Metering System with Programmable Unit
 *         Allocation for Multi-Tenant Buildings.
 *
 * No Wi-Fi, no web page, no Firebase, no balances. Wi-Fi stays off, which
 * also keeps the ESP32's supply current low.
 *
 * Hardware (docs/hardware_setup/HARDWARE_AND_PINS.md):
 *   Serial2 TX GPIO17 / RX GPIO16 -> all PZEM RX / TX (9600 8N1, 0x01-0x04)
 *   I2C SDA GPIO21 / SCL GPIO22   -> MCP23017 at 0x20
 *   MCP23017 GPB0, GPB1, GPB5, GPB7 -> relay IN1..IN4 (tenant 1..4)
 *   Relay inputs are active-LOW: a LOW input energises the relay and
 *   connects the tenant's load.
 *
 * Start-up order: relays forced OFF (safe init), then the relays in
 * RELAYS_AT_START are switched ON, then polling starts 2 s after boot.
 *
 * CSV output, same columns as the Test 1 sketch (pzem_multi_response), so
 * the log can go through tests/analysis:
 *   millis,channel,address,voltage_V,current_A,power_W,energy_kWh,
 *   frequency_Hz,pf,read_ms,cycle_ms,fail_count,status
 * Lines starting with '#' are comments (relay changes, summaries).
 *
 * Serial commands (type in the Serial Monitor, Enter to send):
 *   all on | all off     all 4 relays
 *   r<N> on | r<N> off   one relay, e.g. r2 off
 *   status               relay states and expander state
 * ---------------------------------------------------------------------------
 */

#include <Wire.h>
#include <PZEM004Tv30.h>

// ============================ USER SETTINGS ================================

// Relays switched ON at start-up. Bit 0 = tenant 1 ... bit 3 = tenant 4.
// 0x0F = all 4 tenants connected. 0x00 = all disconnected.
#define RELAYS_AT_START   0x0F

// Relay module input polarity. true = input LOW energises the relay.
#define RELAY_ACTIVE_LOW  true

// PZEM bus (Serial2).
#define PZEM_RX_PIN       16      // ESP32 RX <- PZEM TX
#define PZEM_TX_PIN       17      // ESP32 TX -> PZEM RX

// MCP23017.
#define I2C_SDA_PIN       21
#define I2C_SCL_PIN       22
#define I2C_FREQ_HZ       100000UL   // 100 kHz: more forgiving of weak pull-ups
#define I2C_TIMEOUT_MS    20
#define MCP_ADDR          0x20

// Timing.
#define POLL_INTERVAL_MS  2000UL     // one full poll of 4 meters every 2 s
#define I2C_RETRY_MS      1000UL     // retry a missing expander every 1 s
#define SUMMARY_EVERY     30         // '#' summary every 30 polls (1 min)

#define MONITOR_BAUD      115200
#define NUM_CHANNELS      4

// Status LED pins, only so the LEDs are held OFF (no floating pins).
const uint8_t LED_PIN_GREEN[NUM_CHANNELS] = {32, 23, 33, 25};
const uint8_t LED_PIN_RED[NUM_CHANNELS]   = {26, 27,  2, 15};

// ===========================================================================


// MCP23017 registers (IOCON.BANK = 0). Relays on port B, port A unused.
#define REG_IODIRA    0x00
#define REG_IODIRB    0x01
#define REG_GPINTENB  0x05   // IOCON when BANK = 1, see safeInit()
#define REG_IOCON     0x0A
#define REG_OLATB     0x15
#define IOCON_VALUE   0x20   // BANK = 0, SEQOP = 1
#define IODIRA_VALUE  0xFF   // port A all inputs (unused)
#define IODIRB_VALUE  0x5C   // GPB0, GPB1, GPB5, GPB7 outputs; the rest inputs

const uint8_t RELAY_BIT[NUM_CHANNELS] = {0, 1, 5, 7};   // tenant 1..4 = GPB0, GPB1, GPB5, GPB7
const uint8_t PZEM_ADDRESS[NUM_CHANNELS] = {0x01, 0x02, 0x03, 0x04};

PZEM004Tv30 pzem[NUM_CHANNELS] = {
  PZEM004Tv30(Serial2, PZEM_RX_PIN, PZEM_TX_PIN, 0x01),
  PZEM004Tv30(Serial2, PZEM_RX_PIN, PZEM_TX_PIN, 0x02),
  PZEM004Tv30(Serial2, PZEM_RX_PIN, PZEM_TX_PIN, 0x03),
  PZEM004Tv30(Serial2, PZEM_RX_PIN, PZEM_TX_PIN, 0x04)
};

// Readings.
float Voltage[NUM_CHANNELS], Current[NUM_CHANNELS], Power[NUM_CHANNELS];
float Energy[NUM_CHANNELS], Frequency[NUM_CHANNELS], PF[NUM_CHANNELS];
bool  Responded[NUM_CHANNELS];
unsigned long Read_Start[NUM_CHANNELS], Read_Ms[NUM_CHANNELS];
unsigned long Poll_Count[NUM_CHANNELS], Fail_Count[NUM_CHANNELS];

// Relays.
bool Relay_Status[NUM_CHANNELS];     // true = ON (load connected), as the chip accepted
uint8_t relayWanted = RELAYS_AT_START;
uint8_t olatShadow = 0;              // what OLATB holds; written only by outputsWrite()
bool busOk = false;
uint8_t lastWireErr = 0;

unsigned long lastPoll = 0, lastI2cRetry = 0, totalCycles = 0;

char lineBuf[32];
uint8_t lineLen = 0;


// ---------------------------------------------------------------------------
// MCP23017 and relays (same safe-init order as the Test 3 precursor sketch)
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

// The only function that writes OLATB. The shadow changes only on an ACK.
bool outputsWrite(uint8_t value) {
  if (!mcpWriteReg(REG_OLATB, value)) return false;
  olatShadow = value;
  return true;
}

// Logical ON (load connected) -> latch level. Active-LOW: ON = LOW.
bool relayBitHigh(bool on) {
  return RELAY_ACTIVE_LOW ? !on : on;
}

// Latch byte for a set of relays: bit ch of mask = tenant ch+1 ON.
uint8_t latchFor(uint8_t mask) {
  uint8_t v = 0;                     // unused bits LOW
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    if (relayBitHigh(mask & (1 << ch))) v |= (1 << RELAY_BIT[ch]);
  }
  return v;
}

// Force BANK = 0, IOCON, OLATB = all OFF, read back, only then IODIRB
// (pins become outputs already holding OFF, so no relay pulses at boot).
bool safeInit() {
  busOk = false;
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) Relay_Status[ch] = false;
  uint8_t rb = 0;
  if (!mcpWriteReg(REG_GPINTENB, 0x00) || !mcpWriteReg(REG_IOCON, IOCON_VALUE) ||
      !mcpWriteReg(REG_IODIRA, IODIRA_VALUE) ||
      !outputsWrite(latchFor(0)) || !mcpReadReg(REG_OLATB, rb) || rb != latchFor(0) ||
      !mcpWriteReg(REG_IODIRB, IODIRB_VALUE)) {
    Serial.printf("# RELAY expander not answering at 0x%02X (wire_err %u). Meters are still read; retrying every 1 s.\n",
                  MCP_ADDR, lastWireErr);
    return false;
  }
  busOk = true;
  Serial.println("# RELAY safe init OK, all relays OFF");
  return true;
}

// Write the wanted relay states in one latch write and read them back.
void applyRelays() {
  if (!busOk) return;
  uint8_t next = latchFor(relayWanted);
  uint8_t rb = 0;
  if (!outputsWrite(next) || !mcpReadReg(REG_OLATB, rb)) {
    busOk = false;
    lastI2cRetry = millis();
    Serial.printf("# RELAY write FAIL (wire_err %u), expander lost\n", lastWireErr);
    return;
  }
  Serial.print("# RELAY");
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    Relay_Status[ch] = relayWanted & (1 << ch);
    Serial.printf(" t%u=%s", ch + 1, Relay_Status[ch] ? "ON" : "OFF");
  }
  Serial.printf(" OLATB=0x%02X readback=%s\n", next, rb == next ? "OK" : "MISMATCH");
}

void startI2C() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ);
  Wire.setTimeOut(I2C_TIMEOUT_MS);
}

// Expander missing: try again every I2C_RETRY_MS, quietly.
void i2cRecover(unsigned long now) {
  if (busOk || now - lastI2cRetry < I2C_RETRY_MS) return;
  lastI2cRetry = now;
  Wire.end();
  startI2C();
  Wire.beginTransmission(MCP_ADDR);
  if (Wire.endTransmission() == 0 && safeInit()) {
    applyRelays();
  }
}


// ---------------------------------------------------------------------------
// Meters (same read code as the Test 1 sketch)
// ---------------------------------------------------------------------------

// Voltage first. On NAN the module did not answer and the other getters are
// skipped, because each would wait out its own timeout again.
void readChannel(uint8_t ch) {
  Read_Start[ch] = millis();
  Poll_Count[ch]++;
  Voltage[ch] = pzem[ch].voltage();
  if (isnan(Voltage[ch])) {
    Responded[ch] = false;
    Fail_Count[ch]++;
  } else {
    Responded[ch] = true;
    Current[ch]   = pzem[ch].current();
    Power[ch]     = pzem[ch].power();
    Energy[ch]    = pzem[ch].energy();
    Frequency[ch] = pzem[ch].frequency();
    PF[ch]        = pzem[ch].pf();
  }
  Read_Ms[ch] = millis() - Read_Start[ch];
}

// One CSV row, Test 1 columns. Empty measurement fields on NO_RESPONSE.
void printRow(uint8_t ch, unsigned long cycleMs) {
  Serial.printf("%lu,%u,0x%02X,", Read_Start[ch], ch + 1, PZEM_ADDRESS[ch]);
  if (Responded[ch]) {
    Serial.printf("%.1f,%.3f,%.1f,%.3f,%.1f,%.2f,", Voltage[ch], Current[ch],
                  Power[ch], Energy[ch], Frequency[ch], PF[ch]);
  } else {
    Serial.print(",,,,,,");
  }
  Serial.printf("%lu,%lu,%lu,%s\n", Read_Ms[ch], cycleMs, Fail_Count[ch],
                Responded[ch] ? "OK" : "NO_RESPONSE");
}

void printSummary() {
  Serial.printf("# SUMMARY after %lu polls\n", totalCycles);
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    unsigned long ok = Poll_Count[ch] - Fail_Count[ch];
    float rate = Poll_Count[ch] ? 100.0f * ok / Poll_Count[ch] : 0.0f;
    Serial.printf("#   tenant %u (0x%02X): polls=%lu fails=%lu success=%.2f%% relay=%s\n",
                  ch + 1, PZEM_ADDRESS[ch], Poll_Count[ch], Fail_Count[ch], rate,
                  Relay_Status[ch] ? "ON" : "OFF");
  }
}


// ---------------------------------------------------------------------------
// Serial commands (non-blocking line reader)
// ---------------------------------------------------------------------------
void handleCommand(const char *line) {
  int n;
  char word[8];
  if (!strcmp(line, "all on")) {
    relayWanted = 0x0F;
  } else if (!strcmp(line, "all off")) {
    relayWanted = 0x00;
  } else if (sscanf(line, "r%d %7s", &n, word) == 2 && n >= 1 && n <= NUM_CHANNELS &&
             (!strcmp(word, "on") || !strcmp(word, "off"))) {
    if (!strcmp(word, "on")) relayWanted |= (1 << (n - 1));
    else                     relayWanted &= ~(1 << (n - 1));
  } else if (!strcmp(line, "status")) {
    Serial.printf("# STATUS expander=%s olat=0x%02X", busOk ? "OK" : "DOWN", olatShadow);
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
      Serial.printf(" t%u=%s", ch + 1, Relay_Status[ch] ? "ON" : "OFF");
    }
    Serial.println();
    return;
  } else {
    Serial.println("# commands: all on | all off | r<N> on | r<N> off | status");
    return;
  }
  if (busOk) applyRelays();
  else       Serial.println("# expander down: relay change saved, applied when it answers");
}

void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        handleCommand(lineBuf);
        lineLen = 0;
      }
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = tolower(c);
    }
  }
}


// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {   // status LEDs off
    pinMode(LED_PIN_GREEN[ch], OUTPUT);
    pinMode(LED_PIN_RED[ch], OUTPUT);
    digitalWrite(LED_PIN_GREEN[ch], LOW);
    digitalWrite(LED_PIN_RED[ch], LOW);
  }

  Serial.begin(MONITOR_BAUD);
  startI2C();
  if (safeInit()) {                  // relays OFF first, then the start set ON
    applyRelays();
  }
  lastI2cRetry = millis();

  Serial.println("# sensor_read.ino: relays ON, 4 PZEMs polled every 2 s (no Wi-Fi)");
  Serial.printf("# RELAYS_AT_START=0x%02X RELAY_ACTIVE_LOW=%d, addresses 0x01-0x04 on Serial2 (RX16/TX17, 9600 8N1)\n",
                RELAYS_AT_START, RELAY_ACTIVE_LOW);
  Serial.println("# A meter only answers when its mains side is powered.");
  Serial.println("millis,channel,address,voltage_V,current_A,power_W,energy_kWh,"
                 "frequency_Hz,pf,read_ms,cycle_ms,fail_count,status");

  // First poll after one full interval: the PZEM library returns cached
  // zeros (not a real reading) if called in the first 200 ms after boot.
  lastPoll = millis();
}

void loop() {
  unsigned long now = millis();
  pollSerial();
  i2cRecover(now);

  if (now - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = now;
    unsigned long cycleStart = millis();
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) readChannel(ch);
    unsigned long cycleMs = millis() - cycleStart;
    for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) printRow(ch, cycleMs);

    totalCycles++;
    if (totalCycles % SUMMARY_EVERY == 0) printSummary();
  }
}
