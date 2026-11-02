# Sub-meter dashboard spec

Updated 14 October 2026

## Scope and roles

The dashboard is a web app with 2 roles reading one Firebase Realtime Database. The admin sees and controls all channels. A tenant sees only their own channel and cannot change anything.

| Role | Sees | Can change |
| --- | --- | --- |
| Admin | All channels: live V, A, W, PF, kWh, balance, relay state, history, top-up log, device status | Top up units, assign tenant names and logins to channels |
| Tenant | Own channel only: balance, percent left, relay state, live power, usage history, own top-ups | Nothing (read-only) |
| Device (ESP32) | Not a dashboard user | Live readings, balance after deduction, relay state, heartbeat, history samples |

The prototype has 4 channels. Every page loops over whatever channels exist in the database, so adding channel 5 needs no front-end change. This keeps the scalability claim from Chapter 3 true at the UI layer too.

Units on screen are kWh, labelled "units" in tenant-facing text because that's the word Nigerian prepaid users already know.

## Stack and hosting

Keep it plain: static HTML, CSS and JavaScript with the Firebase JS SDK. No server of your own. Firebase does auth and data; Netlify hosts the files.

| Layer | Choice | Why |
| --- | --- | --- |
| Front end | Vanilla JS (ES modules), no framework | Easy to explain at defence; no build step needed |
| Data | Firebase Realtime Database, `onValue` listeners | Matches Chapter 3 and the firmware; pushes changes to the browser in about 1 s, no polling |
| Auth | Firebase Authentication, email and password | Same accounts work for admin, tenants and the ESP32 device user |
| Charts | Chart.js | Line chart for power, bar chart for daily kWh |
| Hosting | Netlify (decided) | Free tier is enough; HTTPS by default |

Target screens: phone first (tenants will check on a phone), then a wider grid for the admin on a laptop.

Decision: vanilla JS. The supervisor has no framework requirement, and plain JS is the easiest to defend line by line.

Netlify setup notes:

- Add the Netlify domain (for example `submeter.netlify.app`) to Firebase Console > Authentication > Settings > Authorized domains, or sign-in and password-reset links fail.
- Use separate HTML files per page (`index.html` for login, `admin.html`, `tenant.html`) so no redirect rules are needed. If Claude Code builds client-side routes instead, add a `_redirects` file with `/* /index.html 200`.
- The Firebase web config (API key, project ID) sits in the front-end code. That's normal and not a secret; the security rules are what protect the data.
- Netlify serves over HTTPS by default, which Firebase Auth needs.

## Firebase RTDB data contract

The dashboard never writes a tenant's balance directly. The admin writes a top-up request, and the ESP32 applies it in step 4 of the firmware cycle (Check for new allocation). That way only one writer ever touches `remainingBalance`, and a top-up can't be lost when it lands at the same moment the ESP32 deducts.

Write ownership: `channels/{id}/live`, `remainingBalance`, `originalAllocation`, `relayStatus`, `history` and `device` are device-write-only. `channels/{id}/pendingTopUps`, `channels/{id}/tenantName`, `topUpLog` and `users` are admin-write-only. Tenants write nothing.

```json
{
  "device": {
    "lastSeen": 1760440000000,
    "firmware": "1.0.0",
    "ip": "192.168.0.50"
  },
  "channels": {
    "ch1": {
      "tenantName": "Room A",
      "remainingBalance": 12.345,
      "originalAllocation": 20.000,
      "relayStatus": "ON",
      "live": {
        "voltage": 229.8,
        "current": 0.215,
        "power": 48.7,
        "pf": 0.98,
        "energy": 3.412,
        "frequency": 50.0,
        "ts": 1760440000000
      },
      "pendingTopUps": {
        "-Nx9abc": { "kwh": 10.0, "requestedAt": 1760440000000 },
        "-Nx9abd": { "kwh": 5.0, "requestedAt": 1760440300000 }
      },
      "history": {
        "2026-10-14": {
          "1310": { "power": 48.7, "balance": 12.40 }
        }
      }
    }
  },
  "topUpLog": {
    "-Nx9abc": {
      "channel": "ch1",
      "kwh": 10.0,
      "by": "adminUid",
      "requestedAt": 1760440000000,
      "appliedAt": null
    }
  },
  "users": {
    "uid123": { "role": "tenant", "channel": "ch1" },
    "uidAdmin": { "role": "admin" },
    "uidDevice": { "role": "device" }
  }
}
```

