# Wavin ESP32 Bridge — Integration Guide

Everything a controlling application needs to talk to the bridge. The device is a small HTTP/JSON
server on the LAN that fronts a Wavin AHC9000 underfloor-heating controller over RS-485.

- **Base URL:** `http://wavin.local` (mDNS) or `http://<ip>`
- **Content type:** `application/json` throughout
- **Auth:** reads are open on the LAN; **writes require `X-Api-Token`**
- **Transport:** plain HTTP (no TLS). Treat it as a LAN-only device — see [Security](#security).

---

## 1. Model

| Concept | Meaning |
|---|---|
| **Room** | A thermostat and the floor loop(s) it controls. Identified by `id` (1-based, stable). |
| `current` | Measured air temperature. |
| `desired` | The setpoint the controller is actually working to (read-only on the device). |
| `manual` | The manual-mode setpoint — **this is what you usually write**. |
| `comfort` / `eco` / `standby` / `holiday` / `party` | The setpoints those modes use. All writable. |
| `output` | `true` when the room is calling for heat (its actuator is open). |
| `mode` | Which setpoint is in force — see [Modes](#modes). |
| `durationMin` | How long a party or holiday lasts — see [Party and holiday](#party-and-holiday). |
| `cooling` | The room's control loop is **inverted** (opens above setpoint). |
| **Relay** | A controller output. Typically one drives the **circulation pump**, one signals the **heat source**. `id` is 1-based. |

**Reads never touch the RS-485 bus.** They are served from a cache refreshed by a background poll
(one room every ~3 s, a full sweep every 30 s). So you may poll the HTTP API as often as you like —
it costs nothing on the bus. **Writes** go straight to the controller and then re-read that object,
so the response reflects real state, not an echo of your request.

---

## 2. Endpoints

### `GET /health`

```json
{ "status": "ok", "serial": "up", "rooms": 10, "lastPollMs": 4210,
  "uptimeS": 3600, "ip": "192.168.0.42", "rssi": -58,
  "modbusOk": 420, "modbusErr": 3, "relayOverride": false, "chipTemp": 48.3,
  "diag": { "silent": 0, "crcFail": 0, "runt": 0, "badFrame": 3,
            "rxBytes": 8904, "leadJunk": 423,
            "lastFrame": "01 43 10 00 F2 ...", "longestFrame": "01 43 10 00 F2 ...",
            "baud": 38400, "stopBits": 1, "autoDir": false,
            "rxPin": 16, "txPin": 17, "dePin": 4 } }
```

| Field | Use |
|---|---|
| `status` | `ok` \| `degraded` |
| `serial` | `up` \| `down` — **is the RS-485 link alive** (no successful read for ~75 s ⇒ `down`) |
| `lastPollMs` | age of the last completed sweep, ms |
| `modbusOk` / `modbusErr` | cumulative counters; a climbing `modbusErr` means bus trouble |
| `relayOverride` | **true if a relay is currently suppressed** — see §4 |
| `chipTemp` | ESP32 **die** temperature °C, or `null` if the chip has no sensor. Not room temperature — it runs well above ambient. Useful only as a trend (enclosure overheating) |
| `diag` | **Bring-up and bus diagnostics.** Not part of the control contract — a client should never branch on it. See below |

`diag` breaks `modbusErr` into causes, which is what you need when the bus misbehaves:
`silent` (no reply at all) points at wiring or direction control; `crcFail` (reply arrived,
mangled) points at framing or noise; `runt` at truncated frames; `badFrame` at a protocol
mismatch. `lastFrame` / `longestFrame` are hex dumps of what actually came back — `longestFrame`
is kept separately because single-byte line glitches would otherwise overwrite the only real
reply. `leadJunk` counts bytes discarded ahead of the unit id from RS-485 driver turnaround;
about one per transaction is normal. The remaining fields echo the compiled-in serial settings
so you can confirm what was actually flashed. Counters are cumulative since boot and never reset.

Poll this on your own schedule. Treat `serial: "down"` as *"all room data is stale, do not act on
it"*.

### `GET /rooms` · `GET /rooms/{id}`

```json
[ { "id": 1, "syncGroup": 3,
    "current": 22.4, "desired": 21.0, "floor": 23.1,
    "manual": 21.0, "comfort": 22.0, "eco": 19.0, "standby": 10.0, "holiday": 12.0,
    "party": 24.0, "durationMin": 0,
    "mode": "manual", "output": true,
    "alive": true, "lowBattery": false,
    "humidity": 41, "battery": 80,
    "rssi": -61.5, "rssiUnit": -58.0, "cooling": false } ]
```

Unknown sensor values are **`null`**, never `0` — always null-check `floor`, `humidity`,
`desired` and the setpoints. Temperatures are °C rounded to 1 decimal. `alive: false` means that thermostat has not
reported in ~25 minutes: its `current` is stale, so **skip it in control decisions**.

### `POST /rooms/{id}`

Requires `X-Api-Token`. Send any combination of:

```jsonc
{ "manualTemp": 21.5,                   // setpoints, 0–30 °C each
  "comfortTemp": 22, "ecoTemp": 19, "standbyTemp": 10, "holidayTemp": 12, "partyTemp": 24,
  "mode": "comfort",                    // see Modes below
  "durationMin": 180,                   // only with mode party / holiday
  "cooling": true }                     // invert the loop for this room
```

The whole request is validated before anything is written, so a `400` means nothing changed. Then
setpoints are written first, then `mode`, then `cooling` — so `{"comfortTemp":22,"mode":"comfort"}`
switches straight to the new temperature. Returns the **updated room object**.

A setpoint only matters while its mode is in force: `manualTemp` in `manual`, `comfortTemp` in
`comfort`, and so on. Setting one does not change the mode.

#### Modes

| `mode` | Setpoint used | Week schedule | Ends |
|---|---|---|---|
| `manual` | `manual` | off | — |
| `comfort` / `eco` / `standby` | that mode's | off | — (permanent) |
| `schedule` | comfort/eco from the week schedule | on | — |
| `temp-comfort` / `temp-eco` | that mode's | stays on | controller-defined, see below |
| `party` | `party` | kept as it was | after `durationMin` |
| `holiday` | `holiday` | kept as it was | after `durationMin` |

`GET` can also report `party-schedule` and `holiday-schedule` — the same modes, on a room that is
on its week schedule. `temp-standby` exists on the controller but the OEM documentation says not
to use it, so the bridge does not offer it.

`temp-comfort` / `temp-eco` override the week schedule without turning it off. The OEM
documentation does not say when the controller ends them — most likely at the schedule's next
switch. Check on your installation before relying on it.

#### Party and holiday

```jsonc
{ "mode": "party" }                                            // stored temp + duration
{ "mode": "party", "partyTemp": 24, "durationMin": 180 }        // 24 °C for 3 h
{ "mode": "holiday", "holidayTemp": 12, "durationMin": 10080 }  // 12 °C for a week
```

Both hold the room at their own setpoint, `party` or `holiday`. **`partyTemp` is an absolute
temperature**, not an increment. The OEM register map calls it an increment, but on a real
AHC9000 `desired` becomes the stored value itself. `durationMin` is 2–1440 for party and 2–131070 (~91 days) for holiday, stored in 2-minute steps,
so odd values round up. Everything is optional; anything you leave out keeps what the controller
already has, which `GET /rooms/{id}` shows. The **controller runs the timer itself** and falls
back when it ends, so it does not need your client to stay up.

Whether the room is on its week schedule is preserved: a room in `schedule` goes to
`party-schedule` / `holiday-schedule` and returns to its schedule afterwards; a `manual` room
returns to manual. To end early, set the `mode` you want back.

Party and holiday share **one duration register** on the controller. Starting one changes
`durationMin` for the other, and so does setting either at the panel. During an override it
**counts down to 0**, so `GET` shows the time remaining, and the room drops back to its previous
mode when it gets there (measured on a real AHC9000).

### `GET /relays` · `GET /relays/{id}` · `POST /relays/{id}`

```json
[ { "id": 1, "known": true, "assignMap": 65535,
    "enabled": true, "overridden": false, "restoreInS": 0, "event": "on" } ]
```

`assignMap` is the controller's channel bitmap; `0` means no channel can switch that relay.
`event` is the controller's timer state (`on`, `idle`, `start-delay`, …).

```jsonc
POST /relays/2   { "enabled": false, "restoreAfterS": 1800 }   // suppress for 30 min
POST /relays/2   { "enabled": true }                           // restore now
```

See §4 before using this — it has safety semantics you must understand.

### `GET /mqtt` · `POST /mqtt`

The bridge runs a small MQTT broker on port 1883. Whatever other devices
publish to it is cached — last value per topic — and served here, so a client
that already speaks REST does not need an MQTT library to read it.

```json
[ { "topic": "watts/2412000123/measurement",
    "payload": "{\"positive_active_power\":3104,\"voltage_l1\":232.4}",
    "bytes": 48, "ageMs": 1840, "count": 921 } ]
```

| Field | Use |
|---|---|
| `payload` | the message as text, when every byte is printable ASCII |
| `payloadHex` | **replaces** `payload` when the bytes are not printable — binary payloads cannot go in a JSON string |
| `bytes` | payload length actually stored |
| `ageMs` | time since the last publish on this topic — **the freshness check** |
| `count` | publishes seen on this topic since boot |
| `truncated` | present and `true` if the payload was longer than `MQTT_MAX_PAYLOAD_LEN` |

`GET /mqtt?topic=watts/2412000123/measurement` returns the single entry, or
`404` if nothing has ever been published to it.

`POST /mqtt` (**auth**) publishes to connected subscribers:

```bash
curl -X POST http://wavin.local/mqtt \
     -H "X-Api-Token: $TOKEN" -H "Content-Type: application/json" \
     -d '{"topic":"home/heating/state","payload":"idle"}'
```

Nothing is persisted. After a reboot the cache is empty until each device
publishes again, and `ageMs` is the only thing that tells you so — a topic that
has gone silent keeps returning its last value indefinitely. **Always check
`ageMs` before acting on a payload.**

### Errors

```json
{ "error": "out_of_range", "message": "manualTemp must be 0..30" }
```

| Code | Meaning | Your move |
|---|---|---|
| `400` | validation / bad JSON | fix the request; don't retry unchanged |
| `401` | missing or wrong token | fix config |
| `404` | unknown room/relay/path | re-read `/rooms` |
| `409` | Modbus write failed | **retry with backoff** — the bus was busy |

`409` is the only one worth retrying automatically. Use a few attempts with backoff; if it persists,
check `/health`.

⚠️ **`Content-Type: application/json` is mandatory on POST.** Without it the ESP32 web server
parses the body as form data and you get `400 bad_json`.

---

## 3. Writing a controller

### Polling

Poll `/rooms` every **15–30 s**. Faster gains nothing: the underlying data only changes when the
wireless thermostats report in, and the bridge itself sweeps every 30 s. Underfloor heating is a
slow system — design your loop in minutes, not seconds.

### Before acting on data

```
if health.serial != "up"      -> data is stale, do nothing
if room.alive == false        -> skip this room
if room.current == null       -> skip this room
```

### Don't fight the thermostats

Writes are persistent settings on the controller, not momentary commands. Write **only on change**
— compare the desired value against what `/rooms` reports and skip the write if it already matches.
Re-writing the same setpoint every cycle wears the controller's flash and floods the RS-485 bus.

### Suggested mapping to a time-series store

| API field | Series |
|---|---|
| `current` | `R{id}` |
| `output` | `V{id}` (1/0) |
| `desired` | `RD{id}` |

---

## 4. Heat redistribution — the safety-critical part

The bridge can move heat between rooms rather than generate it: put an over-warm room into
**cooling** so its loop absorbs heat into the circulating water, and let cold rooms in heating
release it.

### The physical condition

**Heat only flows into the water if the water is cooler than the warm room.** So this only works
as a **pump-on / heat-off** mode:

- the circulation pump must run, and
- the heat source must be **off**, or supplying below the warm room's temperature.

If the heat pump is pushing 30 °C water while the warm room is at 24 °C, putting that room into
cooling **heats it further** — the opposite of the intent. This is the failure mode to guard against.

### Sequence

```
1. GET /health            -> serial == "up", else abort
2. GET /rooms             -> find warm (current > target + hysteresis)
                             and cold (current < target - hysteresis) rooms
3. POST /relays/{heat}    { "enabled": false, "restoreAfterS": 1800 }   // stop the call for heat
4. POST /rooms/{warm}     { "cooling": true }                           // absorb
   (cold rooms stay in normal heating; their valves open and release)
5. ... wait / monitor ...
6. POST /rooms/{warm}     { "cooling": false }                          // ALWAYS undo
7. POST /relays/{heat}    { "enabled": true }                           // restore heat
```

You must **identify which relay is the heat source** empirically — toggle one and observe. The
controller does not label them. Leave the **pump relay enabled** or nothing circulates.

### Why relay overrides expire on their own

Clearing a relay's assignment map is a **non-volatile change on the controller**. If your
application crashes, loses the network, or simply forgets step 7, the house would stop heating
indefinitely with nothing to explain why.

So the bridge enforces safety itself:

- **Time-limited.** Every override carries a deadline. `restoreAfterS` defaults to **30 min** and is
  **hard-capped at 4 h**; there is no indefinite disable.
- **Persisted.** The original bitmap is written to NVS *before* clearing, so a reboot mid-override
  restores it on boot instead of keeping it.
- **Self-restoring.** The deadline is enforced on-device, independent of your application.
- **Visible.** `relayOverride` in `/health`, and `overridden`/`restoreInS` per relay.

**Design your controller to match**: re-assert the override periodically (each POST extends the
deadline) rather than requesting one long window, so that if *you* die, heat comes back within
minutes. Treat auto-restore as a backstop, not the normal path — always explicitly restore.

### Expectations

Transfer is **slow and modest** — driven by a small ΔT between rooms and limited by flow and loop
length. Good for evening out afternoon solar gain; not room-to-room air conditioning. Condensation
is *not* a concern (the water is at room temperature, unlike real chilled-water cooling).

---

## 5. Security

The bridge is **HTTP only, LAN only**. It has a shared-secret token on writes and nothing else — no
TLS, no rate limiting, no lockout.

- **Do not port-forward it.** For remote access use a VPN, or put a properly hardened service
  (TLS, real auth) in front and let *that* talk to the bridge on the LAN.
- The token travels in cleartext, so it protects against accidents on a trusted LAN, not against an
  attacker on the wire.
- Reads are unauthenticated by design; the token guards writes only.

---

## 6. Quick reference

```
GET  /health                                     link + device status
GET  /rooms                 GET  /rooms/{id}     room state (cached)
POST /rooms/{id}            {*Temp, mode, durationMin, cooling}   auth
GET  /relays                GET  /relays/{id}    relay state
POST /relays/{id}           {enabled, restoreAfterS}       auth
GET  /mqtt                  GET  /mqtt?topic=a/b cached MQTT last values
POST /mqtt                  {topic, payload}               auth
```

```bash
curl http://wavin.local/rooms

curl -X POST http://wavin.local/rooms/1 \
     -H "X-Api-Token: $TOKEN" -H "Content-Type: application/json" \
     -d '{"manualTemp":21.5}'
```

Protocol-level detail (registers, function codes) is in
[WAVIN_REGISTER_MAP.md](WAVIN_REGISTER_MAP.md) — you do not need it to write a client.
