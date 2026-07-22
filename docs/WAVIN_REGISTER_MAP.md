# Wavin AHC9000 (Jablotron AC-116) — Authoritative Modbus Register Map

Transcribed from the OEM document *"Wavin Modbus beskrivelse for AHC 9000 styreenhed"*
(**AC-116 Modbus Register Map, © 2013 Jablotron Alarms a.s.**). This is the source of truth for
the Wavin integration — it supersedes the reverse-engineered values in the legacy j2mod code.
Referenced by [WAVIN_SERVICE_SPEC.md](WAVIN_SERVICE_SPEC.md) and [WAVIN_ESP32_SPEC.md](WAVIN_ESP32_SPEC.md).

---

## 1. Physical layer (OEM Table 5.1)

| Parameter | Value |
|---|---|
| Bus | Modbus **RTU** over RS-485 |
| Baud | **38400** |
| Data bits | 8 |
| Stop bits | **1** |
| Parity | none |

→ **38400 8N1.** (The legacy j2mod/pywavin code used 8N2; that was reverse-engineered. Use **8N1**
per the OEM doc; only try 8N2 if you cannot get valid reads.)

**Addressing:** every control unit responds at logical address **0x01** regardless of any assigned
address. A single-unit install (your case) needs **no enumeration** — just use unit id 1.
Multi-unit chains use the `0x6D` enumeration commands (§4.7).

**Byte order:** register values are **MSB first**. A 32-bit value spanning two registers has the
**lower-address register = lower 16 bits** (little-endian at the register level, e.g. `ADDRESS_L`
then `ADDRESS_H`).

---

## 2. Register model

Registers are 16-bit, grouped into **Categories → Pages → Register Index**. Temperatures are
**signed, 0.1 °C per LSB** (e.g. 45.5 °C = 455 = 0x01C7). Unknown temp/humidity reads back
**0x7FFF**.

| Category | id | Pages | Regs/Page | Meaning of "page" |
|---|---|---|---|---|
| MAIN | 0x00 | 1 | 31 | (global) |
| ELEMENTS | 0x01 | 48 | 13 | element (thermostat/sensor) index |
| PACKED_DATA | 0x02 | 17 | 17 | room / sync-group (channel) index |
| CHANNELS | 0x03 | 17 | 4 | channel (actuator) index |
| RELAYS | 0x04 | 2 | 7 | relay index |
| CLOCK | 0x05 | 1 | 7 | (global) |
| SCHEDULES | 0x06 | 17 | 22 | channel index |
| INFO | 0x07 | 1 | 5 | (global) |

---

## 3. Categories

### 3.1 MAIN (0x00), single page
| Idx | Register | Notes |
|---|---|---|
| 0x00–0x03 | ELEMENT CHANGE FLAGS 0..3 | per-element "changed" bits; **cleared on read** |
| 0x04–0x05 | CHANNEL CHANGE FLAGS L/H | per-channel changed bits (cleared on read) |
| 0x06–0x07 | PACKED DATA CHANGE FLAGS L/H | per-room changed bits (cleared on read) |
| 0x08 | **STATUS L** | see bits below |
| 0x09 | STATUS H | reserved |
| 0x0A–0x0B | LEARN MASK L/H | pairing: channel select bitmap |
| 0x0C | LEARN CHANNEL | primary channel for learning |
| 0x0D | LAST LEARNED ELEMENT INDEX | element idx+1 after learn; clear to 0 |
| 0x0E | **DHW SENSOR TEMPERATURE** | 0.1 °C (read) |
| 0x0F | **INLET SENSOR TEMPERATURE** | 0.1 °C (read) |
| 0x10–0x11 | **TOTAL CURRENT** L/H (32-bit) | actuators; **P_tot = 24·TCURR/0.54 [mW]** (0.54 mA/LSB) |
| 0x12 | CPU TEMPERATURE | (listed in overview; detail marks reserved) |
| 0x13 | INPUT VOLTAGE | (listed in overview; detail marks reserved) |
| 0x14 | **DHW TEMPERATURE COMFORT** | 0.1 °C (r/w) |
| 0x15 | **DHW TEMPERATURE ECO** | 0.1 °C (r/w) |
| 0x16 | **DHW TEMPERATURE CLEANING** | 0.1 °C (r/w) |
| 0x17 | **DHW TEMPERATURE STANDBY** | 0.1 °C (r/w) |
| 0x18 | DHW CLEANING SCHEDULE | `DAYOFWEEK[4:0]`(0=Mon) `HOUR[4:0]` `MINUTE[5:0]` |
| 0x19 | HIGH TEMP CUT OFF TEMPERATURE | 0.1 °C |
| 0x1A | AUTO KICK IN TEMPERATURE | 0.1 °C (cutoff recovery) |
| 0x1B | HIGH TEMP CUT OFF DELAY | seconds |
| 0x1C | ACTUATOR ACTIVATION INTERVAL | seconds (anti-seize cycling) |
| 0x1D | ACTUATOR ACTIVATION DURATION | seconds |
| 0x1E | ACTUATOR POLARITY | per-channel: 0=NC, 1=NO |

