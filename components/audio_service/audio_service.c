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
#include "esp_timer.h"
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

        /* ⚠️ 用 esp_codec_dev_write，**不是**裸 i2s_channel_write。
         *
         * 曾经写的是 `i2s_channel_write(s_tx, buf, N*2, ...)`，把**单声道**
         * 样本直接灌进配置为 **stereo** 的 TX 通道 —— 那样一个样本只占半个
         * 立体声帧，N 个样本只花 N/48000 秒，于是**实际播放速度是标称的 2 倍**
         * （1kHz 听成 2kHz）。P0.5 只验了"有没有声音"，听不出音高，
         * 所以这个错误一直没暴露。
         *
         * esp_codec_dev_write 走的是已验证实现（xiaozhi BoxAudioCodec::Write）
         * 同一条路：codec 是以 `.channel = 1` 打开的，由它负责单声道到
         * 硬件槽位的映射，写进去多少样本就播多少时间。 */
        const int wret = esp_codec_dev_write(s_out_dev, buf,
                                             (int)(TONE_CHUNK_SAMPLES * sizeof(int16_t)));
        if (wret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "写入 codec 失败: %d", wret);
            free(buf);
            return ESP_FAIL;
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
        /* 与 play_tone 同理：走 esp_codec_dev_write，不裸写 I2S */
        if (esp_codec_dev_write(s_out_dev, buf,
                                (int)(TONE_CHUNK_SAMPLES * sizeof(int16_t)))
                != ESP_CODEC_DEV_OK) {
            free(buf);
            return ESP_FAIL;
        }
    }

    free(buf);
    return ESP_OK;
}

/**
 * 播放一段**已经是 24kHz 单声道**的 PCM（P7 下行语音）。
 *
 * 与 play_tone 共用同一条已验证的写入路径（esp_codec_dev_write），
 * 区别只是数据来自网络解码而不是本地生成。
 */
esp_err_t audio_service_play_pcm(const int16_t *pcm, size_t samples)
{
    if (!s_ready || !s_output_on || pcm == NULL || samples == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const int wret = esp_codec_dev_write(s_out_dev, (void *)pcm,
                                         (int)(samples * sizeof(int16_t)));
    if (wret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "播放 PCM 失败: %d", wret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ==========================================================================
 * 流式录音（P7 语音对话）
 *
 * 设计取舍见 audio_service.h。这里补充三处实现层面的决定：
 *
 * ① **通路常开，只切标记**。理由与播放侧同源（见 enable_output 的长注释）：
 *    esp_codec_dev 的通道启停不做幂等检查，反复 open/close 会失败。
 *
 * ② **24k → 16k 用无状态抽取**。比例恰为 3:2，每 3 个输入样本产 2 个输出，
 *    不需要像通用重采样器那样维护小数位置 —— 见 resample_24k_to_16k()。
 *    不用 esp-opus-encoder 里那个 OpusResampler，是为了分层：
 *    audio_service 不该依赖编解码组件。
 *
 * ③ **读进来的是单声道 24kHz，不是 4 通道。**
 *    ⚠️ 这一点 2026-10-13 实测踩过坑，写清楚免得再错：
 *    ES7210 确实是 4 通道 ADC、也确实工作在 TDM 4 槽，但 esp_codec_dev 的
 *    set_drv_fs()（platform/audio_codec_data_i2s.c）会**按 channel_mask
 *    重新配置 TDM 槽位**：
 *        slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(slot_bits,
 *                       I2S_SLOT_MODE_STEREO, (i2s_tdm_slot_mask_t)fs->channel_mask);
 *        slot_cfg.total_slot = fs->channel;
 *    我们给的 channel_mask = CH0 ⇒ **只采槽位 0**，DMA 交出来的是 24kHz
 *    单声道、24000 样本/秒。实测正是 23940 样本/秒。
 *    （xiaozhi 给 CH0|CH1 ⇒ 2 槽位 ⇒ 48000 样本/秒 —— 这正是它每 60ms
 *      读 2880 个 int16 的由来，数字对得上，可作交叉验证。）
 *
 *    此前一度按"480 帧 × 4 通道"解读，算出 6000 帧/秒，误判为"I2S 时钟差 4 倍"。
 *    真相是 **I2S 一直是 24kHz，从头到尾没坏**，错的是对缓冲的解读；连带的
 *    "四路通道 RMS 完全一致"也是同一误读的表现 —— 那本就是同一路信号的相邻
 *    时间样本。教训：**"配置的采样率"与"缓冲里实际装了什么"是两件事，中间隔着
 *    esp_codec_dev 的重配置，必须实测，不能推断。**
 * ========================================================================== */
#define AUDIO_REC_HW_CHANNELS   4    /* ES7210 是 4 通道 ADC（描述硬件，不是缓冲区布局） */
#define AUDIO_REC_IN_PER_OUT    3    /* 24k → 16k：每 2 个输出样本对应 3 个输入样本 */

static bool     s_rec_on = false;
static int16_t *s_rec_in = NULL;   /* 24k 单声道原始样本（DMA 可达） */

bool audio_service_rec_active(void)
{
    return s_rec_on;
}

esp_err_t audio_service_rec_start(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_rec_on) {
        return ESP_OK;              /* 幂等 */
    }

    /* channel = 4 描述的是**硬件**（ES7210 是 4 通道 ADC），必须这么填；
       channel_mask = CH0 才是**我们要哪一路** —— 它同时决定 DMA 实际采几个
       槽位，见本段顶部的 ③。 */
    const esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = AUDIO_REC_HW_CHANNELS,
        .channel_mask    = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate     = AUDIO_SAMPLE_RATE_HZ,
        .mclk_multiple   = 0,
    };
    const int ret = esp_codec_dev_open(s_in_dev, (esp_codec_dev_sample_info_t *)&fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "打开 ES7210 失败: %d", ret);
        return ESP_FAIL;
    }

    /* 麦克风增益必须显式设 —— 不能指望上电默认值。
       出处与踩坑经过见 audio_service.h 的 AUDIO_MIC_GAIN_DB。
       与已验证实现一致：对 CH0 这一路设。 */
    const int gret = esp_codec_dev_set_in_channel_gain(
        s_in_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), AUDIO_MIC_GAIN_DB);
    if (gret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "设置麦克风增益失败: %d（录音可能极轻）", gret);
    }

    if (s_rec_in == NULL) {
        const size_t in_blk = AUDIO_REC_FRAME_SAMPLES * AUDIO_REC_IN_PER_OUT / 2;
        s_rec_in = heap_caps_malloc(in_blk * sizeof(int16_t),
                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_rec_in == NULL) {
            ESP_LOGE(TAG, "录音缓冲分配失败（%u 字节）",
                     (unsigned)(in_blk * sizeof(int16_t)));
            (void)esp_codec_dev_close(s_in_dev);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "录音缓冲就绪：%u 字节（内部 DMA）",
                 (unsigned)(in_blk * sizeof(int16_t)));
    }

    s_rec_on = true;
    ESP_LOGI(TAG, "录音通路已打开（硬件 24kHz 单声道 → 对外 16kHz 单声道）");
    return ESP_OK;
}

