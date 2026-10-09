# AGENTS.md

## Project

XiaoZhi is an ESP-IDF C/C++ voice-assistant firmware supporting many chips, boards, displays, audio devices, and network transports. A build selects exactly one board implementation.

Use ESP-IDF v6.1 when possible. The minimum supported SDK is ESP-IDF v6.0.1. IDF 5.x is not supported.

## Architecture

### Module Map

- `main/main.cc`, `main/application.*`: singleton `Application`, main event loop (`Run()` never returns), protocol lifecycle, and high-level behavior.
- `main/device_state.*`, `main/device_state_machine.*`: legal runtime state transitions.
- `main/boards/common/`: board interfaces (`board.h`, `DECLARE_BOARD`) and reusable hardware/network helpers (wifi/ml307/nt26/rndis/ethernet/dual-network boards, backlight, button, knob, camera, battery, power-save timers, BluFi).
- `main/boards/**/`: board-specific pins, `config.h`, initialization, and build variants. Dozens of vendors (waveshare, espressif, m5stack, xmini, ...).
- `main/audio/`: `audio_service.*` (task/queue orchestration), `audio_codec.*`, `audio/codecs/` (es8311/es8374/es8388/es8389/box/dummy/no-audio), `audio/engines/` (afe, lite), `audio/wake_words/` (esp, custom), `audio/demuxer/` (ogg), `fixed_queue.h`, `audio_debugger.*`.
- `main/protocols/`: transport-neutral `Protocol` plus `websocket_protocol.*` and `mqtt_protocol.*`, `text_glyph_payload.*`.
- `main/display/`: `display.*` base, `lcd_display`, `oled_display`, `emote_display`, `text_glyph`, `epd_display/`, `lvgl_display/` (themes, fonts, gif/jpg, glyph cache, PSRAM pools).
- `main/led/`: `led.h`, `single_led`, `gpio_led`, `circular_strip`.
- `main/notify/notify_player.*`: local notification audio playback.
- `main/mcp_server.*`: common device-side MCP tools and dispatch.
- `main/ota.*`, `main/settings.*` (NVS), `main/system_info.*`, `main/assets.*`, `main/cjson_utils.h`.
- `main/Kconfig.projbuild`: board and feature configuration.
- `main/CMakeLists.txt`: source, board, locale, font, and asset selection.
- `scripts/build.py`: canonical board/variant build entry point (reads `config.json`).
- `scripts/tests/`: host-side unit tests (`test_build.py`, `test_build_default_assets.py`, `test_ogg_demuxer.py`).
- `scripts/` helpers: `gen_lang.py`, `versions.py`, `build_default_assets.py`, `spiffs_assets/`, `Image_Converter/`, `ogg_converter/`, `p3_tools/`, `ci/`.
- `partitions/`, `sdkconfig.defaults*`: flash/partition layout per chip.
- `docs/`: protocols (websocket, mqtt-udp, mcp), board guide, code style, BluFi, glyph-push, notify, build/flash guide.

### Runtime Data Flow

- Capture: mic -> `AudioCodec` -> `AudioService` (engine/wake-word/VAD) -> `Application` event bits (`MAIN_EVENT_*`) -> `Protocol::SendAudio`.
- Playback: `Protocol` audio packets -> `AudioService` playback task -> `AudioCodec` speaker; playback drain posts `MAIN_EVENT_PLAYBACK_DRAINED`.
- Control: WebSocket/MQTT/UDP messages -> `Protocol` callbacks -> `Application::Schedule()` in main task (never mutate state from transport callbacks directly).
- State/UI: `Application::SetDeviceState()` -> `DeviceStateMachine` -> `MAIN_EVENT_STATE_CHANGED` -> display/LED/network/power-save updates.

### Device States

`kDeviceStateUnknown / Starting / WifiConfiguring / Idle / Connecting / Listening / Speaking / Notifying / Upgrading / Activating / AudioTesting / FatalError` — transitions are validated by `DeviceStateMachine`; always route through `Application::SetDeviceState()`.

### Key Abstractions

- `Board` (abstract, one per build via `DECLARE_BOARD` factory): audio codec, display, LED, camera, network, battery, power-save.
- `AudioCodec`, `AudioEngine` (afe/lite), `WakeWord` (esp/custom).
- `Display` (lcd/oled/lvgl/epd/emote), `Backlight`, `Led`.
- `Protocol` (websocket/mqtt): shared message semantics; both transports implement the same contract.

Read the closest existing implementation before adding a new one. Prefer the narrowest owning layer; do not put board-specific behavior into core modules.

## Required Rules

- Preserve unrelated worktree changes and keep patches focused.
- A build must export exactly one board factory through `DECLARE_BOARD(...)`.
- Never alter an existing board's pins to support different hardware. Add a uniquely named board or release variant; board identity affects OTA compatibility.
- Core code depends on `Board` interfaces, never a concrete board class or board `config.h`.
- Treat camera, backlight, display, LED, battery, and similar capabilities as optional.
- Change runtime state through `Application::SetDeviceState()` and the state machine.
- Callbacks may run outside the main task. Schedule application mutations with `Application::Schedule()` or event bits.
- Do not block the main event loop or audio tasks. Avoid unbounded queues and repeated large allocations in audio paths.
- Keep shared message semantics in `Protocol`; verify both transports when changing its contract.
- Validate network input and preserve `cJSON` ownership. NVS keys are persistent API and require migration when changed.
- Guard target-specific features with Kconfig/component rules. Do not assume every target has PSRAM or S3/P4 resources.
- Do not manually edit generated/vendor output: `build/`, `releases/`, `managed_components/`, `components/`, `sdkconfig*`, `main/assets/lang_config.h`, or generated mmap headers.
- Format only touched C/C++ files with the repository `.clang-format`; avoid unrelated mass formatting.

## Boards and Configuration

Board selection is a coupled chain:

`config.json` -> `scripts/build.py` -> `main/Kconfig.projbuild` -> `main/CMakeLists.txt` -> board source and `config.h`.

When adding a board or variant, update every relevant link in that chain. Include a unique board identity, correct chip target, flash/partition settings, exactly one `DECLARE_BOARD`, and board documentation. Follow `docs/custom-board.md`.

## Commands

Source the intended ESP-IDF environment first:

```sh
source /path/to/esp-idf/export.sh   # Windows PowerShell: . $env:IDF_PATH\export.ps1
idf.py --version
```

Quick build + flash (this machine; default serial port is `COM7`):

```powershell
# Windows PowerShell, from repo root — load env once per terminal
. .\.vscode\idf-env.ps1

# Build (board + variant must both be given)
python scripts/build.py <board-directory> --name <variant-name>
# Example:
python scripts/build.py waveshare/esp32-s3-touch-lcd-1.46 --name esp32-s3-touch-lcd-1.46

# Flash + monitor (use COM7 unless told otherwise)
idf.py -p COM7 flash monitor
```