**STATUS L (0x08) bits:** `GLOB STBY`(15, external standby active), `DHW ENA`(13, r/w — enables DHW
on channel 16), `HTC ENA`(12, r/w — high-temp cutoff), `INLET SENS`(11), `DHW SENS`(10),
`RTC UPD`(1), `RTC VALID`(0). Set these with a **masked write (0x45)**.

### 3.2 ELEMENTS (0x01), page = element 0..47
Empty element = ADDRESS_L and ADDRESS_H both 0.
| Idx | Register | Decode |
|---|---|---|
| 0x00–0x01 | ADDRESS L/H | unique 32-bit element address; write 0 to delete |
| 0x02–0x03 | ASSIGNMENT MAP L/H | bitmap of channels this element drives (`CHn AF`) |
| 0x04 | AIR TEMPERATURE | 0.1 °C, 0x7FFF = unknown |
| 0x05 | FLOOR TEMPERATURE | 0.1 °C, 0x7FFF = unknown |
| 0x06 | DEW POINT TEMPERATURE | 0.1 °C, 0x7FFF = unknown |
| 0x07 | RELATIVE HUMIDITY | %, 0x7FFF = unknown |
| 0x08 | **STATUS** | bits below |
| 0x09 | RSSI | `RSSIEL[7:0]` element side (low byte), `RSSICU[7:0]` control-unit side (high byte); dBm = `-74 + 0.5·signed(byte)` |
| 0x0A | BATTERY STATUS | `BATT[3:0]` in 10% units (0=empty,10=full) |
| 0x0B | SYNC GROUP | `SYNCG[7:0]` = element's primary channel (0..15 or 16); lowest channel of a joined room |
| 0x0C | LIVE TIMER | internal, do not use |

**Element STATUS (0x08) bits:** `ALIVE`(15, 0 after ~25 min silence), `LOST`(11), `LOW BATT`(10),
`NAT ELEM`(9, magnetic contact), `TP`(8, is a thermostat), **`TP ACT`(7, thermostat output active =
calling for heat)**, `ACTIVE`(6, magnetic-contact output active), `RFSH`(4, valid packet received).
→ **heat demand = bit 7.**

### 3.3 PACKED_DATA (0x02), page = room / sync-group 0..16
Room-level setpoints & mode, common to all elements in a channel/room. Temps 0.1 °C.
| Idx | Register | Access | Notes |
|---|---|---|---|
| 0x00 | MANUAL TEMPERATURE | r/w | setpoint for Manual mode |
| 0x01 | COMFORT TEMPERATURE | r/w | |
| 0x02 | ECO TEMPERATURE | r/w | |
| 0x03 | HOLIDAY TEMPERATURE | r/w | override temp in Holiday |
| 0x04 | STANDBY TEMPERATURE | r/w | override temp in Standby |
| 0x05 | PARTY TEMPERATURE | r/w | increment for Party mode |
| 0x06 | MODE LENGTH | r/w | override duration, **2 min/LSB** |
| 0x07 | **CONFIGURATION** | r/w | mode + flags, see below |
| 0x08 | MINIMUM TEMPERATURE | r/w | lowest user-settable |
| 0x09 | MAXIMUM TEMPERATURE | r/w | highest user-settable |
| 0x0A | FLOOR MINIMUM TEMPERATURE | r/w | heat regardless of air temp below this |
| 0x0B | FLOOR MAXIMUM TEMPERATURE | r/w | stop heat regardless of air temp above this |
| 0x0C | ALARM MINIMUM TEMPERATURE | r/w | low-temp alarm threshold |
| 0x0D | ALARM MAXIMUM TEMPERATURE | r/w | high-temp alarm threshold |
| 0x0E | HYSTERESIS | r/w | control-loop hysteresis |
| 0x0F | TEMPERATURE OFFSET | — | **removed, do not use** |
| 0x10 | **DESIRED TEMPERATURE** | **read-only** | the setpoint currently being heated to |