esp_err_t audio_service_rec_stop(void)
{
    if (!s_rec_on) {
        return ESP_OK;
    }
    s_rec_on = false;
    (void)esp_codec_dev_close(s_in_dev);
    ESP_LOGI(TAG, "录音通路已关闭");
    return ESP_OK;
}

/** 24k → 16k：精确 3:2。每 3 个输入样本产 2 个输出：
 *      out[2m]   = in[3m]                       （直接取）
 *      out[2m+1] = (in[3m+1] + in[3m+2]) / 2    （相邻平均，兼作极轻的抗混叠）
 *  全程**无状态** —— 这正是选 3:2 这个整数比的好处。 */
static void resample_24k_to_16k(const int16_t *in, size_t in_n,
                                int16_t *out, size_t out_n)
{
    const size_t pairs = out_n / 2;
    for (size_t m = 0; m < pairs; m++) {
        const size_t i = m * AUDIO_REC_IN_PER_OUT;
        if (i + 2 >= in_n) {
            out[2 * m]     = 0;
            out[2 * m + 1] = 0;
            continue;
        }
        out[2 * m]     = in[i];
        out[2 * m + 1] = (int16_t)(((int32_t)in[i + 1] + (int32_t)in[i + 2]) / 2);
    }
}

esp_err_t audio_service_rec_read(int16_t *buf, size_t want, size_t *got)
{
    if (!s_ready || !s_rec_on || buf == NULL || got == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (want == 0 || (want % AUDIO_REC_FRAME_SAMPLES) != 0) {
        ESP_LOGE(TAG, "want 必须是 %d 的整数倍（收到 %u）",
                 AUDIO_REC_FRAME_SAMPLES, (unsigned)want);
        return ESP_ERR_INVALID_ARG;
    }

    *got = 0;
    size_t out_done = 0;

    while (out_done < want) {
        /* 一次搬一个小块：输出 960（16k）对应输入 1440（24k）。 */
        size_t out_here = want - out_done;
        if (out_here > AUDIO_REC_FRAME_SAMPLES) {
            out_here = AUDIO_REC_FRAME_SAMPLES;
        }
        const size_t in_here = out_here * AUDIO_REC_IN_PER_OUT / 2;

        /* 用 esp_codec_dev_read 而**不是**裸 i2s_channel_read —— 与已验证的
           xiaozhi（BoxAudioCodec::Read）同一条路。原因不只是"照搬"：
           codec 是经 esp_codec_dev_open 打开的，通道的启停归它管，
           绕过它直接读 I2S 是在两份所有者之间踩线。
           另：esp_codec_dev_read 内部就是 i2s_channel_read，
           所以目标缓冲同样必须 DMA 可达（s_rec_in 已按此分配）。 */
        const int ret = esp_codec_dev_read(s_in_dev, s_rec_in,
                                           (int)(in_here * sizeof(int16_t)));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "录音读取失败: %d（本轮已取 %u 样本）",
                     ret, (unsigned)out_done);
            break;
        }

        resample_24k_to_16k(s_rec_in, in_here, buf + out_done, out_here);
        out_done += out_here;
    }

    *got = out_done;
    return (out_done > 0) ? ESP_OK : ESP_FAIL;
}

