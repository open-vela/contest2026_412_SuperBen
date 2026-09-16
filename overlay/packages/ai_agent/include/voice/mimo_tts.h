/*
 * MiMo TTS 后端 —— openvela 语音助手
 * 照 volc_tts.c 结构，把 volcengine API 换成 MiMo TTS。
 * 链路：文本 → HTTPS POST /v1/chat/completions (mimo-v2.5-tts)
 *       → 解析 choices[0].message.audio.data (base64 WAV)
 *       → base64 解码 → 跳过 WAV 头 → 输出 16k/16bit/mono PCM
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int mimo_tts_register(void);

#ifdef __cplusplus
}
#endif
