#include "wavin_modbus.h"
#include "config.h"

#define FC_READ_INDEX          0x43
#define FC_WRITE_INDEX         0x44
#define FC_WRITE_INDEX_MASKED  0x45

#define MAX_FRAME 64

static bool     s_busy   = false;
static uint32_t s_errors = 0;
static uint32_t s_ok     = 0;
static uint32_t s_lastOk = 0;   // millis() of last success, 0 = never

// ---------------------------------------------------------------------------
// CRC16-Modbus (poly 0xA001, init 0xFFFF). Appended low byte first.
// ---------------------------------------------------------------------------
static uint16_t crc16(const uint8_t *buf, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= buf[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
  }
  return crc;
}

void wavinBegin() {
#if !AUTO_DIRECTION
  pinMode(DE_PIN, OUTPUT);
  digitalWrite(DE_PIN, LOW);          // start in receive
#endif
  Serial2.begin(WAVIN_BAUD, WAVIN_SERIAL_CFG, RXD2, TXD2);
}

bool     wavinBusy()     { return s_busy; }
uint32_t wavinErrors()   { return s_errors; }
uint32_t wavinOk()       { return s_ok; }
uint32_t wavinLastOkAt() { return s_lastOk; }

// ---------------------------------------------------------------------------
// One request/response transaction. `req` must NOT include the CRC; it is
// appended here. Returns the response length (including CRC) or 0 on failure.
// ---------------------------------------------------------------------------
static size_t txrx(uint8_t *req, size_t reqLen, uint8_t *resp, size_t respMax) {
  uint16_t c = crc16(req, reqLen);
  req[reqLen++] = (uint8_t)(c & 0xFF);         // CRC low byte first
  req[reqLen++] = (uint8_t)(c >> 8);

  while (Serial2.available()) Serial2.read();  // drain stale bytes

#if !AUTO_DIRECTION
  digitalWrite(DE_PIN, HIGH);                  // drive the bus
#endif
  Serial2.write(req, reqLen);
  Serial2.flush();                             // block until fully shifted out
#if !AUTO_DIRECTION
  digitalWrite(DE_PIN, LOW);                   // back to listening
#endif

  size_t n = 0;
  uint32_t start = millis();
  uint32_t last  = start;
  while (millis() - start < RESPONSE_TIMEOUT) {
    if (Serial2.available()) {
      if (n < respMax) resp[n++] = (uint8_t)Serial2.read();
      else             Serial2.read();         // overflow: discard
      last = millis();
    } else if (n > 0 && (millis() - last) > FRAME_GAP_MS) {
      break;                                   // inter-frame silence -> done
    }
    yield();
  }

  if (n < 4) return 0;                         // too short to be valid

  uint16_t got  = (uint16_t)resp[n - 2] | ((uint16_t)resp[n - 1] << 8);
  uint16_t want = crc16(resp, n - 2);
  if (got != want) return 0;

  return n;
}

// Shared prologue/epilogue so every public call is serialized + counted.
static bool transact(uint8_t *req, size_t reqLen, uint8_t expectFc,
                     uint16_t *out, uint8_t outCount) {
  if (s_busy) return false;                    // never re-enter
  s_busy = true;

  uint8_t resp[MAX_FRAME];
  size_t n = txrx(req, reqLen, resp, sizeof(resp));

  bool ok = false;
  if (n >= 5 && resp[0] == UNIT_ID && resp[1] == expectFc) {
    uint8_t byteCount = resp[2];
    // Payload must fit the frame: addr+fc+len + payload + crc
    if (byteCount + 5 <= (int)n) {
      if (out && outCount) {
        uint8_t avail = byteCount / 2;
        uint8_t take  = (outCount < avail) ? outCount : avail;
        for (uint8_t i = 0; i < take; i++)
          out[i] = ((uint16_t)resp[3 + i * 2] << 8) | resp[4 + i * 2]; // MSB first
        ok = (take == outCount);
      } else {
        ok = true;
      }
    }
  }
  // resp[1] == (expectFc | 0x80) is the documented error form (0xC3/0xC4/0xC5)

  if (ok) {
    s_ok++;
    s_lastOk = millis();
    if (s_lastOk == 0) s_lastOk = 1;   // 0 is reserved for "never"
  } else {
    s_errors++;
  }
  s_busy = false;
  return ok;
}

bool wavinReadIndex(uint8_t category, uint8_t index, uint8_t page,
                    uint8_t count, uint16_t *out) {
  if (count < 1 || count > 0x16) return false;         // OEM limit: 22 registers
  uint8_t req[MAX_FRAME];
  size_t i = 0;
  req[i++] = UNIT_ID;
  req[i++] = FC_READ_INDEX;
  req[i++] = category;
  req[i++] = index;        // note: index BEFORE page
  req[i++] = page;
  req[i++] = count;
  return transact(req, i, FC_READ_INDEX, out, count);
}

bool wavinWriteIndex(uint8_t category, uint8_t index, uint8_t page, uint16_t value) {
  uint8_t req[MAX_FRAME];
  size_t i = 0;
  req[i++] = UNIT_ID;
  req[i++] = FC_WRITE_INDEX;
  req[i++] = category;
  req[i++] = index;
  req[i++] = page;
  req[i++] = 1;                        // quantity
  req[i++] = (uint8_t)(value >> 8);    // MSB first
  req[i++] = (uint8_t)(value & 0xFF);
  return transact(req, i, FC_WRITE_INDEX, nullptr, 0);
}

bool wavinWriteIndexMasked(uint8_t category, uint8_t index, uint8_t page,
                           uint16_t data, uint16_t mask) {
  uint8_t req[MAX_FRAME];
  size_t i = 0;
  req[i++] = UNIT_ID;
  req[i++] = FC_WRITE_INDEX_MASKED;
  req[i++] = category;
  req[i++] = index;
  req[i++] = page;
  req[i++] = 1;                        // quantity
  req[i++] = (uint8_t)(data >> 8);
  req[i++] = (uint8_t)(data & 0xFF);
  req[i++] = (uint8_t)(mask >> 8);
  req[i++] = (uint8_t)(mask & 0xFF);
  return transact(req, i, FC_WRITE_INDEX_MASKED, nullptr, 0);
}