Field rules:

- `remainingBalance` and `originalAllocation` are kWh, 3 decimals.
- `originalAllocation` is set by the ESP32 to the balance right after a top-up is applied. The 10% warning uses `remainingBalance / originalAllocation`. This closes the open `originalAllocation` flag.
- `relayStatus` is `"ON"` or `"OFF"`, written by the ESP32 only after the MCP23017 write succeeds, so the dashboard shows the real state, not the intended one.
- All timestamps are milliseconds since epoch from Firebase server time (`ServerValue.TIMESTAMP` on the device side), so the browser can compare them with its own clock offset.
- `history` gets 1 sample per channel every 5 minutes: 288 per channel per day, about 1,150 rows a day for 4 channels. That fits the free Spark plan easily.
- `topUpLog/{id}/appliedAt` is filled by the ESP32 when it applies the request. The dashboard uses it to show "pending" versus "applied".

Top-up handshake:

1. Admin submits 10 kWh for ch1. Dashboard creates `topUpLog/{id}` and adds `channels/ch1/pendingTopUps/{id}` under the same key. Further top-ups add more entries; nothing is overwritten.
2. ESP32, at step 4 of its cycle, reads every entry under `pendingTopUps` and sorts them by `requestedAt`.
3. For each entry, the ESP32 sends one multi-path update: new `remainingBalance`, new `originalAllocation`, `topUpLog/{id}/appliedAt`, and `pendingTopUps/{id}` set to null (deleted).
4. Only when that update succeeds does the ESP32 add the kWh to its local balance and move to the next entry.
5. If the balance was 0 and the relay was OFF, step 5 reconnects in the same cycle.
6. Dashboard sees `appliedAt` change and marks each top-up as applied.

A Realtime Database multi-path update succeeds or fails as a whole. So a request is either applied and deleted together, or left in the queue untouched. If the ESP32 resets mid-cycle, it reloads the balance from Firebase and picks up whatever is still queued. No top-up is lost and none is applied twice.

Decision: top-ups queue. Each one represents money a tenant paid, so several made while the device is offline must all apply on reconnect.

### Landlord controls (added 30 Oct 2026)

```json
"control": {
  "disconnect": { "ch1": true, "ch2": false },
  "resetUnits": { "requestedAt": 1760440000000, "by": "adminUid" }
}
```

- `control/disconnect/chN`: admin writes `true` (Disconnect) or `false` (Reconnect) from the channel card. The ESP32 reads the whole `control` node once per sync (10 s) and holds that relay OFF while the flag is true, whatever the balance. The balance is not touched and deduction carries on (it is 0 W with the relay off). The flag is kept in NVS so a reboot with no internet keeps the tenant off. The tenant page shows "Disconnected by landlord".
- `control/resetUnits`: admin writes it with "Reset all units". The ESP32 applies it with one multi-path update (every `remainingBalance` and `originalAllocation` set to 0, `control/resetUnits` deleted), then zeroes its local balances. Every tenant is then disconnected until topped up. Pending top-ups are not cancelled and apply after the reset. The daily usage chart counts the dropped balance as usage on the reset day.

Rules: `control` is readable by admin and device; `control/disconnect/{id}` also by that tenant. Admin writes `disconnect` (boolean only) and creates `resetUnits`; the device may only delete `resetUnits`.

### Display units (changed 30 Oct 2026)

Firebase, the firmware and the serial log keep kWh. Both dashboard pages show and accept Wh (kWh x 1000): balances with 1 decimal on admin pages and 0 on the tenant page, top-ups as whole Wh. Top-up and request entry is 1 to 500000 Wh (0.001 to 500 kWh stored). The tenant request minimum in the rules dropped from 1 kWh to 0.001 kWh. The top-up log CSV column is `wh`. This replaces "kWh" and "units" wherever they appear in the page sections below.

## Auth and security rules

Every account signs in with email and password. The role lives in `users/{uid}/role`, and the RTDB security rules check it on every read and write. The UI hiding a button is not security; the rules are.

