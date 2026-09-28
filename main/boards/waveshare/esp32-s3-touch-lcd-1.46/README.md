新增 微雪 开发板: ESP32-S3-Touch-LCD-1.46、ESP32-S3-Touch-LCD-1.46B
产品链接：
https://www.waveshare.net/shop/ESP32-S3-Touch-LCD-1.46.htm
https://www.waveshare.net/shop/ESP32-S3-Touch-LCD-1.46B.htm

## 触摸（SPD2010）

触摸控制器是 **SPD2010**，集成在屏模组里（原理图上 `TP_*` 直连 LCD FPC `J5`，查不到型号）。
它使用 **16 位大端寄存器地址 + 私有 HDP 包协议**，因此 ESP-IDF 自带的
`esp_lcd_touch_*` 驱动（GT911 / CST816S / FT5x06 ...）读它只会拿到 `0xFF`，全部不可用。

驱动在 `touch_spd2010.h/.cc`，移植自厂商 demo
`demo/ESP-IDF/ESP32-S3-Touch-LCD-1.46-Test/main/Touch_Driver/`（本地参考文件，未纳入版本库）。
`main/CMakeLists.txt` 会 glob 板子目录下的 `*.cc/*.c`，新增源文件无需改构建脚本。

### 关键引脚与地址

| 信号 | 连接 | 备注 |
|---|---|---|
| `SPD2010_ADDR` | I2C `0x53` | |
| `TP_INT` | `IO4` | 即 `config.h` 的 `TP_PIN_NUM_INT` |
| `TP_RST` | TCA9554 **EXIO1（bit0）= `IO_EXPANDER_PIN_NUM_0`** | 低有效，`InitializeTouch()` 里打 50ms 脉冲 |
| `TP_SDA` / `TP_SCL` | `IO11` / `IO10` | 与 TCA9554、PCF85063 RTC(`0x51`)、QMI8658C IMU(`0x6B`) 共用总线 |
| `LCD_RST` | TCA9554 **EXIO2（bit1）= `IO_EXPANDER_PIN_NUM_1`** | **面板初始化后禁止拨动**，否则 SPD2010 丢初始化寄存器、屏幕变黑 |

`config.h` 里 `TP_PIN_NUM_RST = GPIO_NUM_NC` 是正确的 —— 复位不走 GPIO，走 TCA9554。

### 注意事项

- HDP 数据寄存器是 `0x0003`（厂商算式 `(b0<<8)|b1`，**高字节在前**）。写成 `0x0300` 会因字节序反了导致按屏无数据，且日志看起来一切正常。
- 启动时 `Touch_SPD2010: fw: ...` 打印固件版本；末尾出现 ASCII 片段即代表芯片应答。
- `press (x, y)` / `release` 日志为按屏边沿，手势开发期保留，之后可降为 `ESP_LOGD`。

### 手势与功能卡

固件本身没有任何滑动手势实现（全仓库 0 处 `LV_EVENT_GESTURE`），本板在
`CustomLcdDisplay` 里自建了一套：

- **卡片**：`kCardCount = 2`。card 0 = 原生 AI 主屏；card 1 = `BuildHelloPage()`
  创建的满屏黑底页面（最后创建，层级最高，不透明即可盖住主屏）。
  新增卡片时把 `kCardCount` 加一并扩展 `BuildHelloPage()`/`ShowCard()`。
- **手势**：`ShowCard/NextCard/PrevCard` 只切换卡片页的 `LV_OBJ_FLAG_HIDDEN`，
  不隐藏主屏子对象 —— 否则会破坏 `SetEmotion()`/`SetPreviewImage()` 对
  `emoji_box_` 的显隐状态管理。
- **方向**：横向左滑 → 下一张卡，右滑 → 返回；纵向暂不处理但会打日志。
  阈值 `kSwipeThresholdPx = 60`（约屏宽 15%）。

#### 为什么不用 LVGL 原生 `LV_EVENT_GESTURE`

`lv_indev.c` 的 `indev_gesture()` 在 `scroll_obj != NULL` 时**直接 return**
（滚动优先），其余情况只发给被按对象、需要 `LV_OBJ_FLAG_GESTURE_BUBBLE`
才冒泡。本板 UI 大量用 `lv_obj_create()`，**默认就是可滚动的**，原生手势
会被滚动吃掉。改用手动检测 `LV_EVENT_PRESSED` / `PRESSING` / `RELEASED`，
这三者总会冒泡到 screen、不受滚动状态影响。

