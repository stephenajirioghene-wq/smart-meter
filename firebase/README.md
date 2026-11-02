# Firebase setup

These are the Realtime Database security rules for the sub-metering system. They implement the "Auth and security rules" section of `Sub-meter dashboard spec.md`, along with the tenant top-up request and `mustChangePassword` rules.

## Files

| File | Purpose |
| --- | --- |
| `database.rules.json` | RTDB security rules (paste into the Console or deploy with the CLI) |
| `firebase.json` | Only needed if you deploy with the Firebase CLI (`firebase deploy --only database`) |

## One-time setup in the Firebase Console

1. Create the project and add a **Realtime Database**. Start it in locked mode.
2. Go to **Authentication > Sign-in method** and enable **Email/Password**.
3. Go to **Authentication > Users** and add 2 accounts: the admin and the ESP32 device. Copy each account's UID.
4. Go to **Realtime Database > Data** and add the role records by hand. The rules stop anyone else from doing this, because no admin exists yet:
   ```
   users/<adminUid>   { "role": "admin" }
   users/<deviceUid>  { "role": "device" }
   ```
   You create tenant accounts later from the dashboard Tenants page.
5. Go to **Realtime Database > Rules**, paste in `database.rules.json`, and click Publish.
6. Go to **Authentication > Settings > Authorized domains** and add your Netlify domain once you have one.

## Who can do what

| Path | Read | Write |
| --- | --- | --- |
| `device` | admin | device |
| `channels/{ch}/live`, `history`, `remainingBalance`, `originalAllocation`, `relayStatus` | admin, device, tenant of `{ch}` | device |
| `channels/{ch}/tenantName` | admin, device, tenant of `{ch}` | admin |
| `channels/{ch}/pendingTopUps/{id}` | admin, device | admin; the device may only delete, and only after `topUpLog/{id}/appliedAt` is set in the same update |
| `topUpLog/{id}` | admin; a tenant only through the query `orderByChild('channel').equalTo(<own channel>)` | admin creates (no edits, no deletes); the device may set `appliedAt` once, in the same update that deletes the pending entry |
| `topUpRequests/{id}` | admin; a tenant only through the same channel query | a tenant creates for their own channel with `status` "waiting" and 1 to 500 kWh; the admin changes `waiting` to `approved` or `declined` once |
| `users/{uid}` | admin; the user themselves | admin; the user may set only their own `mustChangePassword` to `false` |

The top-up handshake is enforced in both directions. A `pendingTopUps/{id}` entry must have a matching `topUpLog/{id}` entry with the same channel and kWh. The device can only delete a pending entry and stamp `appliedAt` together, in one multi-path update. So a top-up can't be marked applied while it is still queued, or dropped from the queue without being marked applied.

The device can read the whole `channels` branch. The spec lists only `pendingTopUps` for the device, but the handshake also requires the ESP32 to reload `remainingBalance` and `originalAllocation` from Firebase after a reset.

`users/{uid}/email` is an optional field. The admin writes it so the Tenants page can show which login belongs to which room.

## Quick checks (Rules Playground in the Console)

In **Realtime Database > Rules > Rules Playground**, tick "Authenticated" and paste a UID that has the role you want to test.

| As | Operation | Expected |
| --- | --- | --- |
| tenant ch1 | read `/channels/ch2/remainingBalance` | denied |
| tenant ch1 | read `/channels/ch1/remainingBalance` | allowed |
| tenant ch1 | read `/channels/ch1/pendingTopUps` | denied |
| tenant ch1 | set `/channels/ch1/pendingTopUps/x` = `{"kwh":5,"requestedAt":1}` | denied |
| tenant ch1 | read `/topUpLog` (no query) | denied |
| tenant ch1 | set `/users/<ownUid>/mustChangePassword` = `false` | allowed |
| tenant ch1 | set `/users/<ownUid>/role` = `"admin"` | denied |
| device | set `/channels/ch1/relayStatus` = `"MAYBE"` | denied |
| admin | set `/channels/ch1/remainingBalance` = `50` | denied (only the device writes the balance) |

The Playground can't run a filtered query or a multi-path update. You'll test those from the dashboard in stage 2: the tenant's "My top-ups" list and the admin's top-up submit.
