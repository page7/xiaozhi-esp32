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
- 状态: 已完成（已烧录；待用户滑动验证）
- 范围: main/boards/waveshare/esp32-s3-touch-lcd-1.46/{esp32-s3-touch-lcd-1.46.cc,touch_spd2010.h,touch_spd2010.cc,README.md}、AGENTS.md
- 需求: 硬件验证发现 press/release 日志正常但左滑不切换页面、温湿度计页不出现
- 根因: 对象级 `LV_EVENT_PRESSED/PRESSING/RELEASED` 只发给 hit-test 命中的对象（`container_` 等），`event_send_core()`（lv_obj_event.c:434）向上冒泡要求链条上每个对象带 `LV_OBJ_FLAG_EVENT_BUBBLE`——全仓库 0 处设置 → 注册在 screen 上的 `SwipeEventCb` **从未被调用**（上一轮"这三个冒泡到 screen"的假设错误，当时未硬件验证）
- 方案: 改用 **indev 级** `lv_indev_add_event_cb(indev, cb, filter, this)`：`send_event()`（lv_indev.c:1892/1900）把 `PRESSED`/`RELEASED` 无条件、先于对象分发转发到 indev 自身事件列表，不依赖命中对象与滚动状态。`PRESSING` 不在转发名单 → 手势按**按下点→抬手点差值**计算。配套：`touch_spd2010::Register()` 改返回 `lv_indev_t*`、release 日志带最后接触坐标（`s_last_x/y` 释放时保留）、`InitializeTouch()` 拿到 indev 后调 `display_->RegisterSwipeDetection(indev)`（`display_` 成员类型改为 `CustomLcdDisplay*`）、`HandleSwipe` 先无条件打印 `touch release dx= dy=` 便于调阈值
- 排查确认: `wait_until_release` 仅对象删除时置位（lv_obj_tree.c），正常滑动不影响 release 送达；`LV_USE_GESTURE_RECOGNITION` 依赖 `LV_USE_FLOAT`（未启用）无干扰
- 验证: 目标板编译+clang-format 通过、烧录成功；待用户滑动确认 `touch release dx=..`/`swipe left .. card 1` 日志与页面切换