/* ==========================================================================
 * 诊断：喇叭 → 麦克风 回环自证
 *
 * 【为什么要做这个】
 * "麦克风到底响不响"这件事不能靠"让人在正确时刻说话"来测 —— 那个前提
 * 不受控。2026-10-13 实测就卡在这里：四路 RMS 全程 0.0002，
 * 无法区分"麦克风不响"与"人没说话"。
 *
 * 本项目已因未受控观测栽过跟头（见 app_config.h 中背光结论被自己推翻那段），
 * 同样的坑不踩第二次。**把声源换成板子自己**：
 * 每 500ms 交替「响 1kHz / 静音」共 8 轮，同时录音。
 * 麦克风通不通，看「响」与「静」的 RMS 对比即可，与用户在不在说话无关。
 *
 * 【首轮实测结论（2026-10-13）】四路对比均 ≈15.8 倍 ⇒ **麦克风通路正常**。
 * 同时该试验测出「实测有效速率」——正是它暴露了
 * "配 24000 却只读到 6000" 的假象，进而挖出 esp_codec_dev 按 channel_mask
 * 重配 TDM 槽位这件事（见本文件"流式录音"段顶部的 ③）。
 * 所以本函数不只是"测麦克风"，它是这条链上**唯一不依赖假设**的观测点。
 *
 * ⚠️ 需要播放通路已打开（功放使能），否则喇叭不响，本试验无效。
 * ========================================================================== */

/** 交替相位的长度。500ms 足够长到不被 DMA 抖动淹没，又短到 8 轮只占 8 秒。 */
#define LOOPBACK_PHASE_MS   500
#define LOOPBACK_TONE_HZ    1000
#define LOOPBACK_AMP        20000   /* 够响以便被板上麦克风拾取，又不至于削顶 */

