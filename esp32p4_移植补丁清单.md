# ESP32-P4X 移植补丁清单（v1.0）

> openvela 大赛 · 新硬件适配赛道 · 阶段 3 交付物②
> 日期：2026-08-21 ｜ 状态：编译验证通过（nuttx.bin 268KB）
> 目标板：ESP32-P4X-Function-EV-Board ｜ 配置：configs/nsh
> 本文档记录从"openvela 无 esp32p4"到"固件编译成功"的全部修改，供复现与正式提交。

---

## 〇、总体说明

- **修改范围**：openvela 公共仓（nuttx）的本地修改 + vendor/espressif 板级新增 + esp-hal-3rdparty 本地镜像。
- **合规边界**：公共仓修改用于本地编译验证；正式提交时按官方路径组织（芯片层 PR 到 openvela nuttx 的 dev-ai-contest-2026 分支，板级进专属仓 / vendor_espressif）。
- **编译命令（复现）**：

```bash
cd ~/openvela
export ESP_HAL_3RDPARTY_URL=/home/aurora/esp-hal-3rdparty-src   # 本地 esp-hal 镜像
export ESP_HAL_3RDPARTY_VERSION=ccc41de                          # 含 4 处 openvela 适配修改
export ESP_HAL_3RDPARTY_LOCAL_MBEDTLS_DIR=/home/aurora/mbedtls-src/mbedtls
./build.sh vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/nsh --cmake -j8
```

---

## 一、Kconfig 注册（nuttx/arch/risc-v/Kconfig）

| # | 修改 | 原因 | 官方处理 |
|---|---|---|---|
| 1 | 新增 `config ARCH_CHIP_ESP32P4` 块（采用 NuttX 主线完整 select 列表：ARCH_RV32 / ISA_M_A_C / VECNOTIRQ / BOOTLOADER / I2CRESET / MPU / RESET / RNG / TICKLESS / **MULTICPU** / 全套 LIBC_ARCH_* / RAMFUNCS / ONESHOT_COUNT / **ARCH_MINIMAL_VECTORTABLE**） | 注册 esp32p4 芯片；**漏 select ARCH_MINIMAL_VECTORTABLE 会导致 CONFIG_ARCH_NUSER_INTERRUPTS 不生成**（DYNAMIC 依赖链） | PR 上游 |
| 2 | `default "esp32p4" if ARCH_CHIP_ESP32P4` | 芯片目录名映射 | PR 上游 |
| 3 | `if ARCH_CHIP_ESP32P4` + `source "arch/risc-v/src/esp32p4/Kconfig"` + `endif` | 加载芯片 Kconfig；⚠️ **初版插错位置（嵌套进 ESP32H2 块内导致被屏蔽）**，须放在 H2 块 endif 之后 | PR 上游 |

## 二、芯片层（nuttx/arch/risc-v/src/esp32p4/）

| # | 修改 | 原因 | 官方处理 |
|---|---|---|---|
| 4 | 从 NuttX 主线拷入 6 文件：CMakeLists.txt / Kconfig / Make.defs / esp_chip_rev.c / hal_esp32p4.cmake / hal_esp32p4.mk | openvela 无 esp32p4 芯片层 | PR 上游 |
| 5 | `include/esp32p4/` 头文件（chip.h 等） | 芯片寄存器/IRQ 定义 | PR 上游 |
| 6 | esp32p4/CMakeLists.txt：开头加 `if(NOT DEFINED NUTTX_BINARY_DIR) set(NUTTX_BINARY_DIR ${CMAKE_BINARY_DIR}) endif()` | espressif CMake 依赖 NUTTX_BINARY_DIR，openvela 未定义 → 路径拼接出 `/include` | PR 上游 |
| 7 | 新增 `esp32p4_usleep_shim.c`：`nxsched_usleep → nxsig_usleep` | esp-hal 平台层用 NuttX 新 API nxsched_usleep，openvela 只有 nxsig_usleep | 上游修复后移除 |

## 三、common/espressif 共享层（nuttx/arch/risc-v/src/common/espressif/）

| # | 修改 | 原因 | 官方处理 |
|---|---|---|---|
| 8 | 从 NuttX 主线拉齐 **129 个文件**（esp_irq.c / esp_lowputc.c / esp_gpio.c / Bootloader.* 等） | openvela 该目录仅 3 文件（Make.defs/Kconfig/CMakeLists），缺芯片层核心实现（os.c 依赖的 esp_setup_irq_with_flags_intrstatus 等） | PR 上游（按需） |
| 9 | Kconfig 替换为 NuttX 主线版（4894 行） | 旧版 choice 无 ESP32P4 选项 → CONFIG_ESPRESSIF_CHIP_SERIES 不生成 → CHIP_SERIES 空 | PR 上游 |
| 10 | CMakeLists.txt 替换为 NuttX 主线版 + **mbedtls 本地镜像补丁**（ENV `ESP_HAL_3RDPARTY_LOCAL_MBEDTLS_DIR` 时用 file(COPY) 替代 git submodule） | ① openvela 无此文件；② mbedtls submodule 从 GitHub 拉取失败（网络受限） | 官方环境可访问 GitHub，保留原始 submodule 逻辑 |

