#include "itag.h"
#include <NimBLEDevice.h>
#include "config.h"

namespace {
constexpr uint32_t CLICK_WINDOW_MS = 400;
constexpr uint32_t RECONNECT_MIN_MS = 2000;
constexpr uint32_t RECONNECT_MAX_MS = 60000;
constexpr size_t RAW_KEEP = 20;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t pendingPresses = 0;
volatile uint32_t lastPressMs = 0;

ITag::ClickCb clickCb = nullptr;
NimBLEClient* client = nullptr;
NimBLERemoteCharacteristic* notifyChr = nullptr;
bool bleInit = false;
bool isScanning = false;
bool scanDone = false;
uint32_t reconnectDelay = RECONNECT_MIN_MS;
uint32_t nextAttemptMs = 0;

std::vector<ITag::Found> found;
std::vector<ITag::RawEvent> raw;

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
  portENTER_CRITICAL_ISR(&mux);
  if (pendingPresses < 255) pendingPresses++;
  lastPressMs = now;
  portEXIT_CRITICAL_ISR(&mux);

  // learn mode: keep the last raw notifications for the WebUI
  raw.push_back({now, toHex(data, len)});
  if (raw.size() > RAW_KEEP) raw.erase(raw.begin());
}

class ScanCb : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    std::string n = dev->getName();
    if (n.find("iTag") == std::string::npos && n.find("ITAG") == std::string::npos) return;
    String addr = dev->getAddress().toString().c_str();
    for (auto& f : found) {
      if (f.addr == addr) return;
    }
    found.push_back({String(n.c_str()), addr, dev->getRSSI()});
  }
  void onScanEnd(const NimBLEScanResults&, int) override {
    isScanning = false;
    scanDone = true;
  }
};
ScanCb scanCb;

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

  if (client == nullptr) client = NimBLEDevice::createClient();
  if (client->isConnected()) return true;

  NimBLEAddress addr(cfg.itagAddr, BLE_ADDR_PUBLIC);
  if (!client->connect(addr)) return false;

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
  if (!Config::get().itagEnabled) return;
  ensureInit();
}

void startScan(uint32_t ms) {
  if (!ensureInit() || isScanning) return;
  found.clear();
  scanDone = false;
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCb, false);
  scan->setActiveScan(true);
  isScanning = true;
  scan->start(ms, false);
}

bool scanning() { return isScanning; }
std::vector<Found> scanResults() { return found; }
std::vector<RawEvent> recentRaw() { return raw; }
bool connected() { return client && client->isConnected(); }

void stop() {
  if (client && client->isConnected()) client->disconnect();
}

void tick() {
  uint32_t now = millis();

  // reconnect with backoff 2s -> 60s
  if (Config::get().itagEnabled && !connected() && !isScanning && now >= nextAttemptMs) {
    if (tryConnect()) {
      reconnectDelay = RECONNECT_MIN_MS;
    } else {
      nextAttemptMs = now + reconnectDelay;
      reconnectDelay = min(reconnectDelay * 2, RECONNECT_MAX_MS);
    }
  }

  // group presses into clicks once the window has passed
  uint8_t n = 0;
  uint32_t last = 0;
  portENTER_CRITICAL(&mux);
  n = pendingPresses;
  last = lastPressMs;
  portEXIT_CRITICAL(&mux);
  if (n > 0 && now - last >= CLICK_WINDOW_MS) {
    portENTER_CRITICAL(&mux);
    pendingPresses = 0;
    portEXIT_CRITICAL(&mux);
    if (clickCb) clickCb(n);
  }
}

}  // namespace ITag
