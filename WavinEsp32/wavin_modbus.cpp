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

// Failure breakdown. A bring-up aid: "no reply at all" and "reply arrived but
// was mangled" look identical in s_errors, yet point at opposite faults
// (wiring/direction vs framing/noise).
static uint32_t s_silent   = 0;  // nothing came back
static uint32_t s_runt     = 0;  // <4 bytes: too short to hold a CRC
static uint32_t s_crcFail  = 0;  // bytes arrived, CRC did not match
static uint32_t s_badFrame = 0;  // CRC good, but wrong unit/fc/length
static uint32_t s_rxBytes  = 0;  // cumulative bytes seen on the wire
static uint32_t s_leadJunk = 0;  // bytes dropped ahead of the unit id (DE glitch)

// Last response, kept for hex dump. Captured even when the frame is rejected -
// that is exactly the case you need to look at.
static uint8_t  s_lastResp[MAX_FRAME];
static uint8_t  s_lastRespLen = 0;

// The longest frame ever seen, kept separately: single-byte line glitches are
// frequent and would otherwise overwrite the one real reply you need to read.
static uint8_t  s_bestResp[MAX_FRAME];
static uint8_t  s_bestRespLen = 0;

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

uint32_t wavinSilent()   { return s_silent; }
uint32_t wavinRunt()     { return s_runt; }
uint32_t wavinCrcFail()  { return s_crcFail; }
uint32_t wavinBadFrame() { return s_badFrame; }
uint32_t wavinRxBytes()  { return s_rxBytes; }
uint32_t wavinLeadJunk() { return s_leadJunk; }

static String toHex(const uint8_t *buf, uint8_t len) {
  static const char HEXDIG[] = "0123456789ABCDEF";   // not HEX: Print.h #defines that
  String s;
  s.reserve(len * 3);
  for (uint8_t i = 0; i < len; i++) {
    if (i) s += ' ';
    s += HEXDIG[buf[i] >> 4];
    s += HEXDIG[buf[i] & 0x0F];
  }
  return s;                               // "" when nothing has ever arrived
}

String wavinLastFrameHex()    { return toHex(s_lastResp, s_lastRespLen); }
String wavinLongestFrameHex() { return toHex(s_bestResp, s_bestRespLen); }

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
      uint8_t b = (uint8_t)Serial2.read();
      // Discard anything ahead of the unit id. Releasing DE lets the line float
      // for an instant, which fakes a start bit and injects a 0x00 before the
      // reply. Left in place it both corrupts the CRC and - because it sets
      // n > 0 - starts the inter-frame timer, so we give up during the gap
      // before the controller has even answered.
      if (n == 0 && b != UNIT_ID) { s_leadJunk++; continue; }
      if (n < respMax) resp[n++] = b;
      // else: overflow, drop the byte but keep the frame timer alive
      last = millis();
    } else if (n > 0 && (millis() - last) > FRAME_GAP_MS) {
      break;                                   // inter-frame silence -> done
    }
    yield();
  }

  // Snapshot whatever arrived before judging it. Only on a non-empty read, so
  // a later silent transaction does not erase the evidence.
  if (n > 0) {
    s_rxBytes    += n;
    s_lastRespLen = (uint8_t)((n < MAX_FRAME) ? n : MAX_FRAME);
    for (size_t i = 0; i < s_lastRespLen; i++) s_lastResp[i] = resp[i];
    if (s_lastRespLen > s_bestRespLen) {
      s_bestRespLen = s_lastRespLen;
      for (uint8_t i = 0; i < s_bestRespLen; i++) s_bestResp[i] = s_lastResp[i];
    }
  }

  if (n == 0) { s_silent++; return 0; }        // bus never answered
  if (n < 4)  { s_runt++;   return 0; }        // too short to be valid

  uint16_t got  = (uint16_t)resp[n - 2] | ((uint16_t)resp[n - 1] << 8);
  uint16_t want = crc16(resp, n - 2);
  if (got != want) { s_crcFail++; return 0; }

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
    // n > 0 means txrx() accepted the CRC, so the rejection was ours: the unit
    // id, function code or byte count did not match what we asked for.
    if (n > 0) s_badFrame++;
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
