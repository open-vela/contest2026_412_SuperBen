/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_bringup.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/debug.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <nuttx/fs/fs.h>

#include "esp_board_ledc.h"
#include "esp_board_spiflash.h"
#include "espressif/esp_gpio.h"
#include <arch/chip/gpio_sig_map.h>
#include "esp_board_i2c.h"
#include "esp_board_bmp180.h"

#include "espressif/esp_start.h"

#ifdef CONFIG_MOTOR_SA8339
#  include <nuttx/i2c/i2c_master.h>
#  include <nuttx/motor/sa8339.h>
#  include "espressif/esp_i2c.h"
#endif

#ifdef CONFIG_ESPRESSIF_MIPI_CSI
#  include "esp_mipi_csi.h"
#endif

#ifdef CONFIG_WATCHDOG
#  include "espressif/esp_wdt.h"
#endif

#ifdef CONFIG_TIMER
#  include "espressif/esp_gptimer.h"
#endif

#ifdef CONFIG_ONESHOT
#  include "espressif/esp_oneshot.h"
#endif

#ifdef CONFIG_RTC_DRIVER
#  include "espressif/esp_rtc.h"
#endif

#ifdef CONFIG_DEV_GPIO
#  include "espressif/esp_gpio.h"
#endif

#ifdef CONFIG_INPUT_BUTTONS
#  include <nuttx/input/buttons.h>
#endif

#ifdef CONFIG_ESPRESSIF_EFUSE
#  include "espressif/esp_efuse.h"
#endif

#ifdef CONFIG_ESP_RMT
#  include "esp_board_rmt.h"
#endif

#ifdef CONFIG_ESPRESSIF_I2S0
#  include "esp_board_i2s.h"
#endif

#ifdef CONFIG_ESPRESSIF_SPI
#  include "espressif/esp_spi.h"
#  include "esp_board_spidev.h"
#  ifdef CONFIG_ESPRESSIF_SPI_BITBANG
#    include "espressif/esp_spi_bitbang.h"
#  endif
#endif

#ifdef CONFIG_SPI_SLAVE_DRIVER
#  include "espressif/esp_spi.h"
#  include "esp_board_spislavedev.h"
#endif

#ifdef CONFIG_ESPRESSIF_TEMP
#  include "espressif/esp_temperature_sensor.h"
#endif

#ifdef CONFIG_ESP_MCPWM
#  include "esp_board_mcpwm.h"
#endif

#ifdef CONFIG_ESP_PCNT
#  include "espressif/esp_pcnt.h"
#  include "esp_board_pcnt.h"
#endif

#ifdef CONFIG_ESPRESSIF_ADC
#  include "esp_board_adc.h"
#endif

#ifdef CONFIG_PM
#  include "espressif/esp_pm.h"
#endif

#ifdef CONFIG_SYSTEM_NXDIAG_ESPRESSIF_CHIP_WO_TOOL
#  include "espressif/esp_nxdiag.h"
#endif

#ifdef CONFIG_ESP_SDM
#  include "espressif/esp_sdm.h"
#endif

#ifdef CONFIG_COMP
#  include "espressif/esp_ana_cmpr.h"
#endif

#ifdef CONFIG_ESPRESSIF_USE_LP_CORE
#  include "espressif/esp_ulp.h"
#  ifdef CONFIG_ESPRESSIF_ULP_USE_TEST_BIN
#    include "ulp/ulp_code.h"
#  endif
#  ifdef CONFIG_ESPRESSIF_LP_MAILBOX
#    include "espressif/esp_lp_mailbox.h"
#  endif
#endif

#include "esp32p4-function-ev-board.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_bringup
 *
 * Description:
 *   Perform architecture-specific initialization.
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; A negated errno value is returned on
 *   any failure.
 *
 ****************************************************************************/

int esp_bringup(void)
{
  int ret = OK;

#ifdef CONFIG_ESP32P4_LCD
  /* Initialize ST7789 LCD (SPI) */

  ret = board_lcd_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize LCD: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESP32P4_LCD_EK79007
  /* Initialize EK79007 MIPI-DSI LCD (7 inch, 1024x600) */

  ret = esp32p4_ek79007_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize EK79007 DSI LCD: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_MIPI_CSI
  /* Initialize MIPI-CSI capture (SC2336 RX, /dev/csi0).
   * VDD_MIPI_DPHY (LDO3 2.5 V) is already powered above by the EK79007
   * panel init; both MIPI PHYs share the same analog supply rail. */

  ret = esp_mipi_csi_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize MIPI-CSI: %d\n", ret);
    }
