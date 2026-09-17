# openvela 智能语音助手（VelaVoice）

> 2026 首届 openvela AI 硬件开发者大赛 · 新硬件平台适配赛道
> 队伍：SuperBen（`contest2026_412_SuperBen`） · GitHub：`Aurora-QIU0`

## 项目简介

本项目基于 ESP32-P4 完成了 openvela 系统的深度适配与优化，成功打通图形、AI、多媒体三大核心能力，构建出流畅的语音交互体验。人工智能引擎初始化仅需 410 毫秒，系统整体启动时间不足 1 秒，为端侧实时交互提供了坚实的性能保障。

在图形显示方面，项目成功驱动 7 寸 MIPI-DSI 显示屏并移植 LVGL 图形库，实现了媲美智能手机的微信式聊天界面与流畅动效。多媒体方面，已完成音频通路适配，支持麦克风录音与播放器播放，设备节点注册正常。

网络连接上，通过适配以太网接口，实现了稳定可靠的有线联网方案，并成功与小米 MiMo 大模型完成联调。语音交互链路（ASR+LLM+TTS）已全链路打通并支持自定义人设配置，测试阶段通过文本指令输入验证了大模型的对话与响应能力。

后续将持续优化离线命令词识别与语音唤醒功能，进一步提升交互的自然度与产品的完整度。

> openvela 官方对乐鑫芯片的支持此前仅覆盖 ESP32-S3，ESP32-P4 处于适配空白：
> 无板级支持包、无启动配置、无驱动移植。本项目从零打通了启动引导、PSRAM、
> MIPI-DSI 显示、ES8311 音频、GT911 触摸与以太网的全套板级驱动，
> 让 openvela 在这颗 RISC-V 双核芯片上真正跑了起来。

## 硬件平台

| 模块 | 型号 | 说明 |
|---|---|---|
| 主控 | ESP32-P4 | RISC-V 双核 400MHz + LP 核，32MB PSRAM，16MB Flash |
| 屏幕 | EK79007AD 7″ | 1024×600 RGB565，MIPI-DSI 2-lane @1000Mbps |
| 音频 | ES8311 | 录音 / 播放（`/dev/pcm_in0`、`/dev/pcm0`）+ NS4150B 功放 |
| 触摸 | GT911 | 电容触摸，`/dev/input0`（I2C0，轮询模式） |
| 网络 | 以太网 | 板载 EMAC + IP101 PHY |
| 摄像头 | SC2336 | MIPI-CSI（预留接口，未纳入本版本固件） |

## 功能状态

| 能力 | 状态 |
|---|---|
| ASR + LLM + TTS 全链路（接入 MiMo 模型） | ✅ 真机验证 |
| 微信式聊天界面（LVGL，双缓冲，消息队列解耦） | ✅ 真机验证 |
| 以太网联网，可上外网 | ✅ 真机验证 |
| 本地快路径（固定意图不依赖网络） | ✅ 真机验证 |
| 音频播放到喇叭 | 🔧 驱动链路全通（DMA 传输成功），功放出声最后调优 |
| 离线命令词识别 / 语音唤醒 | 🔧 规划中，当前入口为文本 `ask` 命令 |
| 摄像头实时上屏 | 🔧 曾跑通，未纳入本版本固件 |

> 设备节点全部注册成功：`/dev/fb0`、`/dev/input0`、`/dev/pcm0`、`/dev/pcm_in0`。
> 实测：系统启动 < 1 s，AI 引擎初始化约 410 ms，屏幕 1024×600@60Hz，
> 多轮对话连续运行稳定无崩溃。

## 快速开始

### 1. 获取工作树

```bash
repo init -u https://github.com/open-vela/contest2026_412_SuperBen \
  -b dev-ai-contest-2026 -m contest2026_412_SuperBen.xml
repo sync -c -j8
```

`app/chatui/` 与 `board/contest_board/` 由 manifest 的 `<linkfile>` 映射到工作树：

```
app/chatui           ->  apps/examples/chatui
board/contest_board  ->  vendor/espressif/boards/esp32p4/esp32p4-function-ev-board
```

