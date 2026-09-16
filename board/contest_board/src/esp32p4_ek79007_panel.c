/* ROM ets_printf: declared extern so early-boot probes in this file
 * actually print.  (Previously this was a no-op macro, which silently
 * swallowed every DSI[sN]/DSI[p0] diagnostic message.) */
extern int ets_printf(const char *fmt, ...);

/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_ek79007_panel.c
 *
 *  ESP32-P4 MIPI-DSI 7" 1024x600 面板驱动 (EK79007AD)
 *  ===============================================================
 *  硬件: ESP32-P4X-Function-EV-Board + LCD 适配板 (反向线序)
 *  屏幕: EK79007AD 驱动 IC, 1024x600, RGB565, MIPI-DSI 2-lane @1000Mbps
 *  接线: GPIO27 = RST_LCD, GPIO26 = 背光 PWM (官方默认)
 *  底座: 对接 esp_mipi_dsi.c (MIPI-DSI host 驱动)
 *  参考: esp-idf examples/peripherals/camera/mipi_isp_dsi
 *        esp-iot-solution components/display/lcd/esp_lcd_ek79007
 *
 *  SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/signal.h>
#include <nuttx/arch.h>

#include <errno.h>
#include <debug.h>
#include <string.h>

#include <nuttx/kmalloc.h>
#include <nuttx/video/fb.h>
#include <nuttx/video/mipi_dsi.h>
#include <nuttx/video/mipi_display.h>

#include "esp_mipi_dsi.h"

#include "espressif/esp_gpio.h"
#include "espressif/esp_ldo.h"
#include <arch/chip/gpio_sig_map.h>

#ifdef CONFIG_ESP32P4_LCD_EK79007

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* ---- EK79007 面板参数 (乐鑫 mipi_isp_dsi 示例实测) ---- */

#define EK79007_H_RES                1024
#define EK79007_V_RES                600
#define EK79007_BPP                  16                /* RGB565 */
#define EK79007_BYTES_PER_PX         (EK79007_BPP / 8)

#define EK79007_FB_SIZE              (EK79007_H_RES * EK79007_V_RES * \
                                      EK79007_BYTES_PER_PX)

#define EK79007_DSI_LANES            2
#define EK79007_DSI_LANE_MBPS        1000
#define EK79007_DSI_DPI_CLK_MHZ      48                /* 约 60Hz */

#define EK79007_HSYNC_PULSE          10
#define EK79007_HSYNC_BACK_PORCH     120
#define EK79007_HSYNC_FRONT_PORCH    120
#define EK79007_VSYNC_PULSE          1
#define EK79007_VSYNC_BACK_PORCH     20
#define EK79007_VSYNC_FRONT_PORCH    10

/* ---- 板级接线 (官方用户指南) ---- */
#define EK79007_RST_GPIO             27                /* J1-38 LCD RST */
#define EK79007_BL_GPIO              26                /* J1-31 LCD 背光 */

/* ---- MIPI PHY 模拟电源: 芯片内 LDO_VO3, 2.5V ----
 * DSI PHY PLL 属模拟域, 不上电则 PLL 永远锁不住 (-110)。
 * 参考: IDF mipi_dsi 示例 / NuttX 主线 esp32p4-tab5 esp32p4_hmi_power.c */
#define EK79007_MIPI_PHY_LDO_CHAN       3
#define EK79007_MIPI_PHY_LDO_VOLTAGE_MV 2500

static struct esp_ldo_config_t g_ek79007_phy_ldo_config =
{
  .chan_id    = EK79007_MIPI_PHY_LDO_CHAN,
  .voltage_mv = EK79007_MIPI_PHY_LDO_VOLTAGE_MV,
  .handler    = NULL,
};

/* ---- EK79007 寄存器 ---- */
#define EK79007_PAD_CONTROL          0xB2              /* PAD 控制 */
#define EK79007_DSI_2_LANE           0x10

