#include <nuttx/config.h>
#include <stdbool.h>
#include <stdio.h>
#include <debug.h>
#include <errno.h>
#include <nuttx/audio/audio.h>
#include <nuttx/audio/pcm.h>
#include <nuttx/audio/es8311.h>
#include <nuttx/i2c/i2c_master.h>
#include "espressif/esp_i2c.h"
#include "espressif/esp_i2s.h"

/* 手动声明（头文件里可能被 #ifdef 包起来了）*/
struct i2s_dev_s *esp_i2sbus_initialize(int port);
struct audio_lowerhalf_s *pcm_decode_initialize(FAR struct audio_lowerhalf_s *dev);

#if defined(CONFIG_ESPRESSIF_I2S0) && defined(CONFIG_AUDIO_ES8311)

static struct es8311_lower_s g_es8311_lower[2];

int board_es8311_initialize(int i2c_port, uint8_t i2c_addr,
                            int i2c_freq, int i2s_port)
{
  struct audio_lowerhalf_s *es8311;
  struct i2s_dev_s *i2s;
  struct i2c_master_s *i2c;
  static bool initialized = false;
  int ret;

  if (initialized)
    {
      return OK;
    }

  /* Get I2S bus (data channel) */
  i2s = esp_i2sbus_initialize(i2s_port);
  if (i2s == NULL)
    {
      auderr("Failed to initialize I2S%d\n", i2s_port);
      return -ENODEV;
    }

  /* Get I2C bus (control channel) */
  i2c = esp_i2cbus_initialize(i2c_port);
  if (i2c == NULL)
    {
      auderr("Failed to initialize I2C%d\n", i2c_port);
      return -ENODEV;
    }

  /* Playback: ES8311 -> PCM decoder -> /dev/pcm0 */
  g_es8311_lower[0].address   = i2c_addr;
  g_es8311_lower[0].frequency = i2c_freq;

  es8311 = es8311_initialize(i2c, i2s, &g_es8311_lower[0]);
  if (es8311 == NULL)
    {
      auderr("Failed to initialize ES8311 playback\n");
      return -ENODEV;
    }

  struct audio_lowerhalf_s *pcm = pcm_decode_initialize(es8311);
  if (pcm == NULL)
    {
      auderr("Failed to create PCM decoder\n");
      return -ENODEV;
    }

  ret = audio_register("pcm0", pcm);
  if (ret < 0)
    {
      auderr("Failed to register /dev/pcm0: %d\n", ret);
      return ret;
    }

  /* Record: ES8311 -> /dev/pcm_in0 */
  g_es8311_lower[1].address   = i2c_addr;
  g_es8311_lower[1].frequency = i2c_freq;

  es8311 = es8311_initialize(i2c, i2s, &g_es8311_lower[1]);
  if (es8311 == NULL)
    {
      auderr("Failed to initialize ES8311 record\n");
      return -ENODEV;
    }

  ret = audio_register("pcm_in0", es8311);
  if (ret < 0)
    {
      auderr("Failed to register /dev/pcm_in0: %d\n", ret);
      return ret;
    }

  initialized = true;
  syslog(LOG_INFO, "ES8311 audio registered: /dev/pcm0, /dev/pcm_in0\n");
  return OK;
}

#endif
