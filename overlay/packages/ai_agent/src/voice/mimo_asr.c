/****************************************************************************
 * packages/ai_agent/src/voice/mimo_asr.c
 *
 * MiMo (Xiaomi) ASR backend for the voice_asr framework.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 链路：PCM(16k/16bit/mono) → 套 WAV 头 → base64 → HTTPS POST
 *       POST {host}/v1/chat/completions
 *       {"model":"mimo-v2.5-asr","messages":[...input_audio...],
 *        "asr_options":{"language":"zh"}}
 *     取文本：choices[0].message.content
 *
 * 说明：MiMo 只接受 wav/mp3（不接受裸 PCM），因此这里必须先封装 WAV 头。
 *       base64 后大小上限 10MB。
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <syslog.h>

#include "voice/voice_asr.h"
#include "infra/config_store.h"
#include "infra/http_proxy.h"
#include "agent_compat.h"
#include "agent_config.h"

#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#define TAG "mimo_asr"

/* ── 端点与模型（实测确认） ─────────────────────────────────── */

#define MIMO_ASR_HOST_DEF   "token-plan-cn.xiaomimimo.com"
#define MIMO_ASR_PORT       "443"
#define MIMO_ASR_PATH       "/v1/chat/completions"
#define MIMO_ASR_MODEL      "mimo-v2.5-asr"
#define MIMO_ASR_LANG_DEF   "zh"

#define MIMO_ASR_MAX_B64    (9 * 1024 * 1024)   /* base64 上限 10MB，留余量 */

/* ── TLS 上下文（沿用 volc_asr.c 的范式） ───────────────────── */

typedef struct
{
  mbedtls_ssl_context      ssl;
  mbedtls_ssl_config       cfg;
  mbedtls_net_context      net;
  mbedtls_ctr_drbg_context ctr_drbg;
} mimo_tls_ctx_t;

static char s_api_key[192];
static char s_host[128];
static char s_lang[8];

/* 熵源 */
static int mimo_entropy_func(void *data, unsigned char *out, size_t len)
{
  (void)data;
  if (agent_secure_random(out, len) == 0)
    {
      return 0;
    }

  syslog(LOG_ERR, "[%s] CRITICAL: No secure entropy source available\n", TAG);
  return -1;
}

static int mimo_tls_connect(mimo_tls_ctx_t *ctx, const char *host,
                            const char *port)
{
  int ret;

  mbedtls_ssl_init(&ctx->ssl);
  mbedtls_ssl_config_init(&ctx->cfg);
  mbedtls_net_init(&ctx->net);
  mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

  const char *pers = "mimo_asr";

  ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mimo_entropy_func, NULL,
                              (const unsigned char *)pers, strlen(pers));
  if (ret != 0)
    {
      syslog(LOG_ERR, "[%s] ctr_drbg_seed: -0x%04x\n", TAG, -ret);
      return -EIO;
    }

  if (http_proxy_is_enabled())
    {
      int tunnel_fd = proxy_open_tunnel(host, atoi(port), 30000);
      if (tunnel_fd < 0)
        {
          syslog(LOG_ERR, "[%s] proxy tunnel failed\n", TAG);
          return -ECONNREFUSED;
        }

      ctx->net.fd = tunnel_fd;
      syslog(LOG_INFO, "[%s] Using proxy tunnel fd=%d\n", TAG, tunnel_fd);
    }
  else
    {
      ret = mbedtls_net_connect(&ctx->net, host, port, MBEDTLS_NET_PROTO_TCP);
      if (ret != 0)
        {
          syslog(LOG_ERR, "[%s] net_connect %s:%s: -0x%04x\n",
                 TAG, host, port, -ret);
          return -ECONNREFUSED;
        }
    }

  mbedtls_net_set_block(&ctx->net);
  if (ctx->net.fd >= 0)
    {
      struct timeval tv = { .tv_sec = 60, .tv_usec = 0 };
      setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

  ret = mbedtls_ssl_config_defaults(&ctx->cfg, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0)
    {
      return -EIO;
    }

  mbedtls_ssl_conf_min_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
  mbedtls_ssl_conf_max_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_3);
#else
  mbedtls_ssl_conf_max_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#endif

#if defined(MBEDTLS_SSL_ALPN)
    {
      static const char *alpn[] = { "http/1.1", NULL };
      mbedtls_ssl_conf_alpn_protocols(&ctx->cfg, alpn);
    }
