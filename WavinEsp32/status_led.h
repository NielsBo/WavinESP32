#pragma once
#include <Arduino.h>

// Onboard status LED, driven by the health of the Wavin Modbus link:
//
//   connected  -> slow heartbeat  (brief flash once per second)
//   no link    -> fast blink      (board is alive, but the bus is not talking)
//
// "Connected" means a Modbus transaction succeeded within MODBUS_OK_TIMEOUT_MS.
// Non-blocking: call statusLedLoop() from loop().
void statusLedBegin();
void statusLedLoop();

// True when the Wavin link is considered up (also used by /health).
bool wavinLinkUp();