## 四、构建系统适配（nuttx/ 公共部分）

| # | 修改 | 原因 | 官方处理 |
|---|---|---|---|
| 11 | `cmake/nuttx_toolchain.cmake` L97：`-I${CMAKE_BINARY_DIR}/include` → `-I${NUTTX_BINARY_DIR}/include` | openvela 构建中 CMAKE_BINARY_DIR 展开异常（生成 `-I/include`）→ nuttx/config.h 找不到 | 与主线对齐（主线已用 NUTTX_BINARY_DIR） |
| 12 | `nuttx/CMakeLists.txt`：`set(NUTTX_BINARY_DIR ${CMAKE_BINARY_DIR})`（if not defined） | 配合 #11 | 与主线对齐 |
| 13 | `nuttx/CMakeLists.txt`：LD_SCRIPT 处理改为 **多脚本循环**（foreach + 每个 `-T` 前缀，NuttX 主线逻辑）；链接处 `${LINK_OPTION_FLAG}${ldscript}` → `${ldscript}` | openvela 仅支持单 LD_SCRIPT → esp32p4 的 sections/ROM ld 丢失 → memcpy/符号未定义 | 与主线对齐 |
| 14 | 新增 `include/nuttx/debug.h` 兼容转发头（`#include <debug.h>`） | esp-hal 用新式 `<nuttx/debug.h>`，openvela 是旧式 `<debug.h>` | 上游对齐后移除 |

## 五、板级（vendor/espressif/boards/esp32p4/）

