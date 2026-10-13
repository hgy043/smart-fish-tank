/**
 * @file audio_service.c
 * @brief 音频服务实现
 *
 * I2S 与编解码器配置逐项对齐 box_audio_codec.cc（已验证可发声），
 * 未经验证的参数不得改动：MCLK 倍数 256、TX=std/RX=TDM、dma 6×240、
 * left_align=true(TX) / false(RX)、es8311 hw_gain 5.0V/3.3V。
 */
#include "audio_service.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "driver/gpio.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

#include "gec_board.h"

static const char *TAG = "audio";

/* 与 box_audio_codec.cc 一致的 DMA 参数 */
#define AUDIO_DMA_DESC_NUM   6
#define AUDIO_DMA_FRAME_NUM  240

/* es8311/7210 的 I2C 地址：**8 位写地址形式**，esp_codec_dev 内部会 >>1。
   这是从已验证代码学到的关键点：直接传 7 位地址会配错器件。 */
#define ES8311_ADDR_8BIT     ES8311_CODEC_DEFAULT_ADDR   /* 0x30 -> 7bit 0x18 */
#define ES7210_ADDR_8BIT     0x82                        /* -> 7bit 0x41 */

static i2s_chan_handle_t        s_tx = NULL;
static i2s_chan_handle_t        s_rx = NULL;
static esp_codec_dev_handle_t   s_out_dev = NULL;
static esp_codec_dev_handle_t   s_in_dev  = NULL;

static const audio_codec_data_if_t *s_data_if = NULL;
static const audio_codec_ctrl_if_t *s_out_ctrl = NULL;
static const audio_codec_ctrl_if_t *s_in_ctrl  = NULL;
static const audio_codec_gpio_if_t *s_gpio_if  = NULL;
static const audio_codec_if_t      *s_out_codec = NULL;
static const audio_codec_if_t      *s_in_codec  = NULL;

static bool s_ready = false;
static bool s_output_on = false;
static int  s_volume = AUDIO_VOLUME_DEFAULT;

/* 播放缓冲：24kHz * 2B * 0.1s = 4.8KB，用内部 DMA 内存 */
#define TONE_CHUNK_MS  100
#define TONE_CHUNK_SAMPLES (AUDIO_SAMPLE_RATE_HZ * TONE_CHUNK_MS / 1000)

/* ==========================================================================
 * 初始化
 * ========================================================================== */
