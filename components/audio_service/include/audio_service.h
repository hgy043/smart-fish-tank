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
 * 麦克风增益（dB）。
 *
 * 【取值出处】xiaozhi `audio/audio_codec.h:16` 的 `AUDIO_CODEC_DEFAULT_MIC_GAIN`。
 * 那里的注释写着 `// 从30.0改为37.5` —— 是他们在同款板子上**上调过**的值。
 *
 * ⚠️ 必须显式设置：ES7210 上电后的默认增益很低，不设的话录音"有波形但极轻"，
 *    本地看 RMS 有数、远端 ASR 却听不出内容 —— 现象是"服务器不回话"，
 *    而日志上一切正常，极难定位。（2026-10-13 实际踩过。）
 */
#define AUDIO_MIC_GAIN_DB       37.5f

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
 * @brief 播放一段**已经是 24kHz 单声道**的 PCM（P7 下行语音）。
 *
 * @param pcm     样本指针（可为非 DMA 内存 —— 内部经 esp_codec_dev 转交）
 * @param samples 样本数
 *
 * @note 与 `play_tone` 共用同一条已验证的写入路径（esp_codec_dev_write），
 *       数据来源不同而已。**阻塞**直到数据被 I2S 取走。
 */
esp_err_t audio_service_play_pcm(const int16_t *pcm, size_t samples);

/* ==========================================================================
 * 流式录音（P7 语音对话）
 *
 * 【为什么需要新接口，而不是复用 measure_mic()】
 * `measure_mic()` 是**一次性**的：当场 open→读→close，只回一个 RMS 标量，
 * PCM 在函数内就被 free 掉了。语音对话需要的是「持续取帧」。
 * 而且它每次调用都 open/close codec —— 本文件顶部已记录过
 * 「esp_codec_dev 的通道启停不做幂等检查」，反复 open/close 会失败。
 * 所以录音通路必须是**常开**的（与播放侧同样的处理），只切标记。
 *
 * 【格式】对外只给 **16 kHz 单声道 int16**。
 * 硬件侧是 24 kHz（ES7210 是 4 通道 ADC，但本例只采 1 路 —— 见下），
 * 本组件内部完成 24k → 16k 重采样（比例恰为 3:2）。
 * 上层因此可以直接把读到的 PCM 交给 Opus 编码器（上行格式见
 * xiaozhi_config.h 的 XZ_UP_*）。
 *
 * ⚠️ **"4 通道"与"读到几通道"是两回事**：esp_codec_dev 会按 channel_mask
 *    重配 TDM 槽位，我们给 CH0 就只采 1 路，读回来的是单声道 24kHz。
 *    完整来龙去脉见 audio_service.c "流式录音"段顶部的 ③ ——
 *    这一条曾让本项目误判"I2S 时钟差 4 倍"，别再凭直觉推断。
 * ========================================================================== */

/** 上行帧长：60ms @16kHz = 960 样本。与 xiaozhi 的 OPUS_FRAME_DURATION_MS 一致。 */
#define AUDIO_REC_FRAME_SAMPLES  960

/** 录音通路是否已打开 */
bool audio_service_rec_active(void);

/** @brief 打开录音通路（幂等）。 */
esp_err_t audio_service_rec_start(void);

/** @brief 关闭录音通路（幂等）。 */
esp_err_t audio_service_rec_stop(void);

/**
 * @brief 读一帧 **16 kHz 单声道** PCM。
 *
 * @param[out] buf  输出缓冲，至少 `want` 个 int16
 * @param[in]  want 期望样本数，**必须是 AUDIO_REC_FRAME_SAMPLES 的整数倍**
 * @param[out] got  实际读到的样本数
 *
 * @warning **阻塞**调用（内部等 I2S 数据）。不可在中断里调用。
 * @note  需要先 `audio_service_rec_start()`。
 */
esp_err_t audio_service_rec_read(int16_t *buf, size_t want, size_t *got);

/**
 * @brief 【诊断】喇叭→麦克风 回环自证：板子自己发声，同时录音。
 *
 * 每 500ms 交替「响 1kHz / 静音」，把**响**与**静**下的 RMS 分开统计。
 * 「响」明显高于「静」即麦克风工作正常（首轮实测对比 ≈15.8 倍）。
 *
 * 与"让人在正确时刻说话"相比，它把不受控的前提去掉了：
 * 声源是板子自己，结论与用户在不在说话无关。
 * 它同时报出**实测有效采样率** —— 这一项曾在首轮暴露出
 * "配 24000 却只读到 6000" 的假象（真因见 audio_service.c 的 ③）。
 *
 * @warning 需要播放通路已打开（`audio_service_enable_output(true)`），
 *          否则喇叭不响，试验无意义。
 * @note 本函数自己 open/close 输入 codec，不要与 rec_start/rec_stop 交叉调用。
 */
esp_err_t audio_service_mic_loopback(int duration_ms);

#ifdef __cplusplus
}
#endif
