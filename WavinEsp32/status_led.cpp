#include "status_led.h"
#include "config.h"
#include "wavin_modbus.h"

static bool s_lastState = false;

static void setLed(bool on) {
  if (on == s_lastState) return;          // avoid pointless writes
  s_lastState = on;
#if LED_ACTIVE_HIGH
  digitalWrite(LED_PIN, on ? HIGH : LOW);
#else
  digitalWrite(LED_PIN, on ? LOW : HIGH);
#endif
}

void statusLedBegin() {
  pinMode(LED_PIN, OUTPUT);
  s_lastState = true;                     // force the first setLed() to write
  setLed(false);
}

bool wavinLinkUp() {
  uint32_t lastOk = wavinLastOkAt();
  if (lastOk == 0) return false;                       // never talked to the bus
  return (millis() - lastOk) < MODBUS_OK_TIMEOUT_MS;
}

void statusLedLoop() {
  uint32_t period, onTime;
  if (wavinLinkUp()) {
    period = LED_HEARTBEAT_PERIOD_MS;     // link OK: brief flash once per second
    onTime = LED_HEARTBEAT_ON_MS;
  } else {
    period = LED_ERROR_PERIOD_MS;         // link down: fast blink
    onTime = LED_ERROR_ON_MS;
  }
  // Phase from the free-running clock - no timers, no state to keep in sync.
  setLed((millis() % period) < onTime);
}
