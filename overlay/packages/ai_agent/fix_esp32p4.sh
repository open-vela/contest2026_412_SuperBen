#!/bin/bash
# fix_esp32p4.sh - 把 fix_esp32s3.sh 的 4 个补丁移植到 ESP32-P4
#
# 用法（在首次 distclean 后的构建期间后台运行）：
#   bash packages/ai_agent/fix_esp32p4.sh &
#   ./build.sh esp32p4-function-ev-board:ai_agent
#
# 4 个补丁（都无法用 defconfig 表达）：
#   1. apps/crypto/mbedtls/Make.defs: -I -> -isystem（让 ESP-IDF 的 mbedtls 头优先，
#      因为 ESP-IDF 与 NuttX 的 cipher_info_t 等结构体布局不同）
#   2. ESP-IDF mbedtls_config.h: 关闭 MBEDTLS_CCM_C（CCM*-NO-TAG 结构体冲突）
#   3. ESP-IDF clk_ctrl_os.c: LOCK_INITIALIZER_UNLOCKED 0 -> SP_UNLOCKED
#      （NuttX 的 spinlock_t 是结构体，不是 int）
#   4. esp32p4_bringup.c: 挂载 tmpfs 到 /data（ai_agent 配置存储目录）

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
APPS_DIR="$ROOT_DIR/apps"

echo "[fix-p4] 等待 esp-hal-3rdparty 就绪..."

# 定位 mbedtls_config.h（P4 的 hal 目录与 s3 不同，用 find 兜底）
MBEDTLS_CFG=""
for i in $(seq 1 180); do
    MBEDTLS_CFG=$(find "$ROOT_DIR/nuttx/arch/risc-v/src/common/espressif" \
                       "$ROOT_DIR/nuttx/arch/risc-v/src/esp32p4" \
                       -path "*components/mbedtls/mbedtls/include/mbedtls/mbedtls_config.h" \
                       -print 2>/dev/null | head -1)
    if [ -n "$MBEDTLS_CFG" ] && grep -q "MBEDTLS_THREADING_C" "$MBEDTLS_CFG" 2>/dev/null; then
        break
    fi
    sleep 1
done

if [ -z "$MBEDTLS_CFG" ]; then
    echo "[fix-p4] WARN: 未找到 mbedtls_config.h（可能尚未下载 hal）"
fi

echo "[fix-p4] 应用补丁..."

# ---- Fix 1: NuttX mbedtls 头文件优先级 ----------------------
MAKEDEFS="$APPS_DIR/crypto/mbedtls/Make.defs"
if [ -f "$MAKEDEFS" ]; then
    sed -i 's|CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/include|CFLAGS += -isystem $(APPDIR)/crypto/mbedtls/include|' "$MAKEDEFS"
    sed -i 's|CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/mbedtls/include|CFLAGS += -isystem $(APPDIR)/crypto/mbedtls/mbedtls/include|' "$MAKEDEFS"
    sed -i 's|CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/include|CXXFLAGS += -isystem $(APPDIR)/crypto/mbedtls/include|' "$MAKEDEFS"
    sed -i 's|CXXFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/mbedtls/mbedtls/include|CXXFLAGS += -isystem $(APPDIR)/crypto/mbedtls/mbedtls/include|' "$MAKEDEFS"
    echo "  [1/4] Make.defs: -isystem 已应用"
else
    echo "  [1/4] SKIP: 未找到 $MAKEDEFS"
fi

# ---- Fix 2: 关闭 CCM ---------------------------------------
if [ -n "$MBEDTLS_CFG" ] && [ -f "$MBEDTLS_CFG" ]; then
    sed -i 's/^#define MBEDTLS_CCM_C$/\/\* #define MBEDTLS_CCM_C \*\//' "$MBEDTLS_CFG"
    echo "  [2/4] mbedtls_config.h: CCM 已关闭"
else
    echo "  [2/4] SKIP: mbedtls_config.h 不可用"
fi

# ---- Fix 3: spinlock 初始化宏 ------------------------------
CLK_FILE=$(find "$ROOT_DIR/nuttx/arch/risc-v/src" -name "clk_ctrl_os.c" 2>/dev/null | head -1)
if [ -n "$CLK_FILE" ] && [ -f "$CLK_FILE" ]; then
    sed -i 's/#define LOCK_INITIALIZER_UNLOCKED       0/#define LOCK_INITIALIZER_UNLOCKED       SP_UNLOCKED/' "$CLK_FILE"
    echo "  [3/4] clk_ctrl_os.c: spinlock 已修正"
else
    echo "  [3/4] SKIP: 未找到 clk_ctrl_os.c"
fi

# ---- Fix 4: /data tmpfs 挂载 -------------------------------
BRINGUP="$ROOT_DIR/vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/src/esp32p4_bringup.c"
if [ -f "$BRINGUP" ]; then
    if ! grep -q 'mount tmpfs at /data' "$BRINGUP" 2>/dev/null; then
        python3 - "$BRINGUP" << 'PYEOF'
import sys
path = sys.argv[1]
with open(path) as f:
    content = f.read()

# 在 CONFIG_LIBC_TMPDIR 的 tmpfs 挂载之后追加 /data 挂载
marker = 'CONFIG_LIBC_TMPDIR'
idx = content.find(marker)
if idx < 0:
    print('pattern not found')
    sys.exit(0)

# 找到该 mount 块的 #endif
end = content.find('#endif', idx)
if end < 0:
    print('no endif found')
    sys.exit(0)

insert = '''
  /* Mount tmpfs at /data for ai_agent config store */

  ret = nx_mount(NULL, "/data", "tmpfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to mount tmpfs at /data: %d\\n", ret);
    }
'''
content = content[:end] + insert + content[end:]
with open(path, 'w') as f:
    f.write(content)
print('ok')
PYEOF
        echo "  [4/4] esp32p4_bringup.c: /data tmpfs 挂载已加入"
    else
        echo "  [4/4] esp32p4_bringup.c: /data 挂载已存在"
    fi
else
    echo "  [4/4] SKIP: 未找到 $BRINGUP"
fi

echo "[fix-p4] 全部补丁应用完毕。"