Equivalent VS Code tasks: `Ctrl+Shift+P` → `Tasks: Run Task`
(`IDF: Build (build.py)`, `IDF: Flash Monitor`; `Ctrl+Shift+B` = build).
Details: `docs/build-flash-guide.zh-CN.md`.

```sh
# Discover exact board and variant names
python3 scripts/build.py --list-boards

# Canonical variant build
python3 scripts/build.py <board-directory> --name <variant-name>

# Host-side build tests
python3 -m unittest discover -s scripts/tests -v

# Format/check touched files
clang-format -i <files>
clang-format --dry-run -Werror <files>
```

The build script changes local `sdkconfig` and build state. Do not assume the build directory still represents a previous target.

## Validation

- Board-only change: build affected variants and smoke-test changed hardware.
- Core, common-board, audio, protocol, display, dependency, Kconfig, or CMake change: run host tests and build representative affected chip/network paths.
- Protocol changes: verify WebSocket and MQTT/UDP when shared behavior changes.
- Audio changes: verify capture, playback, wake/VAD, interruption, reconnect, and applicable AEC modes.
- UI/assets changes: verify applicable no-display/OLED/LVGL paths and partition size.
- Always report what was tested and what still needs physical hardware. A successful build is not hardware validation.

## Authoritative Documentation

- Overview and SDK policy: `README.md`
- Board guide: `docs/custom-board.md`
- Audio design: `main/audio/README.md`
- Code style: `docs/code_style.md`
- Protocols: `docs/websocket.md`, `docs/mqtt-udp.md`, `docs/mcp-protocol.md`
- CI matrix: `.github/workflows/build.yml`

Keep detailed or fast-changing information in those files, not here. Add a nested `AGENTS.md` only when a subsystem needs specialized instructions. The change-request log below is the one fast-changing exception kept here on purpose.

## 修改需求记录 (Change Request Log)

Purpose: persistent log of user change requests across sessions. When the user states a new modification requirement, append an entry here (newest at the bottom). When work finishes or status changes, update that entry in place. Keep entries concise and link to touched files.

Entry template:

```markdown
### [YYYY-MM-DD] <short title>
- 状态: 待处理 | 进行中 | 已完成 | 已取消
- 范围: <modules / files affected>
- 需求: <what the user asked for>
- 方案: <implementation notes, decisions>
- 验证: <what was tested; what still needs hardware>
```

Log:

### [2026-09-28] 初始化 AGENTS.md 需求记录区
- 状态: 已完成
- 范围: AGENTS.md
- 需求: 分析项目架构并生成 AGENTS.md，作为快速理解入口和后续修改需求的存档
- 方案: 补充模块地图、运行时数据流、设备状态、关键抽象；新增本需求记录区
- 验证: 仅文档变更，无需构建

### [2026-09-28] AGENTS.md 增加最简编译/烧录命令
- 状态: 已完成
- 范围: AGENTS.md、.vscode/tasks.json、docs/build-flash-guide.zh-CN.md
- 需求: 在 AGENTS.md 记录最简单的打包烧录命令，默认端口 COM7
- 方案: Commands 段新增 PowerShell 快速流程（`. .\.vscode\idf-env.ps1` → `build.py` → `idf.py -p COM7 flash monitor`）；VS Code 任务与文档示例默认端口统一改为 COM7
- 验证: tasks.json 通过 JSON 解析；仅文档/任务配置，无需构建

### [2026-09-28] 取消表情满屏放大，改黑底白字
- 状态: 已完成
- 范围: main/boards/waveshare/esp32-s3-touch-lcd-1.46/esp32-s3-touch-lcd-1.46.cc
- 需求: 表情放大后动画卡顿，恢复图片原尺寸；主屏幕背景改黑色、文字改白色
- 方案: 回滚 SetupUI 中的 LV_IMAGE_ALIGN_COVER 满屏缩放（卡顿因 LVGL 对每帧 GIF 做软件重采样）；改在板子构造函数里把 light/dark 主题的 background/text 置为 #000/#FFF，主题创建早于 SetupUI，故能生效且不改动共享的 lcd_display.cc
- 验证: 目标板编译通过；实际黑底白字与动画流畅度需烧录到硬件确认

### [2026-09-28] 启用 esp32-s3-touch-lcd-1.46 触摸
- 状态: 已完成（硬件验证通过；滑动切功能卡/hello world 页留待下一轮）
- 范围: main/boards/waveshare/esp32-s3-touch-lcd-1.46/{esp32-s3-touch-lcd-1.46.cc,touch_spd2010.h,touch_spd2010.cc}
- 需求: AI 主屏上下左右滑动无反应，先启用触摸
- 根因: (1) config.h 定义了 TP_* 引脚但板子从未调用 InitializeTouch，LVGL 没有 pointer 输入设备；(2) 全仓库 0 处 `LV_EVENT_GESTURE`，本来就没有滑动手势功能；(3) 触摸 IC 是 **SPD2010**，用 16 位大端寄存器 + 私有 HDP 包协议，任何 GT911/CST/FT 标准驱动读它必然全 FF
- 关键引脚/地址（难查，务必保留）: SPD2010 @ **0x53**、TP_INT=**IO4**、TP_RST=**TCA9554 EXIO1(bit0)=`IO_EXPANDER_PIN_NUM_0`**（低有效）、**LCD_RST=`PIN_NUM_1` 禁止在面板 init 后拨动否则黑屏**；同总线 IO10/IO11 上还有 TCA9554(0x20)/PCF85063(0x51)/QMI8658C(0x6B)
- 方案: 新增 `touch_spd2010.h/.cc`（板子目录 glob 自动纳入）移植厂商协议——复位脉冲 + 固件版本读取 + HDP 解析 + LVGL pointer indev 注册；I2C 换新版 `i2c_master` API 并复用单个设备句柄。中途的猜测式探测（ID 寄存器读取、全总线扫描、6 驱动兜底、TP_INT 心跳探针）已全部删除，板子文件 746→285 行
- 坑: HDP 数据寄存器是 **0x0003**（厂商 `reg = (b0<<8)|b1`），写成 `0x0300` 会因字节序反了导致按屏无数据；`0x2600` 读固件正常可反证字节序约定
- 验证: 硬件确认 `press (x, y)` / `release` 坐标随手指变化、`flags=0x09`；临时状态机日志已删，坐标日志保留供手势开发用，确认手势后再降为 ESP_LOGD