#endif

  /* 与 volc_asr 一致：不做证书链严格校验，避免板子缺 CA 导致握手失败 */
  mbedtls_ssl_conf_authmode(&ctx->cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
  mbedtls_ssl_conf_rng(&ctx->cfg, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

  ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->cfg);
  if (ret != 0)
    {
      return -EIO;
    }

  mbedtls_ssl_set_hostname(&ctx->ssl, host);
  mbedtls_ssl_set_bio(&ctx->ssl, &ctx->net, mbedtls_net_send,
                      mbedtls_net_recv, NULL);

  while ((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0)
    {
      if (ret != MBEDTLS_ERR_SSL_WANT_READ
          && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          syslog(LOG_ERR, "[%s] handshake: -0x%04x\n", TAG, -ret);
          return -EIO;
        }
    }

  syslog(LOG_INFO, "[%s] TLS connected to %s:%s\n", TAG, host, port);
  return 0;
}

static void mimo_tls_free(mimo_tls_ctx_t *ctx)
{
  mbedtls_ssl_close_notify(&ctx->ssl);
  mbedtls_net_free(&ctx->net);
  mbedtls_ssl_free(&ctx->ssl);
  mbedtls_ssl_config_free(&ctx->cfg);
  mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
}

static int mimo_tls_write_all(mimo_tls_ctx_t *ctx, const char *buf, size_t len)
{
  size_t written = 0;

  while (written < len)
    {
      int ret = mbedtls_ssl_write(&ctx->ssl,
                                  (const unsigned char *)buf + written,
                                  len - written);
      if (ret > 0)
        {
          written += (size_t)ret;
        }
      else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          syslog(LOG_ERR, "[%s] ssl_write: -0x%04x\n", TAG, -ret);
          return -EIO;
        }
    }

  return 0;
}

/* ── WAV 头（16kHz / 16bit / 单声道） ───────────────────────── */

static size_t mimo_wav_header(unsigned char *h,
                              uint32_t pcm_len,
                              uint32_t rate,
                              uint16_t channels,
                              uint16_t bits)
{
  uint32_t byte_rate = rate * channels * (bits / 8);
  uint16_t align     = channels * (bits / 8);
  size_t   n         = 0;

#define PUT_LE32(v) do { h[n++] = (unsigned char)((v) & 0xff); \
                         h[n++] = (unsigned char)(((v) >> 8) & 0xff); \
                         h[n++] = (unsigned char)(((v) >> 16) & 0xff); \
                         h[n++] = (unsigned char)(((v) >> 24) & 0xff); \
                       } while (0)
#define PUT_LE16(v) do { h[n++] = (unsigned char)((v) & 0xff); \
                         h[n++] = (unsigned char)(((v) >> 8) & 0xff); \
                       } while (0)
#define PUT_TAG(s)  do { h[n++] = (unsigned char)(s)[0]; \
                         h[n++] = (unsigned char)(s)[1]; \
                         h[n++] = (unsigned char)(s)[2]; \
                         h[n++] = (unsigned char)(s)[3]; \
                       } while (0)

  PUT_TAG("RIFF");
  PUT_LE32(36 + pcm_len);
  PUT_TAG("WAVE");
  PUT_TAG("fmt ");
  PUT_LE32(16);
  PUT_LE16(1);              /* PCM */
  PUT_LE16(channels);
  PUT_LE32(rate);
  PUT_LE32(byte_rate);
  PUT_LE16(align);
  PUT_LE16(bits);
  PUT_TAG("data");
  PUT_LE32(pcm_len);

#undef PUT_LE32
#undef PUT_LE16
#undef PUT_TAG

  return n;   /* 44 */
}

/* ── 一次性 HTTPS POST，响应体写入 out ──────────────────────── */

