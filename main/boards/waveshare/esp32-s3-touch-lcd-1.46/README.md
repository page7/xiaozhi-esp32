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
- `press (x, y)` / `release (x, y)` 日志为按屏边沿（release 显示最后接触点，即手势终点），手势开发期保留，之后可降为 `ESP_LOGD`。

### 手势与功能卡

固件本身没有任何滑动手势实现（全仓库 0 处 `LV_EVENT_GESTURE`），本板在
`CustomLcdDisplay` 里自建了一套：

- **卡片**：`kCardCount = 2`。card 0 = 原生 AI 主屏；card 1 = `BuildSensorPage()`
  创建的满屏黑底页面（四圆温湿度仪表盘，最后创建，层级最高，不透明即可盖住主屏）。
  新增卡片时把 `kCardCount` 加一并扩展 `BuildSensorPage()`/`ShowCard()`。
- **手势**：`ShowCard/NextCard/PrevCard` 只切换卡片页的 `LV_OBJ_FLAG_HIDDEN`，
  不隐藏主屏子对象 —— 否则会破坏 `SetEmotion()`/`SetPreviewImage()` 对
  `emoji_box_` 的显隐状态管理。
- **方向**：横向左滑 → 下一张卡，右滑 → 返回；纵向暂不处理但会打日志。
  阈值 `kSwipeThresholdPx = 60`（约屏宽 15%）。
- **语音切屏（MCP）**：`CustomBoard::InitializeTools()` 注册
  `self.screen.show_sensor`（description 指明查看温湿度时调用）。云端 LLM 下发
  `tools/call` → `McpServer::DoToolCall` 经 `Application::Schedule` 在主任务执行
  → public `ShowSensorPage()`：`DisplayLockGuard` + 设置页开着先 `CloseSettings()`
  （`ShowCard` 对 `settings_open_` 是 no-op）+ `ShowCard(1)`。命中依赖后端从
  tools/list 拉到工具并决定调用；触发日志 `show sensor page requested (MCP)`。
- **布局（圆形屏）**：可见区是 412×412 的内切圆（R=206），2×2 网格的对角触点
  `√2·(d+gap)/2 + d/2` 必须小于 R，否则四角被圆形黑边裁掉——
  `kSensorGridMargin = 50` → `d = (412-100-12)/2 = 150`，触点 190px，内缩 16px。
- **padding 坑**：`sensor_page_` 必须 `lv_obj_set_style_pad_all(_, 0, 0)`——
  LVGL 默认主题给每个 `lv_obj_create` 套 `card` 样式（`PAD_DEF`=16px），
  而 `lv_obj_set_pos` 按父对象**内容区**定位（`lv_obj_move_to` 会加
  `space_left/top`），不清零会把四圆整体往右下推 16px（上间隙32、下间隙0，
  表现为"四圆偏下、贴底边"）。
- **读数渲染**：湿度大字 = `lv_font_montserrat_40`（`config.json` 的
  `sdkconfig_append` 开 `CONFIG_LV_FONT_MONTSERRAT_40/12=y`，build.py 每次全量重生成
  sdkconfig）。**伪加粗**：4 层同文本按 (0,0)/(1,0)/(0,1)/(1,1) 叠放——LVGL 描边
  `text_outline_stroke` 只在 `#if LV_USE_FREETYPE && LV_USE_VECTOR_GRAPHIC` 分支生效
  （位图字体无效，本工程也未开 FREETYPE），必须放进**固定尺寸** 130×48 容器：
  `LV_SIZE_CONTENT` 只量得到 align x/y 偏移为 0 的副本，会把 +1px 的三层裁掉。
  圆框按湿度分档：`>20%` 橙 `#FFBB00`、`>30%` 红 `#ff4000`（优先）、否则灰
  `#555555`，判档在 `RefreshSensorLabels()`；**无读数（未绑定/绑定未收到广播）恒灰**。
  每圆底部序号 1-4：`LV_OBJ_FLAG_FLOATING` 让 flex 跳过该子对象
  （`lv_obj_is_layout_positioned` 返回 false），`lv_obj_refr_pos()` 仍按
  `BOTTOM_MID` 摆位；页面底部中间设置图标 = `font_material_symbols_30_4` +
  `MATERIAL_SYMBOLS_SETTINGS`（与主屏 emoji 共用同一已链接字体），已加
  `LV_OBJ_FLAG_CLICKABLE` + `LV_EVENT_CLICKED` 打开设置页。
  **热区**：`pad_hor 16 + pad_bottom 12` 再 `BOTTOM_MID y=+4`——文字视觉位置
  不变（372..404），命中框扩到 372..416 × 175..237；首轮硬件测试里手指稳定
  落在 y=406-408（比原命中框低 2-4px）导致 30+ 次点击全部 miss（无 CLICKED 日志），
  扩大热区后点击正常。
- **状态栏（整条隐藏，顶部留白）**：主屏(card0) 原本有 `top_bar_`（网络/静音/
  电池图标）+ `status_bar_`（居中时钟、状态文字、通知），板级 `SetupUI()` 对二者
  `lv_obj_add_flag(..., HIDDEN)` 后**顶部完全无内容**。时钟/状态/通知的更新链路
  （`UpdateStatusBar()`/`SetStatus()`/`ShowNotification()`）照常运行，只是画进
  隐藏对象。`low_battery_popup_` 是 screen 的独立子对象（lcd_display.cc），不在
  `status_bar_` 内，低电量弹窗仍可弹出；Alert 仍通过 `SetEmotion`+`SetChatMessage`
  （表情+聊天气泡）可见。第二屏(card1) 的 `sensor_page_` 本就全屏不透明盖住二者。

