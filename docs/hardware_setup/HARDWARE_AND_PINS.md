# Hardware and pins

## Parts

| Part | Role | Notes |
|---|---|---|
| ESP32 WROOM dev board | controller, Wi-Fi, status LEDs | |
| 4x PZEM-004T V4.0 | per-tenant V, I, P, E, f, PF | Tenants 1 and 2: 10 A (internal shunt, load current flows through the module). Tenants 3 and 4: 100 A (external CT on the live conductor). Protocol-identical to V3.0 |
| 4x 1N4007 diode | isolate each PZEM TX on the shared return line | one per PZEM TX, see "Meter bus" below. Recommended upgrade: 1N5819 Schottky |
| 10 kΩ resistor (only with Schottky diodes, not fitted) | pull-up on GPIO16 to 3.3 V | see "Meter bus" below |
| MCP23017 | relays over I2C | GPB0, GPB1, GPB5, GPB7 used, rest free |
| 4-channel relay module | disconnects each tenant | usually 10 A contacts |
| 4x red/green status LED | balance status per tenant | replaced the buzzers (Oct 2026). One ESP32 GPIO per colour, HIGH = lit |
| 220 Ω resistors | LED current limit | 1 per colour |
| AC-DC 5 V module + DC-DC buck-boost | power | replaced the HLK-PM01 |
| Per tenant: 2 sockets, 2 lamp holders, 5 W and 50 W bulbs | simulated loads | |

MAX485 transceivers were dropped; the PZEMs share a TTL bus with a diode on each TX line (see "Meter bus"). Keep the bus short (under about 1 m) and inside one enclosure. A building-scale install would need RS-485 transceivers: a stated limitation.

Buzzers are removed from the design. The low-balance warning is now the LED colour.

## Pin map

Confirmed:
```
ESP32 GPIO17 (Serial2 TX) -> all PZEM RX (in parallel)
ESP32 GPIO16 (Serial2 RX) <- each PZEM TX through its own Schottky diode
ESP32 5V                  -> all PZEM 5V pins (interface supply)
ESP32 GND                 -> all PZEM GND
ESP32 GPIO21 (SDA)        -> MCP23017 SDA (pin 13)
ESP32 GPIO22 (SCL)        -> MCP23017 SCL (pin 12)
MCP23017 A0-A2            -> GND (0x20)
Relay module VCC          =  5 V, JD-VCC jumper fitted
```

### MCP23017 outputs

Source: Steve's relay control program (30 Oct 2026), Adafruit pin numbers 8, 9, 13, 15.

| MCP23017 pin | Chip pin | Adafruit no. | Connects to | Notes |
|---|---|---|---|---|
| GPB0 | 1 | 8 | Relay IN1 | Tenant 1 |
| GPB1 | 2 | 9 | Relay IN2 | Tenant 2 |
| GPB5 | 6 | 13 | Relay IN3 | Tenant 3 |
| GPB7 | 8 | 15 | Relay IN4 | Tenant 4. Confirmed by Steve 30 Oct 2026 (the program comment "B6" was wrong, OI-36) |
| other GPB pins, GPA0-GPA7 | | | free | set as inputs by the firmware |

### Status LEDs (ESP32 direct)

One GPIO per colour, HIGH = that colour lit, common leg to GND.

| Tenant | Green | Red |
|---|---|---|
| 1 | GPIO32 | GPIO26 |
| 2 | GPIO23 | GPIO27 |
| 3 | GPIO33 | GPIO2 |
| 4 | GPIO25 | GPIO15 |

Drive table (per tenant):

| Green | Red | LED | Meaning |
|---|---|---|---|
| HIGH | LOW | GREEN | balance above 10% of original allocation |
| LOW | HIGH | RED | balance at or below 10% (includes 0, relay OFF) |
| LOW | LOW | off | boot, before the first balance is known |

GPIO34 and GPIO35 are input only, so they cannot drive LEDs. GPIO2 and GPIO15 are strapping pins. An LED to GND on them is safe for normal boot. GPIO2 may also drive the board's own blue LED.

Threshold: RED when `Remaining_Balance[channel] <= 0.10 x originalAllocation[channel]`, GREEN otherwise. Needs the Firebase field `originalAllocation` (open flag).

Current check: (3.3 V - 2.0 V red Vf) / 220 Ω ≈ 6 mA. Well inside the ESP32 GPIO limit. A pure-green LED (Vf about 3 V) will be dim at 3.3 V; a yellow-green one is fine. Drop to 150 Ω if green is too dim.