| Path | Read | Write | Validation |
| --- | --- | --- | --- |
| `device` | admin | device | `lastSeen` is a number |
| `channels/{id}/live`, `history`, `remainingBalance`, `originalAllocation`, `relayStatus` | admin; tenant whose `users/{uid}/channel` equals `{id}` | device | balance is a number of 0 or more; relay is "ON" or "OFF" |
| `channels/{id}/tenantName` | admin; that tenant | admin | string, 1 to 40 characters |
| `channels/{id}/pendingTopUps` | admin; device | admin writes; device may only delete | `kwh` is a number above 0 and at most 500 |
| `topUpLog/{id}` | admin; tenant only for entries where `channel` is theirs | admin creates; device may only set `appliedAt` | `kwh` matches the rule above |
| `users/{uid}` | admin; the user themselves | admin only (set up in Firebase Console for the prototype) | role is admin, tenant or device |

Device login: the ESP32 signs in as its own user (role `device`), not with the database secret. Store its email and password in NVS (Preferences), not hard-coded in the sketch you show at defence.

Tenant reads of `topUpLog` need a query rule (`orderByChild('channel')`) or a per-channel copy of the log. The simpler option for a 4-tenant prototype: also write each entry under `channels/{id}/topUps/{requestId}` so tenants read their own branch.

The 500 kWh cap is a typo guard. Pick a figure you can justify; it should be well above a realistic month for one room.

## Login

One login page for everyone. After sign-in the app reads `users/{uid}/role` and sends the person to their own view.

| Role found | Goes to |
| --- | --- |
| `admin` | Admin overview |
| `tenant` with a `channel` | Tenant page for that channel |
| `tenant` with no channel, no record, or `device` | Stays on login with "Your account is not linked to a room yet" and is signed out |

Page contents: email, password with a Show/Hide button, "Keep me signed in on this device" (checked by default), Sign in, "Forgot password?". No sign-up link; the landlord creates accounts.

Behaviour:

- Keep me signed in: Firebase `browserLocalPersistence` when checked, `browserSessionPersistence` when not.
- Already signed in: skip the login page and route by role straight away. Show a short loading state while the role is read.
- Every page except login checks auth on load. No user, or the wrong role for that page, goes back to login.
- Errors: one generic message, "Email or password is wrong", for both a wrong password and an unknown email, so the page doesn't reveal which emails have accounts. Firebase's `too-many-requests` shows "Too many attempts. Wait a few minutes or reset your password."
- Forgot password: sends Firebase's reset email and always shows "If that email has an account, a reset link is on its way".
- First login: tenants get a temporary password from the landlord. Prompt them to change it on first sign-in (a `mustChangePassword` flag in `users/{uid}`, cleared after the change; the rules let a user write only this field on their own record, and only to false).
- Sign out: button in the header on every page; clears the session and returns to login.

The ESP32 device account must not be usable on the dashboard. Routing it to "not linked" and signing it out covers that.

## Admin pages

The admin gets 4 pages behind one login. After sign-in, the app reads `users/{uid}/role` and routes admin to Overview, tenant to the tenant page, and anyone else to an "account not set up" message.

### 1. Overview (home)

One card per channel in a grid (2 columns on a laptop, 1 on a phone). A header strip above the cards shows device status and building totals.

Header strip:

- Device status: "Online" if `device/lastSeen` is under 30 s old, else "Offline, last seen 4 min ago".
- Total live power across all channels, in W.
- Supply voltage (from any online channel), in V.

Each channel card:

- Tenant name and channel ID.
- Relay badge: green "Supply ON" or red "Disconnected".
- Remaining balance in kWh, large type.
- Bar showing percent of `originalAllocation` left. Amber at 10% or below (matches the buzzer threshold), red at 0.
- Live row: V, A, W, PF.
- "Top up" button that opens the top-up form pre-set to this channel.
- Whole card links to Channel detail.

If a channel's `live.ts` is over 30 s old while the device is online, grey out its live row and show "No reading". That flags a dead PZEM or a Modbus address fault on that one channel.

### 2. Channel detail

- Same header as the card, bigger.
- Live power line chart for the last 24 hours, from `history`.
- Daily kWh bar chart for the last 7 days, computed as the drop in `balance` plus any top-ups that day, or from the PZEM `energy` counter difference.
- Estimated time left: `remainingBalance / average power over the last hour`, shown as hours. Hide it when average power is under 5 W.
- This channel's top-up history table, newest first.
- Edit tenant name.

### 3. Top up units

- Fields: channel (dropdown), kWh (number, 0.1 to 500, 2 decimals).
- Confirm step: "Add 10.00 kWh to Room A? Balance goes from 0.42 to 10.42 kWh."
- After submit, the row shows "Pending" until `appliedAt` is set, then "Applied".
- If the device is offline at submit time, warn: "Device offline. Units will apply when it reconnects."
- If still pending after 60 s with the device online, show "Not yet applied, check the device".

