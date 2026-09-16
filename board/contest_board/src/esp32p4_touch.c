/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_touch.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * GT911 capacitive touchscreen on I2C0 (SCL=GPIO8, SDA=GPIO7).
 * INT/RST pins are unconnected (NC), so the panel is sampled from the
 * low-priority work queue in polling mode. Adapted from the esp32s3-box
 * GT911 board driver, with release debouncing to suppress false UP
 * events caused by clearing the GT911 status register between polls.
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <syslog.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>
#include <string.h>
#include <nuttx/compiler.h>
#include <nuttx/wqueue.h>
#include <nuttx/input/touchscreen.h>
#include <nuttx/i2c/i2c_master.h>
#include "espressif/esp_i2c.h"
#include "esp32p4-function-ev-board.h"

/* GT911 maximum report frame size */

#define GT911_BUFFER_SIZE     41
#define GT911_TOUCHPOINTS     5

/* GT911 board configuration */

#define GT911_I2C_PORT        0
#define GT911_ADDR            0x5d
#define GT911_CLOCK           400000
#define GT911_PATH            "/dev/input0"
#define GT911_WORK_DELAY      2
#define GT911_ACTIVE_DELAY    2
#define GT911_SAMPLE_CACHES   4

/* Consecutive empty polls before a real release is reported */

#define GT911_RELEASE_MISSES  3

/* GT911 register addresses */

#define GT911_READ_XY_REG     0x814e
#define GT911_READ_DATA_REG   0x814f

struct gt911_dev_s
{
  struct touch_lowerhalf_s touch_lower;
  bool                has_report;
  uint8_t             misses;
  int16_t             last_x;
  int16_t             last_y;
  struct i2c_master_s *i2c;
  struct work_s       work;
  uint8_t             buffer[GT911_BUFFER_SIZE];
};

begin_packed_struct struct gt911_touchpoint_s
{
  uint8_t  id;
  uint16_t x;
  uint16_t y;
  uint16_t pressure;
  uint8_t  reserved;
} end_packed_struct;

begin_packed_struct struct gt911_data_s
{
  uint8_t touchpoints      : 4;
  uint8_t has_key          : 1;
  uint8_t proximity_valid  : 1;
  uint8_t large_detected   : 1;
  uint8_t buffer_status    : 1;
  struct gt911_touchpoint_s touchpoint[0];
} end_packed_struct;

static struct gt911_dev_s g_gt911_dev;

static int gt911_read_reg(struct gt911_dev_s *dev, uint16_t reg,
                          uint8_t *buf, int buflen)
{
  uint8_t regbuf[2] = { reg >> 8, reg & 0xff };
  struct i2c_msg_s msgv[2] =
  {
    {
      .frequency = GT911_CLOCK,
      .addr      = GT911_ADDR,
      .flags     = 0,
      .buffer    = regbuf,
      .length    = sizeof(regbuf)
    },
    {
      .frequency = GT911_CLOCK,
      .addr      = GT911_ADDR,
      .flags     = I2C_M_READ,
      .buffer    = buf,
      .length    = buflen
    }
  };

  int ret = I2C_TRANSFER(dev->i2c, msgv, 2);
  if (ret < 0)
    {
      ierr("GT911 I2C read failed: %d\n", ret);
      return ret;
    }

  return 0;
}

static int gt911_write_reg(struct gt911_dev_s *dev, uint16_t reg,
                           uint8_t val)
{
  uint8_t regbuf[3] = { reg >> 8, reg & 0xff, val };
  struct i2c_msg_s msgv[1] =
  {
    {
      .frequency = GT911_CLOCK,
      .addr      = GT911_ADDR,
      .flags     = 0,
      .buffer    = regbuf,
      .length    = sizeof(regbuf)
    }
  };

  int ret = I2C_TRANSFER(dev->i2c, msgv, 1);
  if (ret < 0)
    {
      ierr("GT911 I2C write failed: %d\n", ret);
      return ret;
    }

  return 0;
}

/* Touch panel calibration: map GT911 raw coordinates to screen 1024x600.
 * Measured from four screen corners on this board.
 */
#define GT911_RAW_X_MIN   28
#define GT911_RAW_X_MAX   1013
#define GT911_RAW_Y_MIN   17
#define GT911_RAW_Y_MAX   593
#define GT911_SCR_X_MAX   1023
#define GT911_SCR_Y_MAX   599