static esp_err_t create_duplex_channels(void)
{
    const i2s_chan_config_t chan_cfg = {
        .id            = I2S_NUM_0,
        .role          = I2S_ROLE_MASTER,
        .dma_desc_num  = AUDIO_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_DMA_FRAME_NUM,
        .auto_clear_after_cb  = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };

    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建 I2S 通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* --- TX：std 模式（ES8311 播放） --- */
    const i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_SAMPLE_RATE_HZ,
            .clk_src        = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode      = I2S_SLOT_MODE_STEREO,
            .slot_mask      = I2S_STD_SLOT_BOTH,
            .ws_width       = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol         = false,
            .bit_shift      = true,
            .left_align     = true,
            .big_endian     = false,
            .bit_order_lsb  = false,
        },
        .gpio_cfg = {
            .mclk = GEC_I2S_MCLK_GPIO,
            .bclk = GEC_I2S_BCLK_GPIO,
            .ws   = GEC_I2S_WS_GPIO,
            .dout = GEC_I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,      /* TX 通道不管输入 */
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    /* --- RX：TDM 模式（ES7210 是 4 通道 ADC） --- */
    const i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_SAMPLE_RATE_HZ,
            .clk_src        = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
            .bclk_div       = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode      = I2S_SLOT_MODE_STEREO,
            .slot_mask      = (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 |
                                                    I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width       = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol         = false,
            .bit_shift      = true,
            .left_align     = false,
            .big_endian     = false,
            .bit_order_lsb  = false,
            .skip_mask      = false,
            .total_slot     = I2S_TDM_AUTO_SLOT_NUM,
        },
        .gpio_cfg = {
            .mclk = GEC_I2S_MCLK_GPIO,
            .bclk = GEC_I2S_BCLK_GPIO,
            .ws   = GEC_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,      /* RX 通道不管输出 */
            .din  = GEC_I2S_DIN_GPIO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    ret = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化 TX(std) 模式失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_init_tdm_mode(s_rx, &tdm_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化 RX(TDM) 模式失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2S 双工通道就绪: MCLK=IO%d BCLK=IO%d WS=IO%d DIN=IO%d DOUT=IO%d @%dHz",
             GEC_I2S_MCLK_GPIO, GEC_I2S_BCLK_GPIO, GEC_I2S_WS_GPIO,
             GEC_I2S_DIN_GPIO, GEC_I2S_DOUT_GPIO, AUDIO_SAMPLE_RATE_HZ);
    return ESP_OK;
}

esp_err_t audio_service_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = gec_board_get_i2c_bus();
    if (bus == NULL) {
        ESP_LOGE(TAG, "I2C 总线未就绪，请先调用 gec_board_i2c_init()");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = create_duplex_channels();
    if (ret != ESP_OK) {
        return ret;
    }

    /* 注意：这些 codec 库接口的入参是**非 const** 指针（见 esp_codec_dev
       的 interface 头文件），不能传 const 结构体的地址，否则在
       -Werror=discarded-qualifiers 下直接编译失败。 */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port      = I2S_NUM_0,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_data_if == NULL) {
        ESP_LOGE(TAG, "创建 I2S data_if 失败");
        return ESP_FAIL;
    }

    s_gpio_if = audio_codec_new_gpio();
    if (s_gpio_if == NULL) {
        ESP_LOGE(TAG, "创建 gpio_if 失败");
        return ESP_FAIL;
    }

    /* --- 播放侧：ES8311 --- */
    audio_codec_i2c_cfg_t out_i2c_cfg = {
        .port       = (i2c_port_t)0,
        .addr       = ES8311_ADDR_8BIT,
        .bus_handle = bus,
    };
    s_out_ctrl = audio_codec_new_i2c_ctrl(&out_i2c_cfg);
    if (s_out_ctrl == NULL) {
        ESP_LOGE(TAG, "创建 ES8311 I2C 控制接口失败（地址 0x%02X）", ES8311_ADDR_8BIT);
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es8311_cfg = { 0 };
    es8311_cfg.ctrl_if     = s_out_ctrl;
    es8311_cfg.gpio_if     = s_gpio_if;
    es8311_cfg.codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC;
    /* pa_pin 传 NC：本板的功放使能经 PCA9557 P1，由 audio_service_enable_output()
       自行控制，不走 esp_codec_dev 的 GPIO 功放脚。 */
    es8311_cfg.pa_pin      = GPIO_NUM_NC;
    es8311_cfg.use_mclk    = true;
    es8311_cfg.hw_gain.pa_voltage        = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    s_out_codec = es8311_codec_new(&es8311_cfg);
    if (s_out_codec == NULL) {
        ESP_LOGE(TAG, "创建 ES8311 codec 接口失败");
        return ESP_FAIL;
    }

    const esp_codec_dev_cfg_t out_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_out_codec,
        .data_if  = s_data_if,
    };
    s_out_dev = esp_codec_dev_new((esp_codec_dev_cfg_t *)&out_dev_cfg);
    if (s_out_dev == NULL) {
        ESP_LOGE(TAG, "创建 ES8311 设备失败");
        return ESP_FAIL;
    }

    /* --- 录音侧：ES7210 --- */
    audio_codec_i2c_cfg_t in_i2c_cfg = {
        .port       = (i2c_port_t)0,
        .addr       = ES7210_ADDR_8BIT,
        .bus_handle = bus,
    };
    s_in_ctrl = audio_codec_new_i2c_ctrl(&in_i2c_cfg);
    if (s_in_ctrl == NULL) {
        ESP_LOGE(TAG, "创建 ES7210 I2C 控制接口失败（地址 0x%02X）", ES7210_ADDR_8BIT);
        return ESP_FAIL;
    }

    es7210_codec_cfg_t es7210_cfg = { 0 };
    es7210_cfg.ctrl_if = s_in_ctrl;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 |
                              ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    s_in_codec = es7210_codec_new(&es7210_cfg);
    if (s_in_codec == NULL) {
        ESP_LOGE(TAG, "创建 ES7210 codec 接口失败");
        return ESP_FAIL;
    }

    const esp_codec_dev_cfg_t in_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_in_codec,
        .data_if  = s_data_if,
    };
    s_in_dev = esp_codec_dev_new((esp_codec_dev_cfg_t *)&in_dev_cfg);
    if (s_in_dev == NULL) {
        ESP_LOGE(TAG, "创建 ES7210 设备失败");
        return ESP_FAIL;
    }

    s_ready = true;
    ESP_LOGI(TAG, "音频服务就绪（ES8311 播放 / ES7210 录音）");
    return ESP_OK;
}

