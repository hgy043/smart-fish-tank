/**
 * @file ui_lvgl.h
 * @brief LVGL 移植层与界面更新入口（P2）
 *
 * ============================================================================
 * 【本组件的定位】
 *
 * 它是**唯一**接触 LVGL 的组件。其余组件（含 main）要改界面，只能调这里的
 * `ui_lvgl_update()`，或者用 `ui_lvgl_lock/unlock` 包住自己的 LVGL 调用。
 *
 * 【为什么手工建显示对象，而不是用 esp_lvgl_port 的 lvgl_port_add_disp()
 *  —— 2026-10-13 修订，原判据已作废，如实记录】
 *
 * ⚠️ **本条原先写的理由是错的，不要采信旧版本**：旧注释说 add_disp「颜色会错」，
 *    因为它绕过了本面板必需的 panel_encode 字段重排。而 panel_encode 本身
 *    是错的（把字段整体错位了 1 个比特），已删除，见 lcd_display.c 的说明。
 *
 * ⚠️⚠️ **本条的前一版又错了一次，务必看清楚**：上一版写着
 *    「`flags.swap_bytes` 必须置 0，本面板不需要字节交换」——
 *    **正好相反**。ESP32-S3 的 SPI LCD 驱动低字节先出，本面板**需要**交换，
 *    而那次判断是建立在「rgb_ele_order 写成了 RGB」这个同时存在的错误上的
 *    （两个错误对红/蓝几乎抵消，所以当时看着"不用交换也对"）。
 *
 * **若日后改用 `lvgl_port_add_disp()`，两处必须这样设：**
 *
 *   ① `flags.swap_bytes` **必须置 0**（注意：置 0，不是置 1）。
 *      原因：字节交换现在已经在 `lcd_display` 的 `panel_send()` 里做了，
 *      那是全项目唯一的出口。端口的 flush 若也交换一次
 *      （`lv_draw_sw_rgb565_swap()`，esp_lvgl_port_disp.c:673-676），
 *      **两次交换互相抵消 ⇒ 颜色又错了**。
 *      （xiaozhi 设 1 是因为它的字节交换在端口里做，本项目不在那里做 ——
 *       两边最终效果相同，但**只能有一处**。）
 *
 *   ② `rotation` 的三个值必须与 lcd_init() 里的 swap_xy/mirror **一致**
 *      （本板为 true/true/true）。端口会在 rotation 0 时按这些值**重新下发**
 *      swap_xy + mirror（esp_lvgl_port_disp.c:711-715），是同一份 MADCTL 状态的
 *      第二个写入者；值不一致时会悄悄改掉 lcd_init 设好的方向。
 *
 * 现状：手工路径已在 P2 上验证工作，且它把「完成信号来自 DMA 完成中断」这件事
 * 显式摆在代码里（下一段的铁律），调试时更容易看清。**暂无改用 add_disp 的理由**，
 * 若改，请先落实上面两条。
 *
 * 所以本组件的做法是（**先例正是 xiaozhi 自己**，
 * 见其 main/boards/waveshare-s3-touch-lcd-3.5b/custom_lcd_display.cc 的 #else 分支）：
 *
 *     lvgl_port_init()        ← 只用它的「任务 + 递归锁 + tick」三样基础设施
 *     lv_display_create()     ← 显示对象手工建，不经 add_disp
 *     lv_display_set_flush_cb()  ← 自己的 flush，走 lcd_draw_bitmap()
 *     lcd_set_flush_done_cb()    ← 完成信号接到 DMA 完成中断上
 *
 * 【一条不可违反的铁律】
 *
 * flush 回调的**成功路径绝不能**调用 lv_display_flush_ready()。
 * LVGL 单缓冲部分刷新靠 flushing 标志等 DMA 读完缓冲（lv_refr.c 原文：
 * "In single buffered mode wait here until the buffer is freed. Else we would
 * draw into the buffer while it's still being transferred to the display"）。
 * 提前清零 = LVGL 立刻重画同一条带 = 随机花屏/撕裂。
 * 完成信号**只能**来自 DMA 完成中断。
 * ============================================================================
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 LVGL 端口、手工创建显示对象、建界面。
 *
 * @note  前置条件：`lcd_init()` 已成功（内部会检查 `lcd_is_ready()`）。
 *        调用方必须保证此前的 LCD 自检（p05_run 等）**已经全部跑完** ——
 *        两套绘制路径同屏混画会交错发 SPI 事务，导致窗口/数据错配。
 * @note  幂等：重复调用返回 ESP_ERR_INVALID_STATE。
 * @note  内部会短暂持有 LVGL 锁并阻塞数秒（色彩自检时）。应在 ui_task 内调用。
 *
 * @return ESP_OK；ESP_ERR_INVALID_STATE（重复调用 / LCD 未就绪）；
 *         ESP_ERR_NO_MEM（绘制缓冲分配失败）；其余为底层透传。
 */
esp_err_t ui_lvgl_init(void);

