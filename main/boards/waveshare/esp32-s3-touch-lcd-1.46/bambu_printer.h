#ifndef BAMBU_PRINTER_H
#define BAMBU_PRINTER_H

#include <functional>
#include <string>

// Local Bambu Lab printer status client for the third-screen dashboard of
// esp32-s3-touch-lcd-1.46. Connects straight to the printer's own MQTT
// broker (TLS on port 8883, username "bblp", password = the printer's LAN
// access code) and parses the push reports on device/<SN>/report - the same
// wire protocol Bambu Handy / BambuStudio use on the LAN. Connection
// parameters are persisted in NVS namespace "bambu_printer" and edited
// through the on-screen settings page (no web portal).
//
// Threading: MQTT events arrive on the esp-mqtt task; the status callback is
// deferred onto the esp_timer task (never the MQTT task) so it can safely
// take the LVGL lock without deadlocking against a client restart issued
// from the UI task.
namespace bambu_printer {

// Lifecycle of the connection itself. kNotConfigured = no (or incomplete)
// NVS entry; kConnecting = client running but no session yet (also while
// Wi-Fi is down); kOnline = MQTT session established; kOffline = an attempt
// failed (printer powered off / unreachable / TLS rejected).
enum class Conn { kNotConfigured, kConnecting, kOnline, kOffline };

struct Status {
    Conn conn = Conn::kNotConfigured;
    // Raw print.gcode_state ("RUNNING"/"PAUSE"/"IDLE"/"FINISH"/"FAILED"/...),
    // empty until the first report of the session arrives.
    char state[24] = {};
    int progress_percent = -1;   // print.mc_percent, -1 = unknown
    int remaining_minutes = -1;  // print.mc_remaining_time, -1 = unknown
    float nozzle_temp = -1.0f;   // print.nozzle_temper, <0 = unknown
    float bed_temp = -1.0f;      // print.bed_temper, <0 = unknown
};

struct Config {
    std::string host;         // printer IP or hostname
    std::string serial;       // printer SN (topic segment)
    std::string access_code;  // 6-digit LAN access code

    bool is_ready() const { return !host.empty() && !serial.empty() && !access_code.empty(); }
};

// Invoked (on the esp_timer task) whenever any field of Status changes.
// Register before the first GetStatus() read to catch early transitions.
using StatusCallback = std::function<void(const Status& status)>;
void SetCallback(StatusCallback callback);

// Snapshot of the current status; safe from any task.
Status GetStatus();

// Snapshot of the persisted configuration (cached copy, no NVS read).
Config GetConfig();

// Persist all three fields (empty values are rejected) and reconnect with
// the new parameters when the network is up. Returns false when any field
// is empty after trimming.
bool SetConfig(const std::string& host, const std::string& serial, const std::string& access_code);

// Network lifecycle hooks; call these from the board's network event
// callback. OnNetworkUp starts the client after a short delay (keeps the
// peak of two TLS handshakes - Bambu's and the XiaoZhi cloud's - apart),
// OnNetworkDown tears it down (also used when entering Wi-Fi config mode,
// where the radio must stay quiet).
void OnNetworkUp();
void OnNetworkDown();

}  // namespace bambu_printer

#endif  // BAMBU_PRINTER_H
