# submeter: integrated firmware, base build with Wi-Fi log

The ESP32 on the board cannot reach the PC over USB, so every Serial line is also shown on a web page served by the ESP32's own access point.

## Files

| File | What it is |
|---|---|
| `submeter.ino` | 7-step cycle: poll, deduct, allocation, relays and LEDs, sync. Wi-Fi AP + STA, OTA |
| `WebLog.h`, `WebLog.cpp` | `Log` (a `Print`, used like `Serial`), 8 KB ring buffer, web server and WebSocket |
| `WebLogPage.h` | the log page (HTML, CSS, JS in flash, no CDN) |
| `FirebaseSync.h`, `FirebaseSync.cpp` | Firebase REST link in its own FreeRTOS task (core 0): sign-in, live sync every 10 s, history every 5 min, top-up handshake |
| `secrets_example.h` | copy to `secrets.h` and fill in the passwords. `secrets.h` is git-ignored |

## Two modes

| Mode | When | Balances |
|---|---|---|
| Firebase (full system) | `STA_SSID`, `FB_EMAIL`, `FB_PASSWORD` set in `secrets.h` | From flash (NVS) at boot; from Firebase on the very first boot. Top-ups from the dashboard. Saved to flash every 1 min and after each top-up |
| Bench test | `FB_EMAIL` empty | Every boot each tenant starts with `TEST_ALLOCATION_KWH` (10 Wh). Nothing saved |

Without internet the Firebase mode keeps metering, deducting and switching relays from the local balance. Only sync and top-ups wait for the link.

## Firebase device account (once)

1. Firebase Console > Authentication > Add user, for example `device@submeter.local` with a strong password.
2. Copy its UID. Realtime Database > `users/<UID>` = `{ "role": "device" }`.
3. Put the email and password in `secrets.h` as `FB_EMAIL` and `FB_PASSWORD`.

The dashboard refuses this account, so it cannot be used to log in there.

TLS certificates are not checked (`setInsecure`). That is a stated prototype limitation.

## Libraries (versions this was compiled against, 27 Oct 2026)

| Library | Version | Install from |
|---|---|---|
| ESP32 Arduino core (esp32 by Espressif) | 3.3.12 | Boards Manager |
| ESP Async WebServer (ESP32Async) | 3.12.1 | Library Manager, search "ESP Async WebServer" |
| Async TCP (ESP32Async) | 3.5.0 | Library Manager, search "Async TCP" |
| ArduinoJson (Benoit Blanchon) | 7.4.3 | Library Manager, search "ArduinoJson" |
| HTTPClient, WiFiClientSecure, Preferences | bundled with the core | |
| PZEM004Tv30 (mandulaj) | 1.2.1 | Library Manager, search "PZEM004Tv30" |
| ArduinoOTA, WiFi, Wire | bundled with the core | |

Use the ESP32Async versions. The old me-no-dev "ESPAsyncWebServer" / "AsyncTCP" do not build on core 3.x.

## Arduino IDE settings

- Board: ESP32 Dev Module
- Partition scheme: **Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)**. "Default 4MB with spiffs" also has two OTA slots, but the app slot is only 1.2 MB.
- Upload speed 921600, CPU 240 MHz, flash 4 MB (defaults).
- First upload by cable (or any working programmer). After that, use OTA.

## Switches

| Where | Name | Default | Effect |
|---|---|---|---|
| `WebLog.h` | `WEBLOG_ENABLED` | 1 | 0 = `Log` is plain `Serial`, no web server compiled |
| `WebLog.h` | `WEBLOG_COMMANDS` | 0 | 1 = command box on the page (status, heap, wifi, ledtest, reboot) |
| `submeter.ino` | `LED_PIN_GREEN` / `LED_PIN_RED` | 32,23,33,25 / 26,27,2,15 | one GPIO per colour; swap the arrays if red and green come out swapped |
| `submeter.ino` | `RELAY_BIT` | 0,1,5,7 | MCP23017 port B bit per tenant (GPB0, GPB1, GPB5, GPB7) |
| `submeter.ino` | `LED_TEST_AT_BOOT` | 1 | each LED GREEN, RED, OFF once after boot (about 9 s, non-blocking) |
| `submeter.ino` | `LOG_PZEM_LINES` / `LOG_CSV_ROWS` | 1 / 1 | human `[PZEM]` lines / contract CSV rows |
| `submeter.ino` | `OTA_MDNS` | 1 | mDNS for the IDE network port list only |

## Use

1. Join the Wi-Fi network `SubMeter-Debug` (password = `AP_PASS`).
2. Open `http://192.168.4.1/`, log in with `WEB_USER` / `WEB_PASS`.
3. `http://192.168.4.1/dump` gives the whole ring buffer as plain text.
4. For thesis logs use **Download CSV**: it keeps the CSV header, data rows and `#` lines (up to 50 000) since the page was opened, without the tagged lines, so the file can be opened as a CSV directly. Keep the page open for the whole run.

The 8 KB ring holds about 20 s of output at the default settings (4 CSV rows + 4 `[PZEM]` lines every 2 s). The browser keeps the long history.

## OTA

Laptop on `SubMeter-Debug`. Arduino IDE: Tools > Port > Network ports > `submeter at 192.168.4.1`. Upload; the IDE asks for `OTA_PASS`. During the upload relays stay as they are, LEDs go off and Firebase work stops. After the upload the ESP32 restarts: relays go OFF at boot and back ON in the first cycle (about 2 s later) for tenants with balance.
