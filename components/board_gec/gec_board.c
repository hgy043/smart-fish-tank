/**
 * @file gec_board.c
 * @brief 粤嵌 GEC-ESP32-S3-小智开发板 V1.0.1 板级初始化实现
 *
 * 引脚与地址定义见 gec_board.h 顶部说明（真源：厂家已验证板级代码）。
 */
#include "gec_board.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "board_gec";

/* ==========================================================================
 * PCA9557 寄存器
 * ========================================================================== */
#define PCA9557_REG_INPUT       0x00    /* 输入寄存器（只读） */
#define PCA9557_REG_OUTPUT      0x01    /* 输出寄存器         */
#define PCA9557_REG_POLARITY    0x02    /* 极性反转寄存器     */
#define PCA9557_REG_CONFIG      0x03    /* 方向寄存器 1=输入  */

/* ==========================================================================
 * 模块内部状态
 * ========================================================================== */
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_pca9557 = NULL;
static SemaphoreHandle_t       s_i2c_mutex = NULL;

/* 上一次写出的输出寄存器镜像，避免读改写竞争 */
static uint8_t  s_pca_output_shadow = 0xFF;
static bool     s_pca_ready = false;

/* ==========================================================================
 * 内部工具
 * ========================================================================== */

/** 惰性创建互斥锁，保证多任务并发调用 init 也安全 */
static esp_err_t ensure_mutex(void)
{
    if (s_i2c_mutex == NULL) {
        s_i2c_mutex = xSemaphoreCreateMutex();
        if (s_i2c_mutex == NULL) {
            ESP_LOGE(TAG, "创建 I2C 互斥锁失败（内存不足）");
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

/* ==========================================================================
 * I2C 总线初始化
 * ========================================================================== */
esp_err_t gec_board_i2c_init(void)
{
    esp_err_t ret = ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);

    if (s_i2c_bus != NULL) {
        /* 幂等：已初始化 */
        xSemaphoreGive(s_i2c_mutex);
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_cfg = {
        /* -1 = 由驱动自动选择空闲 I2C 端口。
           写死端口号会与同工程内的其它 I2C 使用者（SCCB、触摸屏）冲突。 */
        .i2c_port          = -1,
        .sda_io_num        = GEC_I2C_SDA_GPIO,
        .scl_io_num        = GEC_I2C_SCL_GPIO,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };

    ret = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线初始化失败: %s (SDA=%d SCL=%d)",
                 esp_err_to_name(ret), GEC_I2C_SDA_GPIO, GEC_I2C_SCL_GPIO);
        s_i2c_bus = NULL;
        xSemaphoreGive(s_i2c_mutex);
        return ret;
    }

    ESP_LOGI(TAG, "I2C 总线就绪: SDA=IO%d SCL=IO%d %dkHz",
             GEC_I2C_SDA_GPIO, GEC_I2C_SCL_GPIO, GEC_I2C_FREQ_HZ / 1000);
    xSemaphoreGive(s_i2c_mutex);
    return ESP_OK;
}

i2c_master_bus_handle_t gec_board_get_i2c_bus(void)
{
    return s_i2c_bus;
}

/* ==========================================================================
 * PCA9557 操作
 * ========================================================================== */

/** 把 PCA9557 挂到总线上并配置 —— **逐字节对齐参考工程**
 *
 * 参考：D:\ESP32\06-tcp_client_bafa_mqtt\main\esp32_s3_gec.c:194-203
 *       （该工程在同板上 LCD 能正常出图）
 *
 *     pca9557_register_write_byte(PCA9557_OUTPUT_PORT, 0x05);
 *     pca9557_register_write_byte(PCA9557_CONFIGURATION_PORT, 0xf8);
 *
 * 三点与原实现的差异必须照抄，不能"优化"：
 *   1) **先写输出寄存器，再写方向寄存器**。反过来会让引脚在方向切换瞬间
 *      输出未定义的中间电平。
 *   2) 输出初值 **0x05**：P0=1(CS 不选中)、P1=0(功放关)、**P2=1(摄像头断电)**。
 *      本项目此前用 0xFC，P1/P2 恰好相反 —— P2=0 会给摄像头**上电**。
 *   3) 方向 **0xf8**：**仅 P0-P2 为输出，P3-P7 保持输入**。
 *      本项目此前写 0x00（全部输出），会把 P3-P7 也驱动起来，
 *      可能影响板上其它电路。 */
static esp_err_t pca9557_init_locked(void)
{
    if (s_pca_ready) {
        return ESP_OK;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = GEC_I2C_ADDR_PCA9557,
        .scl_speed_hz    = GEC_I2C_FREQ_HZ,
    };

    esp_err_t ret = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_pca9557);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 挂载失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ① 输出寄存器：0x05 = 0000 0101b
     *    P0=1 CS 不选中 / P1=0 功放关 / P2=1 摄像头断电 */
    s_pca_output_shadow = 0x05;
    const uint8_t out_cmd[2] = { PCA9557_REG_OUTPUT, s_pca_output_shadow };
    ret = i2c_master_transmit(s_pca9557, out_cmd, sizeof(out_cmd), 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 输出寄存器写入失败: %s", esp_err_to_name(ret));
        i2c_master_bus_rm_device(s_pca9557);
        s_pca9557 = NULL;
        return ret;
    }

    /* ② 方向寄存器：0xf8 = 1111 1000b
     *    0=输出 1=输入 → P0-P2 输出，P3-P7 输入 */
    const uint8_t cfg_cmd[2] = { PCA9557_REG_CONFIG, 0xF8 };
    ret = i2c_master_transmit(s_pca9557, cfg_cmd, sizeof(cfg_cmd), 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 方向寄存器写入失败: %s", esp_err_to_name(ret));
        i2c_master_bus_rm_device(s_pca9557);
        s_pca9557 = NULL;
        return ret;
    }

    /* 参考工程**不写**极性寄存器，此处同样不写（其默认值 0x00 即不反转）。 */

    s_pca_ready = true;
    ESP_LOGI(TAG, "PCA9557 就绪 @0x%02X (输出=0x%02X 方向=0xF8, 对齐 06 BSP)",
             GEC_I2C_ADDR_PCA9557, s_pca_output_shadow);
    return ESP_OK;
}