Not confirmed (OI-12):
```
Relay input polarity        active-LOW (precursor sketch and Steve's relay program agree; re-confirm in Test 3)
MCP23017 VDD                3.3 V or 5 V?
I2C pull-ups                value and rail
MCP23017 RESET (pin 18)     tied to VCC?
```

Relays must come up OFF at boot: write OLATB with OFF before setting the relay pins of IODIRB to outputs (`safeInit()`). LEDs start with both pins LOW.

## Meter bus (diode-OR return line)

Wiring the 4 PZEM TX pins straight together onto GPIO16 does not work. Each module works alone, but 2 or more on the bus fail. Each PZEM TX is an optocoupler output with its own pull-up resistor to the interface supply. In parallel the pull-ups add up, and a replying module cannot pull the shared line low enough for the ESP32 to read a clean 0. In this build the interfaces run on 5 V, so wired directly those pull-ups would also put 5 V on GPIO16, which is a 3.3 V pin.

Fix used in this build: one 1N4007 diode per PZEM TX, no external pull-up. Confirmed on the bench with all 4 modules connected: all 4 answer. The diodes also keep 5 V off GPIO16, so do not remove them while the interfaces run on 5 V.

```
                 GPIO16 (Serial2 RX)
                        |
        +---------+-----+---+---------+
        |         |         |         |
       ---       ---       ---       ---     diode band (cathode)
       / \       / \       / \       / \     toward each PZEM TX
        |         |         |         |
    PZEM1 TX  PZEM2 TX  PZEM3 TX  PZEM4 TX

GPIO17 (Serial2 TX) -> PZEM1..4 RX in parallel
```

Each diode blocks its module's pull-up from the shared line. Only the module that is replying pulls the line low.

**Why the 1N4007s work, and the margin.** The ESP32's maximum "low" input level is about 0.8 V (at 3.3 V supply). When a module replies, the line sits at the diode drop plus the PZEM optocoupler's saturation voltage. This build has no external pull-up: the line is held high only by the ESP32's weak internal pull-up on the UART RX pin, so very little current flows through the diode. At that current a 1N4007 drops about 0.45 to 0.5 V, which puts the "low" at roughly 0.6 to 0.7 V. That is under the limit, with little margin.

**Do not add a pull-up resistor with 1N4007s.** A stronger pull-up pushes more current through the silicon diodes, raises their drop, and moves the "low" closer to the limit.

**Recommended upgrade (do both together):** replace the 4 diodes with Schottky diodes (1N5819, SS14 or BAT85, about 0.3 V drop) and add a 10 kΩ resistor from GPIO16 to the ESP32 3.3 V pin (4.7 kΩ also works). Tie it to 3.3 V only, never 5 V. This gives a lower "low" level and faster, cleaner edges.

```
ESP32 3V3 ---[10k]---+--- GPIO16      (only with Schottky diodes)
                     |
            1N5819 x4 to PZEM1..4 TX
```

To check which type a diode is, use a multimeter in diode mode, red probe on the end without the band: about 0.15 to 0.35 V is Schottky, about 0.5 to 0.7 V is silicon.

**Request line.** GPIO17 drives all 4 PZEM RX inputs directly. With the interfaces on 5 V, the ESP32's 3.3 V "high" does not fully switch off the PZEM RX optocouplers. It works with 4 modules on the bench. For more modules or longer runs, drive the RX inputs through a 74HCT125 buffer powered from 5 V.

### Bring-up lessons

1. A PZEM only answers when its AC side is live. With only the TTL side powered, every read returns NaN.
2. Use the general address 0xF8 with one module connected only. Several modules on 0xF8 all reply at once and the replies collide.
3. Set addresses one module at a time (library example `PZEMChangeAddress`), read them back, and label each module (for example "T4 · 0x04 · 100A"). The address is stored in the module and survives power loss.
4. Connect or disconnect modules with the power off. Plugging one onto a live bus reset the ESP32 once.
5. The 100 A modules read near zero on very small loads (starting current 0.02 A). Use the 50 W bulb for a visible current on tenants 3 and 4.
6. Each PZEM has 2 LEDs beside the TTL header. One flashes on a received request, the other on a reply. Request only, no reply: the module heard but did not answer (AC off, wrong address, bad frame). Neither: no request or no interface power.

## PZEM facts used by the tests (Peacefair V4.0 manual)

Energy register step 1 Wh. Power resolution 0.1 W. Starting current 0.02 A (100 A version). Accuracy 0.5% (used in the Test 1 limit). A PZEM answers Modbus only when its AC side is powered. General address 0xF8: one module on the bus only.
