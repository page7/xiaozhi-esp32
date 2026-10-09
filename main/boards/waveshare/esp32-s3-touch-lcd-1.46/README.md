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

- **卡片**：`kCardCount = 3`。card 0 = 原生 AI 主屏；card 1 = `BuildSensorPage()`
  创建的满屏黑底页面（四圆温湿度仪表盘）；card 2 = `BuildPrinterPage()` 创建的
  拓竹打印机状态页。`ShowCard()` 按索引表显隐（先隐藏所有卡片页再显示目标页；
  card 0 = 全隐藏露出主屏）。新增卡片时把 `kCardCount` 加一、加一个
  `Build*Page()` 并扩展 `ShowCard()` 的显隐表。
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
  `sdkconfig_append` 开 `CONFIG_LV_FONT_MONTSERRAT_40/30/12=y`，build.py 每次全量重生成
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
- **返回**：按钮 `120x56`、`BOTTOM_MID y=-28`（命中框 328..384 × 146..266）——
  演变：最早 96x40/-14 贴边全 miss → 加大到 120x56/+4 贴屏幕最下沿 →
  2026-10-08 反馈**太靠下、圆弧区不好点**，整体上移 32px；y≤384 时整个
  按钮落在可见弦内（该处半弦 104）→ 同日再反馈"**只有按到文字才生效**"，
  加 `lv_obj_set_ext_click_area(16)`：命中框 = coords + ext（lv_obj_pos.c），
  纯命中扩张、视觉零变化（与齿轮当年 pad 扩热区同一思路，LVGL 自家
  arc/slider 同款手法）。另外设置页打开时**横向右滑 ≥60px 也关闭**
  （`HandleSwipe` 里 `settings_open_` 分支），左滑/竖滑仍被吞。
- **绑定流程**："绑定" → 居中 `slot_panel_` 选 1-4 号位（按钮下"空/已绑"状态，
  `lv_font_montserrat_40` 数字）→ `ApplyBinding()` 写 NVS 并关闭设置页回第二屏。
  同一 MAC 只允许占一个圆（占新位时自动从其它位清除）。暂无解绑入口。
- **扫描生命周期**（`RefreshScanState()`）：仅当 **card 1 可见** 且（设置页打开
  **或** 任一位已绑定）→ `ble_sensor::EnsureScanning()`；否则（含开机默认的
  card 0、AI 主屏、配网模式）→ `StopScanning()`（NimBLE host 常驻不销毁）。
  动机：BLE 与 SoftAP 共享 2.4G 射频，开机常驻扫描会让配网热点 10 秒左右就
  断连；进入第二屏自动恢复扫描出数，滑回主屏即停。
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
  `nimble_port_init()`（即首次进设置页或首次滑到 card 1 且有绑定）。
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

#### 第三屏：拓竹打印机状态页（本地 MQTT）

- **数据模块** `bambu_printer.h/.cc`（板级目录，glob 自动编译）：独立的第二个
  `esp_mqtt_client` 直连打印机自带 broker —— **TLS 8883 / MQTT 3.1.1 /
  username=`bblp` / password=LAN 访问码 / client_id=`xiaozhi-<随机>`**，
  订阅 `device/<SN>/report` 解析 `print.gcode_state / mc_percent /
  mc_remaining_time / nozzle_temper / bed_temper`（增量合并，缺字段保留旧值），
  订阅确认后发 `pushing/start` 并在 1s/60s 周期发 `pushall` 全量补拉。
  与小智云协议的 MQTT 客户端共存（`EspNetwork::CreateMqtt()` 每次新建实例）。
- **TLS 取舍**：打印机是**每台自签名证书**，静态 CA 无法跨型号覆盖 →
  `config.json` 开 `CONFIG_ESP_TLS_INSECURE=y` +
  `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y`（IDF 6.1 无 CA 时 esp-tls 默认
  **直接报错** `ESP_ERR_MBEDTLS_SSL_SETUP_FAILED`，必须开这个开关才是
  VERIFY_NONE）+ 跳过 CN 校验；会话由 LAN 访问码认证，与 HA 本地 Bambu
  `tls_insecure` 的常见做法一致。仅影响"没配 CA"的 esp-tls 连接（本板只有
  这一个），小智云端仍走 crt bundle 正常校验。