**CONFIGURATION (0x07) bits:** `FLOOR SENS`(r), `FLOOR ENA`, `COOL MODE` (**cooling instead of
heating**), `ADAPT MODE`, `INT LOCK`, `CTRL LOCK`, `HOTEL MODE`(r), `SCHED ENA`, `MODE[2:0]`.

**Mode = f(SCHED ENA, MODE[2:0]):**

| SCHED ENA | MODE | Meaning |
|---|---|---|
| 0 | 0 | MANUAL |
| 0 | 1 | PERMANENT STANDBY |
| 0 | 2 | PERMANENT ECO |
| 0 | 3 | PERMANENT COMFORT |
| 0 | 4 | PARTY (on manual) |
| 0 | 5 | HOLIDAY (on manual) |
| 1 | 0 | WEEK SCHEDULE |
| 1 | 1 | TEMPORARY STANDBY (do not use) |
| 1 | 2 | TEMPORARY ECO |
| 1 | 3 | TEMPORARY COMFORT |
| 1 | 4 | PARTY (with week schedule) |
| 1 | 5 | HOLIDAY (with week schedule) |

### 3.4 CHANNELS (0x03), page = channel 0..16
| Idx | Register | Decode |
|---|---|---|
| 0x00 | **TIMER EVENT** | `OUTP ON`(bit4, output active), `OVER CURR`(15), `FROST ALARM`(14), `ACT ENAB`(13, r/w), `BLKD`(12, blocked by magnetic contact), `LOST`(11), `TEVENT[3:0]` (see event codes) |
| 0x01 | CURRENT CONSUMPTION | actuator; `P_ch = 24·CURRENT/0.54 [mW]` → W = `CURRENT·24/540` |
| 0x02 | PRIMARY ELEMENT | `PRIMELEM[5:0]` (first element idx+1), `HOLID ENA`(6), `PARTY ENA`(7), `ALARM LOW`, `ALARM HIGH`, `ALL TP LOST`, `SCHED SYNC`, `SCHED CHNG`(cleared on read) |
| 0x03 | TIMER | internal, do not use |

**TEVENT codes:** 00 NO_EVENT, 01 IDLE_TIMER, 02 CUT_OFF, 03 CONTROL_BYPASS, 04 START_DELAY,
05 OUTPUT_OVERCURRENT, 06 OVERRIDE_OUTPUT_OFF, 07 OUTPUT_ON, 08 FREEZING, 09 DHW_OUTPUT_ON,
0A STOP_DELAY, 0B DHW_CLEANING, 0C OVERRIDE_OUTPUT_ON, 0D PERIODIC_CYCLE.

### 3.5 RELAYS (0x04), page = relay 0..1 (pump / boiler control)
| Idx | Register | Decode |
|---|---|---|
| 0x00 | TIMER EVENT | `TEVENT[3:0]` (same codes as channels) |
| 0x01 | ASSIGNMENT MAP | `CHn ENA` — relay on when any selected channel's actuator active |
| 0x02 | START DELAY | seconds after condition true |
| 0x03 | STOP DELAY | seconds after condition false |
| 0x04 | ACTIVATION INTERVAL | seconds (pump anti-seize) |
| 0x05 | ACTIVATION DURATION | seconds |
| 0x06 | TIMER | internal |

