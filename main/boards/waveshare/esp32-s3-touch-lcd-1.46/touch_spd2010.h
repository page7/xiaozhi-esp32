#pragma once

#include <driver/i2c_master.h>
#include <esp_err.h>
#include <esp_io_expander.h>

// Vendor SPD2010 touch driver for the ESP32-S3-Touch-LCD-1.46 panel, ported
// from demo/ESP-IDF/ESP32-S3-Touch-LCD-1.46-Test/main/Touch_Driver/.
//
// The SPD2010 is a HDP-packet controller addressed with a 16-bit big-endian
// register address, and it only speaks its own command set (point mode / start
// / CPU start / clear-int). That is why none of the bundled esp_lcd_touch_*
// drivers could talk to it: they all read 0xFF back from every register.
//
// Pin facts taken from the vendor header (Touch_SPD2010.h):
//   SPD2010_ADDR              0x53   matches the address seen in the I2C scan
//   EXAMPLE_PIN_NUM_TOUCH_INT  4      TP_INT, matches config.h
//   EXAMPLE_PIN_NUM_TOUCH_RST  -1     no dedicated RST: it is TCA9554 EXIO1
//
// EXIO1 is bit 0 (IO_EXPANDER_PIN_NUM_0 == TCA9554_EXIO1 == 0x01). LCD_RST is
// EXIO2 / IO_EXPANDER_PIN_NUM_1 and must never be driven here - re-asserting
// it after the panel is initialised leaves the screen dark.
namespace spd2010_touch {

// Pulse TP_RST, read the firmware version and open the I2C device. Returns
// false when the controller does not answer, which the caller reports.
bool Init(i2c_master_bus_handle_t i2c_bus, esp_io_expander_handle_t io_expander);

// Register the controller as an LVGL pointer input device. Requires Init() to
// have succeeded first.
bool Register();

}  // namespace spd2010_touch
