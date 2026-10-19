#include "http_api.h"
#include "config.h"
#include "secrets.h"
#include "wavin_domain.h"
#include "wavin_modbus.h"
#include "status_led.h"
#include "relay_control.h"
#include "mqtt_broker.h"

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// ArduinoJson v6 / v7 compatibility
#if ARDUINOJSON_VERSION_MAJOR >= 7
  #define JSON_DOC(name, cap) JsonDocument name
#else
  #define JSON_DOC(name, cap) DynamicJsonDocument name(cap)
#endif

static WebServer server(HTTP_PORT);

static void sendError(int code, const char *err, const char *msg) {
  JSON_DOC(doc, 256);
  doc["error"]   = err;
  doc["message"] = msg;
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

// Adds a float, or null when NaN (JSON has no NaN).
static void addFloat(JsonObject o, const char *key, float v) {
  if (isnan(v)) o[key] = nullptr;
  else          o[key] = roundf(v * 10.0f) / 10.0f;
}

static void roomToJson(const Room *r, JsonObject o) {
  o["id"]        = r->id;
  o["syncGroup"] = r->syncGroup;
  addFloat(o, "current", r->current);
  addFloat(o, "desired", r->desired);
  addFloat(o, "manual",  r->manual);
  addFloat(o, "comfort", r->comfort);
  addFloat(o, "eco",     r->eco);
  addFloat(o, "standby", r->standby);
  addFloat(o, "holiday", r->holiday);
  addFloat(o, "party",   r->party);
  o["durationMin"] = r->durationMin;
  addFloat(o, "floor",   r->floorTemp);
  o["mode"]       = modeName(r->config);
  o["output"]     = r->output;
  o["alive"]      = r->alive;
  o["lowBattery"] = r->lowBattery;
  if (r->humidity >= 0) o["humidity"] = r->humidity; else o["humidity"] = nullptr;
  o["battery"]    = r->battery;
  o["rssi"]       = roundf(r->rssiElement * 10.0f) / 10.0f;
  o["rssiUnit"]   = roundf(r->rssiUnit * 10.0f) / 10.0f;
  o["cooling"]    = (r->config & PD_CFG_COOL) != 0;
}

// ESP32 die temperature, degC, or NAN when the chip has no usable sensor.
// Core 3.x exposes temperatureRead() on every target; on the classic ESP32
// under core 1.x/2.x only the (misspelled) ROM function exists, and it returns
// Fahrenheit, with 128 meaning "no sensor" (ESP32 rev 0 silicon).
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
static float chipTempC() { return temperatureRead(); }
#else
extern "C" uint8_t temprature_sens_read();
static float chipTempC() {
  uint8_t raw = temprature_sens_read();
  if (raw == 128) return NAN;
  return ((float)raw - 32.0f) / 1.8f;
}
#endif

static void handleHealth() {
  JSON_DOC(doc, 1536);   // diag block + the last-frame hex string + mqtt stats
  bool link = wavinLinkUp();
  doc["status"]     = (link && roomCount() > 0) ? "ok" : "degraded";
  doc["serial"]     = link ? "up" : "down";
  doc["rooms"]      = roomCount();
  doc["lastPollMs"] = (uint32_t)(millis() - lastPollAt());
  doc["uptimeS"]    = (uint32_t)(millis() / 1000);
  doc["ip"]         = WiFi.localIP().toString();
  doc["rssi"]       = WiFi.RSSI();
  doc["modbusOk"]   = wavinOk();
  doc["modbusErr"]  = wavinErrors();
  doc["relayOverride"] = anyRelayOverridden();
  addFloat(doc.as<JsonObject>(), "chipTemp", chipTempC());

  // Heap headroom. Worth watching now that the broker accepts inbound
  // connections: minFree is the low-water mark since boot, which catches a
  // squeeze that has already recovered by the time you look.
  doc["freeHeap"]    = (uint32_t)ESP.getFreeHeap();
  doc["minFreeHeap"] = (uint32_t)ESP.getMinFreeHeap();

  if (mqttEnabled()) {
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonObject m = doc["mqtt"].to<JsonObject>();
#else
    JsonObject m = doc.createNestedObject("mqtt");
#endif
    m["port"]       = MQTT_PORT;
    m["clients"]    = mqttClients();
    m["topics"]     = mqttTopicCount();
    m["maxTopics"]  = MQTT_MAX_TOPICS;
    m["messages"]   = mqttMessages();
    m["evicted"]    = mqttEvicted();    // table full: raise MQTT_MAX_TOPICS
    m["truncated"]  = mqttTruncated();  // raise MQTT_MAX_PAYLOAD_LEN
    m["tooBig"]     = mqttTooBig();     // raise MQTT_MAX_MESSAGE_LEN
    m["rejected"]   = mqttRejected();
    m["authFailed"] = mqttAuthFailed();
  }

  // Bring-up diagnostics: which kind of failure, and what the bus actually said.
#if ARDUINOJSON_VERSION_MAJOR >= 7
  JsonObject d = doc["diag"].to<JsonObject>();
#else
  JsonObject d = doc.createNestedObject("diag");
#endif
  d["silent"]    = wavinSilent();
  d["crcFail"]   = wavinCrcFail();
  d["runt"]      = wavinRunt();
  d["badFrame"]  = wavinBadFrame();
  d["rxBytes"]   = wavinRxBytes();
  d["leadJunk"]  = wavinLeadJunk();
  d["lastFrame"]    = wavinLastFrameHex();
  d["longestFrame"] = wavinLongestFrameHex();
  d["baud"]      = WAVIN_BAUD;
  d["stopBits"]  = (WAVIN_SERIAL_CFG == SERIAL_8N2) ? 2 : 1;
  d["autoDir"]   = (bool)AUTO_DIRECTION;
  d["rxPin"]     = RXD2;
  d["txPin"]     = TXD2;
  d["dePin"]     = AUTO_DIRECTION ? -1 : DE_PIN;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
// MQTT last-value cache
// ---------------------------------------------------------------------------
static bool authorized();   // defined with the other write-path helpers below

// Payloads arrive as arbitrary bytes. Watts Live sends JSON, so the common
// case is a readable string - but a broker will happily carry binary, and
// JSON strings cannot hold arbitrary bytes. Emit `payload` when the bytes are
// printable UTF-8-safe ASCII, and fall back to `payloadHex` when they are not,
// so a client can always tell which it is getting.
static void mqttEntryToJson(const MqttEntry &e, JsonObject o) {
  o["topic"]   = e.topic;
  o["bytes"]   = e.len;
  o["ageMs"]   = (uint32_t)(millis() - e.updatedAt);
  o["count"]   = e.count;
  if (e.truncated) o["truncated"] = true;

  bool printable = true;
  for (uint16_t i = 0; i < e.len && printable; i++)
    if (e.payload[i] < 0x20 || e.payload[i] > 0x7E) printable = false;

  if (printable) {
    char buf[MQTT_MAX_PAYLOAD_LEN + 1];
    memcpy(buf, e.payload, e.len);
    buf[e.len] = '\0';
    o["payload"] = buf;            // ArduinoJson copies the string
  } else {
    static const char HEXDIG[] = "0123456789ABCDEF";
    String hex;
    hex.reserve(e.len * 2);
    for (uint16_t i = 0; i < e.len; i++) {
      hex += HEXDIG[e.payload[i] >> 4];
      hex += HEXDIG[e.payload[i] & 0x0F];
    }
    o["payloadHex"] = hex;
  }
}

// GET /mqtt              -> every cached topic
// GET /mqtt?topic=a/b    -> just that one, 404 if never seen
static void handleMqttGet() {
  if (!mqttEnabled()) { sendError(503, "disabled", "MQTT broker is compiled out"); return; }

  String want = server.arg("topic");
  if (want.length()) {
    MqttEntry e;
    if (!mqttEntryFor(want.c_str(), e)) {
      sendError(404, "not_found", "no message cached for that topic");
      return;
    }
    JSON_DOC(doc, MQTT_MAX_PAYLOAD_LEN * 3 + 256);
    mqttEntryToJson(e, doc.to<JsonObject>());
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
    return;
  }

  JSON_DOC(doc, (MQTT_MAX_PAYLOAD_LEN * 3 + 192) * MQTT_MAX_TOPICS + 256);
  JsonArray arr = doc.to<JsonArray>();
  MqttEntry e;
  for (int i = 0; mqttEntryAt(i, e); i++) {
#if ARDUINOJSON_VERSION_MAJOR >= 7
    mqttEntryToJson(e, arr.add<JsonObject>());
#else
    mqttEntryToJson(e, arr.createNestedObject());
#endif
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// POST /mqtt  {"topic":"a/b","payload":"..."}  -> publish to subscribers
static void handleMqttPost() {
  if (!mqttEnabled()) { sendError(503, "disabled", "MQTT broker is compiled out"); return; }
  if (!authorized()) { sendError(401, "unauthorized", "missing or bad X-Api-Token"); return; }

  JSON_DOC(body, MQTT_MAX_PAYLOAD_LEN + 256);
  DeserializationError err = deserializeJson(body, server.arg("plain"));
  if (err) { sendError(400, "bad_json", err.c_str()); return; }

  const char *topic = body["topic"];
  if (!topic || !*topic) { sendError(400, "bad_request", "expected topic"); return; }

  const char *payload = body["payload"] | "";
  if (strlen(payload) > MQTT_MAX_PAYLOAD_LEN) {
    sendError(413, "too_large", "payload exceeds MQTT_MAX_PAYLOAD_LEN");
    return;
  }
  if (!mqttPublish(topic, payload)) {
    sendError(400, "bad_topic", "topic empty or longer than MQTT_MAX_TOPIC_LEN");
    return;
  }

  MqttEntry e;
  JSON_DOC(doc, MQTT_MAX_PAYLOAD_LEN * 3 + 256);
  if (mqttEntryFor(topic, e)) mqttEntryToJson(e, doc.to<JsonObject>());
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleRooms() {
  JSON_DOC(doc, 6144);   // v6 only: ~450 bytes per room x ELEMENT_COUNT
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < roomCount(); i++) {
    const Room *r = roomAt(i);
    if (!r) continue;
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonObject o = arr.add<JsonObject>();
#else
    JsonObject o = arr.createNestedObject();
#endif
    roomToJson(r, o);
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void sendRoom(const Room *r) {
  JSON_DOC(doc, 768);
  JsonObject o = doc.to<JsonObject>();
  roomToJson(r, o);
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void relayToJson(int id, const RelayInfo &r, JsonObject o) {
  o["id"]         = id;
  o["known"]      = r.known;
  o["assignMap"]  = r.assignMap;      // 0 = no channel can switch this relay
  o["enabled"]    = !r.overridden;
  o["overridden"] = r.overridden;
  o["restoreInS"] = r.restoreInS;
  o["event"]      = relayEventName(r.event);
}

static void handleRelays() {
  JSON_DOC(doc, 1024);
  JsonArray arr = doc.to<JsonArray>();
  for (int id = 1; id <= relayCount(); id++) {
    RelayInfo r;
    if (!relayGet(id, r)) continue;
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonObject o = arr.add<JsonObject>();
#else
    JsonObject o = arr.createNestedObject();
#endif
    relayToJson(id, r, o);
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void sendRelay(int id) {
  RelayInfo r;
  if (!relayGet(id, r)) { sendError(404, "not_found", "unknown relay id"); return; }
  JSON_DOC(doc, 384);
  JsonObject o = doc.to<JsonObject>();
  relayToJson(id, r, o);
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static bool authorized() {
  if (!server.hasHeader("X-Api-Token")) return false;
  return server.header("X-Api-Token") == String(API_TOKEN);
}

// Writable room setpoints, by request key.
static const struct { const char *key; uint8_t reg; } SETPOINTS[] = {
  { "manualTemp",  PD_MANUAL  },
  { "comfortTemp", PD_COMFORT },
  { "ecoTemp",     PD_ECO     },
  { "standbyTemp", PD_STANDBY },
  { "holidayTemp", PD_HOLIDAY },
  { "partyTemp",   PD_PARTY   },
};

// Routes /rooms/<id> for GET and POST (WebServer has no path params).
static void handleNotFound() {
  String uri = server.uri();

  if (uri.startsWith("/rooms/")) {
    int id = uri.substring(7).toInt();
    const Room *r = roomById(id);
    if (!r) { sendError(404, "not_found", "unknown room id"); return; }

    if (server.method() == HTTP_GET) { sendRoom(r); return; }

    if (server.method() == HTTP_POST) {
      if (!authorized()) { sendError(401, "unauthorized", "missing or bad X-Api-Token"); return; }

      JSON_DOC(body, 256);
      DeserializationError err = deserializeJson(body, server.arg("plain"));
      if (err) { sendError(400, "bad_json", err.c_str()); return; }

      // Any combination of the keys below may be sent. Everything is
      // validated before the first write, so a bad request changes nothing;
      // then setpoints are written, then the mode, then cooling.
      bool any = false;
      for (const auto &sp : SETPOINTS) {
        if (body[sp.key].isNull()) continue;
        float t = body[sp.key].as<float>();
        if (!(t >= SETPOINT_MIN && t <= SETPOINT_MAX)) {
          sendError(400, "out_of_range", "setpoints must be 0..30"); return;
        }
        any = true;
      }

      const char *m = body["mode"];
      bool hasMode = !body["mode"].isNull();
      if (hasMode && !modeSettable(m)) { sendError(400, "bad_mode", "unsupported mode"); return; }
      bool party   = hasMode && !strcmp(m, "party");
      bool holiday = hasMode && !strcmp(m, "holiday");

      int mins = 0;
      if (!body["durationMin"].isNull()) {
        if (!party && !holiday) { sendError(400, "bad_request", "durationMin needs mode party or holiday"); return; }
        mins = body["durationMin"].as<int>();
        int maxMins = party ? PARTY_DURATION_MAX : HOLIDAY_DURATION_MAX;
        if (mins < 2 || mins > maxMins) {
          sendError(400, "out_of_range", party ? "durationMin must be 2..1440 for party"
                                               : "durationMin must be 2..131070 for holiday");
          return;
        }
      }
      bool hasCooling = !body["cooling"].isNull();
      if (!any && !hasMode && !hasCooling) {
        sendError(400, "bad_request", "expected a setpoint, mode or cooling");
        return;
      }

      for (const auto &sp : SETPOINTS) {
        if (body[sp.key].isNull()) continue;
        if (!setRoomSetpoint(id, sp.reg, body[sp.key].as<float>())) {
          sendError(409, "busy", "modbus write failed"); return;
        }
      }
      if (hasMode) {
        // partyTemp / holidayTemp, if sent, were written with the setpoints above.
        bool ok = party   ? setRoomParty(id, mins)
                : holiday ? setRoomHoliday(id, mins)
                :           setRoomMode(id, m);
        if (!ok) { sendError(409, "busy", "modbus write failed"); return; }
      }
      if (hasCooling && !setRoomCooling(id, body["cooling"].as<bool>())) {
        sendError(409, "busy", "modbus write failed"); return;
      }
      sendRoom(roomById(id));
      return;
    }
  }

  if (uri.startsWith("/relays/")) {
    int id = uri.substring(8).toInt();
    if (id < 1 || id > relayCount()) { sendError(404, "not_found", "unknown relay id"); return; }

    if (server.method() == HTTP_GET) { sendRelay(id); return; }

    if (server.method() == HTTP_POST) {
      if (!authorized()) { sendError(401, "unauthorized", "missing or bad X-Api-Token"); return; }

      JSON_DOC(body, 256);
      DeserializationError err = deserializeJson(body, server.arg("plain"));
      if (err) { sendError(400, "bad_json", err.c_str()); return; }

      if (body["enabled"].isNull()) {
        sendError(400, "bad_request", "expected enabled");
        return;
      }
      bool     enabled = body["enabled"].as<bool>();
      uint32_t after   = body["restoreAfterS"].isNull()
                           ? 0 : body["restoreAfterS"].as<uint32_t>();
      if (!relaySetEnabled(id, enabled, after)) {
        sendError(409, "busy", "relay write failed");
        return;
      }
      sendRelay(id);
      return;
    }
  }
  sendError(404, "not_found", "no such endpoint");
}

void httpBegin() {
  const char *headers[] = { "X-Api-Token" };
  server.collectHeaders(headers, 1);

  server.on("/health", HTTP_GET, handleHealth);
  server.on("/rooms",  HTTP_GET, handleRooms);
  server.on("/relays", HTTP_GET, handleRelays);
  server.on("/mqtt",   HTTP_GET,  handleMqttGet);
  server.on("/mqtt",   HTTP_POST, handleMqttPost);
  server.onNotFound(handleNotFound);
  server.begin();
}

void httpLoop() {
  server.handleClient();
}
