# openvela 智能语音助手（VelaVoice）

> 2026 首届 openvela AI 硬件开发者大赛 · 新硬件平台适配赛道
> 队伍：SuperBen（`contest2026_412_SuperBen`） · 平台：ESP32-P4
> 运行系统：openvela（NuttX） · 作品名：**VelaVoice**

## 项目简介

VelaVoice 是一个运行在 **ESP32-P4 新硬件平台**上的端云协同 AI 语音助手。项目把
openvela（NuttX）完整移植到乐鑫 ESP32-P4 芯片，并落地 **图形（LVGL + MIPI-DSI 屏）、
AI（MiMo 云端语音模型）、多媒体（ES8311 音频）** 三项 openvela 核心能力。

- 语音识别（ASR）、对话（LLM）、语音合成（TTS）均接入大赛官方推荐的 **MiMo 模型**，
  后两个模型的后端为自研 C 实现
- 7 寸 MIPI-DSI 屏实时显示**微信式聊天界面**，对话内容即时上屏
- 板级适配（启动 / PSRAM / MIPI-DSI / 触摸 / 音频 / 以太网）全部从零完成，
  官方此前仅提供 esp32s3-eye 参考

## 硬件平台

| 模块 | 型号 | 说明 |
|---|---|---|
| 主控 | ESP32-P4 | 双核 400MHz + LP 核，32MB PSRAM，16MB Flash |
| 屏幕 | EK79007AD 7 寸 | 1024×600 RGB565，MIPI-DSI 2-lane @1000Mbps |
| 音频 | ES8311 | 输入/输出（`/dev/pcm_in0`、`/dev/pcm0`）+ NS4150B 功放 |
| 触摸 | GT911 | 电容触摸，`/dev/input0`（I2C0，轮询模式） |
| 摄像头 | SC2336 | MIPI-CSI（预留接口，未纳入本版本固件） |
| 网络 | 以太网 | 板载 EMAC + IP101 PHY |

## 功能特性

| 能力 | 状态 |
|---|---|
| 端云协同对话：`ask <text>` → MiMo LLM → 回复即时上屏 | ✅ 真机验证 |
| 微信式实时聊天界面（LVGL，双缓冲，消息队列驱动） | ✅ 真机验证 |
| 自研 MiMo 云端语音后端（ASR / TTS，C 实现 + WAV/base64/HTTPS） | ✅ 后端就绪、真机验证 |
| 个性化问候：输入 `Hello, openvela` 回复 `Hello, Aurora` | ✅ 真机验证 |
| 本地快路径（时间等简单意图，不调用 LLM） | ✅ 真机验证 |
| 以太网联网（EMAC + 通用 PHY，可上外网） | ✅ 真机验证 |
| 音频播放到喇叭 | 🔧 驱动链路全通（DMA 传输成功），功放出声最后调优 |
| 语音唤醒词（"Hello, openvela"） | 🔧 未实现，当前入口为文本 `ask` 命令 |
| 摄像头实时上屏 / TFLite 检测 | 🔧 曾跑通，未纳入本版本固件 |

> 上表口径与技术报告 `docs/技术报告.docx` 第 3.5 节测试表一致。设备节点已经全部注册成功
> （`/dev/fb0`、`/dev/input0`、`/dev/pcm0`、`/dev/pcm_in0`），触摸与界面为真机点验通过。

## 系统架构

```
麦克风 ──> ASR(MiMo) ──> LLM(MiMo) ──> TTS(MiMo) ──> 喇叭
                              │
                              └──> chatui(LVGL) ──> 7寸屏（实时气泡）
```

- **端侧**：openvela(NuttX) 负责采集、播放、显示、网络
- **云端**：MiMo 模型负责 ASR/LLM/TTS 推理（Token Plan，同一 key）
- **通信**：ai_agent 框架的消息总线 + POSIX 消息队列（chatui 实时订阅）

## 本仓内容

```
README.md                      本文件
app/chatui/                    微信式聊天界面（LVGL 应用，本仓真实源码）
board/contest_board/           ESP32-P4 板级支持包（本仓真实源码）
overlay/                       公共仓改动的完整文件（nuttx / packages / apps）
patches/                       同一批改动的 unified diff
docs/技术报告.docx              作品技术报告
docs/演示视频.mp4               演示视频（43 秒）
logs/Aurora-QIU0/              AI Coding 日志
tools/apply_overlay.sh         把 overlay/ 应用到已同步的工作树
tools/fix_openvela_build.sh    构建环境修复脚本
contest2026_412_SuperBen.xml   manifest（linkfile 映射，见下）
esp32p4_移植补丁清单.md          补丁清单与构建复现说明
```

