#include "bambu_printer.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <mqtt_client.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "settings.h"

namespace bambu_printer {
namespace {

constexpr char TAG[] = "bambu_printer";

constexpr char kNs[] = "bambu_printer";
constexpr char kKeyHost[] = "host";
constexpr char kKeySerial[] = "serial";
constexpr char kKeyCode[] = "access_code";

// Delay between STA getting an IP and the first MQTT attempt: keeps our TLS
// handshake apart from the XiaoZhi cloud protocol's first connect so the two
// never peak on the heap at the same moment.
constexpr uint64_t kConnectDelayUs = 2 * 1000 * 1000;
// Coalescing delay for status-change delivery to the UI.
constexpr uint64_t kNotifyDelayUs = 10 * 1000;
// First full-status pull lands shortly after SUBACK (letting the printer's
// session settle - BambuSphere delays its pushall too), then repeats every
// 60s: local reports are diff-based and can silently drop fields.
constexpr uint64_t kPushAllFirstUs = 1 * 1000 * 1000;
constexpr uint64_t kPushAllIntervalUs = 60 * 1000 * 1000;

constexpr char kStartPush[] = "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"start\"}}";
constexpr char kPushAll[] = "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\"}}";

// Ceiling on a reassembled report (a pushall answer is far below this).
constexpr size_t kMaxPayload = 64 * 1024;

// 16 KB receive buffer (pushall responses can be large) - allocated from
// PSRAM via CONFIG_MQTT_BUFFERS_ON_EXTERNAL_MEMORY; stack likewise.
constexpr int kMqttBufferSize = 16384;
constexpr int kMqttOutBufferSize = 2048;
constexpr int kMqttTaskStack = 8192;
constexpr int kMqttKeepaliveS = 30;
constexpr int kMqttReconnectMs = 15000;
constexpr int kMqttNetworkTimeoutMs = 10000;

// Client lifecycle (create/stop/destroy, periodic publish). NEVER taken by
// the MQTT event handler: esp_mqtt_client_stop() joins the MQTT task, so a
// handler blocked on this mutex while stop() waits for the handler would
// deadlock. When both mutexes are needed, life is always taken FIRST.
std::mutex s_life_mutex;
esp_mqtt_client_handle_t s_client = nullptr;

// Status/config/callback. Held only for short, non-blocking stretches.
std::mutex s_status_mutex;
Status s_status;
Status s_last_delivered;
Config s_config;
bool s_config_loaded = false;
StatusCallback s_callback;

// Topic names for the active session; written in ConnectNow() BEFORE the
// client is started (the task spawn is the barrier) and read by the handler.
std::string s_report_topic;
std::string s_request_topic;
bool s_initial_sync_sent = false;

// Chunk reassembly - only touched from the MQTT task (a new client's task
// only exists after the previous one was joined by stop()).
std::string s_incoming;
bool s_drop_payload = false;

std::atomic<bool> s_network_up{false};
esp_timer_handle_t s_connect_timer = nullptr;
esp_timer_handle_t s_notify_timer = nullptr;
esp_timer_handle_t s_pushall_timer = nullptr;

bool StatusDiffers(const Status& a, const Status& b) {
    return a.conn != b.conn || strcmp(a.state, b.state) != 0 ||
           a.progress_percent != b.progress_percent || a.remaining_minutes != b.remaining_minutes ||
           a.nozzle_temp != b.nozzle_temp || a.bed_temp != b.bed_temp;
}

// Arm the (coalescing) delivery timer. Called with NO mutex held.
void ScheduleNotify() {
    if (s_notify_timer != nullptr) {
        // ESP_ERR_INVALID_STATE just means a delivery is already pending;
        // it will observe the latest snapshot anyway.
        esp_timer_start_once(s_notify_timer, kNotifyDelayUs);
    }
}

void EnsureConfigLoadedLocked() {
    if (s_config_loaded) {
        return;
    }
    Settings settings(kNs, false);
    s_config.host = settings.GetString(kKeyHost, "");
    s_config.serial = settings.GetString(kKeySerial, "");
    s_config.access_code = settings.GetString(kKeyCode, "");
    s_config_loaded = true;
}

// Transition the connection state; clears stale telemetry whenever the
// connection leaves kOnline (or enters it - a fresh session starts blank).
void ApplyConn(Conn conn, bool clear_telemetry) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(s_status_mutex);
        if (s_status.conn != conn) {
            s_status.conn = conn;
            changed = true;
        }
        if (clear_telemetry) {
            if (s_status.state[0] != '\0') {
                s_status.state[0] = '\0';
                changed = true;
            }
            if (s_status.progress_percent != -1) {
                s_status.progress_percent = -1;
                changed = true;
            }
            if (s_status.remaining_minutes != -1) {
                s_status.remaining_minutes = -1;
                changed = true;
            }
            if (s_status.nozzle_temp != -1.0f) {
                s_status.nozzle_temp = -1.0f;
                changed = true;
            }
            if (s_status.bed_temp != -1.0f) {
                s_status.bed_temp = -1.0f;
                changed = true;
            }
        }
    }
    if (changed) {
        ScheduleNotify();
    }
}