### 4. Top-up log

One table for all channels, newest first: date and time, channel, tenant, kWh, admin, status (Pending or Applied), applied time. A channel filter and a CSV export button. This log is your evidence in Chapter 4 that allocations reached the hardware.

Left out on purpose: a manual relay ON/OFF switch. It would need its own firmware path and a rule on who wins when the balance says OFF. Add it later only if your supervisor asks.

## Tenant page

One page, phone-sized, read-only. The tenant's channel comes from `users/{uid}/channel`; the page never takes a channel from the URL.

Top to bottom:

1. Greeting with tenant name.
2. Balance block: remaining units in large type, percent bar against `originalAllocation`, and the relay state ("Supply ON" or "Disconnected, units exhausted").
3. Low-balance banner at 10% or below: "You have 1.8 units left (9%). Request a top-up below." Same threshold as the buzzer, so what they hear and what they see agree.
4. Live now: current power in W and the estimated hours left, same rule as the admin detail page.
5. Usage: today's kWh and a 7-day bar chart.
6. My top-ups: date, units, status.

The tenant page hides voltage, current and power factor by default. A "Show details" toggle reveals them. Most tenants only care about units left and how fast they're going.

Empty states:

- Device offline: "Readings paused. Last update 12 min ago." Balance still shows the last known value.
- Account not linked to a channel: "Your account isn't linked to a room yet. Ask the admin."

## Live data behaviour and number formatting

The page subscribes once with `onValue` and redraws only what changed. No `setInterval` polling of the database. A 1 s local timer only re-computes "last seen" text and staleness.

Staleness uses Firebase's clock, not the browser's. Read `.info/serverTimeOffset` once and compare `ts` against `Date.now() + offset`. A phone with a wrong clock would otherwise show every channel as offline.

| Quantity | Unit | Decimals | Example |
| --- | --- | --- | --- |
| Voltage | V | 1 | 229.8 V |
| Current | A | 3 | 0.215 A |
| Active power | W | 1 | 48.7 W |
| Power factor | none | 2 | 0.98 |
| Energy, balance, top-up | kWh | 3 on admin pages, 2 on tenant page | 12.345 kWh / 12.35 units |
| Frequency | Hz | 1 | 50.0 Hz |
| Percent left | % | 0 | 62% |

Thresholds the UI uses (all from data already in the tree):

| Condition | Rule | Shown as |
| --- | --- | --- |
| Low balance | remaining / original at or below 0.10 | Amber bar, tenant banner |
| Exhausted | remaining at or below 0 | Red bar, "Disconnected" |
| Device offline | `lastSeen` over 30 s old | Grey header, "Offline" |
| Channel stale | `live.ts` over 30 s old, device online | Grey live row, "No reading" |
| Relay mismatch | balance above 0 but relay OFF for over 2 cycles | Admin-only warning on the card |

The 30 s figure assumes the ESP32 syncs every 5 to 10 s. Set it to 3 times the real sync interval once the firmware's interval is fixed.

The relay-mismatch check catches a stuck relay or a failed MCP23017 write. It's cheap to add and gives you a fault-detection point to discuss in Chapter 4.

## Tenant top-up requests

A tenant can ask for units from their page; the admin approves or declines. Approval runs the normal top-up flow, so the ESP32 side does not change.

- New path: `topUpRequests/{id}` with `channel`, `kwh`, `requestedAt`, `status` ("waiting", "approved", "declined"), `decidedAt`.
- Rules: a tenant may create an entry only for their own channel, with `status` "waiting" and `kwh` from 1 to 500. Only the admin may change `status`. A tenant reads only their own entries.
- Approve: the dashboard writes `status` "approved" and creates the top-up (`topUpLog` + `pendingTopUps` entry) in one multi-path update.
- The admin overview shows waiting requests in a panel with Approve and Decline buttons. The tenant sees their request status in "My top-ups".

Tenants never see other rooms: no names, balances, usage or building averages. The security rules enforce this, not only the UI.

## Tenant accounts

The admin creates tenant logins from the Tenants page using a second Firebase app instance in the browser, so the admin stays signed in. No server or paid plan needed.

1. Admin enters the tenant's email, a temporary password and the channel.
2. The second instance creates the account and returns its `uid`, then signs itself out.
3. The admin's own session writes `users/{uid}` with role `tenant` and the channel.

