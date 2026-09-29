#include "touch_spd2010.h"

#include <esp_io_expander.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <cstring>

namespace spd2010_touch {
namespace {

const char* TAG = "Touch_SPD2010";

constexpr uint8_t kAddr = 0x53;
constexpr int kInitTimeoutMs = 1000;  // vendor demo used 1 s
constexpr int kPollTimeoutMs = 100;   // keep the LVGL task responsive
constexpr uint32_t kMaxPoints = 5;

// The I2C device handle for kAddr is created once in Init() and then reused;
// the vendor driver opened a new device on every access instead.
i2c_master_bus_handle_t s_bus = nullptr;
esp_io_expander_handle_t s_expander = nullptr;
i2c_master_dev_handle_t s_dev = nullptr;

struct tp_report_t {
    uint8_t id;
    uint16_t x;
    uint16_t y;
    uint8_t weight;
};

struct SPD2010_Touch {
    tp_report_t rpt[10];
    uint8_t touch_num;
    uint8_t pack_code;
    uint8_t down;
    uint8_t up;
    uint8_t gesture;
    uint16_t down_x;
    uint16_t down_y;
    uint16_t up_x;
    uint16_t up_y;
};

struct tp_status_high_t {
    uint8_t none0;
    uint8_t none1;
    uint8_t none2;
    uint8_t cpu_run;
    uint8_t tint_low;
    uint8_t tic_in_cpu;
    uint8_t tic_in_bios;
    uint8_t tic_busy;
};

struct tp_status_low_t {
    uint8_t pt_exist;
    uint8_t gesture;
    uint8_t key;
    uint8_t aux;
    uint8_t keep;
    uint8_t raw_or_pt;
    uint8_t none6;
    uint8_t none7;
};

struct tp_status_t {
    tp_status_low_t status_low;
    tp_status_high_t status_high;
    uint16_t read_len;
};

struct tp_hdp_status_t {
    uint8_t status;
    uint16_t next_packet_len;
};

// ---------------------------------------------------------------- I2C access
// SPD2010 registers are addressed by a 16-bit big-endian value.
esp_err_t I2CRead(uint16_t reg, uint8_t* data, uint32_t len, int timeout_ms) {
    const uint8_t addr[2] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg)};
    return i2c_master_transmit_receive(s_dev, addr, sizeof(addr), data, len, timeout_ms);
}

