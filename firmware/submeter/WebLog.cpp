/*
 * WebLog.cpp
 * ---------------------------------------------------------------------------
 * Ring buffer, line assembly and the AP-only web server for the Wi-Fi
 * serial monitor. See WebLog.h for how to use it.
 *
 * Tasks involved:
 *   loop() task       most Log calls, WebLogCtl.loop() (WebSocket pushes)
 *   async_tcp task    HTTP requests, WebSocket connect/disconnect/data
 *   Wi-Fi event task  [WIFI] messages from the sketch's event handler
 * The ring and line buffer are shared by all three, so every access holds
 * the mutex _mtx. The list of WebSocket clients is shared by loop() and the
 * async_tcp task and has its own short critical section (clientMux).
 * ---------------------------------------------------------------------------
 */
#include "WebLog.h"

#if WEBLOG_ENABLED

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <algorithm>
#include <time.h>
#include "WebLogPage.h"

WebLogger Log;

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");

// Connected WebSocket clients. isNew = still waiting for the history.
static uint32_t clientId[WEBLOG_MAX_CLIENTS];
static bool     clientNew[WEBLOG_MAX_CLIENTS];
static uint8_t  clientCount = 0;
static portMUX_TYPE clientMux = portMUX_INITIALIZER_UNLOCKED;

// Raw text per WebSocket message and its JSON-escaped form. Escaping at
// most doubles the size (see jsonEscape), plus the envelope.
#define CHUNK_RAW   1400
#define CHUNK_JSON  (2 * CHUNK_RAW + 48)
static char rawBuf[CHUNK_RAW];
static char jsonBuf[CHUNK_JSON];


// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

bool WebLogger::begin(bool echoSerial) {
  _echo = echoSerial;
  _mtx  = xSemaphoreCreateMutex();
  _ring = (char *)malloc(WEBLOG_RING_BYTES);
  if (_mtx == nullptr || _ring == nullptr) {
    // No buffer: Log keeps working as plain Serial (see write()).
    Serial.println("[SYS] WebLog: no memory for the ring buffer, Serial only");
    if (_ring) { free(_ring); _ring = nullptr; }
    return false;
  }
  _cap = WEBLOG_RING_BYTES;
  return true;
}


// ---------------------------------------------------------------------------
// Print interface and line assembly
// ---------------------------------------------------------------------------

// Contract lines (docs/firmware/SERIAL_LOG_FORMAT.md) are kept exactly as
// printed: blank, '#...', a digit (CSV row) or the "millis," header.
bool WebLogger::isContractLine(const char *s, size_t n) {
  if (n == 0) return true;
  if (s[0] == '#' || isdigit((unsigned char)s[0])) return true;
  return n >= 7 && strncmp(s, "millis,", 7) == 0;
}