/** 幂等确保 PCA9557 可用（调用方需已持有互斥锁） */
static esp_err_t pca9557_ensure_locked(void)
{
    if (s_pca_ready) {
        return ESP_OK;
    }
    if (s_i2c_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return pca9557_init_locked();
}

esp_err_t gec_pca9557_set_output(uint8_t io, uint8_t level)
{
    if (io > 7) {
        ESP_LOGE(TAG, "PCA9557 输出脚编号非法: %u", io);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_i2c_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_i2c_mutex, portMAX_DELAY);

    esp_err_t ret = pca9557_ensure_locked();
    if (ret != ESP_OK) {
        xSemaphoreGive(s_i2c_mutex);
        ESP_LOGE(TAG, "PCA9557 不可用，无法设置 P%u", io);
        return ret;
    }

    uint8_t next = s_pca_output_shadow;
    if (level) {
        next |= (uint8_t)(1u << io);
    } else {
        next &= (uint8_t) ~(1u << io);
    }

    if (next != s_pca_output_shadow) {
        const uint8_t cmd[2] = { PCA9557_REG_OUTPUT, next };
        ret = i2c_master_transmit(s_pca9557, cmd, sizeof(cmd), 1000);
        if (ret == ESP_OK) {
            s_pca_output_shadow = next;
            ESP_LOGD(TAG, "PCA9557 P%u=%u (镜像 0x%02X)", io, level, next);
        } else {
            ESP_LOGE(TAG, "PCA9557 写入失败: %s", esp_err_to_name(ret));
        }
    }

    xSemaphoreGive(s_i2c_mutex);
    return ret;
}

esp_err_t gec_lcd_cs(uint8_t level)
{
    /* 注意：片选为低有效，调用方语义是「选中/未选中」，
       这里按原厂 BSP 习惯直接透传电平：level 为线上实际电平。 */
    return gec_pca9557_set_output(GEC_PCA9557_IO_LCD_CS, level);
}

esp_err_t gec_pa_en(uint8_t level)
{
    return gec_pca9557_set_output(GEC_PCA9557_IO_PA_EN, level);
}

esp_err_t gec_dvp_power(uint8_t level)
{
    return gec_pca9557_set_output(GEC_PCA9557_IO_DVP_PWDN, level);
}

/* ==========================================================================
 * I2C 扫描
 * ========================================================================== */
int gec_i2c_scan(uint8_t *found_mask)
{
    if (s_i2c_bus == NULL) {
        ESP_LOGE(TAG, "I2C 未初始化，扫描中止");
        return 0;
    }

    /* 预期器件地址表：索引与 found_mask 的 bit 一一对应 */
    static const struct {
        uint8_t     addr7;
        const char *name;
    } k_expected[] = {
        { GEC_I2C_ADDR_ES8311,   "ES8311  音频Codec"    },
        { GEC_I2C_ADDR_ES7210,   "ES7210  音频ADC"      },
        { GEC_I2C_ADDR_PCA9557,  "PCA9557 IO扩展"       },
        { GEC_I2C_ADDR_QMI8658A, "QMI8658A 姿态传感器"  },
        { GEC_I2C_ADDR_FT6X36,   "FT6X36  触摸屏"       },
        { GEC_I2C_ADDR_ADS1115,  "ADS1115 水质ADC"      },
    };
    const size_t k_expected_cnt = sizeof(k_expected) / sizeof(k_expected[0]);

    uint8_t mask = 0;
    int     found = 0;

    ESP_LOGI(TAG, "===== I2C 总线扫描开始 =====");
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_i2c_bus, addr, 50) == ESP_OK) {
            found++;

            /* 匹配预期器件，给出可读名称 */
            const char *name = "未知器件";
            for (size_t i = 0; i < k_expected_cnt; i++) {
                if (k_expected[i].addr7 == addr) {
                    name = k_expected[i].name;
                    mask |= (uint8_t)(1u << i);
                    break;
                }
            }
            ESP_LOGI(TAG, "  发现 0x%02X  %s", addr, name);
        }
    }
    ESP_LOGI(TAG, "===== 扫描结束，共 %d 个器件 =====", found);

    if (found_mask) {
        *found_mask = mask;
    }
    return found;
}

