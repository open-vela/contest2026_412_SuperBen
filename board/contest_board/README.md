# ESP32-P4 Function EV Board — openvela BSP

本目录是作品 **VelaVoice** 的板级支持包（BSP），为 ESP32-P4-Function-EV-Board
提供 openvela / NuttX 的完整适配。

通过 `contest2026_412_SuperBen.xml` 的 `<linkfile>` 映射到工作树位置：

```
board/contest_board  ->  vendor/espressif/boards/esp32p4/esp32p4-function-ev-board
```

该目标路径与构建配置中的 `CONFIG_ARCH_BOARD_CUSTOM_DIR` 一致，同步后无需改路径。

## 内容

```
board/contest_board/
├── CMakeLists.txt                     # 板级 CMake 入口（含 CMakeLists 覆盖修正）
├── Kconfig                            # ESP32P4_LCD 等板级配置项
├── include/
│   └── board.h                        # 板级引脚与设备定义
├── scripts/
│   └── Make.defs                      # 板级 make 片段
├── src/
│   ├── CMakeLists.txt                 # 源文件与 include 路径
│   ├── Make.defs
│   ├── esp32p4-function-ev-board.h    # 板级头文件
│   ├── esp32p4_appinit.c              # board_app_initialize()（openvela boardctl 需要）
│   ├── esp32p4_boot.c                 # 启动初始化
│   ├── esp32p4_bringup.c              # 外设 bring-up（I2C / 音频 / tmpfs / 功放使能）
│   ├── esp32p4_board_lcd.c            # 板级 LCD 封装
│   ├── esp32p4_ek79007_panel.c        # EK79007AD MIPI-DSI 面板驱动（含双缓冲 + pandisplay）
│   ├── esp32p4_touch.c                # GT911 触摸板级封装（I2C0, 轮询模式）
│   ├── esp32p4_es8311.c               # ES8311 codec 板级初始化
│   ├── esp32p4_ethernet.c             # EMAC + 通用 PHY 初始化
│   ├── esp32p4_gpio.c                 # GPIO 扩展
│   ├── esp32p4_buttons.c              # 按键
│   └── esp32p4_reset.c                # 复位
└── configs/
    ├── ai_agent/defconfig             # 参赛主配置（AI 语音助手 + chatui）
    └── nsh/defconfig                  # nsh 基础配置
```

## 引脚（ESP32-P4 Function EV Board）

| 外设 | 接口 | 引脚 / 地址 |
|---|---|---|
| LCD EK79007AD | MIPI-DSI 2-lane @1000Mbps | 1024×600 RGB565；背光 GPIO26；复位 GPIO27 |
| 触摸 GT911 | I2C0 | SCL=GPIO8, SDA=GPIO7, 地址 0x5D，轮询模式（INT/RESET 未接） |
| 音频 ES8311 | I2C0 @0x18 | SCL=GPIO8, SDA=GPIO7 |
| I2S0 | — | BCLK=12, MCLK=13, WS=10, DOUT=9, DIN=11 |
| 功放 NS4150B | PA_CTRL | GPIO53 |
| 以太网 EMAC + IP101 | RMII | PHY 地址 1, MDC=31, MDIO=52, RMII_CLK=50 |
| 调试串口 | UART0 | TX=GPIO37, RX=GPIO38 @115200 |

## 构建

```bash
# 在 openvela 工作树根目录，同步（repo sync）之后
lunch vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/ai_agent \
      cmake_out/esp32p4-function-ev-board_nsh
cd cmake_out/esp32p4-function-ev-board_nsh && cmake . && m -j8
# 产物
# cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin
```

烧录：

```bash
esptool -p /dev/ttyUSB0 -b 460800 write-flash 0x2000 \
  cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin
```

> 板卡为老版 EV board（外置 CH340），`--after hard_reset` 无效，
> 烧录完成后需人工单按一次 RESET 才会运行新固件。

## 适配难点（均已真机验证）

1. **I2C 引脚属性掩码塌缩** — `esp_i2c.c` 中 `SCL/SDA_PIN_ATTR` 误用逻辑或，
   掩码由应有的 `171 (0xab)` 塌缩为 `1`，引脚退化为纯输入 GPIO。
   修复见 `patches/` 与 `overlay/`。
2. **I2C 轮询超时恒不生效** — `clock_t` 无符号导致 `current - timeout < 0` 恒假，
   等待循环从不执行、却返回成功。同一处修复。
3. **32MB PSRAM 未进堆** — `CONFIG_BUILD_FLAT` 下开 `MM_KERNEL_HEAP` 会让 PSRAM
   既不做主堆也不进用户堆，全部掉进 256KB 内核堆；需关 `MM_KERNEL_HEAP`
   并把 `MM_REGIONS` 保持为 2。
4. **PSRAM 200MHz 临界不稳定** — rev v3.2 上会随机启动失败，降为
   `CONFIG_ESPRESSIF_SPIRAM_SPEED_80M`。
5. **EMAC 启动崩溃** — `esp_timer_early_init()` 在 NuttX 下无人调用
   （`ESP_SYSTEM_INIT_FN` 框架不运行），systimer 未初始化导致 ROM 空指针。
6. **屏幕撕裂** — 面板 fb 单缓冲 + 未回写缓存；改为双缓冲 + `pandisplay`。
   残余横条的真实根因是杜邦线供电纹波，改用 USB 供电后消失。
