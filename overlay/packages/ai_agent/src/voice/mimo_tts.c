/*
 * MiMo TTS 后端 —— openvela 语音助手
 *
 * 链路：文本 → HTTPS POST {host}/v1/chat/completions (mimo-v2.5-tts)
 *       → 解析 choices[0].message.audio.data (base64 WAV)
 *       → base64 解码 → 跳过 WAV 头 → 输出 16k/16bit/mono PCM
 *
 * 认证头：api-key（Token Plan 实测可用）
 */

#include "voice/mimo_tts.h"
#include "infra/config_store.h"
#include "infra/vela_tls.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/voice_tts.h"

#include "cJSON.h"
#include "mbedtls/base64.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static const char *TAG = "mimo_tts";

#define MIMO_TTS_HOST   "token-plan-cn.xiaomimimo.com"
#define MIMO_TTS_PORT   "443"
#define MIMO_TTS_PATH   "/v1/chat/completions"
#define MIMO_TTS_MODEL  "mimo-v2.5-tts"
#define MIMO_TTS_VOICE  "mimo_default"     /* 英文音色 */
#define MIMO_TTS_RESP_SIZE (512 * 1024)  /* 响应缓冲（音频 base64 较大） */

static char s_api_key[128];

static int mimo_tts_init(void)
{
    memset(s_api_key, 0, sizeof(s_api_key));
    claw_config_get(AGENT_CFG_KEY_MIMO_API_KEY, s_api_key, sizeof(s_api_key));
    return 0;
}

static char *build_tts_request(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return NULL;

    cJSON_AddStringToObject(root, "model", MIMO_TTS_MODEL);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");

    cJSON *m1 = cJSON_CreateObject();
    cJSON_AddStringToObject(m1, "role", "user");
    cJSON_AddStringToObject(m1, "content", "");
    cJSON_AddItemToArray(messages, m1);

    cJSON *m2 = cJSON_CreateObject();
    cJSON_AddStringToObject(m2, "role", "assistant");
    cJSON_AddStringToObject(m2, "content", text);
    cJSON_AddItemToArray(messages, m2);

    cJSON *audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "format", "wav");
    cJSON_AddStringToObject(audio, "voice", MIMO_TTS_VOICE);
    cJSON_AddItemToObject(root, "audio", audio);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

/* 从 WAV 缓冲区里找到 PCM 数据的偏移（找 "data" chunk） */
static size_t wav_data_offset(const unsigned char *wav, size_t len)
{
    if (len > 12 && memcmp(wav, "RIFF", 4) == 0 &&
        memcmp(wav + 8, "WAVE", 4) == 0)
    {
        size_t pos = 12;
        while (pos + 8 <= len)
        {
            uint32_t sz = (uint32_t)wav[pos + 4] |
                          ((uint32_t)wav[pos + 5] << 8) |
                          ((uint32_t)wav[pos + 6] << 16) |
                          ((uint32_t)wav[pos + 7] << 24);
            if (memcmp(wav + pos, "data", 4) == 0)
            {
                return pos + 8;
            }
            pos += 8 + sz + (sz & 1);   /* chunk 对齐到偶数 */
        }
    }
    return 44;   /* 回退：标准 44 字节 RIFF 头 */
}

static int mimo_tts_synthesize(const char *text,
                               unsigned char *pcm_out,
                               size_t pcm_cap,
                               size_t *pcm_len)
{
    if (!text || !pcm_out || !pcm_len)
        return -EINVAL;

    *pcm_len = 0;

    mimo_tts_init();
    if (s_api_key[0] == '\0')
    {
        syslog(LOG_ERR, "[%s] MiMo api key not configured\n", TAG);
        return -ENOENT;
    }

    char *body = build_tts_request(text);
    if (!body)
        return -ENOMEM;

    vela_header_t hdrs[] =
    {
        { "api-key", s_api_key },
        { "Content-Type", "application/json" },
        { "Connection", "close" },
        { NULL, NULL }
    };

    char *resp = calloc(1, MIMO_TTS_RESP_SIZE);
    if (!resp)
    {
        free(body);
        return -ENOMEM;
    }

    size_t resp_len = 0;
    int status = vela_https_request(MIMO_TTS_HOST, MIMO_TTS_PORT,
                                    "POST", MIMO_TTS_PATH, hdrs,
                                    body, strlen(body),
                                    resp, MIMO_TTS_RESP_SIZE, &resp_len);
    free(body);

    if (status != 200)
    {
        syslog(LOG_ERR, "[%s] TTS HTTP %d: %.256s\n", TAG, status, resp);
        free(resp);
        return -EIO;
    }

    if (resp_len == 0)
    {
        syslog(LOG_ERR, "[%s] TTS: empty response\n", TAG);
        free(resp);
        return -EPROTO;
    }

    /* 解析 choices[0].message.audio.data */
    cJSON *obj = cJSON_Parse(resp);
    if (!obj)
    {
        syslog(LOG_ERR, "[%s] TTS: bad JSON\n", TAG);
        free(resp);
        return -EPROTO;
    }

    cJSON *choices = cJSON_GetObjectItem(obj, "choices");
    cJSON *choice0 = choices ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;
    cJSON *audio = message ? cJSON_GetObjectItem(message, "audio") : NULL;
    cJSON *data_j = audio ? cJSON_GetObjectItem(audio, "data") : NULL;

    if (!data_j || !cJSON_IsString(data_j))
    {
        syslog(LOG_ERR, "[%s] TTS: no audio.data field\n", TAG);
        cJSON_Delete(obj);
        free(resp);
        return -EPROTO;
    }

    const char *b64 = data_j->valuestring;

    /* 剥离可能的 "data:audio/wav;base64," 前缀 */
    const char *comma = strstr(b64, "base64,");
    if (comma)
        b64 = comma + 7;

    size_t b64_len = strlen(b64);
    size_t wav_cap = b64_len * 3 / 4 + 16;
    unsigned char *wav_buf = malloc(wav_cap);
    if (!wav_buf)
    {
        cJSON_Delete(obj);
        free(resp);
        return -ENOMEM;
    }

    size_t wav_len = 0;
    if (mbedtls_base64_decode(wav_buf, wav_cap, &wav_len,
                              (const unsigned char *)b64, b64_len) != 0)
    {
        syslog(LOG_ERR, "[%s] TTS: base64 decode failed\n", TAG);
        free(wav_buf);
        cJSON_Delete(obj);
        free(resp);
        return -EPROTO;
    }

    /* 跳过 WAV 头取 PCM */
    size_t offset = wav_data_offset(wav_buf, wav_len);
    if (offset >= wav_len)
    {
        syslog(LOG_ERR, "[%s] TTS: WAV too short (%zu)\n", TAG, wav_len);
        free(wav_buf);
        cJSON_Delete(obj);
        free(resp);
        return -EPROTO;
    }

    size_t pcm = wav_len - offset;
    if (pcm > pcm_cap)
        pcm = pcm_cap;

    memcpy(pcm_out, wav_buf + offset, pcm);
    *pcm_len = pcm;

    free(wav_buf);
    cJSON_Delete(obj);
    free(resp);

    syslog(LOG_INFO, "[%s] TTS: synthesized %zu PCM bytes\n", TAG, pcm);
    return 0;
}

static void mimo_tts_deinit(void)
{
}

static const voice_tts_ops_t s_mimo_tts_ops =
{
    .name       = "mimo",
    .init       = mimo_tts_init,
    .synthesize = mimo_tts_synthesize,
    .deinit     = mimo_tts_deinit,
};

int mimo_tts_register(void)
{
    return voice_tts_register(&s_mimo_tts_ops);
}
