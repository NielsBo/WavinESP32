#include "mqtt_broker.h"

#if MQTT_ENABLE

#include "secrets.h"

// Cap PicoMQTT's own receive buffer before including it. Messages larger than
// this are dropped by the library (counted via on_message_too_big) rather than
// allocated - the broker is fed by the network, so this is the real ceiling.
#ifndef PICOMQTT_MAX_MESSAGE_SIZE
#define PICOMQTT_MAX_MESSAGE_SIZE MQTT_MAX_MESSAGE_LEN
#endif
#include <PicoMQTT.h>

// ---------------------------------------------------------------------------
// Last-value cache. Fixed storage, no heap: the broker is fed by the network,
// so the worst case has to be bounded at compile time.
// ---------------------------------------------------------------------------
struct Slot {
  char     topic[MQTT_MAX_TOPIC_LEN + 1];
  uint8_t  payload[MQTT_MAX_PAYLOAD_LEN];
  uint16_t len;
  uint32_t updatedAt;
  uint32_t count;
  bool     truncated;
  bool     used;
};

static Slot     s_slots[MQTT_MAX_TOPICS];
static uint32_t s_messages   = 0;
static uint32_t s_evicted    = 0;
static uint32_t s_truncated  = 0;
static uint32_t s_rejected   = 0;
static uint32_t s_tooBig     = 0;
static uint32_t s_authFailed = 0;
static uint32_t s_clients    = 0;

static int findSlot(const char *topic) {
  for (int i = 0; i < MQTT_MAX_TOPICS; i++)
    if (s_slots[i].used && strcmp(s_slots[i].topic, topic) == 0) return i;
  return -1;
}

// A free slot, or the least recently updated one (evicting it).
static int claimSlot() {
  for (int i = 0; i < MQTT_MAX_TOPICS; i++)
    if (!s_slots[i].used) return i;

  int oldest = 0;
  for (int i = 1; i < MQTT_MAX_TOPICS; i++)
    // Signed compare so millis() rollover does not pick the wrong victim.
    if ((int32_t)(s_slots[i].updatedAt - s_slots[oldest].updatedAt) < 0) oldest = i;
  s_evicted++;
  return oldest;
}

// Records one publish. Shared by the broker callback and mqttPublish().
static void store(const char *topic, const uint8_t *payload, size_t len) {
  if (!topic || !*topic) { s_rejected++; return; }
  if (strlen(topic) > MQTT_MAX_TOPIC_LEN) { s_rejected++; return; }

  int i = findSlot(topic);
  if (i < 0) {
    i = claimSlot();
    strncpy(s_slots[i].topic, topic, MQTT_MAX_TOPIC_LEN);
    s_slots[i].topic[MQTT_MAX_TOPIC_LEN] = '\0';
    s_slots[i].count = 0;
    s_slots[i].used  = true;
  }

  Slot &s = s_slots[i];
  s.truncated = (len > MQTT_MAX_PAYLOAD_LEN);
  if (s.truncated) s_truncated++;
  s.len = (uint16_t)(s.truncated ? MQTT_MAX_PAYLOAD_LEN : len);
  if (payload && s.len) memcpy(s.payload, payload, s.len);
  s.updatedAt = millis();
  s.count++;
  s_messages++;
}

// ---------------------------------------------------------------------------
// Broker. Subclassed only for the hooks PicoMQTT exposes as virtuals:
// credential checking and connect/disconnect accounting.
// ---------------------------------------------------------------------------
class Broker : public PicoMQTT::Server {
public:
  Broker(uint16_t port) : PicoMQTT::Server(port) {}

protected:
  PicoMQTT::ConnectReturnCode auth(const char *client_id, const char *username,
                                   const char *password) override {
    (void)client_id;
#if MQTT_REQUIRE_AUTH
    // Watts Live supports username/password but NOT TLS, so these travel in
    // clear text on the LAN. They keep honest devices out; they are not a
    // security boundary.
    if (!username || strcmp(username, MQTT_USER) != 0 ||
        !password || strcmp(password, MQTT_PASS) != 0) {
      s_authFailed++;
      return PicoMQTT::CRC_BAD_USERNAME_OR_PASSWORD;
    }
#else
    (void)username; (void)password;
#endif
    return PicoMQTT::CRC_ACCEPTED;
  }

