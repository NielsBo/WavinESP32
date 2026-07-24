#pragma once

// ---------------------------------------------------------------------------
// RS-485 transceiver wiring
// ---------------------------------------------------------------------------
// AUTO_DIRECTION 1 = module has automatic direction control (pins: VCC GND TXD RXD A B)
//                    -> no DE pin used, DE_PIN is ignored.
// AUTO_DIRECTION 0 = manual module (pins: RO DI RE DE VCC GND A B)
//                    -> tie DE+RE together and wire to DE_PIN.
// Check the silkscreen on your board and set accordingly.
#define AUTO_DIRECTION    0

#define DE_PIN            4      // only used when AUTO_DIRECTION 0
#define RXD2              16     // ESP32 RX2  <- module RO / TXD
#define TXD2              17     // ESP32 TX2  -> module DI / RXD

// IMPORTANT - the ESP32 is not 5V-tolerant on RXD2.
//   Genuine MAX485 (5V-only part): power VCC from 5V and drop RO through a
//     divider (1k series + 2k to GND) before GPIO16. DI/DE/RE connect direct.
//   MAX3485 / SP3485 / auto-direction modules: power VCC from 3V3, RO direct.
// See README section 1.

// ---------------------------------------------------------------------------
// Modbus
// ---------------------------------------------------------------------------
#define WAVIN_BAUD        38400
// OEM spec (AC-116 Modbus Register Map, Table 5.1) says 38400 8N1.
// The legacy j2mod/pywavin code used 8N2 (reverse-engineered).
// Start with 8N1; only switch to SERIAL_8N2 if you cannot get valid reads.
#define WAVIN_SERIAL_CFG  SERIAL_8N1

#define UNIT_ID           1      // every AHC9000 answers at logical address 1
// A healthy AHC9000 replies in single-digit ms. Keep this tight: it is also the
// worst-case time loop() can be blocked by one failed transaction.
#define RESPONSE_TIMEOUT  300    // ms to wait for a reply
#define FRAME_GAP_MS      5      // silence that marks end of frame

// ---------------------------------------------------------------------------
// Polling
// ---------------------------------------------------------------------------
#define ELEMENT_COUNT     10     // how many element pages to scan (max 48)
#define MAX_ROOMS         ELEMENT_COUNT
#define POLL_INTERVAL_MS  30000  // time for one full sweep of all elements
#define MIN_VALID_TEMP    2.0    // room temp <= this means "no sensor" -> skip

// Polling is incremental: one element per step, spread across the interval, so
// loop() is never blocked for more than a single transaction. This keeps HTTP,
// OTA and the status LED responsive even when the bus is dead.
#define POLL_STEP_MS      (POLL_INTERVAL_MS / ELEMENT_COUNT)

// After this many consecutive failures an element is parked for FAIL_BACKOFF_MS
// instead of being retried every sweep (empty pages and dead thermostats).
#define FAIL_THRESHOLD    3
#define FAIL_BACKOFF_MS   300000  // 5 minutes

// ---------------------------------------------------------------------------
// Relay override (suppressing the call-for-heat to the heat source)
// ---------------------------------------------------------------------------
// The AHC9000 has two relays; clearing a relay's assignment map stops any
// channel from switching it. That setting is NON-VOLATILE on the controller,
// so an override must always be time-limited and recoverable - otherwise a
// crash or a forgotten call leaves the house without heat.
#define RELAY_COUNT              2
#define RELAY_OVERRIDE_DEFAULT_S 1800    // 30 min if the caller does not say
#define RELAY_OVERRIDE_MAX_S     14400   // 4 h hard cap - no indefinite disable

// ---------------------------------------------------------------------------
// Status LED
// ---------------------------------------------------------------------------
// GPIO2 is the onboard blue LED on most ESP32 DevKit boards. (It is also a
// strapping pin, but driving it as an output after boot is fine.)
// Set LED_ACTIVE_HIGH to 0 if your board's LED is wired active-low.
#define LED_PIN                 2
#define LED_ACTIVE_HIGH         1

// Link counts as up if a Modbus transaction succeeded this recently.
// Two poll intervals plus margin, so one missed poll does not trip it.
#define MODBUS_OK_TIMEOUT_MS    (POLL_INTERVAL_MS * 2 + 15000)

#define LED_HEARTBEAT_PERIOD_MS 1000   // link OK: flash once per second
#define LED_HEARTBEAT_ON_MS     50
#define LED_ERROR_PERIOD_MS     200    // link down: fast blink (5 Hz)
#define LED_ERROR_ON_MS         100

// ---------------------------------------------------------------------------
// Embedded MQTT broker
// ---------------------------------------------------------------------------
// Other devices publish to this bridge; the last payload per topic is cached
// and served over REST (/mqtt). Requires the PicoMQTT library - set to 0 to
// compile the whole feature out, including the dependency.
//
// This is a broker of convenience, not infrastructure: nothing is persisted,
// there is no authentication on port 1883, and every client drops when the
// bridge reboots or takes an OTA update. Do not put anything on it that the
// heating depends on.
#define MQTT_ENABLE          1
#define MQTT_PORT            1883

// Credentials. Watts Live can send a username/password but cannot do TLS, so
// they cross the LAN in clear text - treat them as a misconfiguration guard,
// not as security. Set to 0 to accept any client (the Watts default, with
// both fields left empty). Values live in secrets.h.
#define MQTT_REQUIRE_AUTH    0

// Cache limits. Statically allocated, so the cost is paid at compile time:
// MQTT_MAX_TOPICS * (MQTT_MAX_TOPIC_LEN + MQTT_MAX_PAYLOAD_LEN + ~16) bytes,
// about 9 kB with the defaults below.
//
// Sized for Watts Live: it publishes to "watts/<serial>/measurement" (~35
// chars) a JSON object of ~13 fields - positive_active_energy,
// positive_active_power, voltage_l1..l3, current_l1..l3 and friends - which
// runs to roughly 400 bytes. 512 leaves headroom; if you see `truncated`
// climbing in /health, raise it.
#define MQTT_MAX_TOPICS      16    // distinct topics kept; oldest is evicted
#define MQTT_MAX_TOPIC_LEN   64    // longer topics are rejected outright
#define MQTT_MAX_PAYLOAD_LEN 512   // longer payloads are stored truncated

// Hard ceiling on a single MQTT message. PicoMQTT drops anything larger
// before buffering it, which is what stops a hostile or broken publisher from
// exhausting the heap. Must be >= MQTT_MAX_PAYLOAD_LEN.
#define MQTT_MAX_MESSAGE_LEN 1024

// ---------------------------------------------------------------------------
// Network / HTTP
// ---------------------------------------------------------------------------
#define MDNS_NAME         "wavin"   // -> http://wavin.local
#define HTTP_PORT         80