/**
 * @brief 取 LVGL 锁。多任务环境下**任何** LVGL 调用都必须先取锁。
 * @param timeout_ms 等待上限；**传 0 表示无限等待**。
 * @return true = 取到（必须配对 `ui_lvgl_unlock()`）；false = 超时。
 */
bool ui_lvgl_lock(uint32_t timeout_ms);

/** @brief 释放 LVGL 锁（与 ui_lvgl_lock 配对，支持递归）。 */
void ui_lvgl_unlock(void);

/**
 * @brief 用一帧生态快照刷新界面。
 *
 * 内部完成：上锁 → 更新控件 → 解锁 → 唤醒渲染任务。
 * 各控件**只在文本真正变化时**才 set，避免每轮产生无谓的重绘请求。
 *
 * @param snap 快照（调用方负责保证生命周期，函数返回后即可释放）。
 * @return true = 已更新；false = 未初始化 / 取锁超时（本轮跳过）。
 *         **取锁超时不算错误**：宁可少刷一帧，也不让调用方被拖住节拍。
 */
bool ui_lvgl_update(const tank_ecosystem_t *snap);

/**
 * @brief 【诊断】让接下来 frames 次 flush 各打印一次「送往面板的首像素」。
 *
 * @note  用途：把「LVGL 说它画了什么」与「我们真往面板送了什么」分开。
 *        若串口打出的值已是期望的 RGB565 而屏幕仍不对，问题就在面板/接线；
 *        否则问题在 LVGL 渲染或调用方的颜色值本身。
 *        平时预算为 0，flush 里只有一次整数比较，无开销。
 */
void ui_lvgl_flush_probe(uint32_t frames);

/* ==========================================================================
 * 摄像头拍照（P6-a）
 *
 * 调用次序（**这个次序是设计的一部分，不要打乱**）：
 *
 *     ui_task:
 *       if (ui_lvgl_capture_requested()) {          // ① 锁外轮询
 *           ui_lvgl_set_ai_status("拍摄中…", false); // ② 内部上锁，很快
 *           camera_service_capture();                // ③ **锁外**，最长 4 秒
 *           ui_lvgl_show_photo(...);                 // ④ 内部上锁
 *       }
 *
 * 关键点：**拍照本身（③）绝不能持 LVGL 锁** —— 它最长阻塞 4 秒，
 * 持锁会把 taskLVGL 一起卡住，整个界面（含触摸）冻结。
 * 所以按钮回调只置标志（①），耗时动作留给锁外的 ui_task。
 * ========================================================================== */

/**
 * @brief 【**不加锁**】取走拍照请求。true = 用户按了「拍照」键。
 * @note  专供 ui_task 在锁外轮询。读取即清零。
 */
bool ui_lvgl_capture_requested(void);

/**
 * @brief 把一帧标准 RGB565 显示到「小智AI互动」页。
 *
 * @param rgb565 标准 native RGB565（即 camera_service_frame() 交出的格式），
 *               尺寸 w×h，行连续。
 * @param w,h    图像尺寸，必须 ≤ UI_LVGL_PHOTO_MAX_W/H（见 ui_lvgl_config.h）。
 *
 * @return ESP_OK；ESP_ERR_INVALID_STATE（未初始化）；ESP_ERR_INVALID_ARG（参数非法）；
 *         ESP_ERR_INVALID_SIZE（尺寸超缓冲容量）；ESP_ERR_TIMEOUT（取锁超时）。
 *
 * @note **双缓冲**：数据先拷进本组件自有的两块缓冲之一（**锁外**完成），
 *       然后才上锁、只做一次 `lv_image_set_src()` 切换指针。锁持有时间是
 *       **微秒级**，不是拷 153 KB 的那几毫秒。
 *       这样做的同时也就满足了「LVGL 正在显示的那块缓冲永远不会被下一帧写」
 *       —— 交替使用两块即可。若改成单缓冲，就会出现"LVGL 正在读、摄像头正在写"
 *       的撕裂（xiaozhi 的 `Capture()` 正是单缓冲，靠的是不发生）。
 *
 * @warning 传给本函数的源缓冲在返回后即可被调用方复用/覆盖 —— 本函数已拷贝。
 */
esp_err_t ui_lvgl_show_photo(const uint16_t *rgb565, int w, int h);

/**
 * @brief 更新拍照区的状态文字与按钮可用性（内部上锁）。
 * @param text           照片区中央的提示；NULL 或空串 = 隐藏。
 * @param button_enabled false = 按钮置灰且点击无效。
 */
void ui_lvgl_set_ai_status(const char *text, bool button_enabled);

/**
 * @brief 读取「色彩数据发送完成中断」的累计次数。
 *
 * 排查用。这是判断渲染链是否通最直接的证据：
 *   · 计数在增长  ⇒ DMA 完成回调正常，LVGL 能拿到完成信号；
 *   · 计数恒为 0  ⇒ 完成链断了，LVGL 会卡在首次刷新的等待里
 *                   （界面停在首帧 / 花帧，且 taskLVGL 占满一个核）。
 * 中断里不能打日志，所以用计数 + 定期读取的方式观测。
 */
uint32_t ui_lvgl_get_flush_done_count(void);

#ifdef __cplusplus
}
#endif