int JsonInt(const cJSON* object, const char* key, int fallback) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(item)) {
        return static_cast<int>(item->valuedouble);
    }
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return atoi(item->valuestring);
    }
    return fallback;
}

float JsonFloat(const cJSON* object, const char* key, float fallback) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(item)) {
        return static_cast<float>(item->valuedouble);
    }
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return strtof(item->valuestring, nullptr);
    }
    return fallback;
}

// Merge one "print" push object into the current status. Absent fields keep
// their previous value - local reports are diffs, not snapshots.
void ParseReport(const char* data, size_t len) {
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) {
        return;
    }
    const cJSON* print = cJSON_GetObjectItemCaseSensitive(root, "print");
    if (!cJSON_IsObject(print)) {
        cJSON_Delete(root);
        return;
    }

    const cJSON* gcode = cJSON_GetObjectItemCaseSensitive(print, "gcode_state");
    const char* gcode_state =
        (cJSON_IsString(gcode) && gcode->valuestring != nullptr) ? gcode->valuestring : nullptr;

    int progress = JsonInt(print, "mc_percent", -1);
    if (progress < 0 || progress > 100) {
        progress = -1;
    }
    int remaining = JsonInt(print, "mc_remaining_time", -1);
    if (remaining < 0) {
        remaining = -1;
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(s_status_mutex);
        Status& s = s_status;
        if (gcode_state != nullptr && strcmp(s.state, gcode_state) != 0) {
            snprintf(s.state, sizeof(s.state), "%s", gcode_state);
            changed = true;
            ESP_LOGI(TAG, "gcode_state=%s progress=%d%% remain=%dmin nozzle=%.0f bed=%.0f", s.state,
                     s.progress_percent, s.remaining_minutes, s.nozzle_temp, s.bed_temp);
        }
        // Only overwrite with fields actually present in this push.
        if (progress >= 0 && progress != s.progress_percent) {
            s.progress_percent = progress;
            changed = true;
        }
        if (remaining >= 0 && remaining != s.remaining_minutes) {
            s.remaining_minutes = remaining;
            changed = true;
        }
        const cJSON* nozzle = cJSON_GetObjectItemCaseSensitive(print, "nozzle_temper");
        if (nozzle != nullptr) {
            float value = JsonFloat(print, "nozzle_temper", s.nozzle_temp);
            if (value != s.nozzle_temp) {
                s.nozzle_temp = value;
                changed = true;
            }
        }
        const cJSON* bed = cJSON_GetObjectItemCaseSensitive(print, "bed_temper");
        if (bed != nullptr) {
            float value = JsonFloat(print, "bed_temper", s.bed_temp);
            if (value != s.bed_temp) {
                s.bed_temp = value;
                changed = true;
            }
        }
    }
    cJSON_Delete(root);
    if (changed) {
        ScheduleNotify();
    }
}

// Runs on the esp_timer task with no mutex held: snapshot, de-duplicate
// against the last delivery, then hand off to the UI (which takes the LVGL
// lock itself). Deferring here is what keeps the UI task free to restart
// the MQTT client without deadlocking against an in-flight event handler.
void NotifyTimerCb(void*) {
    Status snapshot;
    StatusCallback callback;
    {
        std::lock_guard<std::mutex> lock(s_status_mutex);
        if (!StatusDiffers(s_status, s_last_delivered)) {
            return;
        }
        snapshot = s_status;
        s_last_delivered = s_status;
        callback = s_callback;
    }
    if (callback) {
        callback(snapshot);
    }
}

// Runs on the esp_timer task: initial pushall after SUBACK, then re-armed
// as a 60s periodic re-sync for as long as the session stays online.
void PushallTimerCb(void*) {
    std::lock_guard<std::mutex> life(s_life_mutex);
    if (s_client == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_status_mutex);
        if (s_status.conn != Conn::kOnline) {
            return;
        }
    }
    int msg_id = esp_mqtt_client_publish(s_client, s_request_topic.c_str(), kPushAll, 0, 1, 0);
    ESP_LOGD(TAG, "pushall published (msg_id=%d)", msg_id);
    esp_timer_start_once(s_pushall_timer, kPushAllIntervalUs);
}