// Time prefix for human lines: wall clock once NTP has set it, otherwise
// seconds since boot.
size_t WebLogger::prefixNow(char *out, size_t len) {
  time_t t = time(nullptr);
  if (t > 1700000000) {                       // after Nov 2023: clock is set
    struct tm tmv;
    localtime_r(&t, &tmv);
    return snprintf(out, len, "%02d:%02d:%02d ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  }
  unsigned long ms = millis();
  return snprintf(out, len, "+%lu.%lus ", ms / 1000, (ms % 1000) / 100);
}

size_t WebLogger::write(uint8_t c) {
  return write(&c, 1);
}

size_t WebLogger::write(const uint8_t *buf, size_t size) {
  if (_ring == nullptr) {                     // begin() not called or failed
    return Serial.write(buf, size);
  }
  xSemaphoreTake(_mtx, portMAX_DELAY);
  for (size_t i = 0; i < size; i++) {
    char c = (char)buf[i];
    if (c == '\r') continue;                  // lines end in '\n' only
    bool split = (c != '\n' && _lineLen >= WEBLOG_LINE_MAX - 1);
    if (c != '\n' && !split) {
      _line[_lineLen++] = c;
      continue;
    }

    // A line is complete: add the prefix (human lines only) and store it.
    char out[WEBLOG_LINE_MAX + 24];
    size_t n = 0;
    if (!isContractLine(_line, _lineLen)) {
      n = prefixNow(out, sizeof out);
    }
    memcpy(out + n, _line, _lineLen);
    n += _lineLen;

    if (_lineLen >= 7 && strncmp(_line, "millis,", 7) == 0) {
      memcpy(_header, _line, _lineLen);       // keep the CSV header for late readers
      _header[_lineLen] = '\0';
      _headerSeq  = _seq;
      _haveHeader = true;
    }

    out[n++] = '\n';
    if (_echo) {
      Serial.write((const uint8_t *)out, n);
    }
    putLineLocked(out, n);
    _lineLen = 0;

    if (split) {
      _line[_lineLen++] = c;                  // this char starts the next piece
    }
  }
  xSemaphoreGive(_mtx);
  return size;
}


// ---------------------------------------------------------------------------
// Ring buffer (call with _mtx held)
// ---------------------------------------------------------------------------

// Remove the oldest line. Lines are always whole, so _tail stays at the
// start of a line.
void WebLogger::dropOldestLineLocked() {
  while (_used > 0) {
    char c = _ring[_tail];
    _tail = (_tail + 1) % _cap;
    _used--;
    if (c == '\n') break;
  }
}

// Add one complete line (ending in '\n'). Never blocks: if the ring is
// full, the oldest lines are dropped until the new one fits.
void WebLogger::putLineLocked(const char *s, size_t n) {
  if (n > _cap) {                             // cannot happen with LINE_MAX < cap
    s += n - _cap;
    n = _cap;
  }
  while (_cap - _used < n) {
    dropOldestLineLocked();
  }
  size_t head = (_tail + _used) % _cap;
  size_t first = std::min(n, _cap - head);    // up to the end of the array
  memcpy(_ring + head, s, first);
  memcpy(_ring, s + first, n - first);        // wrap to the start
  _used += n;
  _seq  += n;
}

// Copy bytes from position seq onwards (at most max, ending on a whole
// line) into out. If seq has already been overwritten, start at the oldest
// line still held. Moves seq past what was copied.
size_t WebLogger::copySinceLocked(uint64_t &seq, char *out, size_t max) {
  uint64_t oldest = _seq - _used;
  if (seq < oldest) seq = oldest;
  size_t n = (size_t)std::min<uint64_t>(_seq - seq, max);
  if (n == 0) return 0;

  size_t start = (_tail + (size_t)(seq - oldest)) % _cap;
  size_t first = std::min(n, _cap - start);
  memcpy(out, _ring + start, first);
  memcpy(out + first, _ring, n - first);

  if (n < _seq - seq) {                       // cut short: end on a whole line
    size_t k = n;
    while (k > 0 && out[k - 1] != '\n') k--;
    if (k > 0) n = k;
  }
  seq += n;
  return n;
}

bool WebLogger::shrink() {
  if (_ring == nullptr) return false;
  xSemaphoreTake(_mtx, portMAX_DELAY);
  if (_cap <= WEBLOG_RING_SMALL_BYTES) {
    xSemaphoreGive(_mtx);
    return false;
  }
  while (_used > WEBLOG_RING_SMALL_BYTES) {
    dropOldestLineLocked();
  }
  // Rotate the array so the data starts at index 0, then give back the end.
  std::rotate(_ring, _ring + _tail, _ring + _cap);
  _tail = 0;
  char *p = (char *)realloc(_ring, WEBLOG_RING_SMALL_BYTES);
  if (p) _ring = p;                           // if realloc fails the old block stays valid
  _cap = WEBLOG_RING_SMALL_BYTES;
  xSemaphoreGive(_mtx);
  return true;
}

void WebLogger::dumpTo(Print &out) {
  if (_ring == nullptr) return;
  xSemaphoreTake(_mtx, portMAX_DELAY);
  if (_haveHeader && _headerSeq < _seq - _used) {
    out.print(_header);                       // header has scrolled out: put it back
    out.print('\n');
  }
  size_t first = std::min(_used, _cap - _tail);
  out.write((const uint8_t *)_ring + _tail, first);
  out.write((const uint8_t *)_ring, _used - first);
  xSemaphoreGive(_mtx);
}

void WebLogger::queueCommand(const char *cmd) {
  if (_cmdPending) return;                    // one at a time; extra ones are dropped
  strlcpy(_cmd, cmd, sizeof _cmd);
  _cmdPending = true;
}


// ---------------------------------------------------------------------------
// Web server (runs in the async_tcp task)
// ---------------------------------------------------------------------------

// Only answer requests that arrived on the soft AP interface. A request on
// the STA side (building Wi-Fi) has the STA address as its local IP.
static bool apOnly(AsyncWebServerRequest *request) {
  return request->client()->localIP() == WiFi.softAPIP();
}

// Wrap raw log text as {"type":"log","text":"..."}. Quotes, backslashes,
// newlines and tabs become 2-char escapes; other control characters become
// a space, so the output is at most 2x the input plus the envelope.
static size_t jsonLog(const char *in, size_t n, char *out) {
  size_t k = 0;
  const char *head = "{\"type\":\"log\",\"text\":\"";
  memcpy(out, head, strlen(head));
  k = strlen(head);
  for (size_t i = 0; i < n; i++) {
    char c = in[i];
    switch (c) {
      case '"':  out[k++] = '\\'; out[k++] = '"';  break;
      case '\\': out[k++] = '\\'; out[k++] = '\\'; break;
      case '\n': out[k++] = '\\'; out[k++] = 'n';  break;
      case '\t': out[k++] = '\\'; out[k++] = 't';  break;
      default:   out[k++] = ((unsigned char)c < 0x20) ? ' ' : c; break;
    }
  }
  out[k++] = '"';
  out[k++] = '}';
  return k;
}

static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    bool full;
    portENTER_CRITICAL(&clientMux);
    full = (clientCount >= WEBLOG_MAX_CLIENTS);
    if (!full) {
      clientId[clientCount]  = client->id();
      clientNew[clientCount] = true;          // loop() sends the history first
      clientCount++;
    }
    portEXIT_CRITICAL(&clientMux);
    if (full) {
      client->close(1013, "too many clients");
      Log.printf("[WIFI] log page refused: already %u viewers\n", WEBLOG_MAX_CLIENTS);
    } else {
      Log.printf("[WIFI] log page opened by %s\n", client->remoteIP().toString().c_str());
    }

  } else if (type == WS_EVT_DISCONNECT) {
    portENTER_CRITICAL(&clientMux);
    for (uint8_t i = 0; i < clientCount; i++) {
      if (clientId[i] == client->id()) {
        clientId[i]  = clientId[clientCount - 1];
        clientNew[i] = clientNew[clientCount - 1];
        clientCount--;
        break;
      }
    }
    portEXIT_CRITICAL(&clientMux);

  } else if (type == WS_EVT_DATA) {
#if WEBLOG_COMMANDS
    // Commands are short, single-frame text messages.
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len &&
        info->opcode == WS_TEXT && len > 0 && len < 24) {
      char cmd[24];
      size_t k = 0;
      for (size_t i = 0; i < len; i++) {
        char c = tolower(data[i]);
        if (c >= 0x20 && c < 0x7F) cmd[k++] = c;
      }
      cmd[k] = '\0';
      Log.queueCommand(cmd);
    }
#endif
  }
}

