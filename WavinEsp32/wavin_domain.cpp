#include "wavin_domain.h"
#include "wavin_modbus.h"

// One slot per element index. Slots are updated in place by the incremental
// poller, so HTTP reads always see a complete (if slightly stale) record.
struct ElementSlot {
  Room     room;
  uint8_t  failCount;
  uint32_t nextTryAt;   // millis(); 0 = eligible now
};

static ElementSlot s_slots[ELEMENT_COUNT];
static int         s_cursor    = 0;   // which element the next step examines
static uint32_t    s_lastStep  = 0;
static uint32_t    s_lastSweep = 0;

// ---------------------------------------------------------------------------
// Value decoding (docs/WAVIN_REGISTER_MAP.md section 2)
//   temperatures: signed, 0.1 degC per LSB; 0x7FFF means "unknown"
// ---------------------------------------------------------------------------
static float decodeTemp(uint16_t raw) {
  if (raw == 0x7FFF) return NAN;
  return (float)(int16_t)raw / 10.0f;
}

static int decodeHumidity(uint16_t raw) {
  if (raw == 0x7FFF) return -1;
  return (int)raw;
}

// RSSI: low byte = element side, high byte = control unit side.
// Signed 0 means -74 dBm, one step is 0.5 dBm.
static float decodeRssi(uint8_t b) {
  return -74.0f + 0.5f * (float)(int8_t)b;
}

const char* modeName(uint16_t config) {
  uint16_t m     = config & PD_CFG_MODE_MASK;
  bool     sched = config & PD_CFG_SCHED_ENA;
  if (!sched) {
    switch (m) {
      case MODE_MANUAL:  return "manual";
      case MODE_STANDBY: return "standby";
      case MODE_ECO:     return "eco";
      case MODE_COMFORT: return "comfort";
      case MODE_PARTY:   return "party";
      case MODE_HOLIDAY: return "holiday";
    }
  } else {
    switch (m) {
      case MODE_MANUAL:  return "schedule";
      case MODE_STANDBY: return "temp-standby";
      case MODE_ECO:     return "temp-eco";
      case MODE_COMFORT: return "temp-comfort";
      case MODE_PARTY:   return "party-schedule";
      case MODE_HOLIDAY: return "holiday-schedule";
    }
  }
  return "unknown";
}

void wavinDomainBegin() {
  for (int i = 0; i < ELEMENT_COUNT; i++) {
    s_slots[i].room.valid = false;
    s_slots[i].failCount  = 0;
    s_slots[i].nextTryAt  = 0;
  }
  s_cursor = 0;
}

// Reads one element + its room page into `r`. Returns false if there is no
// usable sensor on that element page.
static bool readRoom(int elementIndex, Room &r) {
  uint16_t el[EL_BLOCK_LEN];
  if (!wavinReadIndex(CAT_ELEMENTS, EL_BLOCK_START, (uint8_t)elementIndex,
                      EL_BLOCK_LEN, el))
    return false;

  float air = decodeTemp(el[EL_AIR_TEMP - EL_BLOCK_START]);
  // No sensor / unpopulated page reads back as unknown or a near-zero value.
  if (isnan(air) || air <= MIN_VALID_TEMP) return false;

  uint16_t status = el[EL_STATUS      - EL_BLOCK_START];
  uint16_t rssi   = el[EL_RSSI        - EL_BLOCK_START];
  uint16_t batt   = el[EL_BATTERY     - EL_BLOCK_START];
  uint16_t sync   = el[EL_SYNC_GROUP  - EL_BLOCK_START];

  r.valid        = true;
  r.id           = elementIndex + 1;
  r.elementIndex = elementIndex;
  r.syncGroup    = sync & 0x00FF;
  r.current      = air;
  r.floorTemp    = decodeTemp(el[EL_FLOOR_TEMP - EL_BLOCK_START]);
  r.dewPoint     = decodeTemp(el[EL_DEW_POINT  - EL_BLOCK_START]);
  r.humidity     = decodeHumidity(el[EL_HUMIDITY - EL_BLOCK_START]);
  r.battery      = (int)(batt & 0x000F) * 10;
  r.rssiElement  = decodeRssi((uint8_t)(rssi & 0xFF));
  r.rssiUnit     = decodeRssi((uint8_t)(rssi >> 8));
  r.output       = (status & EL_ST_TP_ACT) != 0;
  r.alive        = (status & EL_ST_ALIVE)  != 0;
  r.lowBattery   = (status & EL_ST_LOW_BATT) != 0;

  // Room-level data lives in PACKED DATA, page = sync group.
  uint16_t pd[PD_PAGE_LEN];
  if (wavinReadIndex(CAT_PACKED, 0x00, (uint8_t)r.syncGroup, PD_PAGE_LEN, pd)) {
    r.desired = decodeTemp(pd[PD_DESIRED]);
    r.manual  = decodeTemp(pd[PD_MANUAL]);
    r.config  = pd[PD_CONFIG];
  } else {
    r.desired = NAN;
    r.manual  = NAN;
    r.config  = 0;
  }
  return true;
}

