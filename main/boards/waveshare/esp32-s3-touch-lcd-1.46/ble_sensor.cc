#include "ble_sensor.h"

#include <esp_log.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "settings.h"

namespace ble_sensor {
namespace {

constexpr char TAG[] = "ble_sensor";

// Advertisement layout (manufacturer data, AD type 0xFF), from the tail:
//   [temp u16 big-endian, 0.1 degC][humidity u8, %][mac 6 bytes]
// e.g. "01 09 01 20 45 ED 68 01 04 91 8C" -> 28.8C, 69%, ED:68:01:04:91:8C
// (the leading "01 09" bytes are ignored, parsing is tail-anchored).
constexpr int kMinMfgLen = 9;  // temp(2) + humidity(1) + mac(6)
constexpr int kMaxLoggedMacs = 8;

const char* kTargetName = "XL0801";

std::atomic<bool> s_started{false};
std::atomic<bool> s_synced{false};
std::atomic<bool> s_want_scan{false};
std::atomic<bool> s_scanning{false};
AdvertisementCallback s_callback;

std::string SlotKey(int slot) { return "slot" + std::to_string(slot + 1); }

// NimBLE reports may arrive several times per second; only log the first
// packet of each device at INFO level so the boot log stays readable.
bool ShouldLogMac(const char* mac) {
    static char logged[kMaxLoggedMacs][18] = {};
    static int count = 0;
    for (int i = 0; i < count; ++i) {
        if (strncmp(logged[i], mac, 17) == 0) {
            return false;
        }
    }
    if (count < kMaxLoggedMacs) {
        snprintf(logged[count], sizeof(logged[0]), "%s", mac);
        ++count;
        return true;
    }
    return false;
}

void HandleAdvertisement(const uint8_t* data, int length) {
    const uint8_t* name = nullptr;
    int name_len = 0;
    const uint8_t* mfg = nullptr;
    int mfg_len = 0;

    // Walk the AD structures; both the name and the manufacturer data may
    // appear in this same payload (the sample capture carries them together).
    int offset = 0;
    while (offset + 1 < length) {
        uint8_t field_len = data[offset];
        if (field_len == 0 || offset + 1 + field_len > length) {
            break;
        }
        uint8_t type = data[offset + 1];
        const uint8_t* value = &data[offset + 2];
        int value_len = field_len - 1;
        if (type == 0x09 /* complete name */ || type == 0x08 /* shortened name */) {
            name = value;
            name_len = value_len;
        } else if (type == 0xFF /* manufacturer data */) {
            mfg = value;
            mfg_len = value_len;
        }
        offset += field_len + 1;
    }

    if (name == nullptr || name_len != static_cast<int>(strlen(kTargetName)) ||
        memcmp(name, kTargetName, name_len) != 0) {
        return;
    }
    if (mfg == nullptr || mfg_len < kMinMfgLen) {
        return;
    }

    Reading reading;
    reading.temperature_c = ((mfg[mfg_len - 9] << 8) | mfg[mfg_len - 8]) / 10.0f;
    reading.humidity_percent = static_cast<float>(mfg[mfg_len - 7]);

    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", mfg[mfg_len - 6], mfg[mfg_len - 5],
             mfg[mfg_len - 4], mfg[mfg_len - 3], mfg[mfg_len - 2], mfg[mfg_len - 1]);

    if (ShouldLogMac(mac)) {
        ESP_LOGI(TAG, "found %s: %.1fC %.0f%%", mac, reading.temperature_c,
                 reading.humidity_percent);
    } else {
        ESP_LOGD(TAG, "%s: %.1fC %.0f%%", mac, reading.temperature_c, reading.humidity_percent);
    }

    if (s_callback) {
        s_callback(mac, reading);
    }
}

int GapEventCallback(ble_gap_event* event, void* /*arg*/);

void StartScanInternal() {
    if (!s_synced) {
        return;  // OnSync() will start the scan when the host is ready.
    }
    if (s_scanning) {
        return;
    }

    uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }

    struct ble_gap_disc_params params = {};
    params.filter_policy = 0;  // BLE_HCI_SCAN_FILT_NO_WL: accept all advertisers
    // Active scan: catches devices that split name/payload over scan
    // response. filter_duplicates must stay 0 - the sensor only updates its
    // temperature/humidity inside repeated advertisements.
    params.passive = 0;
    params.filter_duplicates = 0;
    params.itvl = 0;    // controller default
    params.window = 0;  // controller default
    params.limited = 0;

    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, GapEventCallback, nullptr);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        s_scanning = true;
        ESP_LOGI(TAG, "scan started");
    } else {
        ESP_LOGE(TAG, "ble_gap_disc failed: rc=%d", rc);
    }
}

int GapEventCallback(ble_gap_event* event, void* /*arg*/) {
    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            HandleAdvertisement(event->disc.data, event->disc.length_data);
            break;
        case BLE_GAP_EVENT_DISC_COMPLETE:
            s_scanning = false;
            ESP_LOGW(TAG, "scan stopped, reason=%d", event->disc_complete.reason);
            if (s_want_scan) {
                StartScanInternal();
            }
            break;
        default:
            break;
    }
    return 0;
}

void HostTask(void* /*param*/) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void OnReset(int reason) { ESP_LOGE(TAG, "NimBLE resetting, reason=%d", reason); }

void OnSync() {
    // Make sure we have a proper identity address set (public preferred).
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: rc=%d", rc);
    }
    s_synced = true;
    if (s_want_scan) {
        StartScanInternal();
    }
}

}  // namespace

void SetCallback(AdvertisementCallback callback) { s_callback = std::move(callback); }

bool EnsureScanning() {
    s_want_scan = true;

    if (!s_started) {
        s_started = true;
        esp_err_t ret = nimble_port_init();
        if (ret != ESP_OK) {
            s_started = false;
            ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
            return false;
        }
        ble_hs_cfg.reset_cb = OnReset;
        ble_hs_cfg.sync_cb = OnSync;
        // Scan-only observer: no pairing/bonding, so the security store is
        // never touched and ble_store_config_init() is intentionally skipped.
        nimble_port_freertos_init(HostTask);
        return true;  // scan starts from OnSync()
    }

    StartScanInternal();
    return s_scanning;
}

void StopScanning() {
    s_want_scan = false;
    if (!s_scanning) {
        return;
    }
    int rc = ble_gap_disc_cancel();
    if (rc == 0 || rc == BLE_HS_ENOTCONN) {
        s_scanning = false;
        ESP_LOGI(TAG, "scan stopped");
    } else {
        ESP_LOGW(TAG, "ble_gap_disc_cancel rc=%d", rc);
    }
}

std::string GetBinding(int slot) {
    if (slot < 0 || slot >= kMaxSlots) {
        return "";
    }
    Settings settings("ble_sensor", false);
    return settings.GetString(SlotKey(slot), "");
}

bool SetBinding(int slot, const std::string& mac) {
    if (slot < 0 || slot >= kMaxSlots) {
        return false;
    }
    Settings settings("ble_sensor", true);
    settings.SetString(SlotKey(slot), mac);
    ESP_LOGI(TAG, "slot %d bound to '%s'", slot + 1, mac.c_str());
    return true;
}

}  // namespace ble_sensor
