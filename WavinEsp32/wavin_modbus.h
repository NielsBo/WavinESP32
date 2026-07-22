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
