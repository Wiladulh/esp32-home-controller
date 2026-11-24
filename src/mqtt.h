#pragma once
#include <Arduino.h>

// MQTT over WSS/WS/TCP using ESP-IDF esp_mqtt (bundled with Arduino-ESP32).
//
// Topics (prefix comes from WebUI, e.g. "home1/"; may be empty):
//   command:  {prefix}relay1 .. {prefix}relay8   payload: on | off   (also 1 / 0)
//   command:  {prefix}fan1                        payload: 0 | 1 | 2 | 3
//   state:    {prefix}relay1/state               retained, on | off
//   state:    {prefix}fan1/state                 retained, 0..3
//   sensor:   {prefix}sensor/temp                retained, degrees C
//   sensor:   {prefix}sensor/humidity            retained, %  (DHT22 only)
//   status:   {prefix}status                     retained, online | offline (LWT)
namespace Mqtt {

void begin();                 // builds URI from config; connects asynchronously
void stop();
bool connected();
void publishRelay(uint8_t index, bool on);
void publishFan(uint8_t speed);
void publishSensor(float tempC, float humidity, bool hasHumidity);

}  // namespace Mqtt