#endif

#ifdef CONFIG_FS_PROCFS
  /* Mount the procfs file system */

  ret = nx_mount(NULL, "/proc", "procfs", 0, NULL);
  if (ret < 0)
    {
      _err("Failed to mount procfs at /proc: %d\n", ret);
    }
#endif

#ifdef CONFIG_FS_TMPFS
  /* Mount the tmpfs file system */

  ret = nx_mount(NULL, CONFIG_LIBC_TMPDIR, "tmpfs", 0, NULL);
  if (ret < 0)
    {
      _err("Failed to mount tmpfs at %s: %d\n", CONFIG_LIBC_TMPDIR, ret);
    }

  /* Mount tmpfs at /data for ai_agent config store */

  ret = nx_mount(NULL, "/data", "tmpfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to mount tmpfs at /data: %d\n", ret);
    }
#endif

#if defined(CONFIG_ESPRESSIF_EFUSE)
  ret = esp_efuse_initialize("/dev/efuse");
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to init EFUSE: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_MWDT0
  ret = esp_wdt_initialize("/dev/watchdog0", ESP_WDT_MWDT0);
  if (ret < 0)
    {
      _err("Failed to initialize WDT: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_MWDT1
  ret = esp_wdt_initialize("/dev/watchdog1", ESP_WDT_MWDT1);
  if (ret < 0)
    {
      _err("Failed to initialize WDT: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_RWDT
  ret = esp_wdt_initialize("/dev/watchdog2", ESP_WDT_RWDT);
  if (ret < 0)
    {
      _err("Failed to initialize WDT: %d\n", ret);
    }
#endif

#ifdef CONFIG_TIMER
  ret = esp_timer_initialize(0);
  if (ret < 0)
    {
      _err("Failed to initialize Timer 0: %d\n", ret);
    }

#ifndef CONFIG_ONESHOT
  ret = esp_timer_initialize(1);
  if (ret < 0)
    {
      _err("Failed to initialize Timer 1: %d\n", ret);
    }
#endif
#endif

#ifdef CONFIG_ONESHOT
  ret = esp_oneshot_initialize();
  if (ret < 0)
    {
      _err("Failed to initialize Oneshot Timer: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESP_RMT
  ret = board_rmt_txinitialize(RMT_OUTPUT_PIN);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_rmt_txinitialize() failed: %d\n", ret);
    }

  ret = board_rmt_rxinitialize(RMT_INPUT_PIN);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_rmt_txinitialize() failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_RTC_DRIVER
  /* Initialize the RTC driver */

  ret = esp_rtc_driverinit();
  if (ret < 0)
    {
      _err("Failed to initialize the RTC driver: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_SPI
#  if defined(CONFIG_ESPRESSIF_SPI2_SLAVE) && defined(CONFIG_ESPRESSIF_SPI2)
  ret = board_spislavedev_initialize(ESPRESSIF_SPI2);
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize SPI%d Slave driver: %d\n",
             ESPRESSIF_SPI2, ret);
    }
#  elif defined(CONFIG_ESPRESSIF_SPI2)
  ret = board_spidev_initialize(ESPRESSIF_SPI2);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to init spidev 2: %d\n", ret);
    }
#  endif

#  if defined(CONFIG_ESPRESSIF_SPI3_SLAVE) && defined(CONFIG_ESPRESSIF_SPI3)
  ret = board_spislavedev_initialize(ESPRESSIF_SPI3);
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize SPI%d Slave driver: %d\n",
             ESPRESSIF_SPI3, ret);
    }
#  elif defined(CONFIG_ESPRESSIF_SPI3)
  ret = board_spidev_initialize(ESPRESSIF_SPI3);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to init spidev 3: %d\n", ret);
    }
#  endif

#  ifdef CONFIG_ESPRESSIF_SPI_BITBANG
  ret = board_spidev_initialize(ESPRESSIF_SPI_BITBANG);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to init spidev 3: %d\n", ret);
    }