/* ---- 帧缓冲直接暴露接口 (Phase 5 LVGL / camera 通路用) ---- */
static FAR uint8_t *g_ek79007_fb[2];  /* 双缓冲: [0]显示/[1]绘制 */
static size_t g_ek79007_fb_size = 0;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int ek79007_fb_getvideoinfo(FAR struct fb_vtable_s *vtable,
                                   FAR struct fb_videoinfo_s *vinfo);
static int ek79007_fb_getplaneinfo(FAR struct fb_vtable_s *vtable,
                                   int planeno,
                                   FAR struct fb_planeinfo_s *pinfo);
static int ek79007_fb_pandisplay(FAR struct fb_vtable_s *vtable,
                                 FAR struct fb_planeinfo_s *pinfo);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* EK79007 初始化命令序列 —— 移植自 esp_lcd_ek79007 组件
 * (vendor_specific_init_default), 屏厂序列不同时整体替换即可 */
static const uint8_t g_ek79007_padctl_data = EK79007_DSI_2_LANE;

static const struct
{
  uint8_t       cmd;
  FAR const uint8_t *data;
  uint8_t       data_bytes;
  uint16_t      delay_ms;
} g_ek79007_init_cmds[] =
{
  {0x80, (FAR const uint8_t []){0x8B}, 1, 0},
  {0x81, (FAR const uint8_t []){0x78}, 1, 0},
  {0x82, (FAR const uint8_t []){0x84}, 1, 0},
  {0x83, (FAR const uint8_t []){0x88}, 1, 0},
  {0x84, (FAR const uint8_t []){0xA8}, 1, 0},
  {0x85, (FAR const uint8_t []){0xE3}, 1, 0},
  {0x86, (FAR const uint8_t []){0x88}, 1, 0},
  {0x11, NULL, 0, 120},                /* 退出睡眠, 等待 120ms */
};

