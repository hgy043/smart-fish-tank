/**
 * @file audio_service.h
 * @brief 音频服务（ES8311 播放 + ES7210 录音，I2S 全双工）
 *
 * 配置来源：xiaozhi-esp32-1.9.4-gec/main/audio/codecs/box_audio_codec.cc
 *          （用户已验证可正常发声）。
 *
 * 关键设计（照搬已验证实现，不要"优化"）：
 *   - I2S0 双通道：TX 走 **std 模式**，RX 走 **TDM 模式**（ES7210 是 4 通道 ADC）；
 *   - 两通道共享 MCLK/BCLK/WS，各自独立配置 din/dout；
 *   - 采样率 24kHz，16bit，MCLK = 256 × fs。
 *
 * ⚠️ 引脚冲突：I2S 的 WS=IO40 与 LCD 的 MOSI=IO40 是同一个 GPIO（见 pinmap C1）。
 * 本模块初始化时会占用它，因此**初始化后必须重新验证 LCD 是否还能显示**。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_SAMPLE_RATE_HZ    24000
#define AUDIO_VOLUME_DEFAULT    70      /* 0-100 */

/**
 * @brief 初始化 I2S 双通道与两个编解码器。
 * @note  依赖 gec_board_i2c_init() 已执行（codec 走 I2C 配置）。
 */
esp_err_t audio_service_init(void);

/** @brief 音频是否已初始化成功。 */
bool audio_service_is_ready(void);

/** @brief 打开/关闭播放通路（含功放使能）。 */
esp_err_t audio_service_enable_output(bool enable);

/** @brief 设置音量 0-100。 */
esp_err_t audio_service_set_volume(int volume);

/**
 * @brief 播放一段单频正弦波（自检用）。
 * @param freq_hz   频率，建议 1000
 * @param duration_ms 时长
 * @param amplitude  幅度 0-32767，建议 8000（避免削顶与过响）
 */
esp_err_t audio_service_play_tone(int freq_hz, int duration_ms, int amplitude);

/** @brief 播放一段静音（用于确认通路无噪声）。 */
esp_err_t audio_service_play_silence(int duration_ms);

/**
 * @brief 采集一段音频并返回其 RMS 电平，用于验证麦克风通路是否工作。
 * @param duration_ms 采集时长
 * @param[out] rms_out 归一化 RMS（0.0 - 1.0）
 */
esp_err_t audio_service_measure_mic(int duration_ms, float *rms_out);

#ifdef __cplusplus
}
#endif