#  endif /* CONFIG_ESPRESSIF_SPI_BITBANG */

#  ifdef CONFIG_ESPRESSIF_LPSPI0
  ret = board_spidev_initialize(ESPRESSIF_LPSPI0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to init lpspi: %d\n", ret);
    }
#  endif
#endif /* CONFIG_ESPRESSIF_SPI */

#ifdef CONFIG_ESPRESSIF_SPIFLASH
  ret = board_spiflash_init();
  if (ret)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize SPI Flash\n");
    }
#endif


#if defined(CONFIG_I2C_DRIVER)
  /* Configure I2C peripheral interfaces */

  ret = board_i2c_init();

  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize I2C driver: %d\n", ret);
    }
#endif

/* ES8311 codec must be initialized AFTER board_i2c_init():
 * it internally calls esp_i2cbus_initialize(0) and would otherwise
 * steal I2C0 and make ES8311/GT911 fail with -19 (ENODEV).
 */
#if defined(CONFIG_ESPRESSIF_I2S0)
#ifdef CONFIG_AUDIO_ES8311
  ret = board_es8311_initialize(0, 0x18, 400000, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize ES8311: %d\n", ret);
    }

  /* Enable on-board amplifier NS4150B (PA_CTRL = GPIO53, active high) */
  esp_gpio_matrix_out(53, SIG_GPIO_OUT_IDX, false, false);
  esp_configgpio(53, OUTPUT | PULLUP);
  esp_gpiowrite(53, true);
  syslog(LOG_INFO, "PA_CTRL GPIO53 -> HIGH (amp enabled)\n");
#endif
#endif


#ifdef CONFIG_INPUT_GT9XX
  /* Initialize GT911 capacitive touchscreen on I2C0 (polling, /dev/input0) */

  syslog(LOG_INFO, "BRING[probe] -> board_touchscreen_initialize\n");
  ret = board_touchscreen_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize GT911 touch: %d\n", ret);
    }
#endif
#ifdef CONFIG_SENSORS_BMP180
  /* Try to register BMP180 device in I2C0 */

  syslog(LOG_INFO, "BRING[probe] -> board_bmp180_initialize\n");
  ret = board_bmp180_initialize(0);

  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize BMP180 "
             "Driver for I2C0: %d\n", ret);
    }
#endif

#ifdef CONFIG_MOTOR_SA8339
  /* Register SA8339 4-channel motor driver on the configured I2C bus */

  {
    FAR struct i2c_master_s *i2c;

    i2c = esp_i2cbus_initialize(CONFIG_MOTOR_SA8339_I2C_BUS);
    if (i2c == NULL)
      {
        syslog(LOG_ERR, "Failed to get I2C%d interface for SA8339\n",
               CONFIG_MOTOR_SA8339_I2C_BUS);
      }
    else
      {
        syslog(LOG_INFO, "BRING[probe] -> sa8339_register\n");
        ret = sa8339_register("/dev/sa8339", i2c,
                              CONFIG_MOTOR_SA8339_I2C_ADDR);
        if (ret < 0)
          {
            syslog(LOG_ERR, "Failed to register SA8339 driver: %d\n", ret);
          }
      }
  }
#endif

#ifdef CONFIG_ESP_SDM
  struct esp_sdm_chan_config_s config =
  {
    .gpio_num = 5,
    .sample_rate_hz = 1000 * 1000,
    .flags = 0,
  };

  struct dac_dev_s *dev = esp_sdminitialize(config);
  syslog(LOG_INFO, "BRING[probe] -> dac_register\n");
  ret = dac_register("/dev/dac0", dev);
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize DAC driver: %d\n",
             ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_TEMP
  struct esp_temp_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG(10, 50);
  syslog(LOG_INFO, "BRING[probe] -> esp_temperature_sensor_initialize\n");
  ret = esp_temperature_sensor_initialize(cfg);
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize temperature sensor driver: %d\n",
             ret);
    }