- Remove a tenant: the admin unlinks the account (deletes `users/{uid}` or clears `channel`). The login survives but the rules give it no access.
- Forgotten password: "Send reset email" calls Firebase's password reset.
- Not possible without a server: deleting an account or setting another user's password. Do those in the Firebase Console if ever needed.
- Open sign-up is harmless here: an account with no `users/{uid}` record can read nothing and sees "Account not set up".

## Display mapping

Gauges only where a value has a fixed range; exact readings as numbers; anything over time as a chart. A mockup of both screens sits beside this spec.

| Item | Admin (laptop) | Tenant (phone) |
| --- | --- | --- |
| Units left | Number plus horizontal bar per card (bars line up across 4 rooms) | Ring gauge, units in the centre, ring = percent of `originalAllocation` |
| Bar or ring colour | Green above 10%, amber at 10% or below, red at 0 | Same |
| Relay state | Pill: "Supply ON" or "Disconnected" | Same pill under the gauge |
| Low or exhausted | Amber summary tile in the header | Amber or red banner across the top |
| V, A, W, PF | Row of 4 number tiles per card | Power as a large number; V, A, PF behind "Show meter details" |
| Time left | Channel detail page, text | Text tile: "About 17 days" |
| Usage | Line chart 24 h power, bar chart 7 days kWh (detail page) | Bar chart, 7 days |
| Device health | Dot + text in the status strip | "Updated 3 s ago" footer |
| Top-ups and requests | Requests panel, recent top-ups table, full log page | Request form, "My top-ups" list |

No power gauge: the 10 A and 100 A PZEMs have different full scales (about 2.3 kW and 23 kW), so a 50 W load would barely move a needle on the larger one.

## Firmware and thesis knock-on changes

This spec touches the firmware in 5 places and the thesis in 3. Settle them before Claude Code writes either side, or the two will disagree on field names.

| Area | Change | Cycle step |
| --- | --- | --- |
| Firmware | Read every entry in `pendingTopUps`, oldest first; apply each with one multi-path update (balance, `originalAllocation`, `appliedAt`, delete entry); add to local balance only after it succeeds | Step 4, Check for new allocation |
| Firmware | Write `relayStatus` only after the MCP23017 write returns success | Step 5, Disconnect/reconnect |
| Firmware | Write `device/lastSeen` and each channel's `live` block with server timestamps | Step 6, Sync |
| Firmware | Append a `history` sample per channel every 5 minutes (separate timer from the sync interval) | Step 6, Sync |
| Firmware | Sign in as the device user, credentials stored in NVS | Step 1, Initialize |
| Chapter 3 | Firebase JSON tree section: replace with this tree; state the write-ownership rule once in prose above it | n/a |
| Chapter 3 | Add the top-up handshake as a short flow and explain why the dashboard never writes the balance | n/a |
| Chapter 4 | Dashboard screenshots, the top-up log as evidence of allocation reaching hardware, and the acceptance tests below | n/a |

The 7-step cycle order stays as it is. The handshake depends on Check for new allocation running before Disconnect, which your cycle already does.

## Acceptance tests for Chapter 4

Run these on the 4-channel rig once the dashboard is live. Record the measured value for each so they drop straight into Chapter 4 tables.

- [ ] Live update latency: switch a 50 W bulb on, time until the card shows about 50 W. Target under 10 s.
- [ ] Top-up round trip: submit 0.05 kWh to a disconnected channel, time from submit to "Applied" and from submit to relay click. Target under 15 s.
- [ ] Depletion: allocate 0.01 kWh to a channel with a 50 W load. Confirm amber at 10%, buzzer at the same moment, then "Disconnected" on both admin and tenant pages.
- [ ] Isolation: while ch1 is disconnected, confirm ch2 to ch4 keep reading and stay ON.
- [ ] Offline handling: unplug the ESP32. Dashboard shows "Offline" within 30 s. Submit 2 top-ups to one channel while offline; after reconnect the balance rises by their sum, and each applies exactly once.
- [ ] Access control: sign in as tenant ch1 and try to read ch2 from the browser console. The read must fail with permission denied.
- [ ] Tenant write block: as a tenant, try to write `pendingTopUps` from the console. Must fail.
- [ ] Accuracy cross-check: compare dashboard V and W with a reference multimeter or plug-in meter on one channel. Note the difference in percent.
- [ ] Phone layout: open the tenant page on a 360 px wide phone. No sideways scrolling, balance readable without zoom.
