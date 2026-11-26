#include "itag.h"
#include <NimBLEDevice.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"

namespace {
constexpr uint32_t CLICK_WINDOW_MS = 400;
constexpr uint32_t RECONNECT_MIN_MS = 2000;
constexpr uint32_t RECONNECT_MAX_MS = 60000;
constexpr size_t RAW_KEEP = 20;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t dataMutex = nullptr;
std::atomic<bool> connectedState{false};
volatile uint8_t pendingPresses = 0;
volatile uint32_t lastPressMs = 0;

ITag::ClickCb clickCb = nullptr;
NimBLEClient* client = nullptr;
NimBLERemoteCharacteristic* notifyChr = nullptr;
bool bleInit = false;
bool isScanning = false;

uint32_t reconnectDelay = RECONNECT_MIN_MS;
uint32_t nextAttemptMs = 0;

std::vector<ITag::Found> found;
std::vector<ITag::RawEvent> raw;

bool ensureDataMutex() {
  if (!dataMutex) dataMutex = xSemaphoreCreateMutex();
  return dataMutex != nullptr;
}

String toHex(const uint8_t* d, size_t n) {
  String s;
  char buf[3];
  for (size_t i = 0; i < n; i++) {
    snprintf(buf, sizeof(buf), "%02X", d[i]);
    s += buf;
  }
  return s;
}

void onNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  uint32_t now = millis();
  portENTER_CRITICAL(&mux);
  if (pendingPresses < 255) pendingPresses = (uint8_t)(pendingPresses + 1);
  lastPressMs = now;
  portEXIT_CRITICAL(&mux);

  // learn mode: keep the last raw notifications for the WebUI
  if (dataMutex) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    raw.push_back({now, toHex(data, len)});
    if (raw.size() > RAW_KEEP) raw.erase(raw.begin());
    xSemaphoreGive(dataMutex);
  }
}

class ScanCb : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    std::string n = dev->getName();
    if (n.find("iTag") == std::string::npos && n.find("ITAG") == std::string::npos) return;
    String addr = dev->getAddress().toString().c_str();
    if (!dataMutex) return;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    for (const auto& f : found) {
      if (f.addr == addr) {
        xSemaphoreGive(dataMutex);
        return;
      }
    }
    found.push_back({String(n.c_str()), addr, dev->getRSSI()});
    xSemaphoreGive(dataMutex);
  }
  void onScanEnd(const NimBLEScanResults&, int) override {
    if (!dataMutex) return;
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    isScanning = false;
    xSemaphoreGive(dataMutex);
  }
};
ScanCb scanCb;

class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*) override {
    connectedState.store(true, std::memory_order_release);
  }
  void onDisconnect(NimBLEClient*, int) override {
    connectedState.store(false, std::memory_order_release);
  }
};
ClientCallbacks clientCallbacks;

bool ensureInit() {
  if (bleInit) return true;
  if (!NimBLEDevice::init(Config::get().hostname)) return false;
  bleInit = true;
  return true;
}

bool tryConnect() {
  const AppConfig& cfg = Config::get();
  if (!cfg.itagEnabled || cfg.itagAddr[0] == '\0') return false;
  if (!ensureInit()) return false;

  if (client == nullptr) {
    client = NimBLEDevice::createClient();
    if (!client) return false;
    client->setClientCallbacks(&clientCallbacks, false);
  }
  if (client->isConnected()) return true;

  NimBLEAddress addr(cfg.itagAddr, BLE_ADDR_PUBLIC);
  if (!client->connect(addr)) {
    connectedState.store(false, std::memory_order_release);
    return false;
  }
  connectedState.store(true, std::memory_order_release);

  NimBLERemoteService* svc = client->getService(NimBLEUUID("FFE0"));
  if (!svc) { client->disconnect(); return false; }
  notifyChr = svc->getCharacteristic(NimBLEUUID("FFE1"));
  if (!notifyChr || !notifyChr->canNotify()) { client->disconnect(); return false; }
  if (!notifyChr->subscribe(true, onNotify)) { client->disconnect(); return false; }
  return true;
}
}  // namespace

namespace ITag {

void begin(ClickCb cb) {
  clickCb = cb;
  ensureDataMutex();
  if (!Config::get().itagEnabled) return;
  ensureInit();
}

void startScan(uint32_t ms) {
  if (!ensureDataMutex() || !ensureInit()) return;
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  if (isScanning) {
    xSemaphoreGive(dataMutex);
    return;
  }
  found.clear();
  isScanning = true;
  xSemaphoreGive(dataMutex);
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCb, false);
  scan->setActiveScan(true);
  if (!scan->start(ms, false)) {
    xSemaphoreTake(dataMutex, portMAX_DELAY);
    isScanning = false;
    xSemaphoreGive(dataMutex);
  }
}

bool scanning() {
  if (!dataMutex) return false;
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  const bool value = isScanning;
  xSemaphoreGive(dataMutex);
  return value;
}
std::vector<Found> scanResults() {
  if (!dataMutex) return {};
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  auto result = found;
  xSemaphoreGive(dataMutex);
  return result;
}
std::vector<RawEvent> recentRaw() {
  if (!dataMutex) return {};
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  auto result = raw;
  xSemaphoreGive(dataMutex);
  return result;
}
bool connected() { return connectedState.load(std::memory_order_acquire); }

void stop() {
  if (client && client->isConnected()) client->disconnect();
  connectedState.store(false, std::memory_order_release);
}

void tick() {
  uint32_t now = millis();

  // reconnect with backoff 2s -> 60s
  if (Config::get().itagEnabled && !connected() && !scanning() && now >= nextAttemptMs) {
    if (tryConnect()) {
      reconnectDelay = RECONNECT_MIN_MS;
    } else {
      nextAttemptMs = now + reconnectDelay;
      reconnectDelay = min(reconnectDelay * 2, RECONNECT_MAX_MS);
    }
  }

  // Atomically drain the click batch only after the grouping window has elapsed.
  uint8_t n = 0;
  portENTER_CRITICAL(&mux);
  if (pendingPresses > 0 && now - lastPressMs >= CLICK_WINDOW_MS) {
    n = pendingPresses;
    pendingPresses = 0;
  }
  portEXIT_CRITICAL(&mux);
  if (n > 0 && clickCb) clickCb(n);
}

}  // namespace ITag
