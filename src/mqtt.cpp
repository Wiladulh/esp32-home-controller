#include "mqtt.h"
#include <mqtt_client.h>
#include <esp_crt_bundle.h>
#include <string.h>
#include <strings.h>
#include <atomic>
#include "config.h"
#include "fan.h"
#include "relay.h"

namespace {
esp_mqtt_client_handle_t client = nullptr;
std::atomic<bool> isConnected{false};
String prefix;

String topic(const char* suffix) {
  String base = prefix;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  String leaf = suffix;
  while (leaf.startsWith("/")) leaf.remove(0, 1);
  if (base.length() == 0) return leaf;
  if (leaf.length() == 0) return base;
  return base + "/" + leaf;
}

void subscribeAll() {
  for (uint8_t i = 0; i < MAX_RELAYS; i++) {
    if (!Relay::isEnabled(i)) continue;
    String t = topic(("relay" + String(i + 1)).c_str());
    esp_mqtt_client_subscribe(client, t.c_str(), 1);
  }
  if (Fan::isEnabled()) {
    for (uint8_t speed = 1; speed <= Fan::maxSpeed(); speed++) {
      String t = topic(("fan" + String(speed)).c_str());
      esp_mqtt_client_subscribe(client, t.c_str(), 1);
    }
  }
}

void publishAllStates() {
  for (uint8_t i = 0; i < MAX_RELAYS; i++) {
    if (Relay::isEnabled(i)) Mqtt::publishRelay(i, Relay::get(i));
  }
  if (Fan::isEnabled()) Mqtt::publishFan(Fan::getSpeed());
}

// Payload parsing: on/1 -> true, off/0 -> false, anything else -> -1
int parseOnOff(const char* p, int len) {
  if (len == 2 && strncasecmp(p, "on", 2) == 0) return 1;
  if (len == 1 && p[0] == '1') return 1;
  if (len == 3 && strncasecmp(p, "off", 3) == 0) return 0;
  if (len == 1 && p[0] == '0') return 0;
  return -1;
}

void handleCommand(const char* t, int tlen, const char* d, int dlen, bool retained) {
  // Never execute broker-retained commands replayed during (re)subscription.
  if (retained) return;
  // MQTT event topic/data buffers are length-delimited, not null-terminated.
  char tb[128];
  if (tlen <= 0 || tlen >= (int)sizeof(tb)) return;
  memcpy(tb, t, tlen);
  tb[tlen] = '\0';

  String root = prefix;
  while (root.endsWith("/")) root.remove(root.length() - 1);
  String sTopic(tb);
  String topicRoot = root.length() ? root + "/" : "";
  if (!sTopic.startsWith(topicRoot)) return;
  String rest = sTopic.substring(topicRoot.length());

  if (rest.startsWith("relay") && rest.length() == 6 && rest[5] >= '1' && rest[5] <= '8') {
    const uint8_t index = rest[5] - '1';
    if (!Relay::isEnabled(index)) return;
    const int value = parseOnOff(d, dlen);
    if (value >= 0) Relay::set(index, value == 1);
    return;
  }

  if (rest.startsWith("fan") && rest.length() == 4 && rest[3] >= '1' && rest[3] <= '3') {
    const uint8_t speed = rest[3] - '0';
    if (!Fan::isEnabled() || speed > Fan::maxSpeed()) return;
    const int value = parseOnOff(d, dlen);
    if (value == 1) Fan::setSpeed(speed);
    else if (value == 0) Fan::setSpeedIfCurrent(speed, 0);
  }
}

void eventHandler(void*, esp_event_base_t, int32_t id, void* data) {
  esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
  switch (id) {
    case MQTT_EVENT_CONNECTED: {
      isConnected = true;
      String st = topic("status");
      esp_mqtt_client_enqueue(client, st.c_str(), "online", 0, 1, 1, true);
      subscribeAll();
      publishAllStates();
      break;
    }
    case MQTT_EVENT_DISCONNECTED:
      isConnected = false;
      break;
    case MQTT_EVENT_DATA:
      handleCommand(e->topic, e->topic_len, e->data, e->data_len, e->retain);
      break;
    default:
      break;
  }
}

String buildUri(const MqttCfg& m) {
  // wss://host:port/path  (scheme chosen in config: "wss" or "ws" or "mqtt" for TCP)
  String scheme = m.scheme;
  String path = m.path;
  if (path.length() == 0) path = "/";
  if (!path.startsWith("/")) path = "/" + path;
  return scheme + "://" + m.host + ":" + String(m.port) + path;
}
}  // namespace

namespace Mqtt {

void begin() {
  const AppConfig& cfg = Config::get();
  if (!cfg.mqtt.enabled || cfg.mqtt.host[0] == '\0') return;

  prefix = cfg.mqtt.topicPrefix;
  String uri = buildUri(cfg.mqtt);

  esp_mqtt_client_config_t mc = {};
  mc.broker.address.uri = uri.c_str();
  // Verify server certificate with the built-in public root bundle (no user CA file).
  if (uri.startsWith("wss://") || uri.startsWith("mqtts://")) {
    mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  }
  if (cfg.mqtt.user[0] != '\0') {
    mc.credentials.username = cfg.mqtt.user;
    mc.credentials.authentication.password = cfg.mqtt.pass;
  }
  String willTopic = topic("status");
  mc.session.last_will.topic = willTopic.c_str();
  mc.session.last_will.msg = "offline";
  mc.session.last_will.msg_len = 7;
  mc.session.last_will.qos = 1;
  mc.session.last_will.retain = 1;
  mc.session.keepalive = 30;
  mc.network.reconnect_timeout_ms = 5000;

  client = esp_mqtt_client_init(&mc);
  if (!client) return;
  esp_mqtt_client_register_event(client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, eventHandler, nullptr);
  esp_mqtt_client_start(client);
}

void stop() {
  if (client) {
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = nullptr;
  }
  isConnected = false;
}

bool connected() { return isConnected.load(std::memory_order_acquire); }

void publishRelay(uint8_t index, bool on) {
  if (!client || !isConnected) return;
  String t = topic(("relay" + String(index + 1)).c_str());
  const char* v = on ? "on" : "off";
  esp_mqtt_client_enqueue(client, t.c_str(), v, 0, 1, 0, true);
}

void publishFan(uint8_t speed) {
  if (!client || !isConnected) return;
  for (uint8_t index = 1; index <= Fan::maxSpeed(); index++) {
    String t = topic(("fan" + String(index)).c_str());
    const char* value = speed == index ? "on" : "off";
    esp_mqtt_client_enqueue(client, t.c_str(), value, 0, 1, 0, true);
  }
}

void publishSensor(float tempC, float humidity, bool hasHumidity) {
  if (!client || !isConnected) return;
  char buf[16];
  String t = topic("sensor/temp");
  snprintf(buf, sizeof(buf), "%.1f", tempC);
  esp_mqtt_client_enqueue(client, t.c_str(), buf, 0, 0, 1, true);
  if (hasHumidity) {
    t = topic("sensor/humidity");
    snprintf(buf, sizeof(buf), "%.1f", humidity);
    esp_mqtt_client_enqueue(client, t.c_str(), buf, 0, 0, 1, true);
  }
}

}  // namespace Mqtt
