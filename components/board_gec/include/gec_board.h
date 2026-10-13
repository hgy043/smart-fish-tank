/**
 * @file gec_board.h
 * @brief 粤嵌 GEC-ESP32-S3-小智开发板 V1.0.1 板级引脚与初始化接口
 *
 * ============================================================================
 * 【引脚真源说明 — 请勿凭记忆修改】
 *
 * 本文件的引脚定义全部取自「已验证可编译烧录」的板级代码：
 *   xiaozhi-esp32-1.9.4-gec/main/boards/gec-esp32s3-board/config.h
 *   xiaozhi-esp32-1.9.4-gec/main/boards/gec-esp32s3-board/gec_esp32s3_board.cc
 *
 * 注意：项目 Agent.md 早期版本的引脚表与本文件存在多处冲突（LCD、I2S、LED）。
 * 经与两套厂家代码（小智适配版 + 粤嵌 06 BSP）逐项核对后，以本文件为准；
 * Agent.md 已同步修正。完整对照表与未决项见 smart-fish-tank/docs/pinmap.md。
 * ============================================================================
 */
#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 一、I2C 总线（共享总线：ES8311 / ES7210 / PCA9557 / FT6X36 / ADS1115）
 * ========================================================================== */

#define GEC_I2C_SDA_GPIO        GPIO_NUM_1
#define GEC_I2C_SCL_GPIO        GPIO_NUM_2
#define GEC_I2C_FREQ_HZ         100000      /* 100kHz，已验证值 */

/* I2C 端口号 —— **显式指定，不用自动分配（-1）**。
 *
 * 2026-10-13 改动。原先是 `.i2c_port = -1`，注释理由是「写死端口号会与同工程内
 * 的其它 I2C 使用者（SCCB、触摸屏）冲突」—— **这个理由正好说反了**。
 *
 * esp32-camera 的 `camera_config_t.sccb_i2c_port` 要的正是**端口号**（int），
 * 驱动拿它去 `i2c_master_get_bus_handle(port)` 找到**同一条**总线，再
 * `i2c_master_bus_add_device()` 只挂上自己那个从设备。也就是说：
 * **写死端口号恰恰是 SCCB 能共享本总线的前提**；端口号是 -1（未定）时，
 * 摄像头根本无从引用这条总线。
 *
 * 见 esp_camera.c:191-197 → sccb-ng.c:147-160：`pin_sccb_sda == -1` 时走
 * `SCCB_Use_Port()`，它**绝不调用 i2c_new_master_bus**，只记录端口号，
 * 所以与总线上已有的 PCA9557 / ES8311 / ES7210 / FT6X36 共存是安全的。
 *
 * 风险：低。ESP32-S3 有 2 个 I2C 端口，本工程只用一条总线，
 * 没有任何代码依赖"自动分配的结果恰好是几"。 */
#define GEC_I2C_PORT_NUM        I2C_NUM_0

/* I2C 从机 7 位地址表（ESP-IDF 新驱动 i2c_master 使用 7 位地址） */
#define GEC_I2C_ADDR_ES8311     0x18    /* 音频 Codec（默认地址）        */
#define GEC_I2C_ADDR_ES7210     0x41    /* 音频 ADC（厂家宏写 0x82=8位） */
#define GEC_I2C_ADDR_PCA9557    0x19    /* IO 扩展芯片                   */
#define GEC_I2C_ADDR_QMI8658A   0x6A    /* 六轴姿态传感器                */
#define GEC_I2C_ADDR_FT6X36     0x38    /* 触摸屏（当前板级未启用）      */
#define GEC_I2C_ADDR_ADS1115    0x48    /* 外置水质 ADC（本项目新增）    */

/* ==========================================================================
 * 二、PCA9557 IO 扩展芯片（关键！多个板级功能都靠它）
 *
 * 原厂代码把 LCD 片选、功放使能、摄像头电源全部挂在 PCA9557 上，
 * 而不是占用 ESP32 的 GPIO。这是本项目最重要的板级事实之一。
 * ========================================================================== */

#define GEC_PCA9557_IO_LCD_CS     0   /* P0: LCD 片选（低有效）        */
#define GEC_PCA9557_IO_PA_EN      1   /* P1: 音频功放使能              */
#define GEC_PCA9557_IO_DVP_PWDN   2   /* P2: 摄像头电源（低有效）      */

/* 正确用法：LCD 片选置低 = 选中；功放置高 = 开启；摄像头置低 = 上电 */
#define GEC_PCA9557_LEVEL_LCD_CS_ACTIVE     0
#define GEC_PCA9557_LEVEL_PA_EN_ON          1
#define GEC_PCA9557_LEVEL_DVP_POWER_ON      0