- **配置录入**（屏上，无网页门户）：card 2 右下齿轮 → `printer_settings_page_`
  覆盖层 = 3 行表单（IP 地址/序列号/访问码，行首标签 + 右侧值 label
  `LV_LABEL_LONG_DOT` 省略超长 IP，访问码行显示 `******`）+ 底部
  `保存/返回`（96×44，`BOTTOM_MID ±56 y=-28`，底边 384——与第二屏返回
  按钮同一轮"圆屏上移"调整；随后补 `lv_obj_set_ext_click_area(8)` 扩命中
  框——8 是不与邻居重叠的上限：save 94..206 vs back 206..318，back 为
  后建兄弟会赢任何重叠区）。点行进入编辑态：行列表隐藏，
  上方 textarea（320×48，访问码开 password mode）+ `lv_keyboard`（340×176，
  **y=134..310 必须在圆屏最宽弦带内**，340 宽在 y=310 半弦 178 刚好容纳）；
  IP/访问码用 `LV_KEYBOARD_MODE_NUMBER`（数字+点大键盘），SN 用
  `TEXT_LOWER`；**键盘 ✓ 键等效于"完成"**——LVGL 对 `LV_SYMBOL_OK` 只发
  `LV_EVENT_READY`（键盘图标/关闭键发 `LV_EVENT_CANCEL`），默认处理器不会关
  窗口，板级对 kb 注册 READY/CANCEL、对 ta 注册 READY（Enter 单行键只发给
  textarea）→ `StopPrinterEditing()` 回表单行列表（`printer_focus_ < 0`
  守卫防双发）；每击键 `VALUE_CHANGED` 同步到 pending 字符串；
  `完成` 回到行列表（换字段 = 完成→点另一行），`保存` 校验
  （IP=数字字母点横线≤64、SN=4-24 位字母数字、访问码=4-8 位数字，错误红字显示在
  表单提示行并自动退出编辑态）→ `bambu_printer::SetConfig()` 写 NVS
  ns=`bambu_printer`（key `host/serial/access_code`）→ 立即重连 + 关闭覆盖层。
  编辑态（`printer_focus_ >= 0`）下所有滑动手势被吞，防止键盘上的右滑误关页面。
- **生命周期**：`CustomBoard::SetNetworkEventCallback()` **包装**（不是覆盖）
  Application 的回调 → `Connected` → `OnNetworkUp()`（建 3 个 esp_timer：
  2s 后 `ConnectNow()`，错开小智协议首次 TLS 握手的堆峰值）；`Disconnected` /
  `WifiConfigModeEnter` → `OnNetworkDown()`（销毁客户端，配网 SoftAP 期间保持
  射频安静，与 BLE 扫描同样的理由）。esp-mqtt 自动重连 15s，失败 → `kOffline`。
- **线程模型（关键）**：MQTT 事件跑在 esp-mqtt task，**绝不取
  `s_life_mutex`**（`esp_mqtt_client_stop()` 会 join 该任务，若 handler 需要
  同一把锁即死锁）；状态变更经 10ms 一次性 esp_timer **延迟到 esp_timer task**
  才回调 UI（`OnPrinterStatus` 取 `DisplayLockGuard`）—— 这样 UI 任务在
  `SetConfig` 里 stop/destroy 客户端时，没有任何在途 handler 会等 LVGL 锁。
  双锁顺序恒为 `life → status`。
