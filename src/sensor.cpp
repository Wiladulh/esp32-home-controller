#include "sensor.h"
#include <algorithm>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "config.h"

namespace {
SensorType type = SensorType::None;
int8_t pin = -1;
uint32_t intervalMs = 30000;
uint32_t lastReadMs = 0;
bool haveReading = false;
float tempC = 0;
float hum = 0;
bool humOk = false;
Sensor::ReadingCb readingCb;

DHT* dht = nullptr;
OneWire* oneWire = nullptr;
DallasTemperature* ds = nullptr;
}  // namespace

namespace Sensor {

void begin() {
  const SensorCfg& c = Config::get().sensor;
  type = c.type;
  pin = c.gpio;
  intervalMs = (uint32_t)std::max<uint16_t>(c.intervalSec, (uint16_t)5) * 1000UL;
  haveReading = false;

  if (type == SensorType::Dht22 && pin >= 0) {
    dht = new DHT(pin, DHT22);
    dht->begin();
  } else if (type == SensorType::Ds18b20 && pin >= 0) {
    oneWire = new OneWire(pin);
    ds = new DallasTemperature(oneWire);
    ds->begin();
  }
}

void tick() {
  if (type == SensorType::None || pin < 0) return;
  uint32_t now = millis();
  if (haveReading && now - lastReadMs < intervalMs) return;
  if (!haveReading && lastReadMs != 0 && now - lastReadMs < 2000) return;  // retry spacing
  lastReadMs = now;

  if (type == SensorType::Dht22 && dht) {
    float t = dht->readTemperature();
    float h = dht->readHumidity();
    if (isnan(t) || isnan(h)) return;  // failed read, keep previous value
    tempC = t;
    hum = h;
    humOk = true;
  } else if (type == SensorType::Ds18b20 && ds) {
    ds->requestTemperatures();
    float t = ds->getTempCByIndex(0);
    if (t == DEVICE_DISCONNECTED_C) return;
    tempC = t;
    humOk = false;
  } else {
    return;
  }

  haveReading = true;
  if (readingCb) readingCb(tempC, hum, humOk);
}

bool valid() { return haveReading; }
float lastTemp() { return tempC; }
float lastHumidity() { return hum; }
bool hasHumidity() { return humOk; }
void onReading(ReadingCb cb) { readingCb = cb; }

}  // namespace Sensor
