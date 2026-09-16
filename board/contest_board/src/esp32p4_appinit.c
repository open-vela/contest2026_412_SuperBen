/****************************************************************************
 * vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/src/esp32p4_appinit.c
 *
 * openvela contest port: board_app_initialize for boardctl() support
 * (NuttX esp32p4 does not provide one; openvela boardctl requires it)
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <nuttx/board.h>

#include "esp32p4-function-ev-board.h"

#ifdef CONFIG_BOARDCTL

/****************************************************************************
 * Name: board_app_initialize
 *
 * Description:
 *   Perform application specific initialization via boardctl(BOARDIOC_INIT).
 *
 ****************************************************************************/

int board_app_initialize(uintptr_t arg)
{
#ifdef CONFIG_BOARD_LATE_INITIALIZE
  return OK;
#else
  return esp_bringup();
#endif
}

#endif /* CONFIG_BOARDCTL */