### 3.6 CLOCK (0x05), single page — **read/write the whole page at once**
Idx 0..6 = YEAR (2001–2099), MONTH (1–12), DAY (1–31), DAY OF WEEK (0=Mon..6=Sun), HOUR (0–23),
MINUTE (0–59), SECOND (0–59).

### 3.7 SCHEDULES (0x06), page = channel — **write whole page at once**
Idx 0 = HEADER (`KIND[7:0]`, identifier only). Idx 1..21 = 21 words = 7 days × 3 words (48 bits/day).
Each of the 48 bits = a 30-min slot (bit0 = 00:00–00:30 … bit47 = 23:30–24:00); **1 = Comfort,
0 = Eco**. (Hotel mode inverts the meaning per the doc.)

### 3.8 INFO (0x07), single page
Idx 0–1 = control-unit 32-bit address; 2 = HW VERSION (`MC110<xx>`, xx decimal); 3 = SW VERSION
(`MC610<xx>`, xx BCD, + `BETA[3:0]`); 4 = DEVICE NAME (`AC-<xxx>`, e.g. 116 → AC-116).

---

## 4. Function codes (OEM §4)

Frame body (after unit id, before CRC16). Register value bytes MSB-first.

| FC | Name | Request body | Use |
|---|---|---|---|
| **0x43** | Read Register from Index | `Category, Index, Page, Qty(1..0x16)` | **primary read** |
| **0x44** | Write Register to Index | `Category, Index, Page, Qty, Data[2·Qty]` | **primary write** |
| 0x41 | Read Register from Address | `Category=0x01, Index, Addr_L, Addr_H, Pad=0, Qty(1..0x0D)` | Elements only, by 32-bit addr |
| 0x42 | Write Register to Address | `…same + Data` | Elements only (e.g. delete element = write addr 0) |
| **0x45** | **Write Register Masked to Index** | `Category, Index, Page, Qty, {Data,Mask}[·Qty]` | **set individual bits** without read-modify-write |
| 0x46 | Write Register Masked to Address | `…` | masked write, Elements by address |
| 0x6D | Enumeration (reset / start / assign) | physical-address based | multi-unit chains only |

**Masked write semantics:** `new = (current AND mask) OR (data AND NOT mask)`. **mask bit 1 = keep**
existing; **mask bit 0 = take from data.** (Note the inverted sense.) Example from the doc — enabling
DHW: STATUS_L bit 13, data `0x2000`, mask `0xDFFF`.

**Errors:** the FC's high bit is set on error; error codes are `0xC1`(41), `0xC2`(42), `0xC3`(43),
`0xC4`(44), `0xC5`(45), `0xC6`(46), followed by an exception code.

**Read example** (element page 3 address): req `43 01 00 03 02` → resp `43 04 34 12 78 56`
(address 0x12345678). **Write example** (DHW Eco = 50 °C = 500 = 0x01F4): req
`44 00 15 00 01 01 F4`.

---

## 5. What this enables beyond the current integration

The legacy code only read room temp / output / desired and wrote the manual setpoint. The full map
adds, if wanted:
- **DHW control** (comfort/eco/cleaning/standby setpoints, cleaning schedule) and enabling DHW via a
  masked STATUS write.
- **Mode control per room** — permanent/temporary Standby/Eco/Comfort, Party, Holiday, Week
  schedule — via CONFIGURATION (0x07), ideally with a **masked write** so you set `MODE`+`SCHED ENA`
  without disturbing lock/floor bits.
- **Cooling** (`COOL MODE` bit).
- **Floor temperature limits, alarm thresholds, hysteresis** per room.
- **Actuator/pump/relay** behavior and **power/current** telemetry (`P = 24·I/0.54 mW`), total and
  per-channel.
- **Change-flag polling** (MAIN 0x00–0x07) to detect what changed since last read instead of
  re-reading everything — an efficiency win for the poll loop.
- **Clock** read/write, **device info**, and **element learning** (pairing new thermostats).

For the ESP32/service build, the essential subset stays: read Elements (temp/status/battery/RSSI/
sync-group) + Packed-Data (desired/mode) + Channels (output/current); write Packed-Data
setpoints/mode (prefer masked write for CONFIGURATION). The rest is available when needed.
