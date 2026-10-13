/**
 * @file lcd_display.h
 * @brief LCD 显示服务（ST7796 + SPI3）
 *
 * ============================================================================
 * 【本模块已改为**移植**粤嵌官方已验证 BSP，而不是自行重写】
 *
 * 来源：06-tcp_client_bafa_mqtt/main/esp32_s3_gec.c
 *       bsp_display_brightness_init() / bsp_display_new() / bsp_lcd_init()
 *       / lcd_set_color() / lcd_cs()
 *
 * 移植原则：**照搬，不优化**。凡原实现有的写法一律保留（包括看似"低效"的
 * 逐行发送）。本项目此前自行重写了初始化序列、绘制缓冲、片选步骤与参数扫描，
 * 每次都引入新变量，导致连续多轮无法定位问题。
 *
 * 唯一偏离原实现之处已在该文件内标注（背光占空比方向，以实测为准）。
 * ============================================================================
 *
 * 线程约束：本模块不做内部加锁。P2 接入 LVGL 后，所有绘制必须发生在 ui_task 内。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 屏幕尺寸（06 BSP 值） */
#define LCD_H_RES   480
#define LCD_V_RES   320

/**
 * @brief 初始化 SPI 总线、面板 IO、ST7796 面板与背光 PWM。
 * @note  依赖 gec_board_i2c_init() 已执行（片选经 PCA9557 P0，需 I2C）。
 *        内部已完成：SPI3 模式3、BSP_LCD_RST=NC、
 *        **RGB**（实测裁决，非 BGR）+ invert_color(true) + swap_xy(true)
 *        + mirror(true,true)，MADCTL 终值 **0xE0**、
 *        以及**拉低 PCA9557 P0 选中面板**（这一步曾漏掉，是早期全黑的根因）。
 *
 * @warning 背光**高电平点亮**（LCD_BL_INVERT=1，实测裁决）。
 *          像素时钟 **80MHz**（2026-10-13 由 40MHz 改回，已实测通过）——
 *          至此与能出图的参考工程之间已无未解释差异。
 */
esp_err_t lcd_init(void);

/** @brief 释放并重建（面板 IO / 面板 / SPI 总线全部重建）。 */
esp_err_t lcd_reinit(void);

/** @brief 是否已初始化成功。 */
bool lcd_is_ready(void);

/** @brief 全屏填充单色。 */
esp_err_t lcd_fill(uint16_t color565);

/** @brief 五色竖条：左→右 红|绿|蓝|白|黑（点屏自检主手段）。 */
esp_err_t lcd_draw_color_bars(void);

/**
 * @brief 颜色验证：全屏依次刷 红→绿→蓝→黄→青→品红→白→黑→中性灰→浅灰
 *        （各 2 秒），最后重画彩条；每一步的「应该是什么颜色」同步打印到串口。
 *
 * @note  用途：验证整条颜色链。八色覆盖了单通道与两两组合的全部情形，
 *        任一通道接反都会立刻显形；每次全屏单色，没有彩条那种「左→右」
 *        的人为读数顺序隐患。
 *
 *        **最后两级中性灰才是主判据。** 纯色对「字段错位」天然不敏感 ——
 *        每个通道取极值时，错位一个比特后仍是一个饱和的近似色，肉眼容易放过。
 *        中间调里每一位都参与配比，错位立刻变成明显偏色。
 *
 *        **背景（2026-10-13 定案，推翻了同日的旧结论）**：这颗面板就是**标准
 *        RGB565** 设备（[15:11]→R、[10:5]→G、[4:0]→B），与两套厂家参考一致。
 *        此前一度判定为「R,B,G 字段顺序」并在送数据前重排，那个重排函数
 *        `panel_encode()` 实际把三个字段**整体错位了 1 个比特**，已被证实是
 *        中间调偏色的根因，现已删除。详见 lcd_display.c 的废除说明。
 */
esp_err_t lcd_color_probe(void);

/** @brief 棋盘格（检查行列对齐与边缘缺失）。 */
esp_err_t lcd_draw_checkerboard(int cell);

/** @brief 在指定行区间画一根横条（用于观察画面能否稳定保持）。 */
esp_err_t lcd_draw_progress_bar(int y, int height, uint16_t color);

/* ==========================================================================
 * LVGL 端口支持（P2 新增）
 *
 * 设计意图：把「面板句柄」与「非标准色序」继续封在组件内部。
 * 上层只交出标准 RGB565 位图，不需要、也拿不到 panel 句柄 ——
 * 将来换面板 / 换驱动 / 修正色序结论时，影响范围都锁在本组件内。
 * ========================================================================== */

