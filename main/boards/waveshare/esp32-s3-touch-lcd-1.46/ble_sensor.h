#ifndef BLE_SENSOR_H
#define BLE_SENSOR_H

#include <functional>
#include <string>

// NimBLE observer for the XL0801 temperature/humidity beacon used by the
// second-screen dashboard of esp32-s3-touch-lcd-1.46. The sensor only
// advertises (never connects), so everything here is a passive scan plus
// advertisement parsing. Bindings are persisted in NVS namespace
// "ble_sensor" ("slot1".."slot4", empty string = unbound).
namespace ble_sensor {

struct Reading {
    float temperature_c = 0.0f;
    float humidity_percent = 0.0f;
};

// Called from the NimBLE host task for every advertisement of a device
// named XL0801 with a parseable payload. `mac` is the 6-byte MAC carried in
// the manufacturer data (upper case, colon separated) - the address the
// user sees in the settings list. Must be registered before the first
// EnsureScanning() call.
using AdvertisementCallback = std::function<void(const std::string& mac, const Reading& reading)>;
void SetCallback(AdvertisementCallback callback);

// Start the NimBLE host (once) and a continuous, duplicate-friendly scan.
// Idempotent; safe to call from the LVGL task. The scan only actually
// starts after the host reports sync.
bool EnsureScanning();

// Stop scanning; the NimBLE host stays resident (restarting it is far more
// expensive than leaving it idle).
void StopScanning();

// Slot bindings, 0..kMaxSlots-1. GetBinding returns "" when unbound.
constexpr int kMaxSlots = 4;
std::string GetBinding(int slot);
// Persists to NVS. `mac` == "" clears the slot. Returns false on bad slot.
bool SetBinding(int slot, const std::string& mac);

}  // namespace ble_sensor

#endif  // BLE_SENSOR_H
