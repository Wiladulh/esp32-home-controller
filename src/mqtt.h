#pragma once
#include <Arduino.h>

// MQTT over WSS/WS/TCP using ESP-IDF esp_mqtt (bundled with Arduino-ESP32).
//
// Topics (prefix comes from WebUI, e.g. "set/set"; may be empty):
//   relay command + state: {prefix}/relay1 .. {prefix}/relay8, payload on | off (also 1 / 0)
//   fan command + state:   {prefix}/fan1 .. {prefix}/fan3, payload on | off; ON selects that speed
//   Device status:         {prefix}/status, retained online | offline (LWT)
//   Sensor values:         {prefix}/sensor/temp and /sensor/humidity, retained
//   sensor:   {prefix}/sensor/temp                retained, degrees C
//   sensor:   {prefix}/sensor/humidity            retained, % (DHT22 only)
//   status:   {prefix}/status                     retained, online | offline (LWT)
// One separator slash is inserted automatically after the configured prefix.
namespace Mqtt {

void begin();                 // builds URI from config; connects asynchronously
void stop();
bool connected();
void publishRelay(uint8_t index, bool on);
void publishFan(uint8_t speed);
void publishSensor(float tempC, float humidity, bool hasHumidity);

}  // namespace Mqtt