static int mimo_https_post(const char *host, const char *path,
                           const char *api_key, const char *body,
                           size_t body_len, char **out, size_t *out_len)
{
  mimo_tls_ctx_t ctx;
  char *hdr = NULL;
  char *resp = NULL;
  size_t cap = 256 * 1024;
  size_t total = 0;
  int ret;
  int rc = -EIO;

  *out = NULL;
  *out_len = 0;

  ret = mimo_tls_connect(&ctx, host, MIMO_ASR_PORT);
  if (ret != 0)
    {
      return ret;
    }

  /* 请求头 */
  size_t hdr_cap = strlen(path) + strlen(host) + strlen(api_key) + 256;
  hdr = malloc(hdr_cap);
  if (hdr == NULL)
    {
      rc = -ENOMEM;
      goto out_free;
    }

  int hn = snprintf(hdr, hdr_cap,
                    "POST %s HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "api-key: %s\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %u\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    path, host, api_key, (unsigned)body_len);
  if (hn <= 0 || (size_t)hn >= hdr_cap)
    {
      goto out_free;
    }

  if (mimo_tls_write_all(&ctx, hdr, (size_t)hn) != 0)
    {
      goto out_free;
    }

  if (mimo_tls_write_all(&ctx, body, body_len) != 0)
    {
      goto out_free;
    }

  /* 读响应（Connection: close → 读到 EOF） */
  resp = malloc(cap + 1);
  if (resp == NULL)
    {
      rc = -ENOMEM;
      goto out_free;
    }

  for (;;)
    {
      unsigned char tmp[4096];
      int r = mbedtls_ssl_read(&ctx.ssl, tmp, sizeof(tmp));

      if (r > 0)
        {
          if (total + (size_t)r > cap)
            {
              /* 扩容（ASR 响应很小，正常不会走到这里） */
              size_t ncap = cap * 2;
              char *np = realloc(resp, ncap + 1);
              if (np == NULL)
                {
                  rc = -ENOMEM;
                  goto out_free;
                }

              resp = np;
              cap = ncap;
            }

          memcpy(resp + total, tmp, (size_t)r);
          total += (size_t)r;
        }
      else if (r == MBEDTLS_ERR_SSL_WANT_READ
               || r == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
          continue;
        }
      else
        {
          /* r == 0 (EOF) 或负错误码 → 结束读取 */
          break;
        }
    }

  if (total == 0)
    {
      syslog(LOG_ERR, "[%s] empty response\n", TAG);
      goto out_free;
    }

  resp[total] = '\0';

  /* 跳过 HTTP 头 */
  char *bodyp = strstr(resp, "\r\n\r\n");
  if (bodyp == NULL)
    {
      syslog(LOG_ERR, "[%s] malformed HTTP response\n", TAG);
      goto out_free;
    }

  bodyp += 4;

  /* 简单处理 chunked（若服务器忽略 Connection: close） */
  if (strstr(resp, "Transfer-Encoding: chunked") != NULL)
    {
      char *src = bodyp;
      char *dst = bodyp;
      while (*src != '\0')
        {
          char *endp = NULL;
          long clen = strtol(src, &endp, 16);
          if (clen <= 0 || endp == NULL)
            {
              break;
            }

          src = endp;
          if (*src == '\r') src++;
          if (*src == '\n') src++;

          if ((size_t)clen > (size_t)(resp + total - src))
            {
              clen = (long)(resp + total - src);
            }

          memmove(dst, src, (size_t)clen);
          dst += clen;
          src += clen;

          while (*src == '\r' || *src == '\n') src++;
        }

      *dst = '\0';
    }

  *out = bodyp;
  *out_len = strlen(bodyp);
  rc = 0;

out_free:
  if (hdr != NULL)
    {
      free(hdr);
    }

  mimo_tls_free(&ctx);

  if (rc != 0 && resp != NULL)
    {
      free(resp);
    }

  return rc;
}

/* ── 从响应 JSON 提取 choices[0].message.content ────────────── */

static int mimo_parse_text(const char *json, char *text_out, size_t text_cap)
{
  cJSON *root = cJSON_Parse(json);
  if (root == NULL)
    {
      syslog(LOG_ERR, "[%s] JSON parse failed\n", TAG);
      return -EINVAL;
    }

  int rc = -ENOENT;
  cJSON *choices = cJSON_GetObjectItem(root, "choices");
  cJSON *c0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
  cJSON *msg = c0 != NULL ? cJSON_GetObjectItem(c0, "message") : NULL;
  cJSON *content = msg != NULL ? cJSON_GetObjectItem(msg, "content") : NULL;

  if (content != NULL && cJSON_IsString(content) && content->valuestring != NULL)
    {
      strncpy(text_out, content->valuestring, text_cap - 1);
      text_out[text_cap - 1] = '\0';
      rc = 0;
    }
  else
    {
      /* 打印错误信息便于排障 */
      cJSON *err = cJSON_GetObjectItem(root, "error");
      if (err != NULL)
        {
          cJSON *em = cJSON_GetObjectItem(err, "message");
          syslog(LOG_ERR, "[%s] API error: %s\n", TAG,
                 (em && cJSON_IsString(em)) ? em->valuestring : "(unknown)");
        }
    }

  cJSON_Delete(root);
  return rc;
}

/* ── 后端实现 ───────────────────────────────────────────────── */

static int mimo_asr_init(void)
{
  memset(s_api_key, 0, sizeof(s_api_key));
  memset(s_host, 0, sizeof(s_host));
  memset(s_lang, 0, sizeof(s_lang));

  if (claw_config_get(AGENT_CFG_KEY_MIMO_API_KEY, s_api_key,
                      sizeof(s_api_key)) != OK
      || s_api_key[0] == '\0')
    {
      syslog(LOG_ERR, "[%s] MiMo api key not configured\n", TAG);
    }

  if (claw_config_get(AGENT_CFG_KEY_MIMO_ASR_HOST, s_host,
                      sizeof(s_host)) != OK
      || s_host[0] == '\0')
    {
      strncpy(s_host, MIMO_ASR_HOST_DEF, sizeof(s_host) - 1);
    }

  if (claw_config_get(AGENT_CFG_KEY_MIMO_ASR_LANG, s_lang,
                      sizeof(s_lang)) != OK
      || s_lang[0] == '\0')
    {
      strncpy(s_lang, MIMO_ASR_LANG_DEF, sizeof(s_lang) - 1);
    }

  return 0;
}