| # | 修改 | 原因 | 官方处理 |
|---|---|---|---|
| 15 | 骨架目录：esp32p4-function-ev-board/（configs / scripts / src / include）+ common/（scripts/*.ld + src/esp_board_*.c + include/*.h） | 从 NuttX 主线 esp32p4 板级拷贝（target 板是官方支持的） | 专属仓/vendor_espressif |
| 16 | 板级根 CMakeLists.txt：`add_subdirectory(src)` + `target_include_directories(board ... common/include)`；**不含 LD_SCRIPT 设置** | ① openvela 需要根 CMakeLists（esp32s3 缺它编不过）；② common/include 供 bringup.c 头文件；③ **set_property(LD_SCRIPT) 会覆盖 src/CMakeLists 与 hal 累积的脚本 → 必须删除** | 专属仓 |
| 17 | 新增 `src/esp32p4_appinit.c`：`board_app_initialize → esp_bringup()` | openvela boardctl.c 需要 board_app_initialize；NuttX esp32p4 未提供（缺失） | 专属仓 |
| 18 | defconfig 改造：custom 三件套（ARCH_BOARD_CUSTOM=y / CUSTOM_DIR / CUSTOM_NAME / RELPATH）+ ESPRESSIF_CHIP_SERIES="esp32p4" + ONESHOT + ONESHOT_COUNT + MM_KERNEL_HEAP | ① openvela 板级外挂机制；② CHIP_SERIES 显式指定（Kconfig 默认未生效）；③ ONESHOT 供 riscv_mtimer（oneshot_operations_s 的 start_absolute 在 CONFIG_ONESHOT_COUNT 下）；④ kmm_malloc 供 esp_libc_stubs | 专属仓 |

## 六、esp-hal-3rdparty 本地镜像（~/esp-hal-3rdparty-src，HEAD=ccc41de）

> 背景：esp32p4 依赖 esp-hal-3rdparty，FetchContent 默认从 GitHub 拉取（网络受限）→ 本地镜像 + 环境变量覆盖 URL/VERSION。以下 4 处为 openvela 适配修改（基于 8d0a898）：

| # | 修改 | 原因 | 官方处理 |
|---|---|---|---|
| 19a | `.gitmodules`：mbedtls URL 相对路径 → 绝对 `https://github.com/espressif/mbedtls.git` | 相对路径在本地 clone 时解析成本地路径导致 submodule 拉取失败 | 官方环境不受影响（GitHub 可达） |
| 19b | `nuttx/src/platform/os.c`：加 `#include <fcntl.h>`；`nxtask_init` 调用改为 openvela 旧签名（7 参） | NuttX API 版本差异（O_RDWR/F_GETFL 未声明；新版 9 参 vs openvela 旧版 7 参） | 上游对齐后还原 |
| 19c | `components/esp_hw_support/esp_gpio_reserve.c`：`_Atomic uint64_t` 原子操作 → 普通读写 | RV32 无 64 位原子硬件支持，libgcc 无 __atomic_fetch_or_8 → 链接失败；单核 NSH 场景安全 | 上游方案（libatomic） |
| 19d | `nuttx/arch/risc-v/src/common/espressif/esp_lowputc.c`：去 nxmutex 用法 | openvela 无 nxmutex 外部实现（NOINLINE 未开但 inline 未内联产生外部引用） | 上游对齐后还原 |

## 七、工具补齐（nuttx/tools/espressif/）

| # | 修改 | 原因 | 官方处理 |
|---|---|---|---|
| 20 | 从 NuttX 主线拉 `espressif_mkimage.cmake` + `espressif_esptool_common.cmake` 等（sparse checkout tools/espressif 合并） | openvela 缺失（nuttx_post_build 引用但文件不存在） | PR 上游 |
| 21 | 安装 esptool：`pip3 install --break-system-packages esptool` + root symlink 到 /usr/local/bin | 镜像生成（elf2image）需要 esptool | 环境依赖，非代码 |

## 八、验证结果

```
✅ 编译：build completed successfully（nuttx.bin 268KB / nuttx.hex 644KB / ELF 594KB）
✅ 镜像：esptool v5.3.1 生成 ESP32-P4 镜像（--ram-only-header，SIMPLE_BOOT）
⏳ 烧录验证：待板子到手（预计 8/28）→ esptool write_flash → UART 看 openvela banner + nsh>
```

## 九、后续动作

1. 板子到手：烧录验证 → 确认启动日志 → 保底达成
2. 外设 demo：ST7789（板级驱动）+ MPU6050 / DHT11（I2C/单总线）
3. 正式提交：芯片层改动 PR 到 openvela nuttx dev-ai-contest-2026；板级进专属仓；本地 hack（#6/7/10/14/19）梳理为上游兼容方案
4. 本文档演进为《P4X 适配指南》→ 官方加分项

---

*清单维护：随移植进展更新；最终提交前核对每项的"官方处理"列。*

---

## 十、v1.1 增补（2026-08-26 复现会话）

> 背景：8/21 阶段 3 编译成功后，8/26 环境重做时 HAL 被替换为无补丁版本、mbedtls 子模块丢失，构建回退到链接失败（`__atomic_fetch_or_8`）。本会话将其恢复到等价状态。

| # | 操作 | 说明 | 官方处理 |
|---|---|---|---|
| 22 | HAL 重放为 commit `8d0a8989`（zip 整体替换，md5 与桌面 8d0a8989 包一致） | 恢复 §六 的补丁基座（ccc41de = 8d0a898 + 4 补丁，等价） | 复现说明 |
| 23 | mbedtls 子模块手动放置（commit `582ff482038db6e4010dbf6f943d97b05ad06ea5`，GitHub API 取自 8d0a8989 的 gitlink） | zip 不含子模块；`include/psa` 用**实体目录复制**（`cp -r tf-psa-crypto/include/psa include/psa`，22 文件）替代符号链接，规避解析问题 | 官方环境用 submodule；ENV `LOCAL_MBEDTLS_DIR` 仍可用（#19a/10） |
| 24 | os.c：重打 `#include <fcntl.h>` + `nxtask_init` 7 参（正则幂等，缩进不敏感）；os.h：`nxsched_usleep` 声明补一次 | 同 #19b / #7，防重放丢失 | 上游对齐后还原 |
| 25 | 64 位原子操作：新增 `components/esp_hw_support/esp_atomic_shim.c`（临界区实现 __atomic_fetch_or/and/add/sub/xor_8、load/store/exchange_8、compare_exchange_8）+ CMakeLists 追加 | **已改按 #19c 落地**：`esp_gpio_reserve.c` 整文件重写为普通读写（去掉 `_Atomic`/`atomic_*`），链接通过；shim 保留为兜底（因 CMake 未重配置未编入，无副作用） | 上游方案（libatomic） |
| 26 | 构建脚本 `fix_openvela_build.sh`（幂等、不删 cmake_out、双份补丁、克隆失败自动填充本地 HAL + stamp） | 根治"rm -rf cmake_out 冲掉补丁"的反复；已内置 #19c 重写步骤 | 复现工具 |

**当前状态（v1.1 收口）**：✅ **构建成功** —— `cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin`（280,580 字节 / 280KB，2026-08-26 19:12 生成；ELF 623KB / HEX 679KB）。补丁全量就位：os.c×2、os.h×1、esp_gpio_reserve.c 重写（#19c）、mbedtls/psa 实体目录、原子 shim（兜底）。产物已复制至 `nuttx_20260826.bin`。
**剩余**：烧录验证（esptool write_flash → 串口 banner + `nsh>`）→ 录屏证明 → demo → 提交。