#endif
#ifdef CONFIG_ESPRESSIF_TWAI0

  /* Initialize TWAI and register the TWAI driver. */

  syslog(LOG_INFO, "BRING[probe] -> board_twai_setup\n");
  ret = board_twai_setup(0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: TWAI0 board_twai_setup failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_TWAI1

  /* Initialize TWAI and register the TWAI driver. */

  syslog(LOG_INFO, "BRING[probe] -> board_twai_setup\n");
  ret = board_twai_setup(1);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: TWAI1 board_twai_setup failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_TWAI2

  /* Initialize TWAI and register the TWAI driver. */

  syslog(LOG_INFO, "BRING[probe] -> board_twai_setup\n");
  ret = board_twai_setup(2);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: TWAI2 board_twai_setup failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_DEV_GPIO
  syslog(LOG_INFO, "BRING[probe] -> esp_gpio_init\n");
  ret = esp_gpio_init();
  if (ret < 0)
    {
      ierr("Failed to initialize GPIO Driver: %d\n", ret);
    }
#endif

#if defined(CONFIG_INPUT_BUTTONS) && defined(CONFIG_INPUT_BUTTONS_LOWER)
  /* Register the BUTTON driver */

  ret = btn_lower_initialize("/dev/buttons");
  if (ret < 0)
    {
      ierr("ERROR: btn_lower_initialize() failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_LEDC
  syslog(LOG_INFO, "BRING[probe] -> board_ledc_setup\n");
  ret = board_ledc_setup();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_ledc_setup() failed: %d\n", ret);
    }
#endif /* CONFIG_ESPRESSIF_LEDC */

#ifdef CONFIG_ESP_MCPWM_CAPTURE
  syslog(LOG_INFO, "BRING[probe] -> board_capture_initialize\n");
  ret = board_capture_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_capture_initialize failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESP_MCPWM_MOTOR
  syslog(LOG_INFO, "BRING[probe] -> board_motor_initialize\n");
  ret = board_motor_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_motor_initialize failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESP_PCNT
  syslog(LOG_INFO, "BRING[probe] -> board_pcnt_initialize\n");
  ret = board_pcnt_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_pcnt_initialize failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_PM
  /* Configure PM */

  syslog(LOG_INFO, "BRING[probe] -> esp_pmconfigure\n");
  ret = esp_pmconfigure();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: esp_pmconfigure failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_SYSTEM_NXDIAG_ESPRESSIF_CHIP_WO_TOOL
  syslog(LOG_INFO, "BRING[probe] -> esp_nxdiag_initialize\n");
  ret = esp_nxdiag_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: esp_nxdiag_initialize failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_ADC
  syslog(LOG_INFO, "BRING[probe] -> board_adc_init\n");
  ret = board_adc_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_adc_init failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_ANA_COMPR0
  syslog(LOG_INFO, "BRING[probe] -> esp_cmprinitialize\n");
  ret = esp_cmprinitialize(ESPRESSIF_COMP0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: esp_cmprinitialize(%d) failed: %d\n",
             ESPRESSIF_COMP0, ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_ANA_COMPR1
  syslog(LOG_INFO, "BRING[probe] -> esp_cmprinitialize\n");
  ret = esp_cmprinitialize(ESPRESSIF_COMP1);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: esp_cmprinitialize(%d) failed: %d\n",
             ESPRESSIF_COMP1, ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_EMAC
  syslog(LOG_INFO, "BRING[probe] -> board_emac_init\n");
  ret = board_emac_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_emac_init failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_USE_LP_CORE
#  ifdef CONFIG_ESPRESSIF_LP_MAILBOX
  esp_lp_mailbox_init();
#  endif

  /* ULP initialization should be the handled later than
   * peripherals to use supported peripherals properly on ULP core
   */

  syslog(LOG_INFO, "BRING[probe] -> esp_ulp_init\n");
  ret = esp_ulp_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: esp_ulp_init failed: %d\n", ret);
    }
  else
    {
#  ifdef CONFIG_ESPRESSIF_ULP_USE_TEST_BIN
      esp_ulp_load_bin((char *)esp_ulp_bin, esp_ulp_bin_len);
#  endif
    }
#endif

  /* If we got here then perhaps not all initialization was successful, but
   * at least enough succeeded to bring-up NSH with perhaps reduced
   * capabilities.
   */

  return ret;
}