/**
 * @brief 把一块**标准 RGB565** 位图写入面板。
 *
 * @param x_start,y_start 区域左上角（含）
 * @param x_end,y_end     区域右下角（**不含**，与 esp_lcd_panel_draw_bitmap 一致）
 * @param pixels          宽×高 个标准 RGB565 像素，行连续、无行距。
 *                        ⚠️ **会被就地改写** —— 内部要做字节交换
 *                        （ESP32-S3 的 SPI LCD 驱动低字节先出，面板收到的
 *                        是字节序颠倒的值；不补这一步绿字段会被打散，
 *                        表现为"纯色勉强对、灰阶全错"）。
 *                        在 DMA 传输完成前，调用方不得复用或读取该缓冲。
 *
 * @return ESP_OK；ESP_ERR_INVALID_STATE（LCD 未初始化）；
 *         ESP_ERR_INVALID_ARG（pixels 为 NULL / 越界 / 空区域）；
 *         其余为 esp_lcd_panel_draw_bitmap 透传的错误码。
 *
 * @warning **只能在任务上下文调用**（内部会阻塞等待 SPI 事务槽位），
 *          **绝不可在中断里调用**。DMA 完成中断中只允许调用「只写一个标志」
 *          的函数（如 lv_display_flush_ready），本函数不满足该条件。
 *
 * @note 返回 ESP_OK **≠ 像素已上屏**。esp_lcd_panel_draw_bitmap 是「排队即返回」的，
 *       真正的完成信号由 panel IO 的 on_color_trans_done（中断）异步给出，
 *       经 lcd_set_flush_done_cb() 注册的钩子转达。
 *
 * @note 本函数**不修改** pixels（历史版本曾就地重排字段，现已废除）。
 *       但「DMA 完成前调用方不得复用该缓冲」这条约束**依然成立** ——
 *       它是完成钩子存在的理由，与是否改写无关。
 *
 * @note 本函数不做内部加锁（与 lcd_display 既有约定一致）。
 *       LVGL 端口在它自己的递归锁内串行调用，天然满足「单一绘制者」；
 *       若要与 lcd_fill / lcd_draw_color_bars 等自检函数混用，
 *       **必须由调用方互斥**。
 */
esp_err_t lcd_draw_bitmap(int x_start, int y_start, int x_end, int y_end,
                          uint16_t *pixels);

/** 绘制完成钩子。在 panel IO 的 DMA 完成回调（**中断上下文**）中被调用。 */
typedef void (*lcd_flush_done_cb_t)(void *user);

/**
 * @brief 注册「一块色彩数据已发送完成」的钩子（LVGL 的完成信号接在这里）。
 *
 * @note  可先 lcd_init() 后注册（会立即接到现存 panel IO 上）；
 *        lcd_reinit() 重建 panel IO 时会自动重注册，**调用方无需处理**。
 * @warning 钩子在**中断上下文**运行：不得阻塞、不得打日志、不得分配内存。
 *          典型实现只有一句 `lv_display_flush_ready(disp);`。
 */
void lcd_set_flush_done_cb(lcd_flush_done_cb_t cb, void *user);

/**
 * @brief 背光开/关（IO42，经 LEDC PWM）。
 * @note  极性由 lcd_display.c 的 LCD_BL_INVERT 决定，**实测裁决为 1（高电平点亮）**。
 */
esp_err_t lcd_backlight(bool on);

/** @brief 背光亮度 0-100。 */
esp_err_t lcd_backlight_set_percent(int percent);

/* ==========================================================================
 * 兼容接口
 *
 * 以下接口保留仅为不破坏既有调用点，**实现为空操作**。
 * 本模块现在固定使用 06 BSP 的参数（SPI 模式 3、80MHz、BGR、反相开），
 * 运行期改这些参数需要完整重建总线，容易再引入变量，故明确不支持。
 * 如需变更，请直接改 lcd_display.c 中的 LCD_SPI_MODE 等宏并重新编译。
 * ========================================================================== */
void lcd_set_spi_mode(int mode);
void lcd_set_pclk_hz(unsigned int hz);
void lcd_set_panel_params(unsigned int colmod, bool bgr, bool invert_color);

/**
 * @brief 片选绑定到指定 GPIO（-1 = NC，交给 PCA9557）。需在 lcd_init() 前调用。
 * @note  默认 NC。若面板确实依赖 GPIO 片选（如 IO10），可绑上去试。
 */
void lcd_set_cs_gpio(int gpio_num);

/** @brief 查询当前绑定的片选 GPIO（-1 表示 NC）。 */
int lcd_get_cs_gpio(void);

/**
 * @brief 读面板 ID（命令 0x04）。**不可信**，仅供排查参考。
 * @note  面板 IO 在构造时固定传输参数，缺少读所需的 DUMMY 相位；
 *        实测在彩条正常显示时仍读回 00 00 00。勿据此判断通路好坏。
 */
esp_err_t lcd_read_id(uint8_t id_out[3]);

/** @brief 打印 SPI 相关 GPIO 电平，供万用表/示波器交叉核对。 */
void lcd_dump_pin_levels(void);

/**
 * @brief 枚举试验：逐组尝试「方向(MV/MX/MY) × 背光极性」，每组画纯色。
 *
 * 动机（用户实测）：`lcd_init()` 里的**全屏填白能显示出来**，之后就不输出；
 * 复位时能看到**规律色带**。说明面板、背光、SPI 通路全部正常，
 * 问题在"初始化后画面不对"。
 *
 * 本函数一次烧录穷举全部候选，替代"改一次编译烧一次"的低效试错：
 *   方向 8 种：MV/MX/MY 三位的全部组合
 *   背光 2 种：低电平点亮 / 高电平点亮
 *   共 16 组，每组 4 秒：先红、再绿，让你一眼判断是否命名相符
 *
 * @note 每组都会用串口打出「当前组合编号与预期颜色」，便于对照报告。
 *
 * @warning **本试验已裁决完毕，默认不再调用**（2026-10-13）：方向与背光极性
 *          均已由实测确定。调用方（app_main 的 p05_run）现用
 *          `P05_TEST_LCD_ORIENT_SWEEP` 开关把它关掉了。
 *          保留函数是为了日后怀疑方向/极性时可一键复现 —— 它会跑 64 秒，
 *          期间**来回翻转背光极性并改写 MADCTL**，因此会干扰其它显示裁决，
 *          不要在测量别的变量时开启。
 */
esp_err_t lcd_orientation_bl_sweep(void);

/* 供扫描使用的背光极性覆盖（不影响正常路径） */
void lcd_set_backlight_invert(bool invert);

#ifdef __cplusplus
}
#endif
