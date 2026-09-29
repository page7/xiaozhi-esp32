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
#include <esp_random.h>
#include <esp_timer.h>
#include <iot_button.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
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

// Bigger than the theme's 16 px text font for the humidity readout.
LV_FONT_DECLARE(font_noto_sans_basic_30_4);

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

        lv_obj_t* screen = lv_screen_active();
        BuildSensorPage(screen);
        // Swipe detection is NOT registered here: it now hangs off the touch
        // indev (see RegisterSwipeDetection) and is wired up by the board
        // right after the touch driver registers that indev.
    }

    // Hook for the four sensor readouts. Today the values come from the mock
    // timer below; later the BLE stack can call this from any task.
    void SetSensorReading(int index, float temperature_c, float humidity_percent) {
        if (index < 0 || index >= kSensorCount) {
            return;
        }
        DisplayLockGuard lock(this);
        sensor_readings_[index] = {temperature_c, humidity_percent};
        RefreshSensorLabels(index);
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
        float temperature_c;
        float humidity_percent;
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
            lv_obj_set_style_pad_row(circle, 6, 0);
            lv_obj_set_scrollbar_mode(circle, LV_SCROLLBAR_MODE_OFF);
            lv_obj_remove_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
            // Temperature sits above the big humidity value, pair centered.
            lv_obj_set_flex_flow(circle, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(circle, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);

            sensor_temp_labels_[i] = lv_label_create(circle);
            // Deliberately no explicit font here: LV_STYLE_TEXT_FONT is
            // inheritable, so this label picks up the screen's font at draw
            // time. Capturing lv_obj_get_style_text_font(screen, ...) instead
            // would store a raw pointer into the theme's heap font object;
            // Assets::LvglStrategy::Apply later swaps the theme font via
            // SetTextFont() and frees the old one, leaving this label dangling
            // (boot-loop with InstrFetchProhibited at PC=0).
            lv_obj_set_style_text_color(sensor_temp_labels_[i], lv_color_hex(0xB0B0B0), 0);

            sensor_humidity_labels_[i] = lv_label_create(circle);
            lv_obj_set_style_text_font(sensor_humidity_labels_[i], &font_noto_sans_basic_30_4, 0);
            lv_obj_set_style_text_color(sensor_humidity_labels_[i], lv_color_hex(0xFFFFFF), 0);
        }

        for (int i = 0; i < kSensorCount; ++i) {
            RefreshSensorLabels(i);
        }

        // Mock data until the BLE sensors are wired up.
        sensor_mock_timer_ = lv_timer_create(MockSensorTimerCb, 3000, this);
    }

    void RefreshSensorLabels(int index) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f°C", sensor_readings_[index].temperature_c);
        lv_label_set_text(sensor_temp_labels_[index], buf);
        snprintf(buf, sizeof(buf), "%.0f%%", sensor_readings_[index].humidity_percent);
        lv_label_set_text(sensor_humidity_labels_[index], buf);
    }

    static void MockSensorTimerCb(lv_timer_t* timer) {
        auto* self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        for (int i = 0; i < kSensorCount; ++i) {
            const float temperature =
                std::clamp(self->sensor_readings_[i].temperature_c +
                               (static_cast<int>(esp_random() % 5) - 2) * 0.1f,
                           20.0f, 30.0f);
            const float humidity = std::clamp(self->sensor_readings_[i].humidity_percent +
                                                  (static_cast<int>(esp_random() % 5) - 2) * 0.5f,
                                              40.0f, 70.0f);
            self->SetSensorReading(i, temperature, humidity);
        }
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
        if (sensor_page_ == nullptr) {
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

    SensorReading sensor_readings_[kSensorCount] = {
        {25.3f, 52.0f}, {24.8f, 56.0f}, {26.1f, 47.0f}, {23.9f, 61.0f}};
    lv_obj_t* sensor_temp_labels_[kSensorCount] = {};
    lv_obj_t* sensor_humidity_labels_[kSensorCount] = {};
    lv_obj_t* sensor_page_ = nullptr;
    lv_timer_t* sensor_mock_timer_ = nullptr;
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
