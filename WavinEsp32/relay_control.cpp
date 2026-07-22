#include "relay_control.h"
#include "config.h"
#include "wavin_domain.h"
#include "wavin_modbus.h"
#include <Preferences.h>

struct RelaySlot {
  bool     known;
  uint16_t assignMap;    // live value
  uint16_t event;
  uint16_t savedMap;     // value to restore
  bool     overridden;
  uint32_t restoreAt;    // millis() deadline
};

static RelaySlot  s_relays[RELAY_COUNT];
static Preferences s_prefs;

// NVS keys are limited to 15 chars.
static void keyMap(int i, char *buf) { snprintf(buf, 8, "r%dmap", i); }
static void keyOvr(int i, char *buf) { snprintf(buf, 8, "r%dovr", i); }

const char* relayEventName(uint16_t ev) {
  switch (ev & 0x000F) {
    case 0x00: return "none";
    case 0x01: return "idle";
    case 0x02: return "cut-off";
    case 0x03: return "control-bypass";
    case 0x04: return "start-delay";
    case 0x05: return "overcurrent";
    case 0x06: return "override-off";
    case 0x07: return "on";
    case 0x08: return "freezing";
    case 0x09: return "dhw-on";
    case 0x0A: return "stop-delay";
    case 0x0B: return "dhw-cleaning";
    case 0x0C: return "override-on";
    case 0x0D: return "periodic-cycle";
  }
  return "unknown";
}

int relayCount() { return RELAY_COUNT; }

// Reads TIMER EVENT + ASSIGNMENT MAP (contiguous) for one relay.
static bool readRelay(int idx) {
  uint16_t regs[2];
  if (!wavinReadIndex(CAT_RELAYS, RL_TIMER_EVENT, (uint8_t)idx, 2, regs)) {
    s_relays[idx].known = false;
    return false;
  }
  s_relays[idx].known     = true;
  s_relays[idx].event     = regs[0];
  s_relays[idx].assignMap = regs[1];
  return true;
}

void relayRefresh() {
  for (int i = 0; i < RELAY_COUNT; i++) readRelay(i);
}

// Writes the assignment map and updates the cached copy.
static bool writeMap(int idx, uint16_t map) {
  if (!wavinWriteIndex(CAT_RELAYS, RL_ASSIGN_MAP, (uint8_t)idx, map)) return false;
  s_relays[idx].assignMap = map;
  return true;
}

static void clearPersistedOverride(int idx) {
  char k[8];
  keyOvr(idx, k); s_prefs.putBool(k, false);
}

static void persistOverride(int idx, uint16_t savedMap) {
  char k[8];
  keyMap(idx, k); s_prefs.putUShort(k, savedMap);
  keyOvr(idx, k); s_prefs.putBool(k, true);
}

void relayBegin() {
  for (int i = 0; i < RELAY_COUNT; i++) {
    s_relays[i].known      = false;
    s_relays[i].overridden = false;
    s_relays[i].savedMap   = 0;
    s_relays[i].restoreAt  = 0;
  }

  s_prefs.begin("wavin", false);

  // Crash recovery: if NVS says we left a relay suppressed, put it back before
  // doing anything else. Without this the controller would happily stay in the
  // "no call for heat" state forever.
  for (int i = 0; i < RELAY_COUNT; i++) {
    char k[8];
    keyOvr(i, k);
    if (!s_prefs.getBool(k, false)) continue;

    keyMap(i, k);
    uint16_t saved = s_prefs.getUShort(k, 0);
    Serial.printf("[relay] recovering override on relay %d -> restoring map 0x%04X\n",
                  i + 1, saved);
    if (writeMap(i, saved)) clearPersistedOverride(i);
    else Serial.printf("[relay] WARNING: restore of relay %d FAILED, will retry\n", i + 1);
  }

  relayRefresh();
}

bool relayGet(int id, RelayInfo &out) {
  int idx = id - 1;
  if (idx < 0 || idx >= RELAY_COUNT) return false;
  RelaySlot &s = s_relays[idx];

  out.known      = s.known;
  out.assignMap  = s.assignMap;
  out.event      = s.event;
  out.overridden = s.overridden;
  out.restoreInS = 0;
  if (s.overridden) {
    int32_t left = (int32_t)(s.restoreAt - millis());
    out.restoreInS = (left > 0) ? (uint32_t)(left / 1000) : 0;
  }
  return true;
}

bool relaySetEnabled(int id, bool enabled, uint32_t restoreAfterS) {
  int idx = id - 1;
  if (idx < 0 || idx >= RELAY_COUNT) return false;
  RelaySlot &s = s_relays[idx];

  if (enabled) {
    if (!s.overridden) return true;                  // already enabled
    if (!writeMap(idx, s.savedMap)) return false;
    s.overridden = false;
    s.restoreAt  = 0;
    clearPersistedOverride(idx);
    Serial.printf("[relay] relay %d restored (map 0x%04X)\n", id, s.savedMap);
    return true;
  }

  // Disabling. Clamp the deadline - an indefinite disable is not offered.
  if (restoreAfterS == 0) restoreAfterS = RELAY_OVERRIDE_DEFAULT_S;
  if (restoreAfterS > RELAY_OVERRIDE_MAX_S) restoreAfterS = RELAY_OVERRIDE_MAX_S;

  if (!s.overridden) {
    if (!readRelay(idx)) return false;               // need the real value first
    if (s.assignMap == 0) return false;              // already off; nothing to save
    s.savedMap = s.assignMap;
    persistOverride(idx, s.savedMap);                // save BEFORE clearing
    if (!writeMap(idx, 0x0000)) {
      clearPersistedOverride(idx);
      return false;
    }
    s.overridden = true;
    Serial.printf("[relay] relay %d suppressed for %us (saved map 0x%04X)\n",
                  id, (unsigned)restoreAfterS, s.savedMap);
  }
  s.restoreAt = millis() + restoreAfterS * 1000UL;   // extend the deadline
  return true;
}

void relayLoop() {
  uint32_t now = millis();
  for (int i = 0; i < RELAY_COUNT; i++) {
    RelaySlot &s = s_relays[i];
    if (!s.overridden) continue;
    if ((int32_t)(now - s.restoreAt) < 0) continue;  // deadline not reached

    Serial.printf("[relay] relay %d override expired, restoring map 0x%04X\n",
                  i + 1, s.savedMap);
    if (writeMap(i, s.savedMap)) {
      s.overridden = false;
      s.restoreAt  = 0;
      clearPersistedOverride(i);
    } else {
      s.restoreAt = now + 5000;                      // write failed, retry in 5s
      Serial.printf("[relay] restore of relay %d failed, retrying\n", i + 1);
    }
  }
}

bool anyRelayOverridden() {
  for (int i = 0; i < RELAY_COUNT; i++)
    if (s_relays[i].overridden) return true;
  return false;
}
