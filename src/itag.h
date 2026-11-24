#pragma once
#include <Arduino.h>
#include <vector>

// iTag single-button BLE remote, one device, drives the fan.
// Click counting: each notification counts as one press; presses closer than
// CLICK_WINDOW_MS are grouped. Learn mode exposes raw notification bytes.
namespace ITag {

struct Found {
  String name;
  String addr;   // "AA:BB:CC:DD:EE:FF"
  int rssi;
};

struct RawEvent {
  uint32_t ms;
  String hex;    // raw notification bytes in hex
};

using ClickCb = void (*)(uint8_t clicks);

void begin(ClickCb cb);       // initialises BLE stack (only when iTag is enabled)
void tick();                  // non-blocking: connects, reconnects, groups clicks
void startScan(uint32_t ms = 5000);   // non-blocking scan; results via scanResults()
bool scanning();
std::vector<Found> scanResults();
std::vector<RawEvent> recentRaw();
bool connected();
void stop();

}  // namespace ITag