### [2026-09-28] esp32-s3-touch-lcd-1.46 滑动切换功能卡（含 hello world 页）
- 状态: 已完成（待硬件验证）
- 范围: main/boards/waveshare/esp32-s3-touch-lcd-1.46/esp32-s3-touch-lcd-1.46.cc
- 需求: 上一需求的后续——上一轮触摸成功后，滑动切换功能卡，先做一个 hello world 页面
- 方案: 全部放在板子层 `CustomLcdDisplay`，不动核心 `lcd_display.cc`（本板是唯一有触摸的，改核心会影响 171 个 variant）。卡片模型 `kCardCount=2`：card 0 = 原生 AI 主屏，card 1 = `BuildHelloPage()` 建的满屏黑底页面（最后创建所以层级在最上，只需不透明即可盖住主屏）；`ShowCard/NextCard/PrevCard` 只切换该页的 HIDDEN 标志，**不隐藏主屏子对象**，避免破坏 SetEmotion/SetPreviewImage 对 emoji_box_ 的显隐状态
- 关键决策: ~~不使用 LVGL 原生 `LV_EVENT_GESTURE`~~ 后半句 **已证伪**：原生 gesture 确不可用（`indev_gesture()` 在 `scroll_obj != NULL` 时直接 return），但"PRESSED/PRESSING/RELEASED 这三个冒泡到 screen"是错的——`event_send_core()` 冒泡要求链条上每个对象带 `LV_OBJ_FLAG_EVENT_BUBBLE`，全仓库 0 处设置，对象级回调从未触发。正确方案见 2026-09-29 条（indev 级 `lv_indev_add_event_cb`）
- 手势映射: 横向左滑→下一张卡、右滑→返回；纵向暂不处理但会打日志，便于验证触摸在四个方向都工作
- 验证: 编译通过；2026-09-29 首次烧录硬件验证**证伪手势实现**（press/release 正常但页面不切换），已修复，见下条

### [2026-09-28] 左滑第二页改为四圆温湿度仪表盘（模拟数据）
- 状态: 已完成（已烧录，启动稳定无崩溃；页面布局/滑动手势待肉眼确认）
- 范围: main/boards/waveshare/esp32-s3-touch-lcd-1.46/esp32-s3-touch-lcd-1.46.cc、AGENTS.md
- 需求: 左滑出现第二页，页面黑底，显示四个等大圆圈；每圆圈内大字显示湿度、上方小字显示温度；先用模拟数据，后面要对接蓝牙设备读数
- 方案: 复用上一轮的滑动卡片框架，card 1 的 `BuildHelloPage()` 换成 `BuildSensorPage()`（hello world 页退役）。2×2 等大圆圈（直径由 width_ 推算，margin 16/gap 12，184px），圆 #161616 填充 + 2px #555 描边；圆内 flex 纵向居中：上=温度小字（主题 16px 字体，`%.1f°C`），下=湿度大字（新链入 `font_noto_sans_basic_30_4`，`%.0f%%`，30px 为组件内最大 noto sans basic）。数据层 `SetSensorReading(index, temp, humidity)` 内部取 `DisplayLockGuard`（esp_lvgl_port 的 lvgl_mux 是递归锁，lv_timer 内调用不死锁；后续 BLE 回调可从任意任务调用），另有 3s `lv_timer` 随机游动生成模拟值（温 20~30℃、湿 40~70%）走同一更新路径。°(U+00B0) 已确认在字体 cmap 范围 161~255 内
- 首次烧录崩溃（boot loop，已修复）: `Guru Meditation InstrFetchProhibited PC=0`，栈在 `lv_font_get_glyph_width`。根因：温度 label 用 `lv_obj_set_style_text_font(label, lv_obj_get_style_text_font(screen,...))` **显式捕获 theme 字体裸指针**，而 `LvglBuiltInFont::font_` 是堆上 shared_ptr 拷贝；WiFi 连上后 `Assets::LvglStrategy::Apply → SetTextFont()` 把主题字体换成 assets cbin 字体并 `previous_font.reset()` 释放旧对象（`LcdDisplay::SetTheme` 只回绑核心 widget，不认板级新建 label），下一次 `lv_display_refr_timer` 布局测量温度 label 即跳进已释放内存（0x3fceb66c，DRAM，A4=0x32='2' 正是 "25.3°C" 的首字符）。**修复：删除该显式字体设置**——`LV_STYLE_TEXT_FONT` 是可继承样式（`lv_style.c:122`），label 自动继承 screen 字体，SetTheme 换字体后所有继承者拿到新指针，永不悬空。湿度大字用静态 `&font_noto_sans_basic_30_4`（flash）不受影响。坑：板级 label 一律不要 `lv_obj_get_style_text_font()` 存裸指针，要么继承、要么静态字体
- 开销: 引入 30px 字体使 xiaozhi.bin 2.70→2.87MB（+约 197KB），app 分区仍余 29%
- 验证: 编译+clang-format 通过；烧录后 60s 监视：启动无崩溃、`Refreshing display theme` 原崩溃点通过、MQTT 连上、进入 idle、触摸 press/release 日志正常；待肉眼确认四圆布局、°C/% 字形、模拟值每 3s 变化、左滑/右滑切换

### [2026-09-29] 滑动手势修复：对象级事件改挂 indev 事件列表
- 状态: 已完成（硬件验证通过：左/右滑切换第二屏正常）
- 范围: main/boards/waveshare/esp32-s3-touch-lcd-1.46/{esp32-s3-touch-lcd-1.46.cc,touch_spd2010.h,touch_spd2010.cc,README.md}、AGENTS.md
- 需求: 硬件验证发现 press/release 日志正常但左滑不切换页面、温湿度计页不出现
- 根因: 对象级 `LV_EVENT_PRESSED/PRESSING/RELEASED` 只发给 hit-test 命中的对象（`container_` 等），`event_send_core()`（lv_obj_event.c:434）向上冒泡要求链条上每个对象带 `LV_OBJ_FLAG_EVENT_BUBBLE`——全仓库 0 处设置 → 注册在 screen 上的 `SwipeEventCb` **从未被调用**（上一轮"这三个冒泡到 screen"的假设错误，当时未硬件验证）
- 方案: 改用 **indev 级** `lv_indev_add_event_cb(indev, cb, filter, this)`：`send_event()`（lv_indev.c:1892/1900）把 `PRESSED`/`RELEASED` 无条件、先于对象分发转发到 indev 自身事件列表，不依赖命中对象与滚动状态。`PRESSING` 不在转发名单 → 手势按**按下点→抬手点差值**计算。配套：`touch_spd2010::Register()` 改返回 `lv_indev_t*`、release 日志带最后接触坐标（`s_last_x/y` 释放时保留）、`InitializeTouch()` 拿到 indev 后调 `display_->RegisterSwipeDetection(indev)`（`display_` 成员类型改为 `CustomLcdDisplay*`）、`HandleSwipe` 先无条件打印 `touch release dx= dy=` 便于调阈值
- 排查确认: `wait_until_release` 仅对象删除时置位（lv_obj_tree.c），正常滑动不影响 release 送达；`LV_USE_GESTURE_RECOGNITION` 依赖 `LV_USE_FLOAT`（未启用）无干扰
- 验证: 目标板编译+clang-format 通过、烧录成功；2026-09-29 用户确认左滑/右滑切换正常（indev 级 `PRESSED/RELEASED` + 起终点差值方案有效）