static void gt911_touch_event(struct gt911_dev_s *dev, bool pressed)
{
  struct gt911_data_s *data = (struct gt911_data_s *)dev->buffer;
  struct gt911_touchpoint_s *tp = data->touchpoint;
  struct touch_sample_s sample;
  struct touch_point_s *point = sample.point;
  int32_t cal_x;
  int32_t cal_y;

  /* Linear calibration: map raw touch range to screen resolution */

  cal_x = (int32_t)(tp->x - GT911_RAW_X_MIN) * GT911_SCR_X_MAX
          / (GT911_RAW_X_MAX - GT911_RAW_X_MIN);
  cal_y = (int32_t)(tp->y - GT911_RAW_Y_MIN) * GT911_SCR_Y_MAX
          / (GT911_RAW_Y_MAX - GT911_RAW_Y_MIN);

  if (cal_x < 0) cal_x = 0;
  if (cal_x > GT911_SCR_X_MAX) cal_x = GT911_SCR_X_MAX;
  if (cal_y < 0) cal_y = 0;
  if (cal_y > GT911_SCR_Y_MAX) cal_y = GT911_SCR_Y_MAX;

  /* Rotate 180 degrees: touch panel is mounted inverted relative to screen */

  cal_x = GT911_SCR_X_MAX - cal_x;
  cal_y = GT911_SCR_Y_MAX - cal_y;

  memset(&sample, 0, sizeof(sample));
  sample.npoints = 1;
  point->x        = cal_x;
  point->y        = cal_y;
  point->pressure = tp->pressure;
  point->flags    = TOUCH_POS_VALID | TOUCH_PRESSURE_VALID;

  if (pressed)
    {
      point->flags |= TOUCH_DOWN;
      dev->has_report = true;
    }
  else
    {
      point->flags |= TOUCH_UP;
      dev->has_report = false;
    }

  touch_event(dev->touch_lower.priv, &sample);
}

static void gt911_worker(void *arg)
{
  int ret;
  struct gt911_dev_s *dev = (struct gt911_dev_s *)arg;
  struct gt911_data_s *data = (struct gt911_data_s *)dev->buffer;
  clock_t delay = GT911_WORK_DELAY;

  ret = gt911_read_reg(dev, GT911_READ_XY_REG, dev->buffer, 1);
  if (ret == 0)
    {
      if (data->buffer_status && (data->touchpoints > 0) &&
          (data->touchpoints < GT911_TOUCHPOINTS))
        {
          /* Fresh touch data: read coordinates and report press/move */

          ret = gt911_read_reg(dev, GT911_READ_DATA_REG,
                               &dev->buffer[1],
                               data->touchpoints * 8);
          if (ret == 0)
            {
              if (!data->has_key)
                {
                  struct gt911_touchpoint_s *tp = data->touchpoint;
                  int16_t dx = tp->x - dev->last_x;
                  int16_t dy = tp->y - dev->last_y;
                  if (dx < 0) dx = -dx;
                  if (dy < 0) dy = -dy;
                  if (dx >= 3 || dy >= 3 || !dev->has_report)
                    {
                      gt911_touch_event(dev, true);
                      dev->last_x = tp->x;
                      dev->last_y = tp->y;
                    }
                }

              dev->misses = 0;
              delay = GT911_ACTIVE_DELAY;
            }
        }
      else if (dev->has_report)
        {
          /* No fresh data this poll.  Wait several polls to confirm a
           * real release before emitting a single UP event.
           */

          dev->misses++;
          if (dev->misses >= GT911_RELEASE_MISSES)
            {
              gt911_touch_event(dev, false);
              dev->misses = 0;
            }
        }

      gt911_write_reg(dev, GT911_READ_XY_REG, 0);
    }

  work_queue(LPWORK, &dev->work, gt911_worker, dev, delay);
}

int board_touchscreen_initialize(void)
{
  int ret;
  struct gt911_dev_s *dev = &g_gt911_dev;

  dev->i2c = esp_i2cbus_initialize(GT911_I2C_PORT);
  if (dev->i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize I2C%d for GT911\n",
             GT911_I2C_PORT);
      return -ENODEV;
    }

  dev->touch_lower.maxpoint = 1;
  ret = touch_register(&dev->touch_lower, GT911_PATH, GT911_SAMPLE_CACHES);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: touch_register() failed: %d\n", ret);
      return ret;
    }

  /* Read GT911 configuration: product ID + X/Y resolution.
   * Raw hex probes: distinguishes "no ACK / floating bus" from
   * "ACK but shifted/wrong data".  0x8140 must read 0x39 0x31 0x31 0x00
   * (ASCII "911\0"); anything else means the transfer is broken. */
  {
    uint8_t cfg[8];
    int r1 = gt911_read_reg(dev, 0x8140, cfg, 4);
    syslog(LOG_INFO, "GT911[probe] read 0x8140 ret=%d raw=%02x %02x %02x %02x\n",
           r1, cfg[0], cfg[1], cfg[2], cfg[3]);

    int r2 = gt911_read_reg(dev, 0x8047, cfg, 6);
    syslog(LOG_INFO,
           "GT911[probe] read 0x8047 ret=%d raw=%02x %02x %02x %02x %02x %02x\n",
           r2, cfg[0], cfg[1], cfg[2], cfg[3], cfg[4], cfg[5]);

    /* Read the status register the poll loop uses, to see if I2C works */
    int r3 = gt911_read_reg(dev, 0x814e, cfg, 1);
    syslog(LOG_INFO, "GT911[probe] read 0x814e ret=%d raw=%02x\n", r3, cfg[0]);

    if (r1 == 0)
      {
        uint16_t x_res = cfg[0] | (cfg[1] << 8);
        uint16_t y_res = cfg[2] | (cfg[3] << 8);
        (void)x_res; (void)y_res;
      }
  }

  ret = work_queue(LPWORK, &dev->work, gt911_worker, dev, GT911_WORK_DELAY);
  if (ret != 0)
    {
      syslog(LOG_ERR, "ERROR: GT911 work_queue() failed: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "GT911 touchscreen registered at %s (polling mode)\n",
         GT911_PATH);
  return 0;
}
