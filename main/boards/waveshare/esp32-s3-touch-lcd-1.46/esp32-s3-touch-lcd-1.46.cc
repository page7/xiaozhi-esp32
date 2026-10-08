#include "application.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_spd2010.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <iot_button.h>
#include <material_symbols.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "ble_sensor.h"
#include "esp_io_expander_tca9554.h"
#include "i2c_device.h"
#include "lcd_display.h"
#include "lvgl_theme.h"
#include "touch_spd2010.h"

#define TAG "waveshare_lcd_1_46"

// Function cards reachable by swipe: index 0 is the built-in AI screen, 1 is the
// sensor dashboard built by BuildSensorPage(). More cards can be appended later.
constexpr int kCardCount = 2;

// Horizontal distance needed to switch card, ~15% of the 412 px panel.
constexpr int kSwipeThresholdPx = 60;

// Sensor dashboard (card 1): 2x2 grid of equal circles on a black page.
// The panel is ROUND: the visible area is the circle inscribed in the
// 412x412 framebuffer (R = width_/2 = 206). A corner circle of the grid
// reaches sqrt(2)*(d+gap)/2 + d/2 from the panel centre, so margin 16 with
// d=184 reached 231px and got clipped by the round bezel; margin 50 gives
// d=(412-100-12)/2=150, reach 190px, ~16px inside the visible edge.
constexpr int kSensorCount = 4;
constexpr int kSensorGridMargin = 50;
constexpr int kSensorGridGap = 12;

// Tight gap between the temperature line and the humidity readout inside
// each circle (was 6px before the readout got its own box).
constexpr int kSensorRowGap = 2;

// Humidity readout box: fixed size, holds the four overlapping copies that
// fake bold text (see BuildSensorPage). Must be fixed - LV_SIZE_CONTENT
// would measure only the copies whose align offset is 0 and clip the rest.
constexpr int kHumBoxWidth = 130;
constexpr int kHumBoxHeight = 48;
constexpr int kHumBoldLayers = 4;  // pixel offsets (0,0) (1,0) (0,1) (1,1)

// Humidity ring thresholds: >20% orange, >30% red (red wins), else grey.
constexpr float kHumidityOrange = 20.0f;
constexpr float kHumidityRed = 30.0f;

// Settings gear at the bottom centre of card 1. font_material_symbols_30_4
// is already linked through lcd_display.cc; the Montserrat sizes come from
// config.json sdkconfig_append (CONFIG_LV_FONT_MONTSERRAT_40/12=y).
LV_FONT_DECLARE(font_material_symbols_30_4);

// Settings page (gear overlay): list of discovered XL0801 devices plus the
// slot picker. The panel is round, so the list is a centred column that
// stays inside the widest band of the 412x412 framebuffer.
constexpr int kSettingsListWidth = 280;

// 在waveshare_lcd_1_46类之前添加新的显示类
class CustomLcdDisplay : public SpiLcdDisplay {
public:
    static void rounder_event_cb(lv_event_t* e) {
        lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
        uint16_t x1 = area->x1;
        uint16_t x2 = area->x2;

        area->x1 = (x1 >> 2) << 2;  // round the start of coordinate down to the nearest 4M number
        area->x2 =
            ((x2 >> 2) << 2) + 3;  // round the end of coordinate up to the nearest 4N+3 number
    }

    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle, esp_lcd_panel_handle_t panel_handle,
                     int width, int height, int offset_x, int offset_y, bool mirror_x,
                     bool mirror_y, bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x,
                        mirror_y, swap_xy) {
        // Note: UI customization should be done in SetupUI(), not in constructor
        // to ensure lvgl objects are created before accessing them.
        // Theme color objects, however, already exist once the base constructor
        // has finished, and SetupUI() has not run yet - so adjust them here to
        // keep this board's black background / white text without touching the
        // shared themes in lcd_display.cc.
        for (const char* name : {"light", "dark"}) {
            auto* theme = LvglThemeManager::GetInstance().GetTheme(name);
            if (theme != nullptr) {
                theme->set_background_color(lv_color_hex(0x000000));
                theme->set_text_color(lv_color_hex(0xFFFFFF));
            }
        }
    }

    virtual void SetupUI() override {
        // Call parent SetupUI() first to create all lvgl objects
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);

        // Round-screen main UI keeps the top strip blank: hide the icon bar
        // (network/mute/battery) and the status bar carrying the clock, status
        // texts and notifications. Their update paths (UpdateStatusBar /
        // SetStatus / ShowNotification) keep running - they just draw into
        // hidden objects. low_battery_popup_ is a separate child of the screen
        // (lcd_display.cc), so the low-battery warning still pops up.
        lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t* screen = lv_screen_active();
        BuildSensorPage(screen);
        BuildSettingsPage(screen);
        // Swipe detection is NOT registered here: it now hangs off the touch
        // indev (see RegisterSwipeDetection) and is wired up by the board
        // right after the touch driver registers that indev.

        // Advertisement updates arrive on the NimBLE host task; the callback
        // takes the LVGL lock itself before touching any widget.
        ble_sensor::SetCallback([this](const std::string& mac, const ble_sensor::Reading& reading) {
            OnBleAdvertisement(mac, reading);
        });
        // Restore bindings; bound circles start out waiting for their first
        // advertisement. Keep scanning while anything is bound so readings
        // show up right after boot.
        for (int i = 0; i < kSensorCount; ++i) {
            bindings_[i] = ble_sensor::GetBinding(i);
            sensor_readings_[i].bound = !bindings_[i].empty();
            RefreshSensorLabels(i);
        }
        RefreshScanState();
    }

    // Swipe detection on the indev's OWN event list (lv_indev_add_event_cb).
    // Object-level events cannot be used here: LVGL dispatches PRESSED and
    // RELEASED only to the hit-tested object, and event_send_core() propagates
    // them to parents only while every object in the chain carries
    // LV_OBJ_FLAG_EVENT_BUBBLE (lv_obj_event.c) - nothing in this codebase sets
    // that flag, so a screen-level callback never fires (that was the original
    // no-op bug). The indev list instead receives PRESSED/RELEASED
    // unconditionally in send_event() BEFORE the object dispatch (lv_indev.c),
    // regardless of the hit target or scroll state. PRESSING is not forwarded
    // to the indev list, so the gesture is measured press point -> release
    // point instead of tracking intermediate moves. Safe to call before
    // SetupUI(): indev events do not depend on the widget tree.
    void RegisterSwipeDetection(lv_indev_t* indev) {
        DisplayLockGuard lock(this);
        touch_indev_ = indev;
        lv_indev_add_event_cb(indev, SwipeEventCb, LV_EVENT_PRESSED, this);
        lv_indev_add_event_cb(indev, SwipeEventCb, LV_EVENT_RELEASED, this);
    }