### [2026-09-29] 第二屏布局微调：圆形屏适配 + 修 padding 导致的偏下
- 状态: 已完成（已烧录；待硬件目视确认四圆对称/不被裁切/第二屏无状态栏）
- 范围: main/boards/waveshare/esp32-s3-touch-lcd-1.46/{esp32-s3-touch-lcd-1.46.cc,README.md}、AGENTS.md
- 需求: 四个圆太大（1.46 是圆形屏，需按可见圆布局）；四圆整体偏下；问屏幕顶部是否有状态栏
- 根因（偏下）: LVGL 默认主题随 display **自动**初始化（`lv_display.c:132`，`CONFIG_LV_USE_THEME_DEFAULT=y`）→ 每个 `lv_obj_create()` 经 `theme_apply` 套上 `card` 样式 = `pad_all(PAD_DEF)`（DPI130/DISP_MEDIUM → `(130*20+80)/160` = **16px**）；`lv_obj_set_pos` 按父对象**内容区**定位（`lv_obj_move_to` 加 `space_left/top`），`sensor_page_` 漏了清零 → 四圆整体右下移 16px：上间隙32、**下间隙0（贴屏幕底边）**。修复：`lv_obj_set_style_pad_all(sensor_page_, 0, 0)`（圆 `circle` 本来就有）
- 方案（圆屏）: 可见区 = 412 内切圆 R=206；2×2 网格对角触点 `√2·(d+gap)/2+d/2`。旧 margin16/d184 → 触点 **231 > 206，四角被圆形黑边裁掉**（"太大"的观感来源）。改 `kSensorGridMargin=50` → `d=(412-100-12)/2=150` → 触点 190（内缩16px）；加 `sensor grid: diameter/margin/gap` 日志便于硬件核对
- 状态栏: 主屏(card0) 有 `top_bar_`（左网络+右静音/电池图标，半透明）+ `status_bar_`（居中状态文字），均为 screen 直接子对象；第二屏(card1) `sensor_page_` 是 screen **最后创建**的 412×412 不透明全屏子对象（LVGL 按创建顺序绘制，后建在上；全仓库无 `move_to_index` 改序）→ 完全盖住状态栏，第二屏顶部不应见状态栏；"偏下"与状态栏无关（是 padding bug）
- 验证: 编译+clang-format+烧录+启动正常；待目视确认：四圆四边间距对称（50/50/50/50）、四角完整不被圆形黑边裁切、第二屏顶部纯黑无状态栏

### [2026-09-29] 主屏去掉时钟显示
- 状态: 已被下一条取代（板级 `SetStatus` 过滤实现已随本轮改为整条隐藏而删除；时钟仍被去掉）
- 范围: main/boards/waveshare/esp32-s3-touch-lcd-1.46/esp32-s3-touch-lcd-1.46.cc、README.md、AGENTS.md
- 需求: 主屏顶部的时间可以去掉么 → 可以，已去掉
- 链路: 时钟 = `Application` 1s `CLOCK_TICK` → `LvglDisplay::UpdateStatusBar()`（lvgl_display.cc:218，仅 idle 且时间已同步）→ `strftime("%H:%M")` → `SetStatus(time_str)`（**全仓库唯一 HH:MM 产出点**）→ `status_label_`。`SetStatus` 是 virtual（display.h:43）且 `UpdateStatusBar` 在对象内部虚调用它 → 板级 override 自动生效
- 方案: 板级 `CustomLcdDisplay::SetStatus` override，只拦**裸 HH:MM 形态**（5 字符、`d d : d d`）→ return 丢弃时钟；其余（`STANDBY`/`LISTENING`/Alert 状态等）透传 `LvglDisplay::SetStatus`。零核心改动；`UpdateStatusBar` 内 mute/电池/网络图标逻辑不受影响。去掉时钟后顶部居中保留最近一条状态文字（zh-CN 下为"待机"）
- 验证: 编译+clang-format+烧录+启动正常；待目视确认主屏顶部不再出现 HH:MM、状态文字正常

### [2026-09-29] 主屏顶部整条状态区隐藏（"都去掉留空白"）
- 状态: 已完成（已烧录；待目视确认顶部无任何内容）
- 范围: main/boards/waveshare/esp32-s3-touch-lcd-1.46/{esp32-s3-touch-lcd-1.46.cc,README.md}、AGENTS.md
- 需求: 上一条只去时钟后顶部还剩"待机"等状态文字 → "都去掉留空白"（状态文字+左右图标全部不要）
- 方案: 板级 `SetupUI()` 在父类建完 UI 后直接 `lv_obj_add_flag(top_bar_/status_bar_, HIDDEN)`——**结构性隐藏整条状态区**，时钟/状态文字/通知/图标全部画进隐藏对象，核心 `UpdateStatusBar()/SetStatus()/ShowNotification()` 更新链路零改动（无副作用）。上一条的 `SetStatus` HH:MM 过滤 override **随之删除**（父对象隐藏后冗余，且"status texts yes"注释与新意图矛盾）。已核对：全仓库无任何代码 `remove_flag` 撤销这两个隐藏；非微信样式变体下 `top_bar_`/`status_bar_` 均为 screen 直接子对象
- 关键事实: `low_battery_popup_` = `lv_obj_create(screen)`（lcd_display.cc:1036），**不在** `status_bar_` 内 → 低电量弹窗不受影响；Alert 的状态文字虽被隐藏，但 `SetEmotion`（表情）+`SetChatMessage`（聊天气泡）仍可见
- 验证: 编译+clang-format+烧录+启动正常；待目视确认主屏顶部无任何内容（无时钟/待机/图标）

