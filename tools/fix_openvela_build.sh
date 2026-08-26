#!/bin/bash
# =============================================================================
# fix_openvela_build.sh — 一次性修复 ESP32-P4X openvela 构建（基于存档排障结论）
# 适用：WSL2 Ubuntu 24.04，源码位于 ~/openvela，HAL 位于 ~/openvela/esp-hal-3rdparty
# 用法：cp /mnt/c/Users/Aurora/Desktop/fix_openvela_build.sh ~/ && bash ~/fix_openvela_build.sh
# 特性：幂等（可重复跑）、不删 cmake_out、双份打补丁、自动处理 mbedtls/原子shim
# =============================================================================
set -u

OPENVELA="$HOME/openvela"
HAL="$OPENVELA/esp-hal-3rdparty"
BOARD="vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/nsh"
OUT="$OPENVELA/cmake_out/esp32p4-function-ev-board_nsh"
TARGET="$OUT/arch/risc-v/src/common/espressif/esp-hal-3rdparty"
DEPS="$OUT/_deps/esp_hal_3rdparty-subbuild/esp_hal_3rdparty-populate-prefix/src/esp-hal-3rdparty"
STAMP="$OUT/_deps/esp_hal_3rdparty-subbuild/esp_hal_3rdparty-populate-prefix/src/esp_hal_3rdparty-populate-stamp"

[ -d "$HAL/nuttx" ] || { echo "❌ 未找到 $HAL，请先准备 esp-hal-3rdparty (commit 8d0a8989...)"; exit 1; }

echo "== [1/6] 修复源码 os.c / os.h（幂等） =="
python3 - "$HAL/nuttx/src/platform/os.c" "$HAL/nuttx/include/platform/os.h" <<'PY'
import re, sys
os_c, os_h = sys.argv[1], sys.argv[2]

# ---------- os.c ----------
s = open(os_c, encoding='utf-8', errors='ignore').read()
# 清掉上次残留的错误行
s = re.sub(r'^\s*tcb->priority\s*=\s*priority;\s*$', '', s, flags=re.M)
s = re.sub(r'(ret = nxtask_init\(tcb, name, task_wrapper_entry,\n\s*NULL, NULL, argv, NULL\);\n)\s*task_wrapper_entry, argv, NULL, NULL\);\n', r'\1', s)
# 替换原始 9 参调用为 openvela 7 参签名（正则对缩进不敏感）
pat = re.compile(r'\s*ret = nxtask_init\(tcb, name, priority,\s*\n'
                 r'\s*NULL, stack_size,\s*\n'
                 r'\s*task_wrapper_entry, argv, NULL, NULL\);')
new = ('tcb->init_priority = priority;\n'
       '  ret = nxtask_init(tcb, name, task_wrapper_entry,\n'
       '                    NULL, NULL, argv, NULL);')
s = pat.sub(new, s, count=1)
# 补 fcntl.h
if '#include <fcntl.h>' not in s:
    s = s.replace('#include <semaphore.h>', '#include <semaphore.h>\n#include <fcntl.h>', 1)
open(os_c, 'w', encoding='utf-8').write(s)
print('  os.c OK')

# ---------- os.h ----------
s = open(os_h, encoding='utf-8', errors='ignore').read()
lines = s.split('\n')
# 1) 修复被拆断的 "#include\n <xxx>" 两行（历史 bug 残留）
merged, i = [], 0
while i < len(lines):
    ln = lines[i]
    if re.match(r'^#include\s*$', ln) and i + 1 < len(lines) and re.match(r'^\s*<[^>]+>\s*$', lines[i + 1]):
        merged.append('#include ' + lines[i + 1].strip())
        i += 2
    else:
        merged.append(ln)
        i += 1
# 2) 删除历史声明行（防重复）
merged = [ln for ln in merged if not re.match(r'^\s*int nxsched_usleep\(unsigned int usec\);\s*$', ln)]
# 3) 在第一个完整 #include 行之后插入一次声明
out, inserted = [], False
for ln in merged:
    out.append(ln)
    if not inserted and re.match(r'^#include\s+<', ln):
        out.append('int nxsched_usleep(unsigned int usec);')
        inserted = True
open(os_h, 'w', encoding='utf-8').write('\n'.join(out))
print('  os.h OK')
PY

echo "== [2/6] 初始化 mbedtls 子模块并建立 psa 符号链接 =="
if [ ! -f "$HAL/components/mbedtls/mbedtls/tf-psa-crypto/include/psa/crypto.h" ]; then
    if [ -d "$HAL/.git" ]; then
        git -C "$HAL" submodule update --init components/mbedtls/mbedtls 2>&1 | tail -2 || true
    fi
fi
if [ -d "$HAL/components/mbedtls/mbedtls/tf-psa-crypto/include/psa" ] && [ ! -e "$HAL/components/mbedtls/mbedtls/include/psa" ]; then
    ln -sfn tf-psa-crypto/include/psa "$HAL/components/mbedtls/mbedtls/include/psa"
    echo "  psa 符号链接已建立"
fi
ls "$HAL/components/mbedtls/mbedtls/include/psa/crypto.h" >/dev/null 2>&1 \
    && echo "  psa/crypto.h 就绪" || echo "  ⚠️ psa/crypto.h 仍缺失（网络不行时需手动下载 mbedtls 子模块）"

echo "== [3/6] 生成 64 位原子操作 shim（一次补齐所有常用操作） =="
cat > "$HAL/components/esp_hw_support/esp_atomic_shim.c" <<'EOF'
/* esp_atomic_shim.c — rv32 工具链缺 64 位原子库函数，用临界区实现（openvela contest port） */
#include <nuttx/irq.h>
#include <stdint.h>
#include <stdbool.h>

