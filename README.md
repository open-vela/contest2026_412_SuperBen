# openvela 智能语音助手（VelaVoice）

> 2026 首届 openvela AI 硬件开发者大赛 · 新硬件平台适配赛道
> 队伍：SuperBen · 平台：ESP32-P4

## 项目简介

VelaVoice 是一个运行在 **ESP32-P4 新硬件平台**上的端云协同 AI 语音助手。项目将 openvela（NuttX）完整移植到乐鑫 ESP32-P4 芯片，并落地 **图形（LVGL + MIPI-DSI 屏）、AI（MiMo 云端语音模型）、多媒体（ES8311 音频）** 三项 openvela 核心能力，实现"能听、会说、能看"的智能交互设备。

- 语音识别（ASR）、对话（LLM）、语音合成（TTS）均接入大赛官方推荐的 **MiMo 模型**
- 7 寸 MIPI-DSI 屏实时显示**微信式聊天界面**，对话内容即时上屏
- 支持唤醒词 **"Hello, openvela"**

## 硬件平台

| 模块 | 型号 | 说明 |
|---|---|---|
| 主控 | ESP32-P4 | 双核 400MHz + LP 核，32MB PSRAM |
| 屏幕 | EK79007AD 7 寸 | 1024×600 RGB565，MIPI-DSI 2-lane |
| 音频 | ES8311 | 输入/输出（`/dev/pcm_in0`、`/dev/pcm0`） |
| 触摸 | GT911 | 电容触摸，`/dev/input0` |
| 摄像头 | SC2336 | MIPI-CSI（预留） |
| 网络 | 以太网 | 板载 EMAC |

## 功能特性

- ✅ **语音对话**：麦克风录音 → MiMo ASR → MiMo LLM → MiMo TTS → 喇叭
- ✅ **实时聊天界面**：LVGL 微信式气泡界面，对话文字即时上屏（左=助手，右=用户）
- ✅ **个性化问候**：说出 "Hello, openvela" 时回复 "Hello, Aurora"
- ✅ **本地快路径**：时间、天气等简单意图本地处理，无需等待 LLM
- ✅ **断网降级**：网络异常时返回友好提示

## 系统架构

```
麦克风 ──> ASR(MiMo) ──> LLM(MiMo) ──> TTS(MiMo) ──> 喇叭
                              │
                              └──> chatui(LVGL) ──> 7寸屏（实时气泡）
```

- **端侧**：openvela(NuttX) 负责采集、播放、显示、网络
- **云端**：MiMo 模型负责 ASR/LLM/TTS 推理
- **通信**：ai_agent 框架的消息总线 + POSIX 消息队列（chatui 实时订阅）

## 运行方式

### 构建

```bash
# 在 openvela 工作区（WSL Ubuntu 24.04）
cd openvela
export PATH=$PWD/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin:$PWD/prebuilts/tools/cmake/bin:$PATH
cmake -B cmake_out/esp32p4-function-ev-board_nsh -S nuttx \
  -DBOARD_CONFIG=../vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/ai_agent \
  -DCUSTOM_MODULE_PATH=build/cmake -GNinja
ninja -C cmake_out/esp32p4-function-ev-board_nsh
```

### 烧录

```bash
esptool -p /dev/ttyUSB0 -b 460800 --before default-reset write-flash \
  0x2000 cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin
```

### 使用

```bash
# 1. 启动聊天界面（后台）
nsh> chatui &

# 2. 启动 AI 助手
nsh> ai_agent

# 3. 配置 MiMo（ASR/LLM/TTS 共用一个 key）
vela> set_llm mimo <your-api-key>

# 4. 对话（串口方式）
vela> ask Hello, openvela

# 5. 语音方式（唤醒词）
说出 "Hello, openvela" 开始对话
```

## 目录结构

```
vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/  # 板级支持（BSP）
├── configs/ai_agent/defconfig                             # 参赛配置
└── src/                                                    # 屏/音频/触摸/网络驱动
packages/ai_agent/                                          # AI 助手框架
├── src/voice/mimo_asr.c                                    # MiMo ASR 后端（自研）
├── src/voice/mimo_tts.c                                    # MiMo TTS 后端（自研）
└── src/core/                                               # agent loop / 消息总线
apps/examples/chatui/                                       # 微信式聊天界面（LVGL）
```

## 关键技术难点（已攻克）

1. **全新平台适配**：官方仅提供 esp32s3-eye 参考，ESP32-P4 需从零适配（启动、PSRAM、DSI、音频、触摸）
2. **mbedtls 双 fork 冲突**：修复构建系统 include 路径
3. **PSRAM 启动崩溃**：TFLite arena 溢出 + PSRAM 缺失容错
4. **看门狗假超时**：系统时钟跳变导致，改用 monotonic 时钟
5. **DSI 屏幕撕裂**：定位为供电纹波（杜邦线），改 USB 供电解决

## License

Apache 2.0
