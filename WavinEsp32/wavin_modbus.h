#pragma once
#include <Arduino.h>

// Low-level transport for the Wavin AHC9000 (Jablotron AC-116) custom Modbus
// function codes. See docs/WAVIN_REGISTER_MAP.md section 4.
//
//   0x43 read  by (category, index, page)
//   0x44 write by (category, index, page)
//   0x45 masked write: new = (cur AND mask) OR (data AND NOT mask)
//                      mask bit 1 = KEEP existing, 0 = take from data
//
// All calls are serialized internally (RS-485 is half duplex, one transaction
// at a time). Register values are 16-bit, MSB first on the wire.

void wavinBegin();

// Reads `count` registers into out[]. Returns false on timeout/CRC/protocol error.
bool wavinReadIndex(uint8_t category, uint8_t index, uint8_t page,
                    uint8_t count, uint16_t *out);

// Writes a single register.
bool wavinWriteIndex(uint8_t category, uint8_t index, uint8_t page, uint16_t value);

// Writes selected bits of a single register (see mask semantics above).
bool wavinWriteIndexMasked(uint8_t category, uint8_t index, uint8_t page,
                           uint16_t data, uint16_t mask);

bool     wavinBusy();        // a transaction is in flight
uint32_t wavinErrors();      // cumulative failed transactions
uint32_t wavinOk();          // cumulative successful transactions
uint32_t wavinLastOkAt();    // millis() of last successful transaction, 0 = never

// Failure breakdown, for bring-up. s_errors is the sum of these four; which one
// is counting tells you where to look:
//
//   silent   -> nothing on the wire: DE/direction, A-B polarity, wiring, power
//   crcFail  -> the bus IS talking, the bytes are mangled: baud, stop bits, noise
//   runt     -> a fragment arrived: marginal signal or a too-tight frame gap
//   badFrame -> valid frame, but not the answer we asked for: protocol/unit id
uint32_t wavinSilent();
uint32_t wavinRunt();
uint32_t wavinCrcFail();
uint32_t wavinBadFrame();
uint32_t wavinRxBytes();     // cumulative bytes received, valid or not
uint32_t wavinLeadJunk();    // bytes discarded ahead of the unit id (DE turnaround
                             // glitch). A steady ~1 per transaction is normal.

// Responses as "01 43 06 ..." hex, or "" if nothing has ever arrived.
// Longest is kept separately because 1-byte line glitches would otherwise
// overwrite the only real reply you have.
String   wavinLastFrameHex();
String   wavinLongestFrameHex();
