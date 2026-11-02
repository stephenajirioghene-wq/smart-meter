# Design and implementation of an IoT based energy sub-meter for multi-tenant buildings sharing one meter

Final-year engineering project, University of Port Harcourt. One building supply feeds 4 tenant lines. Each line has its own PZEM-004T meter. An admin assigns each tenant a prepaid energy quota from a web dashboard. The ESP32 deducts what each tenant uses from their balance. It switches off only that tenant's relay when the balance reaches zero. Other tenants stay on.

Current firmware: **0.3.0-firebase**. See [PROJECT_STATUS.md](PROJECT_STATUS.md) and [CHANGELOG.md](CHANGELOG.md).

## Features

- 4 tenant channels, each with its own meter (voltage, current, power, energy, frequency, power factor)
- Per-tenant balance deduction from measured energy
- Red LED warning at 10% of the allocation left
- Relay disconnect at zero balance, reconnect after a top-up
- Admin and tenant web dashboard on Firebase: top-ups, tenant requests with admin approval, top-up log, landlord disconnect, reset all units
- Balances saved in flash (NVS), so a power cut without internet loses nothing
- Metering and switching keep working without internet. Only sync and top-ups wait for the link
- Wi-Fi log page served by the ESP32 on its own access point, and OTA upload

## Hardware

| Part | Role |
|---|---|
| ESP32 WROOM dev board | Controller, Wi-Fi |
| 4x PZEM-004T V4.0 | Per-tenant meter. Tenants 1, 2: 10 A. Tenants 3, 4: 100 A with external CT |
| 4x 1N4007 diode | Isolate each meter's TX on the shared ESP32 RX line |
| MCP23017 I/O expander | Drives the relays over I2C (address 0x20) |
| 4-channel relay module, active-LOW | Disconnects each tenant line |
| 4x red/green LED | Balance status per tenant |
| AC-DC 5 V module and DC-DC buck-boost | Power supply |

The meters share one TTL Modbus-RTU bus on Serial2 (9600 baud, 8N1, addresses 0x01 to 0x04). Keep the bus under about 1 m.

```
ESP32 GPIO17 (TX) -> all PZEM RX                       ESP32 GPIO21 (SDA) -> MCP23017 SDA
ESP32 GPIO16 (RX) <- each PZEM TX via its own diode    ESP32 GPIO22 (SCL) -> MCP23017 SCL
ESP32 5V          -> all PZEM 5V                       MCP23017 GPB0, GPB1, GPB5, GPB7 -> relay IN1..IN4
ESP32 GND         -> all PZEM GND
```

The PZEM TX pins must not be wired straight together: 2 or more modules then fail, and 5 V reaches GPIO16. A diode on each TX line (band toward the PZEM) fixes both. This build uses 1N4007s with no external pull-up. Recommended upgrade: 1N5819 Schottky diodes plus a 10 kΩ pull-up from GPIO16 to 3.3 V, fitted together (a pull-up on its own with 1N4007s reduces the margin). Why, and the bring-up steps: [docs/hardware_setup/HARDWARE_AND_PINS.md](docs/hardware_setup/HARDWARE_AND_PINS.md#meter-bus-diode-or-return-line).

| Tenant | Green LED | Red LED |
|---|---|---|
| 1 | GPIO32 | GPIO26 |
| 2 | GPIO23 | GPIO27 |
| 3 | GPIO33 | GPIO2 |
| 4 | GPIO25 | GPIO15 |

Full pin tables: [docs/hardware_setup/HARDWARE_AND_PINS.md](docs/hardware_setup/HARDWARE_AND_PINS.md). System overview: [docs/architecture/SYSTEM_OVERVIEW.md](docs/architecture/SYSTEM_OVERVIEW.md).

## How it works

The firmware repeats a fixed cycle: poll each meter, deduct consumption, check for a top-up, switch relays, sync to Firebase. For tenant i at poll k with allocation U_i:

```
deltaE_i[k] = max(0, E_i[k] - E_i[k-1])
B_i[k]      = B_i[k-1] - deltaE_i[k]
warning     : B_i[k] <= 0.10 * U_i
disconnect  : B_i[k] <= 0
```

The database layout and the top-up handshake are in [Sub-meter dashboard spec.md](Sub-meter%20dashboard%20spec.md).

## Repository

| Path | What |
|---|---|
| `firmware/submeter/` | Main firmware |
| `firmware/sensor_read/` | Bench check: relays on, all 4 meters printed to Serial |
| `dashboard/` | Web dashboard (HTML, JavaScript, Firebase SDK) |
| `firebase/` | Database security rules |
| `docs/` | System overview, hardware and pins |

## Setup

### Firmware

1. Arduino IDE with the ESP32 board package (compiled with core 3.3.12).
2. Library Manager: PZEM004Tv30 (mandulaj) 1.2.1, ESP Async WebServer 3.12.1, Async TCP 3.5.0, ArduinoJson 7.4.3.
3. In `firmware/submeter/`, copy `secrets_example.h` to `secrets.h` and fill in your own values. `secrets.h` is never committed.
4. Open `firmware/submeter/submeter.ino`, compile and upload.

Without `FB_EMAIL` in `secrets.h` the firmware runs in bench mode with no Firebase. Details: [firmware/submeter/README.md](firmware/submeter/README.md).

### Firebase and dashboard

1. Create a Firebase project with Realtime Database and Email/Password sign-in.
2. Put your web config in `dashboard/firebase-config.js` and the same key and URL in the `CONFIG` section of `submeter.ino`.
3. Deploy `firebase/database.rules.json`.
4. Create a device user and set `users/<uid>/role` to `device`.
5. Host `dashboard/` on any static host, or run `serve-dashboard.ps1` for http://localhost:8080.

## Limitations

- TTL meter bus, not RS-485: fine inside one enclosure, not across a building.
- Single-phase only. No theft or tamper detection.
- TLS certificates are not checked on the Firebase link (prototype).

## Licence

No licence chosen yet. The PZEM004Tv30 library, installed separately, is MIT licensed.
