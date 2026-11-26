#include "sensor.h"
#include <algorithm>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
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
SemaphoreHandle_t stateMutex = nullptr;
}  // namespace

namespace Sensor {

void begin() {
  stateMutex = xSemaphoreCreateMutex();
  if (!stateMutex) return;
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
  if (type == SensorType::None || pin < 0 || !stateMutex) return;
  const uint32_t now = millis();
  if (haveReading && now - lastReadMs < intervalMs) return;
  if (!haveReading && lastReadMs != 0 && now - lastReadMs < 2000) return;
  lastReadMs = now;

  float nextTemp = 0;
  float nextHumidity = 0;
  bool nextHasHumidity = false;
  if (type == SensorType::Dht22 && dht) {
    nextTemp = dht->readTemperature();
    nextHumidity = dht->readHumidity();
    if (isnan(nextTemp) || isnan(nextHumidity)) return;
    nextHasHumidity = true;
  } else if (type == SensorType::Ds18b20 && ds) {
    ds->requestTemperatures();
    nextTemp = ds->getTempCByIndex(0);
    if (nextTemp == DEVICE_DISCONNECTED_C) return;
  } else {
    return;
  }

  ReadingCb cb;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  tempC = nextTemp;
  hum = nextHumidity;
  humOk = nextHasHumidity;
  haveReading = true;
  cb = readingCb;
  xSemaphoreGive(stateMutex);
  if (cb) cb(nextTemp, nextHumidity, nextHasHumidity);
}

bool valid() {
  if (!stateMutex) return false;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool value = haveReading;
  xSemaphoreGive(stateMutex);
  return value;
}
float lastTemp() {
  if (!stateMutex) return 0;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const float value = tempC;
  xSemaphoreGive(stateMutex);
  return value;
}
float lastHumidity() {
  if (!stateMutex) return 0;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const float value = hum;
  xSemaphoreGive(stateMutex);
  return value;
}
bool hasHumidity() {
  if (!stateMutex) return false;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool value = humOk;
  xSemaphoreGive(stateMutex);
  return value;
}
void onReading(ReadingCb cb) {
  if (!stateMutex) return;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  readingCb = cb;
  xSemaphoreGive(stateMutex);
}

}  // namespace Sensor