#define ATOMIC64_FETCH_OP(NAME, OP)                                      \
uint64_t NAME(uint64_t *ptr, uint64_t val, int m)                         \
{                                                                         \
    irqstate_t f = enter_critical_section();                             \
    uint64_t o = *ptr; *ptr = o OP val;                                   \
    leave_critical_section(f); return o;                                 \
}

ATOMIC64_FETCH_OP(__atomic_fetch_or_8, |)
ATOMIC64_FETCH_OP(__atomic_fetch_and_8, &)
ATOMIC64_FETCH_OP(__atomic_fetch_add_8, +)
ATOMIC64_FETCH_OP(__atomic_fetch_sub_8, -)
ATOMIC64_FETCH_OP(__atomic_fetch_xor_8, ^)

uint64_t __atomic_load_8(const uint64_t *ptr, int m)
{
    irqstate_t f = enter_critical_section();
    uint64_t v = *ptr;
    leave_critical_section(f); return v;
}

void __atomic_store_8(uint64_t *ptr, uint64_t val, int m)
{
    irqstate_t f = enter_critical_section();
    *ptr = val;
    leave_critical_section(f);
}

uint64_t __atomic_exchange_8(uint64_t *ptr, uint64_t val, int m)
{
    irqstate_t f = enter_critical_section();
    uint64_t o = *ptr; *ptr = val;
    leave_critical_section(f); return o;
}

bool __atomic_compare_exchange_8(uint64_t *ptr, uint64_t *expected,
                                 uint64_t desired, int succ, int fail)
{
    irqstate_t f = enter_critical_section();
    uint64_t cur = *ptr;
    if (cur == *expected) { *ptr = desired; leave_critical_section(f); return true; }
    *expected = cur; leave_critical_section(f); return false;
}
EOF
# 加入 esp_hw_support 的编译列表
grep -q 'esp_atomic_shim.c' "$HAL/components/esp_hw_support/CMakeLists.txt" 2>/dev/null || \
    sed -i '/esp_gpio_reserve.c/a\    "esp_atomic_shim.c"' "$HAL/components/esp_hw_support/CMakeLists.txt"
echo "  esp_atomic_shim.c 已就绪"

echo "== [3.5/6] 应用 #19c：esp_gpio_reserve.c 去除 64 位原子依赖（已验证方案） =="
cat > "$HAL/components/esp_hw_support/esp_gpio_reserve.c" <<'EOF'
/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * openvela contest port (#19c): replace _Atomic uint64_t + atomic_* with
 * plain read-modify-write. RV32 toolchain has no 64-bit atomics (no
 * __atomic_fetch_or_8 in libgcc); single-core NSH is safe without them.
 */

#include "soc/soc_caps.h"
#include "esp_types.h"
#include "esp_bit_defs.h"
#include "esp_private/esp_gpio_reserve.h"

static uint64_t s_reserved_pin_mask = ~(SOC_GPIO_VALID_GPIO_MASK);

uint64_t esp_gpio_reserve(uint64_t gpio_mask)
{
    uint64_t old = s_reserved_pin_mask;
    s_reserved_pin_mask = old | gpio_mask;
    return old;
}

uint64_t esp_gpio_revoke(uint64_t gpio_mask)
{
    uint64_t old = s_reserved_pin_mask;
    s_reserved_pin_mask = old & ~gpio_mask;
    return old;
}

bool esp_gpio_is_reserved(uint64_t gpio_mask)
{
    return (s_reserved_pin_mask & gpio_mask) != 0;
}
EOF
echo "  esp_gpio_reserve.c 已重写（普通读写，单核安全）"

echo "== [4/6] 确保构建目录存在（首跑允许 FetchContent 失败并生成目录） =="
if [ ! -d "$OUT" ]; then
    echo "  首次配置构建（预计会因网络克隆失败，属正常）..."
    (cd "$OPENVELA" && ./build.sh "$BOARD" --cmake -j8) >/tmp/build_probe.log 2>&1 || true
    tail -3 /tmp/build_probe.log
fi

echo "== [5/6] 用本地已修复 HAL 填充 cmake_out 两份拷贝 =="
if [ ! -d "$DEPS" ]; then
    echo "  FetchContent 克隆失败 → 用本地 HAL 填充 _deps + 打 stamp"
    rm -rf "$DEPS"; mkdir -p "$DEPS"; cp -r "$HAL/." "$DEPS/"
    mkdir -p "$STAMP"
    touch "$STAMP/esp_hal_3rdparty-populate-download"
    touch "$STAMP/esp_hal_3rdparty-populate-update"
fi
if [ -d "$TARGET" ]; then
    echo "  整树同步已修复 HAL → $TARGET"
    rm -rf "$TARGET"; cp -r "$HAL/." "$TARGET/"
fi

echo "== [6/6] 构建（不删除 cmake_out，保留修复） =="
cd "$OPENVELA"
./build.sh "$BOARD" --cmake -j8 2>&1 | tail -25

echo
echo "================ 结果 ================"
if ls "$OUT/nuttx" >/dev/null 2>&1 || ls "$OUT/nuttx.bin" >/dev/null 2>&1; then
    echo "✅ 构建成功！固件位于: $OUT/nuttx.bin"
else
    echo "❌ 仍未成功。请把上面输出贴回来，并检查 /tmp/build_probe.log 与 psa/crypto.h 是否就绪。"
fi
