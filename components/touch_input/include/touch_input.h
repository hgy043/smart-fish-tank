/**
 * @file touch_input.h
 * @brief 触摸输入（FT6X36，I2C）—— P2 新增
 *
 * ============================================================================
 * 【为什么单独做一个组件，而不是塞进 ui_lvgl】
 *
 * 本工程约定「组件之间不得互相依赖」，而触摸是**输入设备**、不是界面：
 *   - ui_lvgl 要用它 → ui_lvgl 依赖它
 *   - 将来 P5 语音唤醒若需要「点一下开始说话」，同样能直接用它
 * 所以本组件**刻意不依赖 LVGL** —— 它只提供「读一次触摸点」，
 * 由 ui_lvgl 把它接到 LVGL 的 indev 上。
 *
 * 【硬件事实（来自粤嵌 06BSP，已验证）】
 *   · FT6X36 @ I2C 0x38，与 ES8311/ES7210/PCA9557 共用总线
 *   · **不用中断脚**（int_gpio_num = NC），**不用复位脚**（rst_gpio_num = NC）
 *     ⇒ 纯轮询模式，读一次触摸就是一次 I2C 事务
 *   · 方向参数 swap_xy=1 / mirror_x=0 / mirror_y=1，与 06BSP 完全一致
 *     （本工程的显示 MADCTL 是 MV|MX|MY，与 06BSP 的几何一致，故可直接沿用）
 *
 * 【与 I2C 互斥的关系】
 * 本组件直接使用 board_gec 的 I2C 总线。board_gec 内部那把互斥锁保护的是
 * **PCA9557 的影子寄存器**，不是总线本身；总线并发由 IDF 的 i2c_master 驱动
 * 自己保证。因此触摸（心跳式轮询）与 PCA9557 访问并存是安全的。
 * ============================================================================
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 FT6X36。依赖 gec_board_i2c_init() 已完成。
 * @note  幂等：重复调用返回 ESP_OK。
 * @return ESP_OK；ESP_ERR_INVALID_STATE（I2C 未初始化）；其余为底层透传。
 */
esp_err_t touch_input_init(void);

/** @brief 是否已初始化成功。 */
bool touch_input_is_ready(void);

/**
 * @brief 读一次触摸状态（内部即一次 I2C 事务）。
 *
 * @param[out] x,y  按下时的屏幕坐标（0..479 / 0..319）。可为 NULL。
 * @return true = 当前有按下；false = 无触摸 / 未初始化 / 读取失败。
 *
 * @note 本函数是**轮询式**的：调用频率就是 I2C 事务频率。
 *       由 LVGL 的 indev 按需调用（约 30 次/秒），对 100kHz 的共享总线
 *       是可接受的负载。
 */
bool touch_input_read(uint16_t *x, uint16_t *y);

#ifdef __cplusplus
}
#endif