/* framebuffer 设备 vtable (基础版: 只实现查询接口) */
static struct fb_vtable_s g_ek79007_fb_vtable =
{
  .getvideoinfo = ek79007_fb_getvideoinfo,
  .getplaneinfo = ek79007_fb_getplaneinfo,
  .pandisplay   = ek79007_fb_pandisplay,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ek79007_fb_getvideoinfo
 ****************************************************************************/

static int ek79007_fb_getvideoinfo(FAR struct fb_vtable_s *vtable,
                                   FAR struct fb_videoinfo_s *vinfo)
{
  if (vinfo == NULL)
    {
      return -EINVAL;
    }

  vinfo->fmt     = FB_FMT_RGB16_565;
  vinfo->xres    = EK79007_H_RES;
  vinfo->yres    = EK79007_V_RES;
  vinfo->nplanes = 1;
  return OK;
}

/****************************************************************************
 * Name: ek79007_fb_getplaneinfo
 ****************************************************************************/

static int ek79007_fb_getplaneinfo(FAR struct fb_vtable_s *vtable,
                                   int planeno,
                                   FAR struct fb_planeinfo_s *pinfo)
{
  if (pinfo == NULL || planeno != 0)
    {
      return -EINVAL;
    }

  pinfo->fbmem    = (FAR void *)g_ek79007_fb[0];
  pinfo->fblen    = g_ek79007_fb_size * 2;   /* 两个缓冲，连续排布 */
  pinfo->stride   = EK79007_H_RES * EK79007_BYTES_PER_PX;
  pinfo->display  = 0;
  pinfo->bpp      = EK79007_BPP;
  pinfo->yres_virtual = EK79007_V_RES * 2;   /* 双缓冲: LVGL 据此启用双缓冲 */
  return OK;
}

/****************************************************************************
 * Name: ek79007_fb_pandisplay
 ****************************************************************************/

static int ek79007_fb_pandisplay(FAR struct fb_vtable_s *vtable,
                                 FAR struct fb_planeinfo_s *pinfo)
{
  int idx = (pinfo != NULL && pinfo->yoffset >= EK79007_V_RES) ? 1 : 0;

  /* 切换 DSI 扫描的帧缓冲，并回写缓存让 GDMA 看到新数据 */
  esp_mipi_dsi_bind_framebuffer(g_ek79007_fb[idx], g_ek79007_fb_size,
                                EK79007_H_RES, EK79007_V_RES, EK79007_BPP);
  esp_mipi_dsi_flush_framebuffer(g_ek79007_fb[idx], g_ek79007_fb_size);
  return OK;
}

/****************************************************************************
 * Name: ek79007_send_cmd
 *
 * Description:
 *   通过 DSI device 发送一条 DCS 写命令。
 ****************************************************************************/

static int ek79007_send_cmd(FAR struct mipi_dsi_device *dev,
                            uint8_t cmd,
                            FAR const uint8_t *data,
                            uint8_t data_bytes)
{
  struct mipi_dsi_msg msg;
  uint8_t tx[256];
  ssize_t ret;

  /* 拼接 DCS 命令字节 + 参数 */
  tx[0] = cmd;
  if (data != NULL && data_bytes > 0)
    {
      memcpy(&tx[1], data, data_bytes);
    }

  memset(&msg, 0, sizeof(msg));
  msg.channel = dev->channel;
  msg.tx_buf  = tx;
  msg.tx_len  = (size_t)data_bytes + 1;
  /* 初始化命令必须走 LP 模式: 视频流未启动时主机无法调度 HS 传输,
   * HS 命令会导致 FIFO 永不排空而超时 (-110)。同 IDF lpm_transfer=true */
  msg.flags   = MIPI_DSI_MSG_USE_LPM;

  if (data_bytes == 0)
    {
      msg.type = MIPI_DSI_DCS_SHORT_WRITE_0_PARAM;
    }
  else if (data_bytes == 1)
    {
      msg.type = MIPI_DSI_DCS_SHORT_WRITE_1_PARAM;
    }
  else
    {
      msg.type = MIPI_DSI_DCS_LONG_WRITE;
    }

  ret = mipi_dsi_transfer(dev, &msg);
  if (ret < 0)
    {
      _err("DCS write 0x%02X failed: %zd\n", cmd, ret);
      return (int)ret;
    }

  return OK;
}

/****************************************************************************
 * Name: ek79007_send_init_sequence
 ****************************************************************************/

static int ek79007_send_init_sequence(FAR struct mipi_dsi_device *dev)
{
  int ret;
  int i;

  /* PAD 控制: 配置 2-lane */
  ret = ek79007_send_cmd(dev, EK79007_PAD_CONTROL,
                         &g_ek79007_padctl_data, 1);
  if (ret < 0)
    {
      return ret;
    }

  /* 厂商初始化序列 */
  for (i = 0; i < sizeof(g_ek79007_init_cmds) /
                  sizeof(g_ek79007_init_cmds[0]); i++)
    {
      ret = ek79007_send_cmd(dev, g_ek79007_init_cmds[i].cmd,
                             g_ek79007_init_cmds[i].data,
                             g_ek79007_init_cmds[i].data_bytes);
      if (ret < 0)
        {
          return ret;
        }

      if (g_ek79007_init_cmds[i].delay_ms > 0)
        {
          nxsig_usleep(g_ek79007_init_cmds[i].delay_ms * 1000);
        }
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32p4_ek79007_initialize
 *
 * Description:
 *   点亮 7 寸 EK79007 屏: DSI 总线 -> 命令序列 -> DPI 时序 ->
 *   帧缓冲(PSRAM) -> 视频流 -> 注册 /dev/fb0, 并填充纯色自检。
 *
 * Returned Value:
 *   OK 或负 errno。
 ****************************************************************************/

int esp32p4_ek79007_initialize(void)
{
  FAR struct mipi_dsi_host *host;
  FAR struct mipi_dsi_device *dev;
  struct esp_mipi_dsi_bus_config_s bus_cfg;
  struct esp_mipi_dsi_dpi_config_s dpi_cfg;
  int ret;

  /* -1. 上电 MIPI PHY 模拟电源 (VDD_MIPI_DPHY = LDO_VO3 2.5V)。
   *     PHY PLL 属模拟域, 不供电则 PLL 永远无法锁定 (-110) */
  ret = esp_ldo_channel_acquire(&g_ek79007_phy_ldo_config);
  if (ret < 0)
    {
      _err("esp_ldo_channel_acquire failed: %d\n", ret);
      return ret;
    }
  ets_printf("DSI[p0]: mipi phy LDO3 2.5V on\n");

  /* 0. GPIO27 reset LCD: LOW 10ms, HIGH 50ms */
  esp_gpio_matrix_out(EK79007_RST_GPIO, SIG_GPIO_OUT_IDX, false, false);
  esp_gpiowrite(EK79007_RST_GPIO, false);
  up_mdelay(10);
  esp_gpiowrite(EK79007_RST_GPIO, true);
  up_mdelay(50);

  /* 1. 初始化 DSI 总线: 2-lane, 1000 Mbps */
  memset(&bus_cfg, 0, sizeof(bus_cfg));
  bus_cfg.num_data_lanes     = EK79007_DSI_LANES;
  bus_cfg.lane_bit_rate_mbps = EK79007_DSI_LANE_MBPS;

  ret = esp_mipi_dsi_initialize(&bus_cfg);
  if (ret < 0)
    {
      _err("esp_mipi_dsi_initialize failed: %d\n", ret);
      return ret;
    }

  /* 2. 获取 host, 注册 device (virtual channel 0) */
  host = esp_mipi_dsi_host_get();
  if (host == NULL)
    {
      _err("esp_mipi_dsi_host_get failed\n");
      return -ENODEV;
    }

  dev = mipi_dsi_device_register(host, "ek79007", 0);
  if (dev == NULL)
    {
      _err("mipi_dsi_device_register failed\n");
      return -ENODEV;
    }

  /* attach 时 host 会校验 device->lanes (0 被拒为 -EINVAL), 必须先设置 */
  dev->lanes = EK79007_DSI_LANES;

  ets_printf("DSI[s2]: attach\n");
  ret = mipi_dsi_attach(dev);
  if (ret < 0)
    {
      _err("mipi_dsi_attach failed: %d\n", ret);
      return ret;
    }

  /* 3. EK79007 初始化命令序列 */
  ets_printf("DSI[s3]: init cmds\n");
  ret = ek79007_send_init_sequence(dev);
  if (ret < 0)
    {
      _err("EK79007 init sequence failed: %d\n", ret);
      return ret;
    }

  /* 4. 配置 DPI 视频模式时序 (RGB565, 1024x600, 48MHz) */
  memset(&dpi_cfg, 0, sizeof(dpi_cfg));
  dpi_cfg.h_res              = EK79007_H_RES;
  dpi_cfg.v_res              = EK79007_V_RES;
  dpi_cfg.hsync_pulse_width  = EK79007_HSYNC_PULSE;
  dpi_cfg.hsync_back_porch   = EK79007_HSYNC_BACK_PORCH;
  dpi_cfg.hsync_front_porch  = EK79007_HSYNC_FRONT_PORCH;
  dpi_cfg.vsync_pulse_width  = EK79007_VSYNC_PULSE;
  dpi_cfg.vsync_back_porch   = EK79007_VSYNC_BACK_PORCH;
  dpi_cfg.vsync_front_porch  = EK79007_VSYNC_FRONT_PORCH;
  dpi_cfg.dpi_clock_freq_mhz = EK79007_DSI_DPI_CLK_MHZ;
  dpi_cfg.virtual_channel    = 0;
  dpi_cfg.format             = MIPI_DSI_FMT_RGB565;

  ets_printf("DSI[s4]: dpi cfg\n");
  ret = esp_mipi_dsi_configure_dpi(&dpi_cfg);
  if (ret < 0)
    {
      _err("esp_mipi_dsi_configure_dpi failed: %d\n", ret);
      return ret;
    }

  /* 5. 分配帧缓冲 (1.2MB 只能放 PSRAM: 内核堆 kmm 只有内部 SRAM,
   *    PSRAM 挂在用户堆 kumm, kmm 失败时回退 kumm) */
  g_ek79007_fb_size = EK79007_FB_SIZE;

  /* 双缓冲: 分配两个连续 PSRAM 帧缓冲 */
  g_ek79007_fb[0] = kmm_zalloc(g_ek79007_fb_size * 2);
  if (g_ek79007_fb[0] == NULL)
    {
      g_ek79007_fb[0] = kumm_zalloc(g_ek79007_fb_size * 2);
    }
  if (g_ek79007_fb[0] == NULL)
    {
      _err("fb alloc failed (%zu bytes)\n", g_ek79007_fb_size * 2);
      return -ENOMEM;
    }

  g_ek79007_fb[1] = g_ek79007_fb[0] + g_ek79007_fb_size;

  ets_printf("DSI[s5]: fb bind (double buffer)\n");
  ret = esp_mipi_dsi_bind_framebuffer(g_ek79007_fb[0], g_ek79007_fb_size,
                                      EK79007_H_RES, EK79007_V_RES,
                                      EK79007_BPP);
  if (ret < 0)
    {
      _err("esp_mipi_dsi_bind_framebuffer failed: %d\n", ret);
      return ret;
    }

  /* 6. 启动视频流 */
  ets_printf("DSI[s6]: video start\n");
  ret = esp_mipi_dsi_video_start();
  if (ret < 0)
    {
      _err("esp_mipi_dsi_video_start failed: %d\n", ret);
      return ret;
    }

  /* 7. 背光: GPIO26 输出高电平点亮 (1024x600 面板用 GPIO26)。
   *    先用普通 GPIO 拉高验证点亮; 后续如需调光再接 LEDC PWM。 */
  esp_gpio_matrix_out(EK79007_BL_GPIO, SIG_GPIO_OUT_IDX, false, false);
  esp_configgpio(EK79007_BL_GPIO, OUTPUT | PULLUP);
  esp_gpiowrite(EK79007_BL_GPIO, true);
  ets_printf("DSI[b1]: backlight GPIO%d -> HIGH\n", EK79007_BL_GPIO);

  /* 8. 纯色自检: 全屏蓝色 (RGB565: 0x001F) */
  memset(g_ek79007_fb[0], 0x1F, g_ek79007_fb_size / 2);
  memset(g_ek79007_fb[0] + g_ek79007_fb_size / 2, 0x00,
         g_ek79007_fb_size / 2);
  esp_mipi_dsi_flush_framebuffer(g_ek79007_fb[0], g_ek79007_fb_size);

  /* 9. 注册 /dev/fb0 (fb.c 由 CONFIG_VIDEO_FB 编入; CONFIG_FB 并非真实符号) */
#ifdef CONFIG_VIDEO_FB
  ets_printf("DSI[s7]: fb register\n");
  ret = fb_register_device(0, 0, &g_ek79007_fb_vtable);
  if (ret < 0)
    {
      _err("fb_register_device failed: %d\n", ret);
      return ret;
    }
#endif

  _info("EK79007 panel up: %dx%d RGB565 fb=%p (double)\n",
        EK79007_H_RES, EK79007_V_RES, g_ek79007_fb[0]);
  return OK;
}

/****************************************************************************
 * Name: esp32p4_ek79007_get_framebuffer
 *
 * Description:
 *   返回帧缓冲指针 (供 Phase 4/5 摄像头通路 / LVGL 直接使用)。
 ****************************************************************************/

FAR void *esp32p4_ek79007_get_framebuffer(size_t *size)
{
  if (size != NULL)
    {
      *size = g_ek79007_fb_size;
    }

  return g_ek79007_fb;
}

#endif /* CONFIG_ESP32P4_LCD_EK79007 */
