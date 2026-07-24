#pragma once
#include <Arduino.h>
#include "config.h"

// Embedded MQTT broker (PicoMQTT) plus a small last-value cache.
//
// Other devices on the LAN publish to this bridge on MQTT_PORT; the most
// recent payload per topic is kept in RAM and served over REST alongside the
// Wavin rooms. Nothing is persisted - a reboot starts from empty.
//
// The cache is deliberately bounded (MQTT_MAX_TOPICS x MQTT_MAX_PAYLOAD_LEN,
// statically allocated). A broker accepts whatever the network sends it, so
// unbounded growth here would be a remote out-of-memory away from taking the
// heating offline. Oversized payloads are stored truncated, and a full table
// evicts the least recently updated topic. Both events are counted so you can
// see it happening in /health instead of guessing.
//
// Non-blocking: call mqttLoop() from loop().

void mqttBegin();
void mqttLoop();

// One cached topic. `payload` is NOT null-terminated-safe for binary data -
// always use `len`.
struct MqttEntry {
  const char *topic;
  const uint8_t *payload;
  uint16_t len;         // bytes stored (already truncated if `truncated`)
  uint32_t updatedAt;   // millis() of the last publish
  uint32_t count;       // publishes seen on this topic since boot
  bool     truncated;   // payload was longer than MQTT_MAX_PAYLOAD_LEN
};

int  mqttTopicCount();                       // topics currently cached
bool mqttEntryAt(int i, MqttEntry &out);     // 0 <= i < mqttTopicCount()
bool mqttEntryFor(const char *topic, MqttEntry &out);

// Publish to connected subscribers and update the cache, as if a client had
// sent it. Returns false if the topic is unusable or the broker is disabled.
bool mqttPublish(const char *topic, const char *payload);

// Drop every cached topic. Does not disconnect clients.
void mqttClearCache();

uint32_t mqttMessages();   // total publishes accepted since boot
uint32_t mqttEvicted();    // topics dropped because the table was full
uint32_t mqttTruncated();  // payloads clipped to MQTT_MAX_PAYLOAD_LEN
uint32_t mqttRejected();   // publishes refused (topic too long / empty)
uint32_t mqttTooBig();     // messages dropped by the library, over MQTT_MAX_MESSAGE_LEN
uint32_t mqttAuthFailed(); // connections refused on bad credentials
uint32_t mqttClients();    // clients currently connected
bool     mqttEnabled();