### 为什么既有 `overlay/` 又有 `patches/`

本作品大部分代码位于**公共仓库**（`nuttx`、`packages`、`apps`），队伍仓无法直接 push，
只能以 PR 形式提交到上游分支。为了让评审能直接阅读与核对代码，两种形式同时提供：

- `overlay/<工作树相对路径>` — 这些文件的**最终完整版本**，可直接阅读、grep、复制
- `patches/*.patch` — 同一批改动的 unified diff，可用 `git apply` / `patch -p1` 应用

两者描述同一棵树的状态，任选其一即可。

而 `app/chatui/` 与 `board/contest_board/` 则是本仓自有的源码，经 manifest 的
`<linkfile>` 映射进工作树：

```
app/chatui           ->  apps/examples/chatui
board/contest_board  ->  vendor/espressif/boards/esp32p4/esp32p4-function-ev-board
```

板级目录的目标路径与 defconfig 中 `CONFIG_ARCH_BOARD_CUSTOM_DIR` 完全一致，
同步后不需要改任何路径。

## 运行方式

### 1. 获取工作树

```bash
repo init -u https://github.com/open-vela/contest2026_412_SuperBen \
  -b dev-ai-contest-2026 -m contest2026_412_SuperBen.xml
repo sync -c -j8
```

同步后 `app/chatui/` 与 `board/contest_board/` 会由 linkfile 出现在工作树对应位置。

### 2. 应用公共仓改动

```bash
./tools/apply_overlay.sh /path/to/openvela
```

（或改用 `patches/`：`git apply patches/*.patch`）

### 3. 构建

```bash
# 在 openvela 工作树根目录（Ubuntu 24.04）
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

> 板卡为老版 EV board（串口挂外置 CH340），`--after hard_reset` 无效，
> 烧录完成后需**人工单按一次 RESET** 才会运行新固件。

### 5. 使用

```bash
# 1. 启动聊天界面（后台）
nsh> chatui &

# 2. 启动 AI 助手
nsh> ai_agent

# 3. 配置 MiMo（ASR/LLM/TTS 共用一个 key）
vela> set_llm mimo <your-api-key>

# 4. 对话（当前入口为文本命令）
vela> ask Hello, openvela
```

## 关键技术难点（已攻克）

1. **全新平台适配**：官方仅提供 esp32s3-eye 参考，ESP32-P4 需从零适配
   （启动、PSRAM、MIPI-DSI、触摸、音频、以太网）
2. **I2C「假成功」根因**：`esp_i2c.c` 中 `clock_t` 为无符号类型，
   `current - timeout < 0` 下溢后恒假，轮询等待循环从不执行却返回 OK；
   同一文件还有引脚属性掩码误用逻辑或（掩码由 171 塌缩为 1）的问题
3. **32MB PSRAM 未进堆**：`CONFIG_BUILD_FLAT` 下开 `MM_KERNEL_HEAP` 会让 PSRAM
   既不做主堆也不进用户堆
4. **PSRAM 200MHz 临界不稳定**：rev v3.2 上随机启动失败，降为 80MHz
5. **EMAC 启动崩溃**：`esp_timer_early_init()` 在 NuttX 下无人调用
   （`ESP_SYSTEM_INIT_FN` 框架不运行），systimer 未初始化
6. **mbedtls 双 fork 冲突**：修复构建系统 include 路径优先级
7. **看门狗假超时**：证书校验改系统墙钟导致计时爆炸，改用 monotonic 时钟
8. **DSI 屏幕撕裂**：面板 fb 单缓冲且未回写缓存；改双缓冲 + `pandisplay`。
   残余横条的真实根因是杜邦线供电纹波，改 USB 供电后消失

## 关于 AI Coding 日志

`logs/` 已按官方要求归集并提交。需要如实说明：本作品的主要开发过程
（板级 bringup、驱动排障、云端语音后端、界面联调）并非全部在官方白名单
工具（Claude Code / AIoT-IDE / OpenCode / Codex）中完成，因此
`logs/` 中可用会话数量少于实际工时。仓内保留的是采集器实际落盘的记录，
未做任何人工增补或修改。`validate-log.py` 可通过合规性校验。

AI 相关产出另有一个可复用的沉淀：`.claude/skills/embedded-false-success-debug/`
（嵌入式驱动「假成功」类故障排查方法论，即本作品 I2C 根因排查过程的方法化）。

## License

Apache 2.0