static int mimo_asr_recognize(const unsigned char *pcm_data, size_t pcm_len,
                              char *text_out, size_t text_cap)
{
  unsigned char wav_hdr[44];
  unsigned char *wav = NULL;
  unsigned char *b64 = NULL;
  char *json = NULL;
  char *resp = NULL;
  size_t resp_len = 0;
  size_t hdr_len;
  size_t wav_len;
  size_t b64_cap;
  size_t b64_len = 0;
  size_t json_cap;
  int rc;

  /* 每次调用重新载入凭证（运行期可能被改） */
  mimo_asr_init();

  if (s_api_key[0] == '\0')
    {
      syslog(LOG_ERR, "[%s] ASR credentials not configured\n", TAG);
      return -ENOENT;
    }

  if (pcm_data == NULL || pcm_len == 0)
    {
      return -EINVAL;
    }

  /* ① 套 WAV 头 */
  hdr_len = mimo_wav_header(wav_hdr, (uint32_t)pcm_len, 16000, 1, 16);
  wav_len = hdr_len + pcm_len;

  wav = malloc(wav_len);
  if (wav == NULL)
    {
      rc = -ENOMEM;
      goto out;
    }

  memcpy(wav, wav_hdr, hdr_len);
  memcpy(wav + hdr_len, pcm_data, pcm_len);

  /* ② base64 */
  b64_cap = 4 * ((wav_len + 2) / 3) + 4;
  if (b64_cap > MIMO_ASR_MAX_B64)
    {
      syslog(LOG_ERR, "[%s] audio too large: base64=%u > %u\n",
             TAG, (unsigned)b64_cap, (unsigned)MIMO_ASR_MAX_B64);
      rc = -EFBIG;
      goto out;
    }

  b64 = malloc(b64_cap);
  if (b64 == NULL)
    {
      rc = -ENOMEM;
      goto out;
    }

  rc = mbedtls_base64_encode(b64, b64_cap, &b64_len, wav, wav_len);
  if (rc != 0)
    {
      syslog(LOG_ERR, "[%s] base64 encode failed: -0x%04x\n", TAG, -rc);
      rc = -EIO;
      goto out;
    }

  /* ③ 拼 JSON（直接拼接，省内存） */
    {
      static const char *pfx =
        "{\"model\":\"" MIMO_ASR_MODEL "\","
        "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_audio\","
        "\"input_audio\":{\"data\":\"data:audio/wav;base64,";
      char sfx[128];

      int sn = snprintf(sfx, sizeof(sfx),
                        "\"}}]}],\"asr_options\":{\"language\":\"%s\"}}", s_lang);
      if (sn <= 0 || (size_t)sn >= sizeof(sfx))
        {
          rc = -EINVAL;
          goto out;
        }

      json_cap = strlen(pfx) + b64_len + (size_t)sn + 1;
      json = malloc(json_cap);
      if (json == NULL)
        {
          rc = -ENOMEM;
          goto out;
        }

      size_t o = 0;
      memcpy(json + o, pfx, strlen(pfx));
      o += strlen(pfx);
      memcpy(json + o, b64, b64_len);
      o += b64_len;
      memcpy(json + o, sfx, (size_t)sn);
      o += (size_t)sn;
      json[o] = '\0';
    }

  syslog(LOG_INFO, "[%s] POST %s (pcm=%u wav=%u json=%u)\n",
         TAG, s_host, (unsigned)pcm_len, (unsigned)wav_len, (unsigned)json_cap);

  /* ④ 发请求 */
  rc = mimo_https_post(s_host, MIMO_ASR_PATH, s_api_key, json,
                       strlen(json), &resp, &resp_len);
  if (rc != 0)
    {
      syslog(LOG_ERR, "[%s] https post failed: %d\n", TAG, rc);
      goto out;
    }

  /* ⑤ 取文本 */
  rc = mimo_parse_text(resp, text_out, text_cap);
  if (rc == 0)
    {
      syslog(LOG_INFO, "[%s] ASR ok: %s\n", TAG, text_out);
    }

out:
  if (wav != NULL)  free(wav);
  if (b64 != NULL)  free(b64);
  if (json != NULL) free(json);
  if (resp != NULL) free(resp);
  return rc;
}

static const voice_asr_ops_t s_mimo_asr_ops =
{
  .name      = "mimo",
  .init      = mimo_asr_init,
  .recognize = mimo_asr_recognize,
  .deinit    = NULL,
};

int mimo_asr_register(void)
{
  return voice_asr_register(&s_mimo_asr_ops);
}