### [2026-09-29] 第二屏仪表盘：40px 湿度+伪加粗+环色三档+序号+设置图标
- 状态: 已完成（已烧录，75s 监视启动无崩溃；视觉效果待目视确认）
- 范围: main/boards/waveshare/esp32-s3-touch-lcd-1.46/{esp32-s3-touch-lcd-1.46.cc,config.json,README.md}、AGENTS.md
- 需求: 湿度字体增大、与温度间距减小、湿度加粗；湿度 >20% 圆框橙 `#FFBB00`、>30% 红 `#ff4000`；每圆底部序号 1-4 小字；页面最底部中间设置图标
- 方案: `config.json` 的 `sdkconfig_append` 加 `CONFIG_LV_FONT_MONTSERRAT_40/12=y`（build.py 全量重生成 sdkconfig，构建输出可见两行 append）；湿度字换 `lv_font_montserrat_40`（line_height 44），删掉 `font_noto_sans_basic_30_4` 引用（map 0 引用被 GC，xiaozhi.bin 2.87→2.65MB）；`pad_row` 6→2
- 伪加粗: LVGL `text_outline_stroke` 只在 `#if LV_USE_FREETYPE && LV_USE_VECTOR_GRAPHIC` 内的 `draw_letter_outline` 生效（位图字体走不进，且 CONFIG_LV_USE_FREETYPE is not set）→ 改 4 层同文本 (0,0)/(1,0)/(0,1)/(1,1) 叠放，放**固定尺寸 130×48 容器**（`LV_SIZE_CONTENT` 会只量 x/y=0 的副本把其余裁掉，见 calc_content_width lv_obj_pos.c:1353 对非 layout 子对象按 align 分档处理）；容器清 bg_opa/border/pad/radius/scrollbar/SCROLLABLE
- 序号定位: `LV_OBJ_FLAG_FLOATING` 让 flex 跳过（lv_flex.c:341/478），`lv_obj_refr_pos()`（lv_obj_pos.c:777）对非 layout_positioned 子对象照常按 `BOTTOM_MID` 摆位，`lv_obj_move_to` 对 FLOATING 用父绝对坐标（lv_obj_pos.c:876）
- 环色: 判档挂 `RefreshSensorLabels()`（mock 与未来 BLE 都经过），新增 `sensor_circles_[4]` 成员；`lv_style_set_prop` 对同 prop 就地更新（lv_style.c:361），每 3s 调用不涨内存。**阈值按字面挂在湿度**，mock 湿度原 40~70 全 >30 恒红 → 改 15~35、初值 {16,24,33,26} 三档全可见；若阈值本意是温度（20/30℃ 更自然）是一行改动
- 设置图标: `font_material_symbols_30_4`（lcd_display.cc 已链接）+ `MATERIAL_SYMBOLS_SETTINGS`，`BOTTOM_MID y-8`（y≈374..404，圆屏该处半宽 ≥57px，30px 图标可见），仅在 card1 页面上
- 坑: 本 LVGL 版本透明度枚举是 `LV_OPA_TRANSP` 不是 `LV_OPA_TRANSPARENT`（首次编译报错已修）；clang-format 本机不在 PATH，pip 装的 clang-format 23.1.1（`E:\pyenv\...\Scripts`）
- 验证: 编译+clang-format+烧录 COM7+75s 监视：`sensor grid: diameter=150` 正常、`Refreshing display theme`（上次崩溃点）通过、MQTT 连接、无 crash/backtrace；待目视确认四圆 40px 加粗湿度、三档环色、序号 1-4、底部齿轮图标

### [2026-10-08] 第二屏设置功能：XL0801 蓝牙绑定 + 去模拟数据
- 状态: 已完成（编译+clang-format+主机测试+烧录硬件验证通过；四圆读数/重启恢复/环色待肉眼确认）
- 范围: main/boards/waveshare/esp32-s3-touch-lcd-1.46/{ble_sensor.h,ble_sensor.cc,esp32-s3-touch-lcd-1.46.cc,config.json,README.md}、AGENTS.md
- 需求: 去掉第二屏模拟数据；默认未绑定时温度显示"未绑定"、湿度留空；齿轮打开设置页自动搜索名称 XL0801 的蓝牙设备，显示 MAC 和绑定按钮；点绑定选 1-4 号位，绑定后返回第二屏按广播数据显示
- 方案: 新增 `ble_sensor.h/.cc`（NimBLE observer：`nimble_port_init`+`ble_gap_disc(BLE_HS_FOREVER)`，filter_duplicates=0 以持续刷新；AD 解析按名称 0x09/0x08 == XL0801 + 厂商数据 0xFF 尾部锚定 `[温度 u16 大端/10][湿度 u8][MAC 6B]`，前缀 `01 09` 忽略；绑定存 NVS ns `ble_sensor` key `slot1..4`）；板级 `CustomLcdDisplay` 增加 `settings_page_`（黑底覆盖层：标题/搜索状态/设备列表行=名称+montserrat_12 MAC+绑定按钮/返回）与 `slot_panel_`（1-4 数字按钮+空/已绑状态），`ApplyBinding` 写 NVS、同 MAC 自动清其它位、关设置页回第二屏；`RefreshSensorLabels` 三态（未绑定→"未绑定"+空+灰环，绑定无数据→`--`，有数据→数值+分档环色）；mock timer/初值/`SetSensorReading` 全部删除；扫描生命周期 = 设置页打开或有绑定才扫，否则 `StopScanning`；`config.json` sdkconfig_append 加 `CONFIG_BT_ENABLED=y`、`CONFIG_BT_NIMBLE_ENABLED=y`（NimBLE host 常驻不销毁）；设置页打开期间 `HandleSwipe` 早退吞手势
- 关键点: 例子 `0201040709584C303830310CFF0109012045ED680104918C` 解析得 28.8°C/69%/ED:68:01:04:91:8C（用户所述 0110=27.2、3E=62 为同布局另一包，位置"往前2位湿度/再往前4位温度"与样本 `45`/`0120` 完全对应）；IDF 6.1 `ble_gap_disc` 是 5 参（own_addr_type 在前）、`BLE_HS_FOREVER=INT32_MAX`、own_addr_type 常量是 `BLE_OWN_ADDR_PUBLIC`（无 `BLE_ADDR_TYPE_*`）；回调跑 NimBLE host task，动控件前取 `DisplayLockGuard`，绑定用内存缓存不碰 NVS；中文 label 一律继承 screen 字体不显式设（防主题换字体悬空，同 2026-09-28 崩溃教训）
- 齿轮热区修复: 首轮烧录后硬件上 30+ 次点击稳定落在 y=406-408（原 label 命中框 372..404，差 2-4px）全部 miss、无 `settings opened`；改为 `pad_hor 16 + pad_bottom 12` 后 `BOTTOM_MID y=+4`——文字视觉位置不变、命中框扩到 372..416 × 175..237，点击立即生效
- 验证: `python scripts/build.py waveshare/esp32-s3-touch-lcd-1.46 --name esp32-s3-touch-lcd-1.46` 通过（xiaozhi.bin 0x2db3a0，app 分区余 27%，sdkconfig 确认 BT_ENABLED/NIMBLE/CONTROLLER/OBSERVER 全 y）；clang-format --dry-run 通过；主机测试 scripts/tests 6 个报错经 stash 对比确认为改动前就存在的环境问题（tempfile 目录锁/缺工具链）；COM7 烧录+两次监视确认：`settings opened` → NimBLE `scan started` → `found ED:68:01:03:B5:AC: 28.5C 36%` → `device row added` → `binding picker` → `slot 1 bound` → `settings closed` 全链路通过、重复绑定正常、设置页打开期间手势被吞、无 crash、free sram 稳定 ~48KB（扫描常驻后，minimal 33KB）；待肉眼确认：四圆 1 号显示 28.5°C/36%、2-4 号"未绑定"+空湿度、设置页布局与三档环色