// Applies the outcome of one element read to its slot (shared by the blocking
// sweep and the incremental poller).
static bool updateSlot(int e) {
  ElementSlot &s = s_slots[e];
  Room r;
  if (readRoom(e, r)) {
    s.room      = r;                 // r.valid was set by readRoom()
    s.failCount = 0;
    s.nextTryAt = 0;
    return true;
  }
  if (s.failCount < 255) s.failCount++;
  if (s.failCount >= FAIL_THRESHOLD) {
    s.room.valid = false;            // stop advertising a room we cannot read
    s.nextTryAt  = millis() + FAIL_BACKOFF_MS;
    if (s.nextTryAt == 0) s.nextTryAt = 1;   // 0 is the "eligible now" sentinel
  }
  return false;
}

int pollAllRooms() {
  int n = 0;
  for (int e = 0; e < ELEMENT_COUNT; e++) {
    if (updateSlot(e)) n++;
    yield();
  }
  s_lastSweep = millis();
  return n;
}

void pollStep() {
  uint32_t now = millis();
  if (now - s_lastStep < POLL_STEP_MS) return;
  s_lastStep = now;

  int e = s_cursor;
  if (++s_cursor >= ELEMENT_COUNT) {   // wrapped -> a full sweep just finished
    s_cursor    = 0;
    s_lastSweep = now;
  }

  // Signed compare so millis() rollover is handled correctly.
  ElementSlot &s = s_slots[e];
  if (s.nextTryAt != 0 && (int32_t)(now - s.nextTryAt) < 0) return;  // backing off

  updateSlot(e);
}

uint32_t lastPollAt() { return s_lastSweep; }

int roomCount() {
  int n = 0;
  for (int i = 0; i < ELEMENT_COUNT; i++)
    if (s_slots[i].room.valid) n++;
  return n;
}

const Room* roomAt(int idx) {
  if (idx < 0) return nullptr;
  for (int i = 0; i < ELEMENT_COUNT; i++) {
    if (!s_slots[i].room.valid) continue;
    if (idx-- == 0) return &s_slots[i].room;
  }
  return nullptr;
}

const Room* roomById(int id) {
  for (int i = 0; i < ELEMENT_COUNT; i++)
    if (s_slots[i].room.valid && s_slots[i].room.id == id)
      return &s_slots[i].room;
  return nullptr;
}

// Refresh a single room in place after a write.
static void refreshRoom(int id) {
  for (int i = 0; i < ELEMENT_COUNT; i++)
    if (s_slots[i].room.valid && s_slots[i].room.id == id) {
      updateSlot(i);
      return;
    }
}

bool setRoomManualTemp(int id, float celsius) {
  const Room *r = roomById(id);
  if (!r) return false;
  if (celsius < 0.0f)  celsius = 0.0f;         // OEM range guard
  if (celsius > 30.0f) celsius = 30.0f;

  uint16_t raw = (uint16_t)lroundf(celsius * 10.0f);
  if (!wavinWriteIndex(CAT_PACKED, PD_MANUAL, (uint8_t)r->syncGroup, raw))
    return false;
  refreshRoom(id);
  return true;
}

bool setRoomMode(int id, const char *mode) {
  const Room *r = roomById(id);
  if (!r || !mode) return false;

  uint16_t m;
  bool sched = false;
  if      (!strcmp(mode, "manual"))   m = MODE_MANUAL;
  else if (!strcmp(mode, "standby"))  m = MODE_STANDBY;
  else if (!strcmp(mode, "eco"))      m = MODE_ECO;
  else if (!strcmp(mode, "comfort"))  m = MODE_COMFORT;
  else if (!strcmp(mode, "schedule")) { m = MODE_MANUAL; sched = true; }
  else return false;

  uint16_t data = m | (sched ? PD_CFG_SCHED_ENA : 0);
  // Masked write: mask bit 1 = keep. Change only MODE[2:0] + SCHED ENA (bits 3..0),
  // leaving lock/floor/cool bits untouched and avoiding a read-modify-write race.
  if (!wavinWriteIndexMasked(CAT_PACKED, PD_CONFIG, (uint8_t)r->syncGroup,
                             data, 0xFFF0))
    return false;
  refreshRoom(id);
  return true;
}