  void on_connected(const char *) override    { s_clients++; }
  void on_disconnected(const char *) override { if (s_clients) s_clients--; }

  void on_message_too_big(const char *topic, PicoMQTT::IncomingPacket &packet) override {
    (void)topic; (void)packet;
    s_tooBig++;
  }
};

static Broker s_broker(MQTT_PORT);

void mqttBegin() {
  // Subscribing to "#" makes the broker its own client: Server::on_message()
  // fires local callbacks for everything a client publishes.
  s_broker.subscribe("#", [](char *topic, void *payload, size_t size) {
    store(topic, (const uint8_t *)payload, size);
  });
  s_broker.begin();
}

void mqttLoop() { s_broker.loop(); }

bool mqttPublish(const char *topic, const char *payload) {
  if (!topic || !*topic || strlen(topic) > MQTT_MAX_TOPIC_LEN) {
    s_rejected++;
    return false;
  }
  s_broker.publish(topic, payload ? payload : "");
  // Server::publish() does not feed our own "#" subscription, so cache it here.
  store(topic, (const uint8_t *)payload, payload ? strlen(payload) : 0);
  return true;
}

int mqttTopicCount() {
  int n = 0;
  for (int i = 0; i < MQTT_MAX_TOPICS; i++) if (s_slots[i].used) n++;
  return n;
}

static bool fill(const Slot &s, MqttEntry &out) {
  out.topic     = s.topic;
  out.payload   = s.payload;
  out.len       = s.len;
  out.updatedAt = s.updatedAt;
  out.count     = s.count;
  out.truncated = s.truncated;
  return true;
}

bool mqttEntryAt(int idx, MqttEntry &out) {
  int n = 0;
  for (int i = 0; i < MQTT_MAX_TOPICS; i++) {
    if (!s_slots[i].used) continue;
    if (n++ == idx) return fill(s_slots[i], out);
  }
  return false;
}

bool mqttEntryFor(const char *topic, MqttEntry &out) {
  int i = topic ? findSlot(topic) : -1;
  return (i < 0) ? false : fill(s_slots[i], out);
}

void mqttClearCache() {
  for (int i = 0; i < MQTT_MAX_TOPICS; i++) s_slots[i].used = false;
}

uint32_t mqttMessages()   { return s_messages; }
uint32_t mqttEvicted()    { return s_evicted; }
uint32_t mqttTruncated()  { return s_truncated; }
uint32_t mqttRejected()   { return s_rejected; }
uint32_t mqttTooBig()     { return s_tooBig; }
uint32_t mqttAuthFailed() { return s_authFailed; }
uint32_t mqttClients()    { return s_clients; }
bool     mqttEnabled()    { return true; }

#else   // ---- compiled out -------------------------------------------------

void mqttBegin() {}
void mqttLoop()  {}
bool mqttPublish(const char *, const char *) { return false; }
int  mqttTopicCount() { return 0; }
bool mqttEntryAt(int, MqttEntry &)           { return false; }
bool mqttEntryFor(const char *, MqttEntry &) { return false; }
void mqttClearCache() {}
uint32_t mqttMessages()   { return 0; }
uint32_t mqttEvicted()    { return 0; }
uint32_t mqttTruncated()  { return 0; }
uint32_t mqttRejected()   { return 0; }
uint32_t mqttTooBig()     { return 0; }
uint32_t mqttAuthFailed() { return 0; }
uint32_t mqttClients()    { return 0; }
bool     mqttEnabled()    { return false; }

#endif