### [2026-10-08] 设置页二轮调整：返回热区 + 行首改实时读数
- 状态: 已完成（编译+clang-format+烧录通过，开机绑定恢复自动扫描已日志确认；返回/读数显示待实测）
- 范围: main/boards/waveshare/esp32-s3-touch-lcd-1.46/{esp32-s3-touch-lcd-1.46.cc,README.md}、AGENTS.md
- 需求: 1) 返回按钮关不掉设置页；2) 列表里所有设备都叫 XL0801，换成广播温湿度以区分
- 方案: 1) 返回按钮 96x40/-14（358..398）→ `120x56` + `BOTTOM_MID y=+4`（360..416 × 146..266，与齿轮同款圆屏边缘落点问题）；另加兜底：设置页打开时横向右滑 ≥60px 关闭（左/竖滑仍吞）。2) `FoundDevice` 增 `reading_label`，行首 "XL0801" 换成 `%.1f°C %.0f%%` 实时读数；`UpdateDeviceReading()` 每包 strcmp 节流后才 `lv_label_set_text`；行仅设置页打开时创建、关闭后仍静默刷新
- 验证: 编译（xiaozhi.bin 0x2db4d0，分区余 27%）+clang-format 通过；COM7 烧录+5min 监视：开机即恢复扫描（slot1 绑定存在 → 1.3s `scan started` → 4s `found ED:68:01:03:B5:AC: 28.5C 36%`）、无 crash、free sram 稳定 48KB；该轮无触摸操作，返回按钮/行读数需实测

### [2026-10-08] 语音切屏：MCP 工具 self.screen.show_sensor
- 状态: 已完成（编译+clang-format+烧录通过，工具注册/启动已日志确认；语音触发待实测）
- 范围: main/boards/waveshare/esp32-s3-touch-lcd-1.46/esp32-s3-touch-lcd-1.46.cc、AGENTS.md
- 需求: 第一屏说"我要查看温湿度"自动跳转第二屏仪表盘
- 方案: 云端 MCP 路径（协议/核心零改动）。板级 `CustomLcdDisplay` 增 public `ShowSensorPage()`——`DisplayLockGuard`（工具回调在主任务、非 LVGL 上下文，`ShowCard` 假定已持锁）+ 设置页开着先 `CloseSettings()`（`ShowCard` 对 `settings_open_` 是 no-op）+ `ShowCard(1)`；`CustomBoard::InitializeTools()`（构造函数调用，含 `#include "mcp_server.h"`）注册普通 `AddTool("self.screen.show_sensor", ...)`，description 写明查看温湿度时调用。工具回调经 `McpServer::DoToolCall → Application::Schedule` 在主任务执行，无需再 Schedule。触发依赖云端 LLM 从 tools/list 拉到该工具并决定调用
- 验证: clang-format --dry-run 通过；`python scripts/build.py waveshare/esp32-s3-touch-lcd-1.46 --name esp32-s3-touch-lcd-1.46` 编译通过（xiaozhi.bin 0x2db6f0，分区余 27%）；COM7 烧录+65s 监视：`MCP: Add tool: self.screen.show_sensor`（板级工具先于核心工具注册）、MQTT 连上进 idle、无 crash、free sram 48KB、BLE 读数正常；待实测语音"我要查看温湿度"→ 跳第二屏（看 `show sensor page requested (MCP)` 日志）、设置页打开时语音指令（应先关设置页）、右滑返回

### [2026-10-08] BLE 扫描改为仅第二屏激活（排查配网热点掉线）
- 状态: 已完成（编译+烧录+硬件复测通过；配网一次成功）
- 范围: main/boards/waveshare/esp32-s3-touch-lcd-1.46/{esp32-s3-touch-lcd-1.46.cc,README.md}、AGENTS.md
- 需求: 换网络环境后手机连配网热点 Xiaozhi-2CC9 约 10 秒即断、配网页打不开；串口日志（两轮，`wifi_config_log.txt`）显示设备无崩溃、页面曾成功打开但链路在活跃使用时段反复 `Station left`（19:44:06~19:44:56 五次掉线后又稳定 4 分钟），组件日志丢弃 disassoc reason 无法定责；用户判断是 BLE 常驻扫描（`duration=forever`，当天新增 XL0801 绑定功能）与 SoftAP 2.4G 共存干扰 → 要求 BLE 改为只在第二屏才扫描
- 方案: `RefreshScanState()` 门槛改为 `card_index_ == 1 && (settings_open_ || any_bound)`（原为开机有绑定即常扫）；`ShowCard()` 切卡后调用 `RefreshScanState()` 实现进第二屏开扫、回主屏停扫；开机默认 card 0 → NimBLE host 首次初始化推迟到首次进第二屏/设置页
- 验证: 编译（xiaozhi.bin 0x2db700，分区余 27%）+clang-format+烧录 COM7 通过；第三轮日志（20:15~20:25）：开机日志**完全没有** BLE_INIT/NimBLE/ble_sensor（free sram 183KB vs 原 117KB，NimBLE 栈不再常驻）；20:16:54 手机入网→20:17:16 提交密码→CSA 切信道→保存→退出配网→STA 拿到 192.168.31.196→MQTT 连上进 idle，**全程 0 次 `Station left`**，配网一次成功（此前同环境下 10 秒即断）；配网期间 config AP 的 10s 扫描仍在跑但不再掉线，反证 BLE 共存是主因；待肉眼确认：滑到第二屏 BLE 恢复扫描出数、滑回主屏停扫

