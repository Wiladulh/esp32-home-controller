#pragma once
#include <Arduino.h>
#include <functional>

// One temperature sensor per device: DHT22 or DS18B20 on the GPIO map.
namespace Sensor {

using ReadingCb = std::function<void(float celsius, float humidity, bool hasHumidity)>;

void begin();          // reads config, initialises the selected sensor
void tick();           // reads at configured interval (blocking only during the read itself)
bool valid();          // at least one successful reading
float lastTemp();
float lastHumidity();
bool hasHumidity();
void onReading(ReadingCb cb);

}  // namespace Sensor