板级目录目标路径与 defconfig 中 `CONFIG_ARCH_BOARD_CUSTOM_DIR` 完全一致，同步后无需改路径。

### 2. 应用公共仓改动

作品大部分代码位于**公共仓**（`nuttx` / `packages` / `apps`），队伍仓无法直接 push，
故以两种等价形式提供，任选其一：

```bash
./tools/apply_overlay.sh /path/to/openvela     # 复制改动后的完整文件
# 或
git apply patches/*.patch                       # 应用 unified diff
```

- `overlay/` — 这些文件的**最终完整版本**，可直接阅读、grep、复制
- `patches/` — 同一批改动的 **unified diff**
  （`nuttx-esp32p4-port` / `packages-ai_agent` / `apps-chatui` / `esp32p4_i2c_clock_timeout_fix`）

### 3. 构建

```bash
cd openvela
export PATH=$PWD/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin:$PWD/prebuilts/tools/cmake/bin:$PATH
lunch vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/ai_agent \
      cmake_out/esp32p4-function-ev-board_nsh
cd cmake_out/esp32p4-function-ev-board_nsh && cmake . && m -j8
```

产物：`cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin`

> 修改 defconfig 后需 `rm -f cmake_out/<dir>/.config` 再 cmake，否则配置不会重新生成。

### 4. 烧录

```bash
esptool -p /dev/ttyUSB0 -b 460800 write-flash 0x2000 \
  cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin
```

> 老版 EV board 的串口挂外置 CH340，`--after hard_reset` 无效，
> 烧录完成后需**人工单按一次 RESET** 才会运行新固件。

### 5. 运行

```bash
nsh> chatui &                        # 启动聊天界面（后台）
nsh> ai_agent                        # 启动 AI 助手
vela> set_llm mimo <your-api-key>    # 配置 MiMo（ASR/LLM/TTS 共用一个 key）
vela> ask Hello, openvela            # 对话（当前入口为文本命令）
```

## 本仓内容

```
app/chatui/                    微信式聊天界面（LVGL 应用）
board/contest_board/           ESP32-P4 板级支持包（启动/PSRAM/DSI/触摸/音频/以太网）
overlay/                       公共仓改动的完整文件
patches/                       同一批改动的 unified diff
docs/技术报告.docx              作品技术报告
docs/演示视频.mp4               演示视频（43 秒）
logs/Aurora-QIU0/              AI Coding 日志
tools/apply_overlay.sh         overlay 一键应用脚本
tools/fix_openvela_build.sh    构建环境修复脚本
contest2026_412_SuperBen.xml   manifest（linkfile 映射）
```

## 关于 AI Coding 日志

`logs/` 已按官方要求归集：共 **3 个会话 / 11 条事件**，位于 `logs/Aurora-QIU0/2026-09-09/`，
通过官方 `contest-log-collector/tools/validate-log.py` 校验（✅ ALL OK）。

| 会话 | 事件数 | 采集方式 |
|---|---|---|
| `ses_f79bf4b32ffe5qQgVNArrGIZsP` | 7 | 插件自动采集（`cli`），含 reasoning 与工具调用 |
| `ses_f79c4b883ffec44l1cNMN8UnXx` | 2 | 官方 `export-session.py --backfill --source opencode` 补导 |
| `ses_f79c263c1ffe3ZHW1Nplhv9bHc` | 2 | 同上 |

需要如实说明：本作品的主要开发过程（板级 bringup、驱动排障、云端语音后端、界面联调）
并非全部在官方白名单工具（Claude Code / AIoT-IDE / OpenCode / Codex）中完成，
因此 `logs/` 中记录的会话数与实际工时并不相称。仓内保留的是采集器与官方 backfill
**实际产出的记录**，未做任何人工增补、改写或美化。

AI 相关沉淀：`.claude/skills/embedded-false-success-debug/`
（嵌入式驱动「假成功」类故障排查方法论，源自本项目 I2C 根因排查过程）。

## License

Apache 2.0