void MqttEventHandler(void* /*arg*/, esp_event_base_t /*base*/, int32_t /*id*/, void* event_data) {
    auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);
    if (event == nullptr) {
        return;
    }

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "mqtt connected");
            s_initial_sync_sent = false;
            s_incoming.clear();
            s_drop_payload = false;
            ApplyConn(Conn::kOnline, true);
            int msg_id = esp_mqtt_client_subscribe(event->client, s_report_topic.c_str(), 1);
            ESP_LOGI(TAG, "subscribed %s (msg_id=%d)", s_report_topic.c_str(), msg_id);
            break;
        }

        case MQTT_EVENT_SUBSCRIBED: {
            if (s_initial_sync_sent) {
                break;
            }
            s_initial_sync_sent = true;
            // Enable the printer's continuous push stream, then pull a full
            // snapshot shortly after the session settles.
            esp_mqtt_client_publish(event->client, s_request_topic.c_str(), kStartPush, 0, 1, 0);
            if (s_pushall_timer != nullptr) {
                esp_timer_stop(s_pushall_timer);
                esp_timer_start_once(s_pushall_timer, kPushAllFirstUs);
            }
            break;
        }

        case MQTT_EVENT_DATA: {
            if (event->topic == nullptr || event->topic_len < 0 ||
                static_cast<size_t>(event->topic_len) != s_report_topic.size() ||
                memcmp(event->topic, s_report_topic.data(), s_report_topic.size()) != 0) {
                break;
            }
            const size_t consumed =
                static_cast<size_t>(event->current_data_offset) + event->data_len;
            if (event->current_data_offset == 0) {
                s_incoming.clear();
                s_drop_payload = static_cast<size_t>(event->total_data_len) > kMaxPayload;
                if (s_drop_payload) {
                    ESP_LOGW(TAG, "dropping oversized report: %d bytes", event->total_data_len);
                }
            }
            if (s_drop_payload) {
                if (consumed >= static_cast<size_t>(event->total_data_len)) {
                    s_drop_payload = false;
                }
                break;
            }
            s_incoming.append(event->data, event->data_len);
            if (consumed >= static_cast<size_t>(event->total_data_len)) {
                ParseReport(s_incoming.c_str(), s_incoming.size());
                s_incoming.clear();
            }
            break;
        }

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "mqtt disconnected");
            ApplyConn(Conn::kOffline, true);
            break;

        case MQTT_EVENT_ERROR:
            if (event->error_handle != nullptr) {
                ESP_LOGW(TAG,
                         "mqtt error type=%d tls=0x%x tls_stack=0x%x verify=0x%x sock_errno=%d",
                         event->error_handle->error_type, event->error_handle->esp_tls_last_esp_err,
                         event->error_handle->esp_tls_stack_err,
                         event->error_handle->esp_tls_cert_verify_flags,
                         event->error_handle->esp_transport_sock_errno);
            } else {
                ESP_LOGW(TAG, "mqtt error");
            }
            ApplyConn(Conn::kOffline, true);
            break;

        default:
            break;
    }
}

// Caller must hold s_life_mutex.
void DestroyClientLocked() {
    if (s_pushall_timer != nullptr) {
        esp_timer_stop(s_pushall_timer);
    }
    if (s_client != nullptr) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
        ESP_LOGI(TAG, "client destroyed");
    }
    s_initial_sync_sent = false;
    s_incoming.clear();
    s_drop_payload = false;
}

