/*
 * WebLog.h
 * ---------------------------------------------------------------------------
 * Wi-Fi serial monitor for the sub-metering firmware.
 *
 * "Log" is used exactly like "Serial":
 *     Log.println("[SYS] boot");
 *     Log.printf("[PZEM] ch%u V=%.1f\n", ch + 1, v);
 *
 * Every line goes to the USB Serial port AND into an 8 KB ring buffer in RAM.
 * A small async web server on the ESP32's own access point (192.168.4.1)
 * shows the buffer live in a browser over a WebSocket.
 *
 * Two kinds of line:
 *   - Human lines, starting with a tag such as "[PZEM]". These get a time
 *     prefix: "14:03:22 " once NTP time is known, "+123.4s " before that.
 *   - Contract lines, read by tests/analysis (docs/firmware/SERIAL_LOG_FORMAT.md):
 *     the CSV header ("millis,..."), CSV rows (start with a digit) and '#'
 *     lines (#EV, #H, comments). These are stored exactly as printed, with
 *     no prefix, so a downloaded log still parses.
 *
 * Setup calls use the name WebLogCtl:
 *     WebLogCtl.begin();  WebLogCtl.beginServer(...);  WebLogCtl.loop();
 *
 * Thread safety: one FreeRTOS mutex protects the line buffer and the ring,
 * so Log can be called from loop(), from the Wi-Fi event task and from the
 * web server task. Print each line with ONE call (println / printf with
 * "\n") when calling from a task other than loop(), or two tasks printing
 * at once may interleave their halves of a line.
 *
 * NEVER call Log from an interrupt service routine (ISR): it takes a mutex
 * and may write to the UART, and neither is allowed inside an ISR.
 *
 * WEBLOG_ENABLED 0 turns all of this off: Log becomes plain Serial and no
 * server code is compiled.
 * ---------------------------------------------------------------------------
 */
#pragma once

#include <Arduino.h>

// ============================ SWITCHES =====================================
// These live here (not in the .ino) because WebLog.cpp is compiled on its
// own and would not see a #define made in the sketch.

// 1 = Wi-Fi log page + ring buffer. 0 = Log is plain Serial, no web server.
#ifndef WEBLOG_ENABLED
#define WEBLOG_ENABLED     1
#endif

// 1 = the page gets a command box (whitelist: status, heap, wifi, ledtest,
// reboot). 0 = read-only page. No relay or balance command exists either way.
#ifndef WEBLOG_COMMANDS
#define WEBLOG_COMMANDS    0
#endif

#define WEBLOG_RING_BYTES       8192   // normal ring buffer size
#define WEBLOG_RING_SMALL_BYTES 4096   // size after a low-heap cut
#define WEBLOG_LINE_MAX         256    // longer lines are split
#define WEBLOG_MAX_CLIENTS      3      // browser tabs at once
#define WEBLOG_PUSH_MS          100    // new lines go out at most every 100 ms
#define WEBLOG_STATUS_MS        5000   // status JSON every 5 s
#define WEBLOG_CLEANUP_MS       1000   // ws.cleanupClients() every 1 s

// ===========================================================================

// Sketch-side callbacks.
//
// WebLogStatusFn: write the per-channel part of the status JSON into buf,
// for example  [{"led":"G","relay":1},{"led":"R","relay":0},...]
// It is called from loop() only.
typedef void (*WebLogStatusFn)(char *buf, size_t len);

// WebLogCommandFn: run one whitelisted command. Called from loop() only,
// never from the web server task, so the command can use the I2C bus,
// pins and sketch variables safely.
typedef void (*WebLogCommandFn)(const char *cmd);


#if WEBLOG_ENABLED

class WebLogger : public Print {
public:
  // Allocate the ring buffer and the mutex. Call first thing in setup(),
  // after Serial.begin(). echoSerial = also write every line to Serial.
  bool begin(bool echoSerial = true);

  // Start the web server on port 80. Call after the soft AP is up.
  // The strings must stay valid for the life of the program.
  void beginServer(const char *deviceName, const char *user, const char *pass);

  void onStatus(WebLogStatusFn fn)   { _statusFn = fn; }
  void onCommand(WebLogCommandFn fn) { _commandFn = fn; }

  // Call every loop(). Pushes new lines to browsers (batched), sends the
  // status JSON, cleans up dead clients, runs a queued command. Never blocks.
  void loop();

  // Cut the ring from 8 KB to 4 KB (oldest lines dropped). Returns true if
  // it was cut now, false if it was already small.
  bool shrink();
  size_t capacity() const { return _cap; }

  // Print interface.
  size_t write(uint8_t c) override;
  size_t write(const uint8_t *buf, size_t size) override;
  using Print::write;

  // Used by the web server task (WebLog.cpp). Copy the whole ring, with
  // the CSV header put back in front if it has scrolled out.
  void dumpTo(Print &out);

  // Used by the web server task: queue a command for loop().
  void queueCommand(const char *cmd);

private:
  void   putLineLocked(const char *s, size_t n);   // add one complete line
  void   dropOldestLineLocked();
  size_t copySinceLocked(uint64_t &seq, char *out, size_t max);
  size_t prefixNow(char *out, size_t len);
  static bool isContractLine(const char *s, size_t n);

  SemaphoreHandle_t _mtx = nullptr;
  bool   _echo = true;

  // Ring buffer of complete lines, each ending in '\n'.
  char  *_ring = nullptr;
  size_t _cap  = 0;        // bytes allocated
  size_t _tail = 0;        // index of the oldest byte
  size_t _used = 0;        // bytes in use
  uint64_t _seq = 0;       // total bytes ever added (newest byte = _seq - 1)

  // Line being assembled (bytes since the last '\n').
  char   _line[WEBLOG_LINE_MAX];
  size_t _lineLen = 0;

  // Copy of the CSV header line, so a late /dump still has it.
  char     _header[WEBLOG_LINE_MAX];
  uint64_t _headerSeq = 0;   // _seq where the header started in the ring
  bool     _haveHeader = false;

  // WebSocket bookkeeping (loop() only).
  uint64_t _sentSeq = 0;
  unsigned long _lastPush = 0, _lastStatus = 0, _lastCleanup = 0;

  // Command handed from the web task to loop().
  char _cmd[24];
  volatile bool _cmdPending = false;

  const char *_name = "SubMeter";
  WebLogStatusFn  _statusFn  = nullptr;
  WebLogCommandFn _commandFn = nullptr;
};

extern WebLogger Log;

// Setup and housekeeping calls go through WebLogCtl (begin, beginServer,
// loop...). With WEBLOG_ENABLED 1 it is the same object as Log. With 0, Log
// is Serial, and Serial.begin(true) would set the baud rate to 1, so these
// calls must not use the name Log.
#define WebLogCtl Log

#else  // WEBLOG_ENABLED == 0 ---------------------------------------------------

// Plain Serial. The extra calls become empty so the sketch compiles unchanged.
class WebLogStub {
public:
  bool begin(bool = true) { return true; }
  void beginServer(const char *, const char *, const char *) {}
  void onStatus(WebLogStatusFn) {}
  void onCommand(WebLogCommandFn) {}
  void loop() {}
  bool shrink() { return false; }
  size_t capacity() const { return 0; }
};
extern WebLogStub WebLogCtl;
#define Log Serial

#endif