bool audio_service_is_ready(void)
{
    return s_ready;
}

/* ==========================================================================
 * 播放通路
 * ========================================================================== */
esp_err_t audio_service_enable_output(bool enable)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enable == s_output_on) {
        return ESP_OK;
    }

    if (enable) {
        const esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel         = 1,
            .channel_mask    = 0,
            .sample_rate     = AUDIO_SAMPLE_RATE_HZ,
            .mclk_multiple   = 0,
        };
        /* 逐步打点：任何一步失败都要能一眼看出是哪一步。 */
        int oret = esp_codec_dev_open(s_out_dev, (esp_codec_dev_sample_info_t *)&fs);
        ESP_LOGI(TAG, "  [步骤1] esp_codec_dev_open(ES8311) -> %d %s",
                 oret, (oret == ESP_CODEC_DEV_OK) ? "OK" : "失败");
        if (oret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "打开 ES8311 失败: %d", oret);
            return ESP_FAIL;
        }

        int vret = esp_codec_dev_set_out_vol(s_out_dev, s_volume);
        ESP_LOGI(TAG, "  [步骤2] 设置音量 %d -> %d %s",
                 s_volume, vret, (vret == ESP_CODEC_DEV_OK) ? "OK" : "失败");

        /* ⚠️ 不要手动 i2s_channel_enable(s_tx)：esp_codec_dev_open() 内部已经启用。 */

        /* 功放使能：PCA9557 P1（厂家做法，PA_EN 接 NS4150B 的 CTRL）。 */
        esp_err_t pret = gec_pa_en(1);
        ESP_LOGI(TAG, "  [步骤4] PCA9557 P1 功放使能 -> %s", esp_err_to_name(pret));
    } else {
        /* ⚠️ 这里**不要**调 esp_codec_dev_close()！
         *
         * 真因（实测）：esp_codec_dev 的 `_i2s_drv_enable()` 直接调
         * i2s_channel_enable/disable 而**不做幂等检查**：
         *     if (enable) ret = i2s_channel_enable(channel);
         *     else        ret = i2s_channel_disable(channel);
         * 而 ESP-IDF 的 i2s_channel_disable() 在通道未启用时返回
         * ESP_ERR_INVALID_STATE。于是「关→开」循环会得到：
         *     E i2s_common: i2s_channel_disable: the channel has not been enabled yet
         *     E i2s_common: i2s_channel_enable: the channel has already enabled
         * 实测现象就是「喇叭只响第一次」。
         *
         * 因此本实现**让 codec 通路常开**，只切功放使能来静音。
         * 代价是 I2S 与 MCLK 持续运行（约几十 mW），换来可靠的重复播放。 */
        esp_err_t pret = gec_pa_en(0);
        ESP_LOGI(TAG, "  [步骤4] PCA9557 P1 功放关闭 -> %s", esp_err_to_name(pret));
    }

    s_output_on = enable;
    ESP_LOGI(TAG, "播放通路 %s（codec 常开，仅切功放）", enable ? "已打开" : "已静音");
    return ESP_OK;
}

esp_err_t audio_service_set_volume(int volume)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (volume < 0)   { volume = 0; }
    if (volume > 100) { volume = 100; }
    s_volume = volume;
    const esp_err_t ret = esp_codec_dev_set_out_vol(s_out_dev, s_volume);
    return (ret == ESP_CODEC_DEV_OK) ? ESP_OK : ESP_FAIL;
}

/* ==========================================================================
 * 测试音
 * ========================================================================== */