private:
    struct SensorReading {
        bool bound = false;     // slot has a MAC assigned
        bool has_data = false;  // an advertisement has been parsed for it
        float temperature_c = 0.0f;
        float humidity_percent = 0.0f;
    };

    struct FoundDevice {
        std::string mac;
        lv_obj_t* bind_btn = nullptr;
        lv_obj_t* reading_label = nullptr;  // live "28.5C 36%" header
    };

    // Card 1: the four-circle sensor dashboard. Created last, which puts it
    // above the AI screen, so an opaque background is enough to cover the UI
    // underneath.
    void BuildSensorPage(lv_obj_t* screen) {
        sensor_page_ = lv_obj_create(screen);
        lv_obj_set_size(sensor_page_, width_, height_);
        lv_obj_align(sensor_page_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(sensor_page_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(sensor_page_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sensor_page_, 0, 0);
        lv_obj_set_style_radius(sensor_page_, 0, 0);
        // The default theme gives every lv_obj the `card` style with
        // pad_all(PAD_DEF) = 16px, and lv_obj_set_pos() positions children
        // inside the padded content area (lv_obj_move_to adds space_left/top).
        // Without this the whole grid sat 16px down-right (top gap 32, bottom
        // gap 0 - circles touched the bottom edge).
        lv_obj_set_style_pad_all(sensor_page_, 0, 0);
        lv_obj_set_scrollbar_mode(sensor_page_, LV_SCROLLBAR_MODE_OFF);
        // Swipe detection lives on the touch indev's event list, so scrolling
        // state is irrelevant here; still keep the page non-scrollable so a
        // horizontal drag cannot rubber-band it.
        lv_obj_remove_flag(sensor_page_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(sensor_page_, LV_OBJ_FLAG_HIDDEN);

        const int diameter = (width_ - 2 * kSensorGridMargin - kSensorGridGap) / 2;
        ESP_LOGI(TAG, "sensor grid: diameter=%d margin=%d gap=%d", diameter, kSensorGridMargin,
                 kSensorGridGap);
        for (int i = 0; i < kSensorCount; ++i) {
            lv_obj_t* circle = lv_obj_create(sensor_page_);
            lv_obj_set_size(circle, diameter, diameter);
            lv_obj_set_pos(circle, kSensorGridMargin + (i % 2) * (diameter + kSensorGridGap),
                           kSensorGridMargin + (i / 2) * (diameter + kSensorGridGap));
            lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(circle, lv_color_hex(0x161616), 0);
            lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(circle, lv_color_hex(0x555555), 0);
            lv_obj_set_style_border_width(circle, 2, 0);
            lv_obj_set_style_pad_all(circle, 0, 0);
            lv_obj_set_style_pad_row(circle, kSensorRowGap, 0);
            lv_obj_set_scrollbar_mode(circle, LV_SCROLLBAR_MODE_OFF);
            lv_obj_remove_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
            // Temperature sits above the big humidity value, pair centered.
            lv_obj_set_flex_flow(circle, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(circle, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            sensor_circles_[i] = circle;

            sensor_temp_labels_[i] = lv_label_create(circle);
            // Deliberately no explicit font here: LV_STYLE_TEXT_FONT is
            // inheritable, so this label picks up the screen's font at draw
            // time. Capturing lv_obj_get_style_text_font(screen, ...) instead
            // would store a raw pointer into the theme's heap font object;
            // Assets::LvglStrategy::Apply later swaps the theme font via
            // SetTextFont() and frees the old one, leaving this label dangling
            // (boot-loop with InstrFetchProhibited at PC=0).
            lv_obj_set_style_text_color(sensor_temp_labels_[i], lv_color_hex(0xB0B0B0), 0);

            // Humidity readout: a fixed-size box holding four copies of the
            // same text at pixel offsets (0,0)/(1,0)/(0,1)/(1,1) - a 2x2
            // dilation that fakes bold. LVGL's text outline stroke only
            // renders for FreeType vector glyphs (draw_letter_outline sits
            // behind `#if LV_USE_FREETYPE && LV_USE_VECTOR_GRAPHIC`) and
            // CONFIG_LV_USE_FREETYPE is off, so stacking copies is the only
            // way to embolden a built-in bitmap font.
            lv_obj_t* hum_box = lv_obj_create(circle);
            lv_obj_set_size(hum_box, kHumBoxWidth, kHumBoxHeight);
            lv_obj_set_style_bg_opa(hum_box, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(hum_box, 0, 0);
            lv_obj_set_style_radius(hum_box, 0, 0);
            lv_obj_set_style_pad_all(hum_box, 0, 0);
            lv_obj_set_scrollbar_mode(hum_box, LV_SCROLLBAR_MODE_OFF);
            lv_obj_remove_flag(hum_box, LV_OBJ_FLAG_SCROLLABLE);
            for (int k = 0; k < kHumBoldLayers; ++k) {
                lv_obj_t* label = lv_label_create(hum_box);
                // Static built-in font: flash-resident, immune to the theme
                // font swap described above.
                lv_obj_set_style_text_font(label, &lv_font_montserrat_40, 0);
                lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
                lv_obj_align(label, LV_ALIGN_CENTER, k & 1, (k >> 1) & 1);
                sensor_humidity_labels_[i][k] = label;
            }

            // Sequence number pinned to the circle's bottom edge. FLOATING
            // makes flex skip the child (lv_obj_is_layout_positioned()
            // returns false for LV_OBJ_FLAG_FLOATING) while lv_obj_refr_pos()
            // still applies the BOTTOM_MID align against the circle.
            lv_obj_t* index_label = lv_label_create(circle);
            lv_obj_add_flag(index_label, LV_OBJ_FLAG_FLOATING);
            lv_obj_set_style_text_font(index_label, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(index_label, lv_color_hex(0x888888), 0);
            char index_buf[4];
            snprintf(index_buf, sizeof(index_buf), "%d", i + 1);
            lv_label_set_text(index_label, index_buf);
            lv_obj_align(index_label, LV_ALIGN_BOTTOM_MID, 0, -6);
            sensor_index_labels_[i] = index_label;
        }

        // Settings gear at the bottom centre of the page. On the round panel
        // y~400 still leaves ~57px of visible half-width, enough for the
        // ~30px icon; sits 12px below the bottom circles (grid ends at 362).
        // Hit area: first widen/extend with padding (pad_bottom pushes the
        // border box past the text without moving it), then align - hardware
        // taps repeatedly landed at y=406-408, a few px below the plain
        // 372..404 text box, and silently missed (no LV_EVENT_CLICKED).
        lv_obj_t* settings_icon = lv_label_create(sensor_page_);
        lv_obj_set_style_text_font(settings_icon, &font_material_symbols_30_4, 0);
        lv_obj_set_style_text_color(settings_icon, lv_color_hex(0xAAAAAA), 0);
        lv_label_set_text(settings_icon, MATERIAL_SYMBOLS_SETTINGS);
        lv_obj_set_style_pad_hor(settings_icon, 16, 0);
        lv_obj_set_style_pad_bottom(settings_icon, 12, 0);
        lv_obj_align(settings_icon, LV_ALIGN_BOTTOM_MID, 0, 4);
        lv_obj_add_flag(settings_icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(settings_icon, SettingsIconCb, LV_EVENT_CLICKED, this);

        for (int i = 0; i < kSensorCount; ++i) {
            RefreshSensorLabels(i);
        }
    }

    void RefreshSensorLabels(int index) {
        const SensorReading& reading = sensor_readings_[index];
        char buf[16];
        if (!reading.bound) {
            // Unbound: temperature says so, humidity stays empty.
            lv_label_set_text(sensor_temp_labels_[index], "未绑定");
            buf[0] = '\0';
        } else if (!reading.has_data) {
            // Bound but no advertisement parsed yet.
            lv_label_set_text(sensor_temp_labels_[index], "--");
            buf[0] = '\0';
        } else {
            snprintf(buf, sizeof(buf), "%.1f°C", reading.temperature_c);
            lv_label_set_text(sensor_temp_labels_[index], buf);
            snprintf(buf, sizeof(buf), "%.0f%%", reading.humidity_percent);
        }
        for (int k = 0; k < kHumBoldLayers; ++k) {
            lv_label_set_text(sensor_humidity_labels_[index][k], buf);
        }
        // Ring colour follows humidity: red above 30%, orange above 20%,
        // grey otherwise (red checked last so it wins over orange); grey
        // whenever there is no reading yet. lv_style_set_prop() updates the
        // local prop in place, so calling this every refresh does not grow
        // the style list.
        lv_color_t ring = lv_color_hex(0x555555);
        if (reading.has_data) {
            if (reading.humidity_percent > kHumidityOrange) {
                ring = lv_color_hex(0xFFBB00);
            }
            if (reading.humidity_percent > kHumidityRed) {
                ring = lv_color_hex(0xff4000);
            }
        }
        lv_obj_set_style_border_color(sensor_circles_[index], ring, 0);
    }

    // NimBLE host-task entry point: refresh bound circles and (while the
    // settings page is open) grow the discovered-device list.
    void OnBleAdvertisement(const std::string& mac, const ble_sensor::Reading& reading) {
        DisplayLockGuard lock(this);
        // Every XL0801 looks alike in the scan list, so the row header is the
        // live broadcast reading (not the model name) - that is how the user
        // tells devices apart. Rows are only created while the settings page
        // is open, but existing rows keep refreshing even while hidden.
        FoundDevice* device = FindFoundDevice(mac);
        if (device != nullptr) {
            UpdateDeviceReading(device, reading);
        } else if (settings_open_) {
            AddFoundDeviceRow(mac, reading);
        }
        for (int i = 0; i < kSensorCount; ++i) {
            if (bindings_[i].empty() || bindings_[i] != mac) {
                continue;
            }
            sensor_readings_[i] = {true, true, reading.temperature_c, reading.humidity_percent};
            RefreshSensorLabels(i);
        }
    }

    // Scanning is needed while the settings list is open (to discover
    // devices) or while any slot is bound (to keep readings live); otherwise
    // park the radio.
    void RefreshScanState() {
        bool any_bound = false;
        for (const auto& binding : bindings_) {
            if (!binding.empty()) {
                any_bound = true;
                break;
            }
        }
        if (settings_open_ || any_bound) {
            ble_sensor::EnsureScanning();
        } else {
            ble_sensor::StopScanning();
        }
    }

    static void SettingsIconCb(lv_event_t* e) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        self->OpenSettings();
    }

    static void BackButtonCb(lv_event_t* e) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        self->CloseSettings();
    }

    static void BindButtonCb(lv_event_t* e) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
        for (const auto& device : self->found_devices_) {
            if (device.bind_btn == btn) {
                self->OpenSlotPicker(device.mac);
                return;
            }
        }
    }

    static void SlotButtonCb(lv_event_t* e) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
        for (int i = 0; i < kSensorCount; ++i) {
            if (self->slot_buttons_[i] == btn) {
                self->ApplyBinding(i, self->pending_mac_);
                return;
            }
        }
    }

    void OpenSettings() {
        if (settings_open_ || settings_page_ == nullptr) {
            return;
        }
        ESP_LOGI(TAG, "settings opened");
        settings_open_ = true;
        lv_obj_add_flag(slot_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(settings_page_, LV_OBJ_FLAG_HIDDEN);
        RefreshScanState();
    }

    void CloseSettings() {
        if (!settings_open_) {
            return;
        }
        ESP_LOGI(TAG, "settings closed");
        settings_open_ = false;
        lv_obj_add_flag(slot_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(settings_page_, LV_OBJ_FLAG_HIDDEN);
        RefreshScanState();
    }

    void OpenSlotPicker(const std::string& mac) {
        pending_mac_ = mac;
        for (int i = 0; i < kSensorCount; ++i) {
            const bool bound = !bindings_[i].empty();
            lv_label_set_text(slot_state_labels_[i], bound ? "已绑" : "空");
            lv_obj_set_style_text_color(slot_state_labels_[i],
                                        bound ? lv_color_hex(0xFFBB00) : lv_color_hex(0x888888), 0);
        }
        lv_obj_remove_flag(slot_panel_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "binding picker for %s", mac.c_str());
    }

    // Assign `mac` to `slot`: one physical sensor occupies exactly one
    // circle, so the same MAC is cleared from any other slot. Then return to
    // the dashboard and keep scanning for readings.
    void ApplyBinding(int slot, const std::string& mac) {
        if (mac.empty() || slot < 0 || slot >= kSensorCount) {
            return;
        }
        for (int i = 0; i < kSensorCount; ++i) {
            if (i == slot || bindings_[i] != mac) {
                continue;
            }
            bindings_[i].clear();
            ble_sensor::SetBinding(i, "");
            sensor_readings_[i] = {};
            RefreshSensorLabels(i);
        }
        bindings_[slot] = mac;
        ble_sensor::SetBinding(slot, mac);
        sensor_readings_[slot] = {true, false, 0.0f, 0.0f};
        RefreshSensorLabels(slot);
        ESP_LOGI(TAG, "bound %s to slot %d", mac.c_str(), slot + 1);
        CloseSettings();  // also refreshes the scan state with new bindings
    }

    FoundDevice* FindFoundDevice(const std::string& mac) {
        for (auto& device : found_devices_) {
            if (device.mac == mac) {
                return &device;
            }
        }
        return nullptr;
    }

    static void FormatReading(char* buf, size_t size, const ble_sensor::Reading& reading) {
        snprintf(buf, size, "%.1f°C %.0f%%", reading.temperature_c, reading.humidity_percent);
    }

    // Rewrite the row header only when the formatted text actually changed:
    // advertisements arrive several times per second and lv_label_set_text()
    // reallocates the label buffer each call.
    void UpdateDeviceReading(FoundDevice* device, const ble_sensor::Reading& reading) {
        if (device->reading_label == nullptr) {
            return;
        }
        char buf[24];
        FormatReading(buf, sizeof(buf), reading);
        const char* current = lv_label_get_text(device->reading_label);
        if (current == nullptr || strcmp(current, buf) != 0) {
            lv_label_set_text(device->reading_label, buf);
        }
    }

    // Card 1 overlay: scan results with a bind button per XL0801 device and
    // a slot picker (choose circle 1-4). Created after the dashboard, so its
    // opaque background fully covers it while open.
    void BuildSettingsPage(lv_obj_t* screen) {
        settings_page_ = lv_obj_create(screen);
        lv_obj_set_size(settings_page_, width_, height_);
        lv_obj_align(settings_page_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(settings_page_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(settings_page_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(settings_page_, 0, 0);
        lv_obj_set_style_radius(settings_page_, 0, 0);
        lv_obj_set_style_pad_all(settings_page_, 0, 0);
        lv_obj_set_scrollbar_mode(settings_page_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(settings_page_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(settings_page_, LV_OBJ_FLAG_HIDDEN);

        // Round panel: keep the header short - at y~44 the visible half-width
        // is only ~130px. All Chinese text inherits the screen font (the
        // theme swap swaps the pointer behind it; never capture it here).
        lv_obj_t* title = lv_label_create(settings_page_);
        lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(title, "蓝牙设置");
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

        lv_obj_t* status = lv_label_create(settings_page_);
        lv_obj_set_style_text_color(status, lv_color_hex(0x888888), 0);
        lv_label_set_text(status, "正在搜索附近设备...");
        lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 62);

        settings_list_ = lv_obj_create(settings_page_);
        lv_obj_set_size(settings_list_, kSettingsListWidth, 180);
        lv_obj_align(settings_list_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_opa(settings_list_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(settings_list_, 0, 0);
        lv_obj_set_style_radius(settings_list_, 0, 0);
        lv_obj_set_style_pad_all(settings_list_, 0, 0);
        lv_obj_set_style_pad_row(settings_list_, 10, 0);
        lv_obj_set_scrollbar_mode(settings_list_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(settings_list_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(settings_list_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        settings_empty_label_ = lv_label_create(settings_list_);
        lv_obj_set_style_text_color(settings_empty_label_, lv_color_hex(0x666666), 0);
        lv_label_set_text(settings_empty_label_, "未找到 XL0801 设备");
        lv_obj_set_style_pad_all(settings_empty_label_, 24, 0);

        // Back to the dashboard (also reachable while the slot picker is
        // open - the picker only covers the middle band of the round screen).
        // Hit area: like the gear, taps land on the visible bottom arc
        // (y=406-408, below the plain 358..398 box) - grow the button down
        // to the screen edge so the whole lower band closes the page.
        lv_obj_t* back_btn = lv_button_create(settings_page_);
        lv_obj_set_size(back_btn, 120, 56);
        lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, 4);
        lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x333333), 0);
        lv_obj_t* back_label = lv_label_create(back_btn);
        lv_label_set_text(back_label, "返回");
        lv_obj_center(back_label);
        lv_obj_add_event_cb(back_btn, BackButtonCb, LV_EVENT_CLICKED, this);

        BuildSlotPicker();
    }

    void BuildSlotPicker() {
        slot_panel_ = lv_obj_create(settings_page_);
        lv_obj_set_size(slot_panel_, 300, 230);
        lv_obj_align(slot_panel_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(slot_panel_, lv_color_hex(0x111111), 0);
        lv_obj_set_style_bg_opa(slot_panel_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(slot_panel_, lv_color_hex(0x555555), 0);
        lv_obj_set_style_border_width(slot_panel_, 1, 0);
        lv_obj_set_style_radius(slot_panel_, 16, 0);
        lv_obj_set_style_pad_all(slot_panel_, 16, 0);
        lv_obj_set_style_pad_row(slot_panel_, 12, 0);
        lv_obj_set_scrollbar_mode(slot_panel_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(slot_panel_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(slot_panel_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(slot_panel_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(slot_panel_, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t* title = lv_label_create(slot_panel_);
        lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(title, "选择绑定位置");

        lv_obj_t* row = lv_obj_create(slot_panel_);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

        for (int i = 0; i < kSensorCount; ++i) {
            lv_obj_t* column = lv_obj_create(row);
            lv_obj_set_size(column, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(column, 0, 0);
            lv_obj_set_style_radius(column, 0, 0);
            lv_obj_set_style_pad_all(column, 0, 0);
            lv_obj_set_style_pad_row(column, 2, 0);
            lv_obj_set_scrollbar_mode(column, LV_SCROLLBAR_MODE_OFF);
            lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);

            lv_obj_t* btn = lv_button_create(column);
            lv_obj_set_size(btn, 56, 56);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x2B2B2B), 0);
            lv_obj_set_style_radius(btn, 12, 0);
            lv_obj_set_style_pad_all(btn, 0, 0);
            lv_obj_t* digit = lv_label_create(btn);
            lv_obj_set_style_text_font(digit, &lv_font_montserrat_40, 0);
            char buf[4];
            snprintf(buf, sizeof(buf), "%d", i + 1);
            lv_label_set_text(digit, buf);
            lv_obj_center(digit);
            lv_obj_add_event_cb(btn, SlotButtonCb, LV_EVENT_CLICKED, this);
            slot_buttons_[i] = btn;

            lv_obj_t* state = lv_label_create(column);
            lv_obj_set_style_text_color(state, lv_color_hex(0x888888), 0);
            lv_label_set_text(state, "空");
            slot_state_labels_[i] = state;
        }

        lv_obj_t* cancel_btn = lv_button_create(slot_panel_);
        lv_obj_set_size(cancel_btn, 96, 36);
        lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x333333), 0);
        lv_obj_t* cancel_label = lv_label_create(cancel_btn);
        lv_label_set_text(cancel_label, "取消");
        lv_obj_center(cancel_label);
        lv_obj_add_event_cb(cancel_btn, BackButtonCb, LV_EVENT_CLICKED, this);
    }

    // One row per unique XL0801; called from the NimBLE host task (holds the
    // LVGL lock) while the settings page is open. The header shows the live
    // broadcast reading instead of the model name - all nearby sensors are
    // called XL0801, the reading is what tells them apart.
    void AddFoundDeviceRow(const std::string& mac, const ble_sensor::Reading& reading) {
        if (settings_list_ == nullptr) {
            return;
        }
        if (settings_empty_label_ != nullptr) {
            lv_obj_delete(settings_empty_label_);
            settings_empty_label_ = nullptr;
        }

        lv_obj_t* row = lv_obj_create(settings_list_);
        lv_obj_set_size(row, kSettingsListWidth - 16, 64);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x161616), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        lv_obj_t* info = lv_obj_create(row);
        lv_obj_set_size(info, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(info, 0, 0);
        lv_obj_set_style_radius(info, 0, 0);
        lv_obj_set_style_pad_all(info, 0, 0);
        lv_obj_set_scrollbar_mode(info, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(info, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);

        // Reading header: inherits the screen font (has °C), updated in
        // place by UpdateDeviceReading() on every parsed advertisement.
        lv_obj_t* reading_label = lv_label_create(info);
        lv_obj_set_style_text_color(reading_label, lv_color_hex(0xFFFFFF), 0);
        char buf[24];
        FormatReading(buf, sizeof(buf), reading);
        lv_label_set_text(reading_label, buf);

        // MAC uses the small static font: ASCII only, immune to theme swaps.
        lv_obj_t* mac_label = lv_label_create(info);
        lv_obj_set_style_text_font(mac_label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(mac_label, lv_color_hex(0xAAAAAA), 0);
        lv_label_set_text(mac_label, mac.c_str());

        lv_obj_t* bind_btn = lv_button_create(row);
        lv_obj_set_size(bind_btn, 72, 40);
        lv_obj_set_style_bg_color(bind_btn, lv_color_hex(0x2F6BFF), 0);
        lv_obj_t* bind_label = lv_label_create(bind_btn);
        lv_label_set_text(bind_label, "绑定");
        lv_obj_center(bind_label);
        lv_obj_add_event_cb(bind_btn, BindButtonCb, LV_EVENT_CLICKED, this);

        found_devices_.push_back({mac, bind_btn, reading_label});
        ESP_LOGI(TAG, "device row added: %s %.1fC %.0f%%", mac.c_str(), reading.temperature_c,
                 reading.humidity_percent);
    }

    // Fires for PRESSED and RELEASED on the touch indev's event list. The
    // point read at PRESSED is the swipe start; at RELEASED the driver has
    // already latched the final contact position (s_last_x/y survive the
    // release read), so end - start is the full gesture delta.
    static void SwipeEventCb(lv_event_t* e) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(e));
        if (self->touch_indev_ == nullptr) {
            return;
        }

        lv_point_t p;
        lv_indev_get_point(self->touch_indev_, &p);

        if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
            self->swipe_start_ = p;
            self->swiping_ = true;
        } else {  // LV_EVENT_RELEASED
            if (!self->swiping_) {
                return;
            }
            self->swiping_ = false;
            self->HandleSwipe(p.x - self->swipe_start_.x, p.y - self->swipe_start_.y);
        }
    }

    void HandleSwipe(int dx, int dy) {
        // Logged unconditionally while the gesture threshold is still being
        // tuned on hardware, so even sub-threshold moves are visible.
        ESP_LOGI(TAG, "touch release dx=%d dy=%d card=%d", dx, dy, card_index_);
        // The settings overlay owns the screen while open. A deliberate
        // right swipe closes it (same direction as going "back" on the
        // dashboard); everything else is swallowed so horizontal drags on
        // the list cannot flip cards.
        if (settings_open_) {
            if (LV_ABS(dx) >= LV_ABS(dy) && dx >= kSwipeThresholdPx) {
                ESP_LOGI(TAG, "swipe right closes settings");
                CloseSettings();
            } else {
                ESP_LOGI(TAG, "swipe ignored (settings open)");
            }
            return;
        }
        if (LV_ABS(dx) >= LV_ABS(dy)) {
            if (LV_ABS(dx) < kSwipeThresholdPx) {
                return;
            }
            // Finger moving left advances, moving right goes back.
            ESP_LOGI(TAG, "swipe %s dx=%d dy=%d card %d", dx < 0 ? "left" : "right", dx, dy,
                     card_index_);
            dx < 0 ? NextCard() : PrevCard();
        } else if (LV_ABS(dy) >= kSwipeThresholdPx) {
            // Vertical swipes are reserved for scrolling; logged so a working
            // touch panel is visible even though nothing happens yet.
            ESP_LOGI(TAG, "swipe %s dx=%d dy=%d (vertical, unhandled)", dy < 0 ? "up" : "down", dx,
                     dy);
        }
    }

    void ShowCard(int index) {
        if (index < 0 || index >= kCardCount || index == card_index_) {
            return;
        }
        // Registered in the board constructor, swipe events can arrive before
        // SetupUI() built the page; ignore them instead of advancing
        // card_index_ with no page to show.
        if (sensor_page_ == nullptr || settings_open_) {
            return;
        }
        card_index_ = index;
        if (card_index_ == 0) {
            lv_obj_add_flag(sensor_page_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(sensor_page_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void NextCard() { ShowCard(card_index_ + 1); }
    void PrevCard() { ShowCard(card_index_ - 1); }

    SensorReading sensor_readings_[kSensorCount];
    // Cached NVS bindings ("" = unbound), loaded in SetupUI and kept in
    // sync by ApplyBinding(); the NimBLE task reads them without touching
    // NVS.
    std::array<std::string, kSensorCount> bindings_;
    std::vector<FoundDevice> found_devices_;
    std::string pending_mac_;
    bool settings_open_ = false;
    lv_obj_t* sensor_temp_labels_[kSensorCount] = {};
    lv_obj_t* sensor_circles_[kSensorCount] = {};
    lv_obj_t* sensor_humidity_labels_[kSensorCount][kHumBoldLayers] = {};
    lv_obj_t* sensor_index_labels_[kSensorCount] = {};
    lv_obj_t* sensor_page_ = nullptr;
    lv_obj_t* settings_page_ = nullptr;
    lv_obj_t* settings_list_ = nullptr;
    lv_obj_t* settings_empty_label_ = nullptr;
    lv_obj_t* slot_panel_ = nullptr;
    lv_obj_t* slot_buttons_[kSensorCount] = {};
    lv_obj_t* slot_state_labels_[kSensorCount] = {};
    lv_indev_t* touch_indev_ = nullptr;
    int card_index_ = 0;
    bool swiping_ = false;
    lv_point_t swipe_start_ = {0, 0};
};

class CustomBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    esp_io_expander_handle_t io_expander = NULL;
    // Concrete type (not LcdDisplay*) because InitializeTouch() hands the
    // touch indev to the board-specific RegisterSwipeDetection().
    CustomLcdDisplay* display_;
    button_handle_t boot_btn, pwr_btn;
    button_driver_t* boot_btn_driver_ = nullptr;
    button_driver_t* pwr_btn_driver_ = nullptr;
    static CustomBoard* instance_;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = I2C_SDA_IO,
            .scl_io_num = I2C_SCL_IO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeTca9554(void) {
        esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, I2C_ADDRESS, &io_expander);
        if (ret != ESP_OK)
            ESP_LOGE(TAG, "TCA9554 create returned error");

        // uint32_t input_level_mask = 0;
        // ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1,
        // IO_EXPANDER_INPUT);               // 设置引脚 EXIO0 和 EXIO1 模式为输入 ret =
        // esp_io_expander_get_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1,
        // &input_level_mask);             // 获取引脚 EXIO0 和 EXIO1 的电平状态,存放在
        // input_level_mask 中

        // ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_3,
        // IO_EXPANDER_OUTPUT);              // 设置引脚 EXIO2 和 EXIO3 模式为输出 ret =
        // esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_2 | IO_EXPANDER_PIN_NUM_3, 1);
        // // 将引脚电平设置为 1 ret = esp_io_expander_print_state(io_expander); // 打印引脚状态

        ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1,
                                      IO_EXPANDER_OUTPUT);  // 设置引脚 EXIO0 和 EXIO1 模式为输出
        ESP_ERROR_CHECK(ret);
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1,
                                        1);  // 复位 LCD 与 TouchPad
        ESP_ERROR_CHECK(ret);
        vTaskDelay(pdMS_TO_TICKS(300));
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1,
                                        0);  // 复位 LCD 与 TouchPad
        ESP_ERROR_CHECK(ret);
        vTaskDelay(pdMS_TO_TICKS(300));
        ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1,
                                        1);  // 复位 LCD 与 TouchPad
        ESP_ERROR_CHECK(ret);
    }

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize QSPI bus");

        const spi_bus_config_t bus_config = TAIJIPI_SPD2010_PANEL_BUS_QSPI_CONFIG(
            QSPI_PIN_NUM_LCD_PCLK, QSPI_PIN_NUM_LCD_DATA0, QSPI_PIN_NUM_LCD_DATA1,
            QSPI_PIN_NUM_LCD_DATA2, QSPI_PIN_NUM_LCD_DATA3, QSPI_LCD_H_RES * 80 * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(QSPI_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));
    }

    void InitializeSpd2010Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGI(TAG, "Install panel IO");

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = QSPI_PIN_NUM_LCD_CS;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 3;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST,
                                                 &io_config, &panel_io));

        ESP_LOGI(TAG, "Install SPD2010 panel driver");

        spd2010_vendor_config_t vendor_config = {
            .flags =
                {
                    .use_qspi_interface = 1,
                },
        };
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = QSPI_LCD_BIT_PER_PIXEL;
        panel_config.reset_gpio_num = QSPI_PIN_NUM_LCD_RST;
        panel_config.vendor_config = &vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_spd2010(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_disp_on_off(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = new CustomLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                        DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    // The panel ships with an SPD2010 touch controller: I2C 0x53, INT on IO4
    // and reset driven by TCA9554 EXIO1 (bit 0). It speaks its own HDP packet
    // protocol over 16-bit big-endian registers, so none of the esp_lcd_touch_*
    // drivers bundled with ESP-IDF can drive it - they all read 0xFF back.
    // touch_spd2010.cc carries the vendor implementation (ported from
    // demo/ESP-IDF/ESP32-S3-Touch-LCD-1.46-Test/main/Touch_Driver/).
    void InitializeTouch() {
        if (!spd2010_touch::Init(i2c_bus_, io_expander)) {
            ESP_LOGE(TAG, "SPD2010 touch controller not responding");
            return;
        }
        lv_indev_t* touch_indev = spd2010_touch::Register();
        if (touch_indev == nullptr) {
            ESP_LOGE(TAG, "failed to register LVGL touch input device");
            return;
        }
        // Runs before Application::Initialize() calls SetupUI(); swipe events
        // arrive on the indev list and do not touch any widget, so ordering
        // against the UI build does not matter.
        display_->RegisterSwipeDetection(touch_indev);
    }

    void InitializeButtonsCustom() {
        gpio_reset_pin(BOOT_BUTTON_GPIO);
        gpio_set_direction(BOOT_BUTTON_GPIO, GPIO_MODE_INPUT);
        gpio_reset_pin(PWR_BUTTON_GPIO);
        gpio_set_direction(PWR_BUTTON_GPIO, GPIO_MODE_INPUT);
        gpio_reset_pin(PWR_Control_PIN);
        gpio_set_direction(PWR_Control_PIN, GPIO_MODE_OUTPUT);
        // gpio_set_level(PWR_Control_PIN, false);
        gpio_set_level(PWR_Control_PIN, true);
    }

    void InitializeButtons() {
        instance_ = this;
        InitializeButtonsCustom();

        // Boot Button
        button_config_t boot_btn_config = {.long_press_time = 2000, .short_press_time = 0};
        boot_btn_driver_ = (button_driver_t*)calloc(1, sizeof(button_driver_t));
        boot_btn_driver_->enable_power_save = false;
        boot_btn_driver_->get_key_level = [](button_driver_t* button_driver) -> uint8_t {
            return !gpio_get_level(BOOT_BUTTON_GPIO);
        };
        ESP_ERROR_CHECK(iot_button_create(&boot_btn_config, boot_btn_driver_, &boot_btn));
        iot_button_register_cb(
            boot_btn, BUTTON_SINGLE_CLICK, nullptr,
            [](void* button_handle, void* usr_data) {
                auto self = static_cast<CustomBoard*>(usr_data);
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateStarting) {
                    self->EnterWifiConfigMode();
                    return;
                }
                app.ToggleChatState();
            },
            this);
        iot_button_register_cb(
            boot_btn, BUTTON_LONG_PRESS_START, nullptr,
            [](void* button_handle, void* usr_data) {
                // 长按无处理
            },
            this);

        // Power Button
        button_config_t pwr_btn_config = {.long_press_time = 5000, .short_press_time = 0};
        pwr_btn_driver_ = (button_driver_t*)calloc(1, sizeof(button_driver_t));
        pwr_btn_driver_->enable_power_save = false;
        pwr_btn_driver_->get_key_level = [](button_driver_t* button_driver) -> uint8_t {
            return !gpio_get_level(PWR_BUTTON_GPIO);
        };
        ESP_ERROR_CHECK(iot_button_create(&pwr_btn_config, pwr_btn_driver_, &pwr_btn));
        iot_button_register_cb(
            pwr_btn, BUTTON_SINGLE_CLICK, nullptr,
            [](void* button_handle, void* usr_data) {
                // 短按无处理
            },
            this);
        iot_button_register_cb(
            pwr_btn, BUTTON_LONG_PRESS_START, nullptr,
            [](void* button_handle, void* usr_data) {
                auto self = static_cast<CustomBoard*>(usr_data);
                if (self->GetBacklight()->brightness() > 0) {
                    self->GetBacklight()->SetBrightness(0);
                    gpio_set_level(PWR_Control_PIN, false);
                } else {
                    self->GetBacklight()->RestoreBrightness();
                    gpio_set_level(PWR_Control_PIN, true);
                }
            },
            this);
    }

public:
    CustomBoard() {
        InitializeI2c();
        InitializeTca9554();
        InitializeSpi();
        InitializeSpd2010Display();
        // Must run after the display exists (the touch input device is attached
        // to lv_display_get_default) and after I2C is up.
        InitializeTouch();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_SPK_GPIO_BCLK,
            AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, I2S_STD_SLOT_LEFT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN,
            I2S_STD_SLOT_RIGHT);  // I2S_STD_SLOT_LEFT / I2S_STD_SLOT_RIGHT / I2S_STD_SLOT_BOTH

        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(CustomBoard);

CustomBoard* CustomBoard::instance_ = nullptr;