### [2026-10-08] 第三屏：拓竹打印机状态页（本地 MQTT + 屏上设置）
- 状态: 进行中（编译+clang-format+主机测试通过，待烧录硬件验证）
- 范围: main/boards/waveshare/esp32-s3-touch-lcd-1.46/{bambu_printer.h,bambu_printer.cc,esp32-s3-touch-lcd-1.46.cc,config.json,README.md}、AGENTS.md
- 需求: 参考 https://github.com/dssoftx/BambuSphere 规划第三屏显示拓竹打印机状态；先调研其打印机绑定方式；实施方向经确认=本地 MQTT + 第三屏设置页录入 IP/SN/访问码 + 核心四项显示（进度/剩余时间/喷嘴热床温度/状态），本期纯展示无控制按钮
- 方案: BambuSphere 绑定结论：本地模式三要素=打印机 IP+SN+LAN 访问码，TLS 8883、user=bblp、password=访问码、主题 device/<SN>/report|request、连后 pushall；云模式=账号登录+/v1/iot-service/api/user/bind 拉绑定列表+云端 MQTT（本期不做）。参考实现（**未抄代码**，FNCL 非商业许可，协议本身公开）新增板级 `bambu_printer.h/.cc`：独立第二个 esp_mqtt_client（与小智协议客户端共存，`EspNetwork::CreateMqtt()` 每次新建）、分块重组≤64KB、增量合并解析 5 字段、双锁 life→status 且 handler 绝不取 life 锁（stop 会 join MQTT task）、状态变更经 10ms esp_timer 延迟到 esp_timer task 再回调 UI（防 SetConfig stop 与 LVGL 锁互等死锁）、NVS ns=`bambu_printer`。板级 UI：`kCardCount=3`、`ShowCard` 改多页显隐表、`BuildPrinterPage`（状态文字着色+lv_arc 进度环+montserrat_40 百分比+剩余时间+喷嘴/热床+IP 提示+齿轮）、`BuildPrinterSettings`（3 行表单+lv_keyboard 编辑态：IP/访问码 NUMBER 数字盘、SN TEXT_LOWER，textarea password mode 掩码访问码，编辑态吞全部滑动手势，保存校验 IP/SN/访问码格式）；`settings_open_` 扩展为任一覆盖层打开、新增 `CloseAnySettings()`；MCP 工具 `self.screen.show_printer`；`CustomBoard::SetNetworkEventCallback` 包装 Application 回调挂 OnNetworkUp/Down（Connected→2s 延迟连、Disconnected/配网模式→销毁）。config.json 新增 `CONFIG_LV_USE_KEYBOARD`、`CONFIG_ESP_TLS_INSECURE`+`CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`（IDF 6.1 无 CA 时 esp-tls 直接 ESP_ERR_MBEDTLS_SSL_SETUP_FAILED，必须开 VERIFY_NONE——打印机自签名证书无法静态 CA 覆盖，由访问码认证会话）、`CONFIG_MQTT_BUFFERS_ON_EXTERNAL_MEMORY`+`CONFIG_MQTT_TASK_STACK_ON_EXTERNAL_MEMORY`（16KB 缓冲/8KB 栈进 PSRAM）。**首轮硬件反馈修复**：输入 IP 后点键盘 ✓ 无法退出编辑窗——LVGL 对 `LV_SYMBOL_OK` 只发 `LV_EVENT_READY`（默认处理器不关窗，此前仅"完成"按钮能退出）→ 对 kb 注册 READY/CANCEL、对 ta 注册 READY（Enter 单行键只发 ta）→ `StopPrinterEditing()` 回表单，`printer_focus_<0` 守卫防双发
- 验证: `python scripts/build.py waveshare/esp32-s3-touch-lcd-1.46 --name esp32-s3-touch-lcd-1.46` 编译通过（xiaozhi.bin 0x2df3a0，分区余 27%，sdkconfig 确认 5 个新 flag 全部 =y）；clang-format --dry-run 通过；主机测试 scripts/tests 6 个报错经 stash 对比确认为改动前即存在的环境问题（tempfile RecursionError）；COM7 烧录 + 75s 监视冒烟通过：启动无 crash/backtrace、`MCP: Add tool: self.screen.show_printer` 注册、`Refreshing display theme`（历史崩溃点）通过、WiFi Connected 后 2s 准时出现 `bambu_printer: printer not configured yet`（延迟连接链路实测工作）、steady free sram 117KB/min 33KB（无内存压力）；✓修复后二次编译（xiaozhi.bin 0x2df3f0）+烧录+45s 冒烟通过（同上指标）；**待实机功能验证**（需打印机+录入参数）：左滑 0→1→2、未配置提示、齿轮→键盘录入→**✓ 退出编辑窗**→保存→MQTT 连接出数、圆屏键盘可点性、配置后双 TLS 并存的 free sram、语音"打开打印机页面"

### [2026-10-08] 第三屏页面优化：全表盘进度环 + BambuSphere 图标
- 状态: 进行中（编译+clang-format+烧录+355s 监视通过，待目视确认布局/图标/颜色）
- 范围: main/boards/waveshare/esp32-s3-touch-lcd-1.46/{esp32-s3-touch-lcd-1.46.cc,font_bambu_icons_20.c(新增),README.md}、AGENTS.md
- 需求: 进度条移到表盘最外圈、去掉蓝色圆点、轨道深灰、进度绿 `#21A452`；环内信息自上而下=进度%(绿)+打印状态(绿、字号大一号)+IP(灰、状态下)；喷嘴/热床温度移到靠下、汉字换成 BambuSphere 的图标；设置按钮位置不变、进度条从设置按钮两侧开始和结束
- 方案: arc 404px（中心半径195/外缘202，贴 R=206 黑边），`lv_arc_set_bg_angles(108°,72°)` 底部缺口环绕齿轮（两端 (146,391)/(266,391) = 齿轮命中框两侧 ~29px；wrap 语义同默认 135/45）；主题蓝点 = arc 的 LV_PART_KNOB，bg_opa/border/outline/shadow 全置透明去掉；状态字换 `font_noto_sans_basic_20_4`（16→20 大一号，静态 flash 字体），在线状态一律 `#21A452` 仅 FAILED 保留红、未配置/连接中/离线灰；温度行 = 图标+数值 flex 行，图标为 MDI `printer-3d-nozzle`(U+F0E5B)/`waves-arrow-up`(U+F185B)（BambuSphere `kMdiNozzle/kMdiBed` 同款，经 fonttools 确认 codepoint→glyph 名），**未抄其 FNCL 字体文件**，用本机 node+lv_font_conv 从 Apache-2.0 MaterialDesign TTF（@mdi/font 7.4.47 下载）生成 `font_bambu_icons_20.c`（板级目录 glob 编译，头注释带出处许可）；温度值去掉"喷嘴/热床"前缀
- 验证: 编译通过（xiaozhi.bin 0x2fd700，+124KB 主要为20px中文字体，分区余 24%）；clang-format --dry-run 全部通过（生成的字体文件 -i 后）；COM7 烧录 + 355s 监视：`connecting to printer mqtt 192.168.31.172 (serial=0309AA452001874)`（NVS 配置持久化）→ `mqtt connected` → `subscribed` → `gcode_state=FINISH`；用户实测滑动 card0→1→2 与右滑返回、BLE 扫描随卡片启停正常、无 crash；free sram 稳定 115KB（card2 双 MQTT）/49KB（card1 BLE 扫描）；**待目视**：环贴边、缺口包住齿轮、无蓝点、进度/状态绿、20px 状态字、两个 MDI 图标正常显示、双温靠下

