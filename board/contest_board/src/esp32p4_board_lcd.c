/****************************************************************************
 * vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/src/esp32p4_board_lcd.c
 *
 * openvela contest port: ST7789 LCD board driver skeleton.
 * SPI hooks (esp_spi2_cmddata/select) are provided by ../common/src/esp_board_spi.c.
 * GPIO pins are CONFIG placeholders - fill actual pins once the board arrives.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdbool.h>
#include <debug.h>
#include <errno.h>

#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/spi/spi.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/lcd/st7789.h>

#include <arch/board/board.h>

#include "esp_spi.h"
#include "esp32p4-function-ev-board.h"

#ifdef CONFIG_ESP32P4_LCD

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* ST7789 SPI port - TODO: fill real pins (DC/BCKL) from P4X board schematic */
#ifndef CONFIG_ESP32P4_LCD_SPI_PORT
#  define CONFIG_ESP32P4_LCD_SPI_PORT 2
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct spi_dev_s *g_spidev;
static struct lcd_dev_s *g_lcd;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_lcd_initialize
 ****************************************************************************/

int board_lcd_initialize(void)
{
  /* TODO: configure backlight GPIO (esp_configgpio/esp_gpiowrite) once
   *       real pin is known from P4X board schematic.
   */

  /* Initialize SPI bus for LCD (port from CONFIG, default SPI2) */

  g_spidev = esp_spibus_initialize(CONFIG_ESP32P4_LCD_SPI_PORT);
  if (g_spidev == NULL)
    {
      lcderr("ERROR: Failed to initialize SPI port %d\n",
             CONFIG_ESP32P4_LCD_SPI_PORT);
      return -ENODEV;
    }

  /* Initialize ST7789 (devid 0) */

  g_lcd = st7789_lcdinitialize(g_spidev);
  if (g_lcd == NULL)
    {
      lcderr("ERROR: Failed to initialize ST7789\n");
      return -ENODEV;
    }

  return OK;
}

/****************************************************************************
 * Name: board_lcd_getdev
 ****************************************************************************/

FAR struct lcd_dev_s *board_lcd_getdev(int devno)
{
  return g_lcd;
}

/****************************************************************************
 * Name: board_lcd_uninitialize
 ****************************************************************************/

void board_lcd_uninitialize(void)
{
  if (g_spidev != NULL)
    {
      /* Release the SPI bus */

      esp_spibus_uninitialize(g_spidev);
      g_spidev = NULL;
    }

  g_lcd = NULL;
}

#endif /* CONFIG_ESP32P4_LCD */
