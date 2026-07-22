#include "http_api.h"
#include "config.h"
#include "secrets.h"
#include "wavin_domain.h"
#include "wavin_modbus.h"
#include "status_led.h"
#include "relay_control.h"

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
  JSON_DOC(doc, 1024);   // diag block + the last-frame hex string
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

static void handleRooms() {
  JSON_DOC(doc, 4096);
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
  JSON_DOC(doc, 512);
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

      if (!body["manualTemp"].isNull()) {
        float t = body["manualTemp"].as<float>();
        if (t < 0.0f || t > 30.0f) { sendError(400, "out_of_range", "manualTemp must be 0..30"); return; }
        if (!setRoomManualTemp(id, t)) { sendError(409, "busy", "modbus write failed"); return; }
        sendRoom(roomById(id));
        return;
      }
      if (!body["mode"].isNull()) {
        const char *m = body["mode"];
        if (!setRoomMode(id, m)) { sendError(400, "bad_mode", "unsupported mode or write failed"); return; }
        sendRoom(roomById(id));
        return;
      }
      if (!body["cooling"].isNull()) {
        bool on = body["cooling"].as<bool>();
        if (!setRoomCooling(id, on)) { sendError(409, "busy", "modbus write failed"); return; }
        sendRoom(roomById(id));
        return;
      }
      sendError(400, "bad_request", "expected manualTemp, mode or cooling");
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
  server.onNotFound(handleNotFound);
  server.begin();
}

void httpLoop() {
  server.handleClient();
}