/* ---- LCD 片选必须显式拉低（关键！）----------------------------------------
 *
 * 两套厂家已验证代码都在面板复位后立刻执行本操作：
 *   06 BSP (esp32_s3_gec.c) : esp_lcd_panel_reset(); lcd_cs(0); esp_lcd_panel_init();
 *                             lcd_cs() => pca9557_set_output_state(LCD_CS_GPIO=0, 0)
 *   xiaozhi (gec_esp32s3_board.cc): pca9557_->SetOutputState(0, 0)
 *
 * ⚠️ PCA9557 上电后输出为高阻/高电平，即 LCD_CS **未被拉低**。此时面板不被选中，
 * 后续所有初始化命令与像素数据都会被忽略 —— 表现为**全程全黑**。
 *
 * 实测现象吻合：通电/复位期间面板仍可能显示出厂默认内容，一旦程序跑完初始化
 * 序列（却没拉低 CS）就变成全黑。
 *
 * 本项目初版曾因误判"PCA9557 未接拓展"而删掉此步骤，导致连续多轮排查无果。
 * 该判断有误：日志显示 0x19 正常应答，芯片在工作；"未接拓展"指的是拓展口
 * 没有外接设备，与芯片是否工作无关。
 * ------------------------------------------------------------------------- */
#define GEC_LCD_CS_REQUIRES_PCA9557         1

/* ==========================================================================
 * 三、LCD 显示屏（ST7796, SPI3_HOST）
 * ========================================================================== */

#define GEC_LCD_SPI_HOST        SPI3_HOST
#define GEC_LCD_MOSI_GPIO       GPIO_NUM_40
#define GEC_LCD_SCLK_GPIO       GPIO_NUM_41
#define GEC_LCD_DC_GPIO         GPIO_NUM_39
#define GEC_LCD_BACKLIGHT_GPIO  GPIO_NUM_42
/* LCD_CS 不由 GPIO 直接驱动 —— 见下方 GEC_LCD_CS_REQUIRES_PCA9557；
   LCD_RST 未接线（NC），复位由软件命令完成。 */
#define GEC_LCD_H_RES           480
#define GEC_LCD_V_RES           320
#define GEC_LCD_PIXEL_CLOCK_HZ  (80 * 1000 * 1000)

/* 背光 LEDC 参数（对齐 06 BSP 的 bsp_display_brightness_init）：
   10 位分辨率 / 5 kHz / 定时器 1 / 通道 0。 */
#define GEC_LCD_BL_LEDC_TIMER   LEDC_TIMER_1
#define GEC_LCD_BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define GEC_LCD_BL_LEDC_FREQ_HZ 5000
#define GEC_LCD_BL_LEDC_RES     LEDC_TIMER_10_BIT

/* ==========================================================================
 * 四、音频 I2S（ES7210 录音 + ES8311 播放，共用一条总线）
 *
 * 全双工配置：MCLK/BCLK/WS 共用，DIN 收录音，DOUT 放音。
 *
 * ⚠️ 2026-10-13 **重大更正：BCLK/WS 曾被错写为 IO39/IO40**
 *
 * 那正是 LCD 的 DC / MOSI（见第三节）。这个错误把本不存在的「C1 引脚冲突」
 * 引成了「本项目最大架构风险」，并浪费了多轮排查。两套厂家已验证代码
 * **完全一致**，都写 BCLK=IO14 / WS=IO13：
 *
 *   信号   [06BSP] esp32_s3_gec.h:198-202   [小智] gec-esp32s3-board/config.h:21-25
 *   MCLK   GPIO_NUM_38                      GPIO_NUM_38
 *   BCLK   GPIO_NUM_14                      GPIO_NUM_14     ← 本文件曾误写 IO39
 *   WS     GPIO_NUM_13                      GPIO_NUM_13     ← 本文件曾误写 IO40
 *   DOUT   GPIO_NUM_45                      GPIO_NUM_45
 *   DIN    (-1，其录音路径未启用)            GPIO_NUM_12     ← 取小智的 IO12
 *
 * 【实测后果】用错引脚的版本音频软件链路全绿（open/音量/功放/播放全部返回
 * ESP_OK）但**完全无声** —— 因为 BCLK/WS 被 LCD 的 SPI 占着，编解码器收不到
 * 时钟。另注：早期某次「音频验证通过」是因为那次 `spi_bus_initialize` 返回
 * INVALID_STATE、SPI 并未真正生效，IO39/IO40 空着，I2S 才碰巧能用。
 *
 * 【教训】引脚表抄错一位，会被后续所有推理继承，越推越"合理"。凡有源码可查
 * 的取值，一律回源码核对，不要转抄二手表格。
 * ========================================================================== */

#define GEC_I2S_MCLK_GPIO       GPIO_NUM_38
#define GEC_I2S_BCLK_GPIO       GPIO_NUM_14    /* 位时钟（曾误写 IO39 = LCD DC） */
#define GEC_I2S_WS_GPIO         GPIO_NUM_13    /* 帧同步（曾误写 IO40 = LCD MOSI） */
#define GEC_I2S_DIN_GPIO        GPIO_NUM_12    /* 麦克风 -> ESP32 */
#define GEC_I2S_DOUT_GPIO       GPIO_NUM_45    /* ESP32 -> 扬声器 */