esp_err_t audio_service_play_tone(int freq_hz, int duration_ms, int amplitude)
{
    if (!s_ready || !s_output_on) {
        return ESP_ERR_INVALID_STATE;
    }
    if (freq_hz <= 0 || duration_ms <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (amplitude > 32767) { amplitude = 32767; }
    if (amplitude < 0)     { amplitude = 0; }

    int16_t *buf = heap_caps_malloc(TONE_CHUNK_SAMPLES * sizeof(int16_t),
                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (buf == NULL) {
        ESP_LOGE(TAG, "测试音缓冲分配失败（%u 字节）",
                 (unsigned)(TONE_CHUNK_SAMPLES * sizeof(int16_t)));
        return ESP_ERR_NO_MEM;
    }

    const int total_chunks = duration_ms / TONE_CHUNK_MS;
    double phase = 0.0;
    const double phase_step = 2.0 * M_PI * (double)freq_hz / (double)AUDIO_SAMPLE_RATE_HZ;

    ESP_LOGI(TAG, "播放 %dHz 测试音 %dms（幅度 %d）", freq_hz, duration_ms, amplitude);

    for (int c = 0; c < total_chunks; c++) {
        for (int i = 0; i < TONE_CHUNK_SAMPLES; i++) {
            buf[i] = (int16_t)(sin(phase) * amplitude);
            phase += phase_step;
            if (phase > 2.0 * M_PI) {
                phase -= 2.0 * M_PI;
            }
        }

        size_t written = 0;
        esp_err_t ret = i2s_channel_write(s_tx, buf,
                                          TONE_CHUNK_SAMPLES * sizeof(int16_t),
                                          &written, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "写入 I2S TX 失败: %s", esp_err_to_name(ret));
            free(buf);
            return ret;
        }
    }

    free(buf);
    ESP_LOGI(TAG, "测试音播放完成");
    return ESP_OK;
}

esp_err_t audio_service_play_silence(int duration_ms)
{
    if (!s_ready || !s_output_on) {
        return ESP_ERR_INVALID_STATE;
    }

    int16_t *buf = heap_caps_calloc(TONE_CHUNK_SAMPLES, sizeof(int16_t),
                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const int total_chunks = duration_ms / TONE_CHUNK_MS;
    for (int c = 0; c < total_chunks; c++) {
        size_t written = 0;
        esp_err_t ret = i2s_channel_write(s_tx, buf,
                                          TONE_CHUNK_SAMPLES * sizeof(int16_t),
                                          &written, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            free(buf);
            return ret;
        }
    }

    free(buf);
    return ESP_OK;
}

/* ==========================================================================
 * 麦克风电平测量
 * ========================================================================== */
esp_err_t audio_service_measure_mic(int duration_ms, float *rms_out)
{
    if (!s_ready || rms_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 录音按 4 通道打开（ES7210 是 4 通道 ADC），与已验证实现一致 */
    const esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 4,
        .channel_mask    = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate     = AUDIO_SAMPLE_RATE_HZ,
        .mclk_multiple   = 0,
    };
    int ret = esp_codec_dev_open(s_in_dev, (esp_codec_dev_sample_info_t *)&fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "打开 ES7210 失败: %d", ret);
        return ESP_FAIL;
    }

    /* 同播放侧：esp_codec_dev_open() 已启用 I2S RX 通道，不要手动再启用。
       下面的 esp_codec_dev_close() 会负责停用。 */

    const int chunk = 480;      /* 20ms @24kHz */
    int16_t *buf = heap_caps_malloc(chunk * sizeof(int16_t) * 4,
                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (buf == NULL) {
        (void)esp_codec_dev_close(s_in_dev);
        return ESP_ERR_NO_MEM;
    }

    double sum_sq = 0.0;
    long   count  = 0;
    const int chunks = duration_ms / 20;

    for (int c = 0; c < chunks; c++) {
        size_t got = 0;
        esp_err_t rret = i2s_channel_read(s_rx, buf, chunk * sizeof(int16_t) * 4, &got,
                                          pdMS_TO_TICKS(500));
        if (rret != ESP_OK) {
            ESP_LOGW(TAG, "读取 I2S RX 失败: %s", esp_err_to_name(rret));
            break;
        }
        const int n = (int)(got / sizeof(int16_t));
        for (int i = 0; i < n; i++) {
            const double v = (double)buf[i] / 32768.0;
            sum_sq += v * v;
            count++;
        }
    }

    free(buf);
    (void)esp_codec_dev_close(s_in_dev);

    if (count == 0) {
        return ESP_FAIL;
    }
    *rms_out = (float)sqrt(sum_sq / (double)count);
    return ESP_OK;
}
