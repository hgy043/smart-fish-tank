/**
 * @file opus_codec.c
 * @brief Opus 编解码实现。参数出处与设计取舍见 opus_codec.h。
 */
#include "opus_codec.h"

#include <string.h>
#include "esp_log.h"
#include "opus.h"

static const char *TAG = "opus";

/* 下行缺省值：与 xiaozhi_config.h 的 XZ_DOWN_SAMPLE_RATE_DEF 一致。 */
#define DOWN_RATE_DEF   24000
#define DOWN_FRAME_MS   60

static OpusEncoder *s_enc = NULL;
static OpusDecoder *s_dec = NULL;

static int s_down_rate = DOWN_RATE_DEF;
static int s_down_ms   = DOWN_FRAME_MS;

esp_err_t opus_codec_init(void)
{
    if (s_enc != NULL && s_dec != NULL) {
        return ESP_OK;              /* 幂等 */
    }

    if (s_enc == NULL) {
        int err = OPUS_OK;
        s_enc = opus_encoder_create(OPUS_UP_SAMPLE_RATE, OPUS_UP_CHANNELS,
                                    OPUS_APPLICATION_VOIP, &err);
        if (s_enc == NULL || err != OPUS_OK) {
            ESP_LOGE(TAG, "创建编码器失败: %s", opus_strerror(err));
            s_enc = NULL;
            return ESP_FAIL;
        }
        /* DTX 打开、复杂度 0 —— 与参考实现逐项一致（见头文件的参数出处）。 */
        opus_encoder_ctl(s_enc, OPUS_SET_DTX(1));
        opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(0));
        ESP_LOGI(TAG, "编码器就绪：%dHz %dch %dms/帧（%d 样本/帧，DTX 开，复杂度 0）",
                 OPUS_UP_SAMPLE_RATE, OPUS_UP_CHANNELS, OPUS_UP_FRAME_MS,
                 OPUS_UP_FRAME_SAMPLES);
    }

    if (s_dec == NULL) {
        int err = OPUS_OK;
        s_dec = opus_decoder_create(s_down_rate, 1, &err);
        if (s_dec == NULL || err != OPUS_OK) {
            ESP_LOGE(TAG, "创建解码器失败: %s", opus_strerror(err));
            s_dec = NULL;
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "解码器就绪：%dHz 单声道 %dms/帧", s_down_rate, s_down_ms);
    }

    return ESP_OK;
}

bool opus_codec_is_ready(void)
{
    return (s_enc != NULL) && (s_dec != NULL);
}

esp_err_t opus_codec_set_down(int sample_rate, int frame_ms)
{
    /* 参数非法时**保留原配置**并报错，而不是悄悄用一个默认值 ——
       服务器给的 audio_params 若解析出错，宁可让它出声不对被立刻发现。 */
    if (sample_rate <= 0 || frame_ms <= 0) {
        ESP_LOGE(TAG, "下行参数非法: %dHz %dms", sample_rate, frame_ms);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_dec != NULL && s_down_rate == sample_rate && s_down_ms == frame_ms) {
        return ESP_OK;              /* 没变，不重建（重建会丢解码器状态） */
    }

    if (s_dec != NULL) {
        opus_decoder_destroy(s_dec);
        s_dec = NULL;
    }

    s_down_rate = sample_rate;
    s_down_ms   = frame_ms;

    int err = OPUS_OK;
    s_dec = opus_decoder_create(s_down_rate, 1, &err);
    if (s_dec == NULL || err != OPUS_OK) {
        ESP_LOGE(TAG, "按 %dHz/%dms 重建解码器失败: %s",
                 sample_rate, frame_ms, opus_strerror(err));
        s_dec = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "解码器已按服务器参数重建：%dHz 单声道 %dms/帧（%u 样本/包）",
             s_down_rate, s_down_ms, (unsigned)opus_codec_down_frame_samples());
    return ESP_OK;
}

size_t opus_codec_down_frame_samples(void)
{
    return (size_t)(s_down_rate / 1000 * s_down_ms * 1);
}

esp_err_t opus_codec_encode(const int16_t *pcm, uint8_t *out, size_t out_cap,
                            size_t *out_len)
{
    if (!opus_codec_is_ready() || pcm == NULL || out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 容量按调用方给的 out_cap 传下去 —— opus_encode 只写到这个上限为止，
       不会溢出。**不能**在这里写死 OPUS_MAX_PACKET_SIZE：调用方的缓冲
       可能比它小，那样就是一次实打实的缓冲区溢出。 */
    const int n = opus_encode(s_enc, pcm, OPUS_UP_FRAME_SAMPLES,
                              out, (opus_int32)out_cap);
    if (n < 0) {
        ESP_LOGE(TAG, "编码失败: %s", opus_strerror(n));
        return ESP_FAIL;
    }

    *out_len = (size_t)n;
    return ESP_OK;
}

esp_err_t opus_codec_decode(const uint8_t *in, size_t len,
                            int16_t *pcm, size_t pcm_cap, size_t *pcm_len)
{
    if (!opus_codec_is_ready() || pcm == NULL || pcm_len == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pcm_cap < opus_codec_down_frame_samples()) {
        ESP_LOGE(TAG, "输出缓冲太小：%u < %u",
                 (unsigned)pcm_cap, (unsigned)opus_codec_down_frame_samples());
        return ESP_ERR_INVALID_ARG;
    }

    /* len == 0 表示**丢包**：Opus 会靠 PLC 补一帧，这是合法输入，不是错误。
       xiaozhi 的 OpusDecoderWrapper::Decode 同样把空包交给 opus_decode 处理。 */
    const int n = opus_decode(s_dec, (len > 0) ? in : NULL, (opus_int32)len,
                              pcm, (int)pcm_cap, 0);
    if (n < 0) {
        ESP_LOGW(TAG, "解码失败: %s（包长 %u）", opus_strerror(n), (unsigned)len);
        return ESP_FAIL;
    }

    *pcm_len = (size_t)n;
    return ESP_OK;
}

void opus_codec_reset(void)
{
    if (s_enc != NULL) {
        opus_encoder_ctl(s_enc, OPUS_RESET_STATE);
    }
    if (s_dec != NULL) {
        opus_decoder_ctl(s_dec, OPUS_RESET_STATE);
    }
}
