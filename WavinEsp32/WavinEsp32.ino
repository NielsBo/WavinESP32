// ---------------------------------------------------------------------------
// Wavin AHC9000 -> HTTP bridge for ESP32
//
// Reads room temperatures / setpoints / heat demand from a Wavin AHC9000
// (Jablotron AC-116) over RS-485 Modbus RTU using the vendor function codes
// 0x43 / 0x44 / 0x45, and exposes them as a small JSON API.
//
// Protocol reference: docs/WAVIN_REGISTER_MAP.md
// Wiring and build instructions: README.md
// ---------------------------------------------------------------------------

#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>

#include "config.h"
#include "secrets.h"
#include "wavin_modbus.h"
#include "wavin_domain.h"
#include "http_api.h"
#include "status_led.h"
#include "relay_control.h"
#include "mqtt_broker.h"

static uint32_t lastWifiAttempt = 0;
static uint32_t lastLoggedSweep = 0;

static void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                 // keep the HTTP server responsive
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi connecting");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi OK, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi FAILED (will retry in loop)");
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== Wavin AHC9000 ESP32 bridge ===");
#if AUTO_DIRECTION
  Serial.println("RS-485: auto-direction module (no DE pin)");
#else
  Serial.printf("RS-485: manual direction, DE on GPIO%d\n", DE_PIN);
#endif
  Serial.printf("RS-485: %d baud 8N1 on RX=GPIO%d TX=GPIO%d\n", WAVIN_BAUD, RXD2, TXD2);

  statusLedBegin();
  wavinBegin();
  wavinDomainBegin();
  relayBegin();   // also recovers any relay override left over from a crash

  wifiConnect();

  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", HTTP_PORT);
    Serial.printf("mDNS: http://%s.local\n", MDNS_NAME);
  }

  ArduinoOTA.setHostname(MDNS_NAME);
  ArduinoOTA.begin();

  httpBegin();
  Serial.printf("HTTP server on port %d\n", HTTP_PORT);

  mqttBegin();
  if (mqttEnabled()) Serial.printf("MQTT broker on port %d\n", MQTT_PORT);

  // First poll immediately so the API has data right away.
  int n = pollAllRooms();
  Serial.printf("Initial poll: %d room(s) found\n", n);
}

void loop() {
  ArduinoOTA.handle();
  httpLoop();
  mqttLoop();       // embedded broker: accepts clients, caches last values
  statusLedLoop();
  relayLoop();      // enforces relay override auto-restore deadlines

  // Non-blocking WiFi reconnect with backoff.
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiAttempt > 10000) {
    lastWifiAttempt = millis();
    Serial.println("WiFi lost, reconnecting...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }

  // Incremental: at most one element per pass, so a dead bus cannot stall HTTP.
  pollStep();

  // Log once per completed sweep rather than once per element.
  if (lastPollAt() != lastLoggedSweep) {
    lastLoggedSweep = lastPollAt();
    Serial.printf("[sweep] %d room(s)  ok=%lu err=%lu\n",
                  roomCount(), (unsigned long)wavinOk(), (unsigned long)wavinErrors());
    for (int i = 0; i < roomCount(); i++) {
      const Room *r = roomAt(i);
      Serial.printf("  room %d (sync %d): %.1fC -> %.1fC  %s  %s\n",
                    r->id, r->syncGroup, r->current, r->desired,
                    modeName(r->config), r->output ? "HEATING" : "idle");
    }
  }
}