- **第三屏 UI**（黑底页，样板同 card 1）：**进度环贴表盘最外圈**——arc 404px
  （中心半径 195、外缘 202，距 R=206 黑边 4px），轨道深灰 `#333333`、进度
  `#21A452`，主题画在指示末端的**蓝色圆点（KNOB part）全部样式置透明**
  （bg_opa/border/outline/shadow）；环的**底部缺口环绕设置齿轮**：
  `lv_arc_set_bg_angles(108°, 72°)`（<start 的 wrap 与默认 135/45 同机制），
  两端落在 (146,391)/(266,391)，即齿轮命中框两侧各 ~29px——齿轮位置与
  热区参数不变、最后创建（绘制/命中都在环之上）。环内信息自上而下：
  **进度 %**（montserrat_40，恒绿 `#21A452`，y=96）→ **打印状态**
  （`lv_font_montserrat_30`，30px——应"字体增大"由 20px 提上来，y=166；在线一律绿、
  仅 FAILED 红 `#FF4000`，setup/connecting/offline 灰）→ **打印机 IP**（灰
  `#888`，y=206；未配置时此行显示引导文字）→ **剩余时间行**（灰 y=234，
  见下）→ **喷嘴/热床双温**（y=296，行中心 x=146/266）。
  - **状态行 = 英文小写，词表同 BambuSphere**（`lifecycle_label`/ui_status）：
    `setup`(未配置) / `connecting` / `offline` / `connected`(刚连上) /
    `printing` / `preparing` / `paused` / `done` / `failed` / `idle`，
    未知 gcode_state 原样显示。**该行只能放英文**：现用
    `lv_font_montserrat_30` 是纯 ASCII 字体，中文会整字不画；改英文前用的
    静态 `font_noto_sans_basic_20_4` 则是 ASCII 完整、**CJK 仅 531 汉字
    残缺子集**，LVGL 对缺字形静默跳过——"打印机离线"只剩"机"（2026-10-08
    实机复现的根因）；代码处有防回归注释。16px 标签不受影响：它们继承
    主题字体，assets 加载时被 `LvglStrategy` 换成全字库 cbin。
  - **剩余时间 = 时钟图标 + 数值**（同 BambuSphere 的 remaining 行结构：
    居中 flex 行 = MDI `clock-time-four-outline`(U+F144E，即它的
    `kMdiClock`) + 数值），格式同它的 `remaining_text()`：
    `Done`（FINISH）/ `--m`（无数据/未连接）/ `1h 35m` / `45m`。
  - **温度行汉字换图标**：MDI `printer-3d-nozzle`(U+F0E5B) +
    `waves-arrow-up`(U+F185B)——BambuSphere 同款。未抄其字体文件（FNCL），
    而是用 `lv_font_conv` 从 Apache-2.0 的 MaterialDesign TTF 重新生成
    **三字形** `font_bambu_icons_20.c`（板级目录，glob 编译，文件头带
    出处与许可）。
  - 状态字体演进：16 继承 → 20px 静态 `font_noto_sans_basic_20_4`（+124KB，
    CJK 残缺坑见上）→ **30px `lv_font_montserrat_30`**（应"字体增大"，
    `config.json` 新增 `CONFIG_LV_FONT_MONTSERRAT_30=y`，纯 ASCII 约 +15KB；
    noto20 不再被引用，链接器整体回收，bin 净减 ~80KB：0x2fd880→0x2ea150，
    分区余 24%→26%）。静态 flash 字体，与主题换字体无耦合。中文（引导语
    等）一律继承 screen 字体，仅百分比/图标/状态用静态字体。
- **语音切屏**：MCP 工具 `self.screen.show_printer`
  （`ShowPrinterPage()` → `CloseAnySettings()` + `ShowCard(2)`）。
- **sdkconfig**：`sdkconfig_append` 新增 `CONFIG_LV_USE_KEYBOARD=y`（LVGL
  keyboard 组件默认未编译）、`CONFIG_LV_FONT_MONTSERRAT_30=y`（状态行 30px）、
  上述两个 esp-tls 开关、
  `CONFIG_MQTT_BUFFERS_ON_EXTERNAL_MEMORY=y` +
  `CONFIG_MQTT_TASK_STACK_ON_EXTERNAL_MEMORY=y`（16KB 收包缓冲 + 8KB 任务栈
  放 PSRAM，内部 SRAM 只剩 ~48KB，放内部撑不住）。