esp_err_t gec_board_selftest(void)
{
    uint8_t mask = 0;
    int     found = gec_i2c_scan(&mask);

    if (found == 0) {
        ESP_LOGE(TAG, "自检失败：I2C 总线上未发现任何器件，请检查 IO1/IO2 接线与上拉");
        return ESP_ERR_NOT_FOUND;
    }

    /* 逐项报告预期器件的存在性。ADS1115 是本项目外接件，缺失属正常，
       单独提示而不判失败，避免无 ADS1115 时自检永远无法通过。 */
    struct {
        uint8_t     bit;
        uint8_t     addr7;
        const char *name;
        bool        required;
    } items[] = {
        { 0, GEC_I2C_ADDR_ES8311,   "ES8311  音频Codec",   false },
        { 1, GEC_I2C_ADDR_ES7210,   "ES7210  音频ADC",     false },
        { 2, GEC_I2C_ADDR_PCA9557,  "PCA9557 IO扩展",      true  },
        { 3, GEC_I2C_ADDR_QMI8658A, "QMI8658A 姿态传感器", false },
        { 4, GEC_I2C_ADDR_FT6X36,   "FT6X36  触摸屏",      false },
        { 5, GEC_I2C_ADDR_ADS1115,  "ADS1115 水质ADC",     false },
    };

    esp_err_t worst = ESP_OK;
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        if (mask & (1u << items[i].bit)) {
            ESP_LOGI(TAG, "[ 存在 ] %s (0x%02X)", items[i].name, items[i].addr7);
        } else if (items[i].required) {
            ESP_LOGE(TAG, "[ 缺失 ] %s (0x%02X) —— 必需器件！", items[i].name, items[i].addr7);
            worst = ESP_ERR_NOT_FOUND;
        } else {
            ESP_LOGW(TAG, "[ 缺失 ] %s (0x%02X)", items[i].name, items[i].addr7);
        }
    }

    return worst;
}