void WebLogger::beginServer(const char *deviceName, const char *user, const char *pass) {
  _name = deviceName;

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
          request->send(200, "text/html", (const uint8_t *)WEBLOG_PAGE, strlen_P(WEBLOG_PAGE));
        })
      .setFilter(apOnly)
      .setAuthentication(user, pass, AsyncAuthType::AUTH_BASIC);

  server.on("/dump", HTTP_GET, [](AsyncWebServerRequest *request) {
          AsyncResponseStream *resp = request->beginResponseStream("text/plain; charset=utf-8");
          Log.dumpTo(*resp);
          request->send(resp);
        })
      .setFilter(apOnly)
      .setAuthentication(user, pass, AsyncAuthType::AUTH_BASIC);

  ws.onEvent(onWsEvent);
  ws.setFilter(apOnly);
  ws.setAuthentication(user, pass, AsyncAuthType::AUTH_BASIC);
  server.addHandler(&ws);

  // Anything else, and anything arriving on the STA interface.
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(apOnly(request) ? 404 : 403, "text/plain", "not here");
  });

  server.begin();
  Log.printf("[SYS] log page on http://%s/ (port 80, AP only)\n",
             WiFi.softAPIP().toString().c_str());
}


// ---------------------------------------------------------------------------
// loop(): batched pushes, status, cleanup, commands. Never blocks.
// ---------------------------------------------------------------------------

