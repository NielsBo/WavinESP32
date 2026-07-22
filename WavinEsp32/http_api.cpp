#include "http_api.h"
#include "config.h"
#include "secrets.h"
#include "wavin_domain.h"
#include "wavin_modbus.h"
#include "status_led.h"

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

static void handleHealth() {
  JSON_DOC(doc, 384);
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
      sendError(400, "bad_request", "expected manualTemp or mode");
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
  server.onNotFound(handleNotFound);
  server.begin();
}

void httpLoop() {
  server.handleClient();
}