void ConnectNow() {
    if (!s_network_up.load()) {
        return;
    }

    Config cfg;
    {
        std::lock_guard<std::mutex> lock(s_status_mutex);
        EnsureConfigLoadedLocked();
        cfg = s_config;
    }
    if (!cfg.is_ready()) {
        ESP_LOGI(TAG, "printer not configured yet");
        ApplyConn(Conn::kNotConfigured, false);
        return;
    }

    std::lock_guard<std::mutex> life(s_life_mutex);
    DestroyClientLocked();
    ApplyConn(Conn::kConnecting, true);

    s_report_topic = "device/" + cfg.serial + "/report";
    s_request_topic = "device/" + cfg.serial + "/request";

    // Random suffix: the printer rejects a second client presenting the same
    // client_id as a live session (e.g. a phone app that never disconnected).
    static char client_id[32];
    snprintf(client_id, sizeof(client_id), "xiaozhi-%08x", static_cast<unsigned>(esp_random()));

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.hostname = cfg.host.c_str();
    mqtt_cfg.broker.address.port = 8883;
    mqtt_cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
    // The printer presents a per-device self-signed certificate that no
    // static CA can cover across models/firmware, so server verification is
    // skipped (CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY) and the LAN access
    // code authenticates the peer - the same trade-off every local-Bambu
    // integration makes. CN check off as well; the cert CN is the serial.
    mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
    mqtt_cfg.credentials.client_id = client_id;
    mqtt_cfg.credentials.username = "bblp";
    mqtt_cfg.credentials.authentication.password = cfg.access_code.c_str();
    mqtt_cfg.session.keepalive = kMqttKeepaliveS;
    mqtt_cfg.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    mqtt_cfg.buffer.size = kMqttBufferSize;
    mqtt_cfg.buffer.out_size = kMqttOutBufferSize;
    mqtt_cfg.task.stack_size = kMqttTaskStack;
    mqtt_cfg.network.timeout_ms = kMqttNetworkTimeoutMs;
    mqtt_cfg.network.reconnect_timeout_ms = kMqttReconnectMs;

    ESP_LOGI(TAG, "connecting to printer mqtt %s:8883 (serial=%s)", cfg.host.c_str(),
             cfg.serial.c_str());

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == nullptr) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed (heap=%u)",
                 static_cast<unsigned>(esp_get_free_heap_size()));
        ApplyConn(Conn::kOffline, true);
        return;
    }
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, MqttEventHandler, nullptr);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(err));
        DestroyClientLocked();
        ApplyConn(Conn::kOffline, true);
    }
}

void ConnectTimerCb(void*) { ConnectNow(); }

std::string TrimCopy(const std::string& value) {
    const char* spaces = " \t\r\n";
    size_t begin = value.find_first_not_of(spaces);
    if (begin == std::string::npos) {
        return "";
    }
    size_t end = value.find_last_not_of(spaces);
    return value.substr(begin, end - begin + 1);
}

}  // namespace

void SetCallback(StatusCallback callback) {
    {
        std::lock_guard<std::mutex> lock(s_status_mutex);
        s_callback = std::move(callback);
    }
    // Deliver a pending change (e.g. the connection came up between boot
    // and SetupUI) as soon as the UI can take it.
    ScheduleNotify();
}

Status GetStatus() {
    std::lock_guard<std::mutex> lock(s_status_mutex);
    return s_status;
}

Config GetConfig() {
    std::lock_guard<std::mutex> lock(s_status_mutex);
    EnsureConfigLoadedLocked();
    return s_config;
}

bool SetConfig(const std::string& host, const std::string& serial, const std::string& access_code) {
    Config cfg;
    cfg.host = TrimCopy(host);
    cfg.serial = TrimCopy(serial);
    cfg.access_code = TrimCopy(access_code);
    if (!cfg.is_ready()) {
        return false;
    }

    {
        Settings settings(kNs, true);
        settings.SetString(kKeyHost, cfg.host);
        settings.SetString(kKeySerial, cfg.serial);
        settings.SetString(kKeyCode, cfg.access_code);
    }
    {
        std::lock_guard<std::mutex> lock(s_status_mutex);
        s_config = cfg;
        s_config_loaded = true;
    }
    ESP_LOGI(TAG, "config saved: host=%s serial=%s code_len=%u", cfg.host.c_str(),
             cfg.serial.c_str(), static_cast<unsigned>(cfg.access_code.size()));
    // Reconnect immediately when the network is already up; otherwise the
    // next OnNetworkUp() picks the new config up.
    ConnectNow();
    return true;
}

void OnNetworkUp() {
    if (s_connect_timer == nullptr) {
        // First call arrives on the esp_event task before any client exists.
        esp_timer_create_args_t args = {};
        args.callback = ConnectTimerCb;
        args.name = "bambu_conn";
        esp_timer_create(&args, &s_connect_timer);

        args.callback = NotifyTimerCb;
        args.name = "bambu_notify";
        esp_timer_create(&args, &s_notify_timer);

        args.callback = PushallTimerCb;
        args.name = "bambu_push";
        esp_timer_create(&args, &s_pushall_timer);
    }
    s_network_up.store(true);
    if (s_connect_timer != nullptr) {
        esp_timer_start_once(s_connect_timer, kConnectDelayUs);
    } else {
        ConnectNow();
    }
}

void OnNetworkDown() {
    s_network_up.store(false);
    if (s_connect_timer != nullptr) {
        esp_timer_stop(s_connect_timer);
    }
    std::lock_guard<std::mutex> life(s_life_mutex);
    DestroyClientLocked();
    ApplyConn(Conn::kConnecting, true);
}

}  // namespace bambu_printer