void WebLogger::loop() {
  if (_ring == nullptr) return;
  unsigned long now = millis();

  // Queued command from the page (runs here, in loop context).
  if (_cmdPending) {
    char cmd[24];
    strlcpy(cmd, _cmd, sizeof cmd);
    _cmdPending = false;
    printf("[SYS] > %s\n", cmd);
    if (_commandFn) _commandFn(cmd);
  }

  if (now - _lastCleanup >= WEBLOG_CLEANUP_MS) {
    _lastCleanup = now;
    ws.cleanupClients(WEBLOG_MAX_CLIENTS);
  }

  if (now - _lastPush < WEBLOG_PUSH_MS) return;
  _lastPush = now;

  // Snapshot the client list.
  uint32_t ids[WEBLOG_MAX_CLIENTS];
  bool     isNew[WEBLOG_MAX_CLIENTS];
  uint8_t  count;
  portENTER_CRITICAL(&clientMux);
  count = clientCount;
  for (uint8_t i = 0; i < count; i++) {
    ids[i]   = clientId[i];
    isNew[i] = clientNew[i];
  }
  portEXIT_CRITICAL(&clientMux);

  if (count == 0) {
    // Nobody watching: nothing is owed to anyone.
    xSemaphoreTake(_mtx, portMAX_DELAY);
    _sentSeq = _seq;
    xSemaphoreGive(_mtx);
    return;
  }

  // Back-pressure: if any browser's queue is full, try again next time.
  // The lines stay in the ring until then.
  if (!ws.availableForWriteAll()) return;

  // 1. New lines since the last push, to clients that already have history.
  for (uint8_t chunk = 0; chunk < 4; chunk++) {
    xSemaphoreTake(_mtx, portMAX_DELAY);
    size_t n = copySinceLocked(_sentSeq, rawBuf, sizeof rawBuf);
    xSemaphoreGive(_mtx);
    if (n == 0) break;
    size_t j = jsonLog(rawBuf, n, jsonBuf);
    for (uint8_t i = 0; i < count; i++) {
      if (!isNew[i]) ws.text(ids[i], jsonBuf, j);
    }
  }

  // 2. History for new clients: everything in the ring up to _sentSeq, so
  //    the next push continues exactly where the history ended.
  for (uint8_t i = 0; i < count; i++) {
    if (!isNew[i]) continue;
    xSemaphoreTake(_mtx, portMAX_DELAY);
    uint64_t seq = _seq - _used;
    uint64_t end = _sentSeq;
    bool hdr = _haveHeader && _headerSeq < seq;
    xSemaphoreGive(_mtx);

    if (hdr) {
      size_t h = strlen(_header);
      memcpy(rawBuf, _header, h);
      rawBuf[h++] = '\n';
      size_t j = jsonLog(rawBuf, h, jsonBuf);
      ws.text(ids[i], jsonBuf, j);
    }
    while (seq < end) {
      xSemaphoreTake(_mtx, portMAX_DELAY);
      size_t n = copySinceLocked(seq, rawBuf, (size_t)std::min<uint64_t>(sizeof rawBuf, end - seq));
      xSemaphoreGive(_mtx);
      if (n == 0) break;
      size_t j = jsonLog(rawBuf, n, jsonBuf);
      ws.text(ids[i], jsonBuf, j);
    }

    portENTER_CRITICAL(&clientMux);
    for (uint8_t k = 0; k < clientCount; k++) {
      if (clientId[k] == ids[i]) clientNew[k] = false;
    }
    portEXIT_CRITICAL(&clientMux);
  }

  // 3. Status JSON every 5 s (and straight after a new client joined).
  bool anyNew = false;
  for (uint8_t i = 0; i < count; i++) anyNew |= isNew[i];
  if (anyNew || now - _lastStatus >= WEBLOG_STATUS_MS) {
    _lastStatus = now;
    char ch[200] = "[]";
    if (_statusFn) _statusFn(ch, sizeof ch);
    bool sta = WiFi.isConnected();
    int n = snprintf(jsonBuf, sizeof jsonBuf,
                     "{\"type\":\"status\",\"name\":\"%s\",\"ip\":\"%s\",\"sta\":%d,"
                     "\"staip\":\"%s\",\"rssi\":%d,\"up\":%lu,\"heap\":%u,\"minheap\":%u,"
                     "\"ring\":%u,\"cmd\":%d,\"ch\":%s}",
                     _name, WiFi.softAPIP().toString().c_str(), sta ? 1 : 0,
                     sta ? WiFi.localIP().toString().c_str() : "",
                     sta ? (int)WiFi.RSSI() : 0, now / 1000,
                     (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                     (unsigned)_cap, WEBLOG_COMMANDS, ch);
    ws.textAll(jsonBuf, n);
  }
}

#else

WebLogStub WebLogCtl;

#endif
