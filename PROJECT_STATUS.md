# Project status

As recorded on 30 Oct 2026.

## Current version

Firmware 0.3.0-firebase in `firmware/submeter/`. Compiled with ESP32 core 3.3.12 (1,248,667 bytes, 95% of the default partition).

## Done

- Main firmware: metering, deduction, relays, LEDs, Firebase sync, top-ups, flash storage, landlord controls, Wi-Fi log page, OTA
- Bench sketch `firmware/sensor_read/`
- Admin and tenant dashboard, hosted on Netlify
- Firebase security rules

## Verified on the bench

- All 4 meters answer on the shared bus with a diode on each TX line

## Not yet verified

- The firmware has not been recorded as run on the hardware or against Firebase
- The 30 Oct dashboard changes have not been tested in a browser
- Wiring still to measure: MCP23017 supply voltage, I2C pull-ups, RESET pin
- Bus margin over a long run with the 1N4007 diodes (the "low" level sits close to the ESP32 limit). Recommended upgrade: 1N5819 diodes plus a 10 kΩ pull-up to 3.3 V, fitted together

## Next

1. Run `firmware/sensor_read/` to check all 4 meters and relays
2. Deploy the rules and create the device user
3. Run the main firmware and the acceptance tests