### [2026-10-08] 第三屏状态行英文化 + 剩余时间改时钟图标
- 状态: 进行中（编译+clang-format+烧录+258s 监视通过，待目视确认英文状态词/时钟图标/时间格式）
- 范围: main/boards/waveshare/esp32-s3-touch-lcd-1.46/{esp32-s3-touch-lcd-1.46.cc,font_bambu_icons_20.c,README.md}、AGENTS.md
- 需求: 未连接时状态只显示一个"机"字（bug）→ 查明根因后用户选择"仅状态行改英文"；英文词表要求对齐 BambuSphere；剩余时间去掉"剩余"字样，改成同 BambuSphere 的时间图标行
- 根因（只读排查确证）: 状态行专用的静态 `font_noto_sans_basic_20_4` 对 ASCII 0x20-0x7E 是完整 dense 覆盖，但 **CJK 仅 531 汉字的 SPARSE 子集**（解析 cmap 逐字核对："打印机离线"只有`机`有字形→LVGL 静默跳过其余四字，屏上只剩"机"；"未配置"→"配"、"打印中"→"中"、"空闲"→全无）。16px 标签正常是因为它们继承主题字体、assets 加载时被 `LvglStrategy` 换成全字库 cbin——上一轮把状态字改为显式静态 20px 后脱离了 cbin 换字路径
- 方案: **状态行全英文小写，词表照抄 BambuSphere**（`lifecycle_label`/`select_ui_status`）：未配置→`setup`、连接中→`connecting`（BS 兜底为 `waiting...`，刻意取更明确的词）、断开→`offline`、刚连上→`connected`、RUNNING→`printing`、PREPARE/INIT/SLICING→`preparing`、PAUSE→`paused`、FINISH→`done`、FAILED→`failed`（保留红）、IDLE→`idle`、OFFLINE→`offline`、未知原样；`PrinterStateText` 与 conn 分支改词 + 两处防回归注释（该行禁中文）。**剩余时间行**：`font_bambu_icons_20.c` 重新生成为三字形（+ U+F144E `clock-time-four-outline` = BS 的 `kMdiClock`，lv_font_conv 工具链沿用、字体名不变），单 label 换成居中 flex 行 [时钟图标(灰 #999, gap 6) + 数值]，格式照 BS `remaining_text()`：FINISH→`Done`、无数据/未连接→`--m`、≥60min→`1h 35m`、<60min→`45m`（`PrinterRemainingText()`，静态 buf 仅持锁调用）；引导语/设置页中文不动（走继承字体正常）
- 验证: clang-format --dry-run 通过；编译 xiaozhi.bin 0x2fd880（+384B=第三字形，分区余 24%）；COM7 烧录 + 258s 监视：启动无 crash、`show_printer` 注册、历史崩溃点通过；**该轮设备已换网络**（page7-jm→Halosee, 192.168.110.x），配置的打印机 192.168.31.172 不在同网段 → `esp-tls select() timeout`→`mqtt error`→offline，按设计 ~30s 退避重试（预期内环境性失败，正好走 offline 状态路径）；用户实测滑动 card0↔1↔2 正常、BLE 随卡片启停、free sram 49KB(扫描)/115KB 稳定；**待目视/待原网络复测**：`offline`/`idle`/`done` 等全词显示、时钟图标 + `--m`/`Done`/`1h 35m`、回原网络后在线状态词

### [2026-10-08] 微调：状态字体 30px + 设置页按钮上移
- 状态: 进行中（编译+clang-format+烧录待监视验证，待目视确认字号与按钮落点）
- 范围: main/boards/waveshare/esp32-s3-touch-lcd-1.46/{config.json,esp32-s3-touch-lcd-1.46.cc,README.md}、AGENTS.md
- 需求: 1) 打印机状态字体再增大；2) 第二屏、第三屏设置界面的保存/返回按钮太靠下，圆屏不好点，上移
- 方案: **状态字体 20→30px**：改用 `lv_font_montserrat_30`（`config.json` 加 `CONFIG_LV_FONT_MONTSERRAT_30=y`，lv_font.h 自带声明无需宏；纯 ASCII ~+15KB，状态行本就英文），y 170→166（行高33，避让 IP 行 y=206）；删除 `LV_FONT_DECLARE(font_noto_sans_basic_20_4)` 及唯一引用 → noto20 被链接器 GC，**bin 净减 ~80KB（0x2fd880→0x2ea150，分区余 24%→26%）**；两处注释同步（纯 ASCII 字体中文会整字不画 + 531 汉字历史根因）。**按钮上移**：第二屏 BLE 返回 `120x56 BOTTOM_MID y=+4→-28`（底边416→384，框328..384×146..266，该处半弦104整键入弦内）；第三屏保存/完成/返回 `96x44 ±56 y=+2→-28`（底边384，弦 x=102..310 覆盖三键，键盘310以下不重叠）；两处注释/README 写明"贴边圆弧区难按"的上移理由
- 验证: clang-format --dry-run 通过；编译通过（含 CONFIG_LV_FONT_MONTSERRAT_30 生效）；烧录 COM7；待目视：状态字 30px 明显变大、两个设置页按钮位置舒服好点、编辑态（键盘弹出）下按钮不被遮挡

### [2026-10-08] 设置页按钮命中框扩大（ext_click_area）
- 状态: 进行中（编译+烧录+170s 监视通过；监视中用户实测两页设置开关多次、关闭均走按钮回调，待手感确认）
- 范围: main/boards/waveshare/esp32-s3-touch-lcd-1.46/{esp32-s3-touch-lcd-1.46.cc,README.md}、AGENTS.md
- 需求: 状态字体已认可；但设置页保存/返回按钮"不太容易触发，必须按到文字才能操作？"
- 排查（只读）: LVGL9 命中链查清——`lv_indev_search_obj` 取最深坐标命中并跳过 hidden（lv_indev.c:618）；label 构造即去 CLICKABLE（lv_label.c:762）→ 点文字最终落到父按钮；`lv_obj_hit_test` = CLICKABLE + coords+ext_click_pad（lv_obj_pos.c:1199 / lv_obj_get_click_area）；`lv_obj` 默认带 CLICKABLE（lv_obj.c:584）、`lv_button` 去 SCROLLABLE（lv_button.c:66）→ 纯代码路径整块按钮都该可点；已排除 hidden 的"完成"同位遮挡、邻居容器遮挡、滚动吞点击、CLICKED 事件缺失（槽位/绑定按钮同事件码实机一直好用）。与本板历史一致：中区按钮（56×56/72×40）从无问题，贴下缘控件（齿轮）当年必须扩热区才能点 → 判定为下缘区按压/坐标误差把边缘点击带到盒外
- 方案: `lv_obj_set_ext_click_area()`（命中框=coords+ext，纯命中扩张、视觉零变化，LVGL 自家 arc/slider 同款）：第二屏返回 **+16**（孤立无邻居）；第三屏保存/完成/返回 **+8**（上限：save 扩后 94..206、back 206..318，零重叠——back 是后建兄弟，重叠区会归它，会偷走 save 右缘）
- 验证: format + 编译 0x2ea180（+48B）+ 烧录通过；170s 监视：`settings opened/closed`×2、`printer settings opened/closed`×3，**每次 closed 前均无 `swipe right closes settings` → 关闭全部来自按钮回调**（返回/保存实机已触发）；无 crash；待用户确认手感，若仍不灵 → 下一步加点击目标探针日志（indev 目标对象 + 坐标）