#### 第二屏设置页与 XL0801 蓝牙绑定

- **状态显示**：模拟数据已移除。未绑定 → 温度位显示"未绑定"、湿度留空、圆环灰；
  绑定但未收到广播 → 温度位 `--`；收到广播 → `%.1f°C` / `%.0f%%`。
- **设置页**（齿轮 → `OpenSettings()`）：满屏黑底覆盖层（`settings_page_`，
  在 `sensor_page_` 之后创建所以层级在上），打开即自动扫描（标题/状态/
  居中设备列表/底部"返回"）。列表按**去重 MAC** 追加命中的 XL0801 行：
  **行首是实时广播读数 `28.5°C 36%`**（所有传感器都叫 XL0801，读数才是区分
  依据；`UpdateDeviceReading()` 逐包刷新，strcmp 只在文本变化时才
  `lv_label_set_text`，避免广播频率下的重复分配）+ montserrat_12 的 MAC +
  "绑定"按钮；空列表提示"未找到 XL0801 设备"（首个设备出现时删除）。
  行仅在设置页打开期间创建，但已存在行在关闭后仍随广播静默刷新。
- **返回**：按钮 `120x56`、`BOTTOM_MID y=+4`（命中框 360..416 × 146..266）——
  与齿轮同样的问题：圆屏边缘手指落点稳定在 y=406-408，原 96x40/-14 按钮
  （358..398）全部 miss；另外设置页打开时**横向右滑 ≥60px 也关闭**
  （`HandleSwipe` 里 `settings_open_` 分支），左滑/竖滑仍被吞。
- **绑定流程**："绑定" → 居中 `slot_panel_` 选 1-4 号位（按钮下"空/已绑"状态，
  `lv_font_montserrat_40` 数字）→ `ApplyBinding()` 写 NVS 并关闭设置页回第二屏。
  同一 MAC 只允许占一个圆（占新位时自动从其它位清除）。暂无解绑入口。
- **扫描生命周期**（`RefreshScanState()`）：设置页打开 **或** 任一位已绑定 →
  `ble_sensor::EnsureScanning()`；两者皆无 → `StopScanning()`（NimBLE host
  常驻不销毁）。绑定后开机会自动恢复扫描并直接出数。
- **广播解析**（`ble_sensor.cc`，尾部锚定，前缀字节忽略）：
  厂商数据 `0xFF` 尾部 = `[温度 u16 大端, 0.1°C][湿度 u8, %][MAC 6B]`，
  例如 `...01 20 45 ED 68 01 04 91 8C` → 28.8°C / 69% / `ED:68:01:04:91:8C`
  （用户给的 0110/3E 分别 = 27.2°C/62% 是同布局的另一包）。
  名称必须为 `XL0801`（AD type 0x09/0x08），厂商数据至少 9 字节。
- **线程模型**：扫描回调跑在 NimBLE host task → `CustomLcdDisplay::OnBleAdvertisement()`
  先取 `DisplayLockGuard` 再动控件；绑定缓存在 `bindings_[]`（NVS 只在
  SetupUI/写入时访问），回调路径不碰 NVS。
- **sdkconfig**：`config.json` 的 `sdkconfig_append` 增加
  `CONFIG_BT_ENABLED=y`、`CONFIG_BT_NIMBLE_ENABLED=y`（`BT_NIMBLE_ROLE_OBSERVER`
  默认即 y）；本仓库 `main` 组件本就 `PRIV_REQUIRES bt`，BluFi 未启用，无冲突。
  NimBLE host + controller 只在 `EnsureScanning()` 首次调用时才
  `nimble_port_init()`（即首次进设置页或已有绑定的开机）。
- **设置页打开期间**：左滑/竖滑被吞掉（防止误切卡片），**右滑 ≥60px 关闭设置页**，
  关闭主要仍靠底部"返回"按钮。

#### 为什么挂在 indev 事件上（而不是对象事件）

两套 LVGL 机制都被证伪过，都不可用：

- **原生 `LV_EVENT_GESTURE`**：`indev_gesture()` 在 `scroll_obj != NULL` 时
  **直接 return**（滚动优先），其余情况只发给被按对象、需要
  `LV_OBJ_FLAG_GESTURE_BUBBLE` 才冒泡。本板 UI 大量用 `lv_obj_create()`，
  **默认就是可滚动的**，原生手势会被滚动吃掉。
- **对象级 `PRESSED/PRESSING/RELEASED`**（第一版实现）：只发给 hit-test 命中的
  对象（如 `container_`），`event_send_core()` 向上冒泡要求链条上每个对象都带
  `LV_OBJ_FLAG_EVENT_BUBBLE`（`lv_obj_event.c`），而**全仓库没有任何对象设置过
  该 flag** → 注册在 screen 上的回调从未被触发（滑动完全无反应，2026-09-29
  硬件验证证实）。
- **现方案**：`lv_indev_add_event_cb()` 挂在触摸 indev 自身的事件列表上。
  `send_event()`（`lv_indev.c`）把 `PRESSED`/`RELEASED` **无条件、先于对象分发**
  转发到 indev 列表，与命中对象、滚动状态均无关。`PRESSING` 不在转发名单内，
  因此手势位移按**按下点 → 抬手点**的差值计算（驱动在 release 时保留
  `s_last_x/y` 为最后接触点）。