esp_err_t I2CWrite(uint16_t reg, const uint8_t* data, uint32_t len) {
    uint8_t buf[16];
    if (len + sizeof(uint16_t) > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = static_cast<uint8_t>(reg >> 8);
    buf[1] = static_cast<uint8_t>(reg);
    memcpy(buf + 2, data, len);
    return i2c_master_transmit(s_dev, buf, len + sizeof(uint16_t), kInitTimeoutMs);
}

// ------------------------------------------------------------ vendor commands
esp_err_t WritePointModeCmd() {
    const uint8_t data[2] = {0x00, 0x00};
    esp_err_t err = I2CWrite(0x5000, data, sizeof(data));
    esp_rom_delay_us(200);
    return err;
}

esp_err_t WriteStartCmd() {
    const uint8_t data[2] = {0x00, 0x00};
    esp_err_t err = I2CWrite(0x4600, data, sizeof(data));
    esp_rom_delay_us(200);
    return err;
}

esp_err_t WriteCpuStartCmd() {
    const uint8_t data[2] = {0x01, 0x00};
    esp_err_t err = I2CWrite(0x0400, data, sizeof(data));
    esp_rom_delay_us(200);
    return err;
}

esp_err_t WriteClearIntCmd() {
    const uint8_t data[2] = {0x01, 0x00};
    esp_err_t err = I2CWrite(0x0200, data, sizeof(data));
    esp_rom_delay_us(200);
    return err;
}

esp_err_t ReadTpStatusLength(tp_status_t* status) {
    uint8_t data[4] = {0};
    esp_err_t err = I2CRead(0x2000, data, sizeof(data), kInitTimeoutMs);
    if (err != ESP_OK) {
        return err;
    }
    status->status_low.pt_exist = (data[0] & 0x01);
    status->status_low.gesture = (data[0] & 0x02);
    status->status_low.aux = (data[0] & 0x08);
    status->status_high.tic_busy = ((data[1] & 0x80) >> 7);
    status->status_high.tic_in_bios = ((data[1] & 0x40) >> 6);
    status->status_high.tic_in_cpu = ((data[1] & 0x20) >> 5);
    status->status_high.tint_low = ((data[1] & 0x10) >> 4);
    status->status_high.cpu_run = ((data[1] & 0x08) >> 3);
    status->read_len = (static_cast<uint16_t>(data[3]) << 8) | data[2];
    return ESP_OK;
}

esp_err_t ReadTpHdp(const tp_status_t* status, SPD2010_Touch* touch) {
    // 4 byte header + 10 fingers * 6 bytes.
    uint8_t data[4 + (10 * 6)];
    uint32_t len = status->read_len;
    if (len > sizeof(data)) {
        len = sizeof(data);
    }
    if (len < 4) {
        len = 4;
    }
    // Vendor uses reg = (0x00 << 8) | 0x03 == 0x0003 for the HDP data FIFO
    // (see read_tp_hdp in Touch_SPD2010.c), and I2CRead sends high byte first.
    esp_err_t err = I2CRead(0x0003, data, len, kInitTimeoutMs);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t check_id = data[4];
    if (check_id <= 0x0A && status->status_low.pt_exist) {
        touch->touch_num = (len - 4) / 6;
        if (touch->touch_num > 10) {
            touch->touch_num = 10;
        }
        touch->gesture = 0x00;
        for (uint8_t i = 0; i < touch->touch_num; i++) {
            const uint8_t offset = i * 6;
            touch->rpt[i].id = data[4 + offset];
            touch->rpt[i].x = ((data[7 + offset] & 0xF0) << 4) | data[5 + offset];
            touch->rpt[i].y = ((data[7 + offset] & 0x0F) << 8) | data[6 + offset];
            touch->rpt[i].weight = data[8 + offset];
        }
        // Slide gesture bookkeeping, kept from the vendor driver.
        if (touch->rpt[0].weight != 0 && touch->down != 1) {
            touch->down = 1;
            touch->up = 0;
            touch->down_x = touch->rpt[0].x;
            touch->down_y = touch->rpt[0].y;
        } else if (touch->rpt[0].weight == 0 && touch->down == 1) {
            touch->up = 1;
            touch->down = 0;
            touch->up_x = touch->rpt[0].x;
            touch->up_y = touch->rpt[0].y;
        }
    } else if (check_id == 0xF6 && status->status_low.gesture) {
        touch->touch_num = 0x00;
        touch->up = 0;
        touch->down = 0;
        touch->gesture = data[6] & 0x07;
    } else {
        touch->touch_num = 0x00;
        touch->gesture = 0x00;
    }
    return ESP_OK;
}

esp_err_t ReadHdpStatus(tp_hdp_status_t* hdp_status) {
    uint8_t data[8] = {0};
    esp_err_t err = I2CRead(0xFC02, data, sizeof(data), kInitTimeoutMs);
    if (err != ESP_OK) {
        return err;
    }
    hdp_status->status = data[5];
    hdp_status->next_packet_len = data[2] | (data[3] << 8);
    return ESP_OK;
}

esp_err_t ReadHdpRemainData(const tp_hdp_status_t* hdp_status) {
    uint8_t data[32] = {0};
    uint32_t len = hdp_status->next_packet_len;
    if (len > sizeof(data)) {
        len = sizeof(data);
    }
    return I2CRead(0x0003, data, len, kPollTimeoutMs);
}

esp_err_t ReadFwVersion() {
    uint8_t data[18] = {0};
    esp_err_t err = I2CRead(0x2600, data, sizeof(data), kInitTimeoutMs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "firmware version read failed: %s", esp_err_to_name(err));
        return err;
    }
    // Log the raw block: byte 10..17 hold the ASCII part number ("2010" / "SPD")
    // and bytes 4..5 the device version, so a live controller is obvious in the
    // boot log.
    ESP_LOGI(TAG,
             "fw: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X "
             "%02X %02X %02X",
             data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8],
             data[9], data[10], data[11], data[12], data[13], data[14], data[15], data[16],
             data[17]);
    return ESP_OK;
}

