#pragma once
#include <Arduino.h>

// Control of the AHC9000's relay outputs - typically one drives the circulation
// pump and one signals the heat source ("call for heat").
//
// Suppressing a relay means clearing its ASSIGNMENT MAP so no channel can
// switch it. That register is NON-VOLATILE on the controller, which makes a
// naive on/off dangerous: if this device crashes or is forgotten while the heat
// call is suppressed, the house simply stops heating and nothing says why.
//
// So every override is:
//   * time limited      - a deadline enforced in relayLoop(), hard-capped
//   * persisted to NVS  - the original bitmap is saved before clearing, so a
//                         reboot mid-override restores it instead of keeping it
//   * visible           - reported by /relays and /health

struct RelayInfo {
  bool     known;        // successfully read from the controller
  uint16_t assignMap;    // live ASSIGNMENT MAP value
  uint16_t event;        // TIMER EVENT register
  bool     overridden;   // we are currently suppressing it
  uint32_t restoreInS;   // seconds until auto-restore (0 when not overridden)
};

void relayBegin();     // recovers any override left over from a crash/reboot
void relayLoop();      // enforces auto-restore deadlines
void relayRefresh();   // re-read both relays from the controller

int  relayCount();
bool relayGet(int id, RelayInfo &out);                 // id is 1..RELAY_COUNT

// enabled=false suppresses the relay for restoreAfterS seconds (clamped to
// RELAY_OVERRIDE_MAX_S; 0 means RELAY_OVERRIDE_DEFAULT_S).
// enabled=true restores the saved bitmap immediately.
bool relaySetEnabled(int id, bool enabled, uint32_t restoreAfterS);

bool anyRelayOverridden();
const char* relayEventName(uint16_t ev);
