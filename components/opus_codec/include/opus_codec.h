/**
 * @file opus_codec.h
 * @brief Opus 编解码（P7 语音对话的音频编解码层）
 *
 * ============================================================================
 * 【为什么单独一个组件，而不是塞进 xiaozhi_client】
 * xiaozhi_client 的 CMakeLists 里写明了它的边界：「只负责设备 ⇄ 服务器这条链路，
 * 不碰界面、不碰音频编解码」。这个边界不是洁癖 —— 它让协议层能**独立验证**：
 * 先证明连得上、握手过、能收发文本，再谈音频。本项目正是按这个次序做的，
 * 协议层在没有任何音频代码时就跑通并看到 session_id 了。
 *
 * ============================================================================
 * 【为什么直接用 C API，不用 78/esp-opus-encoder 那个 C++ Wrapper】
 * ① 本工程是纯 C。引入 Wrapper 就要拉进 std::vector/std::mutex，
 *    只为调一个 opus_encode。
 * ② Wrapper 是**面向不定长输入**设计的：它内部维护 in_buffer_，
 *    每次 Encode 都 erase 掉已编码的部分（O(n) 搬移），并对每次输出
 *    构造一个 std::vector。我们的调用方（audio_service_rec_read）
 *    保证每次都是**精确的一帧**，所以那些缓冲与搬移在这里是纯开销。
 * ③ 参数仍与它**逐项对齐**（见下），所以"少了 Wrapper"不影响行为一致。
 *
 * ============================================================================
 * 【参数出处】全部取自 xiaozhi-esp32-1.9.4-gec，不是自选的
 *   · 编码器：opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err)
 *             —— managed_components/78__esp-opus-encoder/opus_encoder.cc:9
 *   · DTX 打开、复杂度 0
 *             —— 同上:16-17（注释原文：complexity 5 almost uses up all CPU
 *                of ESP32C3 while complexity 0 uses the least）
 *   · 上行帧长 60ms / 16kHz 单声道
 *             —— audio/audio_service.cc:39 与协议 hello 的 audio_params
 *   · 下行解码器按服务器给的采样率创建
 *             —— audio/audio_service.cc:394-404
 * ============================================================================
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 上行（设备 → 服务器）—— **协议契约，不要改**
 * 数值与 xiaozhi_config.h 的 XZ_UP_* 必须一致；服务器按这个契约收音频。
 * ========================================================================== */
#define OPUS_UP_SAMPLE_RATE     16000
#define OPUS_UP_FRAME_MS        60
#define OPUS_UP_FRAME_SAMPLES   ((OPUS_UP_SAMPLE_RATE) / 1000 * (OPUS_UP_FRAME_MS))  /* 960 */
#define OPUS_UP_CHANNELS        1

/** 单个 Opus 包的最大字节数。与 Wrapper 的 MAX_OPUS_PACKET_SIZE 一致。
 *  16kHz 单声道 60ms 的语音帧实测只有几十字节，1000 是充裕的上限。 */
#define OPUS_MAX_PACKET_SIZE    1000

/** 下行解码输出的上限（样本数）。按服务器最可能给的参数留足：
 *  24kHz × 60ms = 1440。真实值以 opus_codec_set_down() 后查询为准。 */
#define OPUS_DOWN_MAX_SAMPLES   2048

/**
 * @brief 创建编/解码器。
 *
 * @note **内存**：Opus 的编解码状态是内部大块分配（几十 KB 级），
 *       本工程 CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096，故它们会自动落到
 *       PSRAM，不占内部 RAM。上板后请用 monitor_task 的读数复核这一点。
 * @note 幂等。
 */
esp_err_t opus_codec_init(void);

/** @brief 编解码器是否已就绪。 */
bool opus_codec_is_ready(void);

/**
 * @brief 按服务器 hello 给的 audio_params 配置下行解码器。
 *
 * @param sample_rate 下行采样率（通常 24000）
 * @param frame_ms    下行帧长（通常 60）
 *
 * @note 参数与当前不一致时才重建解码器 —— 重建会丢状态，不该每次调用都做。
 *       参照 audio/audio_service.cc:394-399 的做法。
 */
esp_err_t opus_codec_set_down(int sample_rate, int frame_ms);

/** @brief 当前下行配置下，一个包最多解出多少样本。 */
size_t opus_codec_down_frame_samples(void);

/**
 * @brief 编码**恰好一帧**。
 *
 * @param[in]  pcm     必须正好 OPUS_UP_FRAME_SAMPLES 个样本（16kHz 单声道）
 * @param[out] out     输出缓冲
 * @param[in]  out_cap 输出缓冲容量，建议 OPUS_MAX_PACKET_SIZE
 * @param[out] out_len 实际写入字节数
 *
 * @note **不做内部缓冲**：调用方必须交整帧。这是刻意的 ——
 *       多一层"攒够一帧再编"的缓冲，就多一处只有在特定时序下才走到的代码，
 *       而它恰恰是最难测的。audio_service_rec_read() 已保证按帧交付。
 * @note DTX 打开时，静音帧会编成极短的包（几字节），这是**正常现象**，
 *       不是失败 —— 服务器侧同样按 DTX 解码。
 */
esp_err_t opus_codec_encode(const int16_t *pcm, uint8_t *out, size_t out_cap,
                            size_t *out_len);

/**
 * @brief 解码一个下行 Opus 包。
 *
 * @param[in]  in        包数据
 * @param[in]  len       包长度
 * @param[out] pcm       输出缓冲
 * @param[in]  pcm_cap   输出缓冲能放多少样本
 * @param[out] pcm_len   实际解出的样本数
 *
 * @note 丢包时服务器可能发空包，此时 Opus 能靠 PLC（丢包隐藏）补出一帧；
 *       本函数把 len==0 视为合法的"丢包"而不是错误。
 */
esp_err_t opus_codec_decode(const uint8_t *in, size_t len,
                            int16_t *pcm, size_t pcm_cap, size_t *pcm_len);

/**
 * @brief 复位编解码器状态（清掉 DTX / 预测器的历史）。
 * @note 开一段新对话时调用，避免把上一段的话音"尾巴"带进来。
 */
void opus_codec_reset(void);

#ifdef __cplusplus
}
#endif