#define GEC_AUDIO_INPUT_SAMPLE_RATE     24000
#define GEC_AUDIO_OUTPUT_SAMPLE_RATE    24000

/* ==========================================================================
 * 五、摄像头（DVP 并口）
 * ========================================================================== */

#define GEC_CAM_PIN_XCLK        GPIO_NUM_5
#define GEC_CAM_PIN_PCLK        GPIO_NUM_7
#define GEC_CAM_PIN_VSYNC       GPIO_NUM_3
#define GEC_CAM_PIN_HREF        GPIO_NUM_46
#define GEC_CAM_PIN_D0          GPIO_NUM_16
#define GEC_CAM_PIN_D1          GPIO_NUM_18
#define GEC_CAM_PIN_D2          GPIO_NUM_8
#define GEC_CAM_PIN_D3          GPIO_NUM_17
#define GEC_CAM_PIN_D4          GPIO_NUM_15
#define GEC_CAM_PIN_D5          GPIO_NUM_6
#define GEC_CAM_PIN_D6          GPIO_NUM_4
#define GEC_CAM_PIN_D7          GPIO_NUM_9
/* SCCB 复用主 I2C 总线（SDA=IO1, SCL=IO2）；摄像头电源经 PCA9557 P2 控制 */
#define GEC_CAM_XCLK_FREQ_HZ    24000000

/* ==========================================================================
 * 六、板载 LED 与按键
 *
 * ⚠️ LED 引脚存在两套厂家资料矛盾，尚未实测裁决：
 *   - 粤嵌 06 BSP (esp32_s3_gec.c) 的 led_init()/led_toggle() 实际操作 IO10
 *   - 小智适配 (gec-esp32s3-board/config.h) 定义 BUILTIN_LED_GPIO = IO48，
 *     但该宏在其 .cc 全文中从未被引用
 * 详见 docs/pinmap.md 第 6 节。P0.5 实测前不要依赖 LED。
 * ========================================================================== */

#define GEC_LED_GPIO_06BSP      GPIO_NUM_10    /* 粤嵌 BSP 实际操作脚 */
#define GEC_LED_GPIO_XIAOZHI    GPIO_NUM_48    /* 小智适配定义但未使用 */

/* 按键：两套资料都支持 IO0；IO11 为粤嵌 BSP 独有的第二按键 */
#define GEC_BOOT_KEY_GPIO       GPIO_NUM_0
#define GEC_KEY2_GPIO           GPIO_NUM_11

/* ==========================================================================
 * 七、外接水质 ADC（ADS1115）
 *
 * 【硬性约束】pH / TDS / 浊度探头严禁直接接入 ESP32 ADC 引脚。
 * 必须经 ADS1115（I2C, 0x48）接入，探头与主控共地、供电隔离。
 * ========================================================================== */

#define GEC_ADS1115_ADDR        GEC_I2C_ADDR_ADS1115
#define GEC_ADS1115_CH_PH       0   /* AIN0: pH        */
#define GEC_ADS1115_CH_TDS      1   /* AIN1: TDS       */
#define GEC_ADS1115_CH_TURBID   2   /* AIN2: 浊度      */

/* ==========================================================================
 * 八、对外接口
 * ========================================================================== */

/**
 * @brief 初始化板级 I2C 总线并挂载 PCA9557。
 * @note  幂等：重复调用直接返回 ESP_OK。内部已加互斥保护。
 */
esp_err_t gec_board_i2c_init(void);

/** @brief 获取 I2C 总线句柄（调用前需先 gec_board_i2c_init）。 */
i2c_master_bus_handle_t gec_board_get_i2c_bus(void);

/**
 * @brief 控制 PCA9557 某个输出脚。
 * @param io   输出脚编号 0..7，见 GEC_PCA9557_IO_*
 * @param level 0 或 1
 */
esp_err_t gec_pca9557_set_output(uint8_t io, uint8_t level);

/** @brief LCD 片选（内部转为 PCA9557 P0）。level: 0=选中 */
esp_err_t gec_lcd_cs(uint8_t level);

/** @brief 音频功放使能。level: 1=开启 */
esp_err_t gec_pa_en(uint8_t level);

/** @brief 摄像头电源。level: 0=上电 */
esp_err_t gec_dvp_power(uint8_t level);

/**
 * @brief 扫描 I2C 总线并把结果对照 7 位地址表打印出来。
 * @param[out] found_mask 可选，bit0..7 对应地址表索引，命中置 1
 * @return 发现的设备数量
 */
int gec_i2c_scan(uint8_t *found_mask);

/** @brief 板级上电自检：I2C 扫描 + 关键器件存在性检查。 */
esp_err_t gec_board_selftest(void);

#ifdef __cplusplus
}
#endif
