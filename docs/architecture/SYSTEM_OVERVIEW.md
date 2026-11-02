# System overview

One building supply feeds 4 tenant lines. Each line has its own PZEM-004T energy meter. The admin gives each tenant units (kWh) from the web dashboard. The ESP32 reads every meter about every 2 s, subtracts what each tenant used from that tenant's balance, sounds a buzzer at 10% left, and switches off only that tenant's relay at zero. Everyone else stays on. (PDF p.1)

## Data flow

```
             Modbus-RTU over TTL, 9600 8N1             I2C 400 kHz
4x PZEM-004T  <------------------------------>  ESP32  ------------>  MCP23017 --> 4 relays, buzzer(s)
(0x01..0x04)        Serial2 GPIO16/17             |
                                                   | Wi-Fi, Firebase RTDB (device account)
                                                   v
                                     Firebase Realtime Database  <---->  Dashboard (Netlify)
                                     channels/chN, topUpLog,              admin.html, tenant.html
                                     topUpRequests, users, device
```

Local energy accounting and relay switching never depend on the internet. Only sync and remote top-ups need a connection (CLAUDE.md scope).

## Firmware cycle (fixed order)

1. Initialize
2. Poll each tenant's PZEM
3. Deduct consumption from balance (Eq. 3.1, 3.2)
4. Check for new allocation (top-ups from Firebase or, in Stage 2, typed)
5. Disconnect on zero balance / reconnect if topped up (Eq. 3.3, 3.4)
6. Sync to Firebase
7. Repeat

Step 4 before step 5 stops a relay blink when a top-up arrives at zero (Test 5 step 5 checks this).

## Top-up path (from `Sub-meter dashboard spec.md`)

Admin (or approved tenant request) writes `topUpLog/{id}` and `channels/chN/pendingTopUps/{id}`. The ESP32 applies each pending entry oldest first with one multi-path update (new balance, new `originalAllocation`, `appliedAt`, delete the pending entry), and only then adds it locally. The dashboard never writes a balance.

## What exists on 22 Oct 2026

| Part | State |
|---|---|
| Thesis chapters 1 to 3 | done (outside this folder) |
| Test 1 sketch | written, not run |
| Test 2 sketch | specified, not written |
| Relay precursor sketch | written (relay only), not run |
| Integrated firmware (`firmware/`) | not started |
| Firebase rules | written (`firebase/database.rules.json`) |
| Dashboard | written and hosted on Netlify |
| Test documentation and analysis | this repository reorganisation |
| Physical test results | none |
