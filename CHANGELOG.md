# Changelog

Newest first.

## 0.3.0-firebase, 30 Oct 2026 (current)

- Landlord disconnect from the dashboard, held in flash across resets
- Reset all units
- Dashboard shows and accepts Wh. Firebase and firmware stay in kWh
- Rules: new `control` node, tenant request minimum 0.001 kWh

## 0.2.1, 30 Oct 2026

- Relays moved to MCP23017 port B (GPB0, GPB1, GPB5, GPB7)
- One ESP32 GPIO per LED colour

## 0.2.0, 27 Oct 2026

- Firebase sync in its own FreeRTOS task: live values every 10 s, history every 5 min, top-up handshake
- Balances saved in flash (NVS)

## 0.1.0-weblog, 27 Oct 2026

- First integrated firmware: polling, deduction, relays, LEDs
- Wi-Fi log page on the ESP32's own access point, OTA upload

## Hardware changes, October to November 2026

- 1N4007 diode on each PZEM TX line (diode-OR into ESP32 RX). Without it, 2 or more meters on the bus failed. Confirmed working with all 4 meters
- PZEM interface supply moved from ESP32 3.3 V to 5 V
- Buzzers replaced by red/green LEDs
- HLK-PM01 replaced by an AC-DC 5 V module and DC-DC buck-boost
- MAX485 transceivers dropped; the meters share a TTL bus