// --------------------------------------------------------------- data reading
esp_err_t ReadData(SPD2010_Touch* touch) {
    tp_status_t status = {};
    tp_hdp_status_t hdp_status = {};
    esp_err_t err = ReadTpStatusLength(&status);
    if (err != ESP_OK) {
        return err;
    }

    if (status.status_high.tic_in_bios) {
        WriteClearIntCmd();
        WriteCpuStartCmd();
    } else if (status.status_high.tic_in_cpu) {
        WritePointModeCmd();
        WriteStartCmd();
        WriteClearIntCmd();
    } else if (status.status_high.cpu_run && status.read_len == 0) {
        WriteClearIntCmd();
    } else if (status.status_low.pt_exist || status.status_low.gesture) {
        ReadTpHdp(&status, touch);
    hdp_done_check:
        ReadHdpStatus(&hdp_status);
        if (hdp_status.status == 0x82) {
            WriteClearIntCmd();
        } else if (hdp_status.status == 0x00) {
            ReadHdpRemainData(&hdp_status);
            goto hdp_done_check;
        }
    } else if (status.status_high.cpu_run && status.status_low.aux) {
        WriteClearIntCmd();
    }

    return ESP_OK;
}

// Returns the number of touch points found (0 when nothing is pressed).
uint8_t ReadPoints(uint16_t* x, uint16_t* y, uint8_t max_points) {
    SPD2010_Touch touch = {};
    ReadData(&touch);
    uint8_t count = (touch.touch_num > max_points) ? max_points : touch.touch_num;
    for (uint8_t i = 0; i < count; i++) {
        x[i] = touch.rpt[i].x;
        y[i] = touch.rpt[i].y;
    }
    return count;
}

// ----------------------------------------------------------- LVGL glue
uint16_t s_last_x = 0;
uint16_t s_last_y = 0;
bool s_pressed = false;

void ReadCb(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
    uint16_t x[kMaxPoints] = {0};
    uint16_t y[kMaxPoints] = {0};
    uint8_t count = ReadPoints(x, y, kMaxPoints);

    if (count > 0) {
        if (!s_pressed) {
            ESP_LOGI(TAG, "press  (%u, %u)", x[0], y[0]);
        }
        s_pressed = true;
        s_last_x = x[0];
        s_last_y = y[0];
    } else if (s_pressed) {
        // s_last_x/y are kept on release, so this is the last contact
        // position - exactly the swipe endpoint the gesture code needs.
        ESP_LOGI(TAG, "release (%u, %u)", s_last_x, s_last_y);
        s_pressed = false;
    }

    data->point.x = s_last_x;
    data->point.y = s_last_y;
    data->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

}  // namespace

bool Init(i2c_master_bus_handle_t i2c_bus, esp_io_expander_handle_t io_expander) {
    if (i2c_bus == nullptr || io_expander == nullptr) {
        ESP_LOGE(TAG, "missing I2C bus or IO expander handle");
        return false;
    }

    s_bus = i2c_bus;
    s_expander = io_expander;

    // Open the device once; every register access reuses this handle.
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = kAddr;
    dev_cfg.scl_speed_hz = 400 * 1000;
    esp_err_t err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot open I2C device 0x%02X: %s", kAddr, esp_err_to_name(err));
        return false;
    }

    // TP_RST is TCA9554 EXIO1 == bit 0. Pulse it exactly like the vendor
    // driver does: assert low, then release high.
    ESP_LOGI(TAG, "pulsing TP_RST (TCA9554 EXIO1 / PIN0)");
    esp_io_expander_set_level(s_expander, IO_EXPANDER_PIN_NUM_0, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_io_expander_set_level(s_expander, IO_EXPANDER_PIN_NUM_0, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    err = ReadFwVersion();
    if (err != ESP_OK) {
        return false;
    }
    return true;
}

lv_indev_t* Register() {
    lv_indev_t* indev = lv_indev_create();
    if (indev == nullptr) {
        ESP_LOGE(TAG, "cannot create LVGL input device");
        return nullptr;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, ReadCb);
    if (lv_display_get_default() != nullptr) {
        lv_indev_set_display(indev, lv_display_get_default());
    }
    ESP_LOGI(TAG, "LVGL pointer input device registered");
    return indev;
}

}  // namespace spd2010_touch