esp_err_t audio_service_mic_loopback(int duration_ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_output_on) {
        ESP_LOGE(TAG, "播放通路未打开 —— 喇叭不响，回环试验无效");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_rec_on) {
        ESP_LOGE(TAG, "录音通路已打开，请先 audio_service_rec_stop()");
        return ESP_ERR_INVALID_STATE;
    }

    /* 与 rec_start 用完全相同的参数 —— 试验要测的就是录音真正走的那条路，
       参数只要有一点不同，结论就不能外推。 */
    const esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = AUDIO_REC_HW_CHANNELS,
        .channel_mask    = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate     = AUDIO_SAMPLE_RATE_HZ,
        .mclk_multiple   = 0,
    };
    if (esp_codec_dev_open(s_in_dev, (esp_codec_dev_sample_info_t *)&fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "打开 ES7210 失败");
        return ESP_FAIL;
    }
    /* 与 rec_start 用**完全相同**的增益 —— 试验要测的就是录音真正走的那条路，
       参数差一点，结论就不能外推（增益尤其：不设的话回环对比会假性偏低）。 */
    (void)esp_codec_dev_set_in_channel_gain(s_in_dev,
                                            ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
                                            AUDIO_MIC_GAIN_DB);

    const int chunk_tx = AUDIO_SAMPLE_RATE_HZ / 50;   /* 20ms */
    const int chunk_rx = AUDIO_SAMPLE_RATE_HZ / 50;   /* 20ms（单声道 24k） */

    int16_t *tx = heap_caps_malloc(chunk_tx * sizeof(int16_t),
                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    int16_t *rx = heap_caps_malloc(chunk_rx * sizeof(int16_t),
                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (tx == NULL || rx == NULL) {
        free(tx);
        free(rx);
        (void)esp_codec_dev_close(s_in_dev);
        return ESP_ERR_NO_MEM;
    }

    const int64_t t_start    = esp_timer_get_time();
    const int64_t t_end      = t_start + (int64_t)duration_ms * 1000;
    int64_t       next_phase = t_start + (int64_t)LOOPBACK_PHASE_MS * 1000;
    bool          tone_on    = true;
    double        phase      = 0.0;
    const double  phase_step = 2.0 * M_PI * (double)LOOPBACK_TONE_HZ
                               / (double)AUDIO_SAMPLE_RATE_HZ;

    long   tx_samples = 0, rx_samples = 0;
    double sec_sq = 0.0;
    long   sec_n  = 0;
    /* 按"响"和"静"分别累计 —— **对比度才是判据** */
    double on_sq  = 0.0, off_sq = 0.0;
    long   on_n   = 0,   off_n  = 0;

    ESP_LOGW(TAG, "===== 回环自证：喇叭每 %dms 交替 响%dHz / 静音，共 %.1f 秒 =====",
             LOOPBACK_PHASE_MS, LOOPBACK_TONE_HZ, (double)duration_ms / 1000.0);
    ESP_LOGW(TAG, "  判据：[响] 行的 RMS 明显高于 [静] 行 ⇒ 麦克风确实在拾音");

    while (esp_timer_get_time() < t_end) {
        /* --- 生成并写出一个 TX 块 ---
           与 play_tone() 走同一条已验证的写入路径（esp_codec_dev_write），
           不裸写 I2S —— 裸写会把单声道样本灌进 stereo 通道，
           播出来是 2 倍速、高八度，作为"已知频率的声源"就不成立了。
           本试验一律以墙上时钟为准，不按样本数折算时间。 */
        for (int i = 0; i < chunk_tx; i++) {
            tx[i] = tone_on ? (int16_t)(sin(phase) * LOOPBACK_AMP) : 0;
            phase += phase_step;
            if (phase > 2.0 * M_PI) {
                phase -= 2.0 * M_PI;
            }
        }
        if (esp_codec_dev_write(s_out_dev, tx, (int)(chunk_tx * sizeof(int16_t)))
                == ESP_CODEC_DEV_OK) {
            tx_samples += chunk_tx;
        }

        /* --- 读一个 RX 块（单声道 24k） --- */
        if (esp_codec_dev_read(s_in_dev, rx, (int)(chunk_rx * sizeof(int16_t)))
                == ESP_CODEC_DEV_OK) {
            for (int i = 0; i < chunk_rx; i++) {
                const double v = (double)rx[i] / 32768.0;
                sec_sq += v * v;
                if (tone_on) {
                    on_sq += v * v;
                } else {
                    off_sq += v * v;
                }
            }
            rx_samples += chunk_rx;
            sec_n += chunk_rx;
            if (tone_on) {
                on_n += chunk_rx;
            } else {
                off_n += chunk_rx;
            }
        }

        if (esp_timer_get_time() >= next_phase) {
            tone_on = !tone_on;
            next_phase += (int64_t)LOOPBACK_PHASE_MS * 1000;
            if (sec_n > 0) {
                /* 刚翻转过，所以上一相位用的是相反的那个名字 */
                ESP_LOGW(TAG, "  [%s] RMS=%.5f", tone_on ? "静" : "响",
                         sqrt(sec_sq / (double)sec_n));
                sec_sq = 0.0;
                sec_n  = 0;
            }
        }
    }

    const int64_t elapsed_us = esp_timer_get_time() - t_start;

    free(tx);
    free(rx);
    (void)esp_codec_dev_close(s_in_dev);

    const double rx_rate = (double)rx_samples * 1000000.0 / (double)elapsed_us;
    const double on_rms  = (on_n  > 0) ? sqrt(on_sq  / (double)on_n)  : 0.0;
    const double off_rms = (off_n > 0) ? sqrt(off_sq / (double)off_n) : 0.0;
    const double ratio   = (off_rms > 1e-9) ? (on_rms / off_rms) : 0.0;

    ESP_LOGW(TAG, "===== 回环自证结果 =====");
    ESP_LOGW(TAG, "  TX 写出 %ld 样本（此数受 DMA 缓冲影响，仅供参照）", tx_samples);
    ESP_LOGW(TAG, "  RX 实测速率 %.0f Hz（配置 %d Hz）%s",
             rx_rate, AUDIO_SAMPLE_RATE_HZ,
             (rx_rate < AUDIO_SAMPLE_RATE_HZ * 0.9) ? "  <-- 偏低，需查" : "");
    ESP_LOGW(TAG, "  [响]=%.5f  [静]=%.5f  对比=%.2f 倍", on_rms, off_rms, ratio);
    ESP_LOGW(TAG, "========================");
    ESP_LOGW(TAG, "判读：对比 > 3 ⇒ 麦克风工作正常；≈1 ⇒ 麦克风/FE 这条链不通。");
    return ESP_OK;
}
