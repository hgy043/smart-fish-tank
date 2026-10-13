/**
 * @file ui_pages.h
 * @brief 页面管理与导航（P2）
 *
 * ============================================================================
 * 【页面结构】
 *
 *     主菜单 (UI_PAGE_MENU)
 *       ├─ 鱼缸情况 (UI_PAGE_TANK)   —— 六张数据卡片
 *       ├─ 小智AI互动 (UI_PAGE_AI)   —— 拍照 + 照片显示（P6-a 接入真实摄像头）
 *       └─ SD卡相册 (UI_PAGE_ALBUM)  —— 九宫格缩略图（假数据，待 TF 卡引脚确认）
 *
 * 【实现方式：预建 + 显隐切换】
 * 四个页面在初始化时一次建齐，切页只切 LV_OBJ_FLAG_HIDDEN。
 * 不采用「销毁/重建」的理由：本工程的 LVGL 是**单缓冲部分刷新**，
 * 反复创建销毁会产生大量重绘区域与堆碎片；而四个页面的控件总数很小
 * （约 40 个对象），常驻内存完全可接受。
 *
 * 【线程约定】
 * 本模块所有函数都在 **LVGL 锁内**调用 —— 由 ui_lvgl 负责加解锁。
 * **唯一的例外是 `ui_pages_take_capture_request()`**，见其说明。
 * ============================================================================
 */
#pragma once

#include "lvgl.h"
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_PAGE_MENU = 0,
    UI_PAGE_TANK,
    UI_PAGE_AI,
    UI_PAGE_ALBUM,
    UI_PAGE_CNT
} ui_page_t;

/**
 * @brief 建齐四个页面并显示初始页。**调用者须已持 LVGL 锁。**
 * @note  必须在 lvgl 显示对象与绘制缓冲就绪之后调用。
 */
void ui_pages_init(void);

/** @brief 切到指定页。**调用者须已持 LVGL 锁。** 越界值被忽略。 */
void ui_pages_show(ui_page_t page);

/** @brief 当前页。 */
ui_page_t ui_pages_current(void);

/** @brief 用一帧快照刷新鱼缸页。**调用者须已持 LVGL 锁。** */
void ui_pages_update_tank(const tank_ecosystem_t *snap);

/**
 * @brief 建一个统一的页面顶栏（标题 + 可选返回键），返回该页面的内容容器。
 *
 * 四个页面共用它，保证标题字号、位置、返回键样式一致 ——
 * 各页各写一份最容易出现「这个页面的返回键偏了 2px」这类问题。
 *
 * @param parent     页面根容器
 * @param title      标题文字
 * @param with_back  true = 加返回键（子页面用；主菜单不需要）
 * @return 内容容器（已按顶栏高度下移），供各页往里放自己的控件
 */
lv_obj_t *ui_page_make_shell(lv_obj_t *parent, const char *title, bool with_back);

/* ==========================================================================
 * 「小智AI互动」页的拍照接口（P6-a）
 * ========================================================================== */

/**
 * @brief 【**不加锁**】取走拍照请求。返回 true 表示用户按了拍照键。
 *
 * @note  本函数是**唯一**可以不用持 LVGL 锁的本模块函数，因为它是给
 *        `ui_task` 在**锁外**轮询用的 —— 拍照动作用户按下后必须离开锁外执行
 *        （`camera_service_capture()` 最长阻塞 4 秒，持锁会把 taskLVGL 卡死）。
 *
 *        「按了键」这个动作本身发生在 **taskLVGL 上下文且已持 LVGL 锁**，
 *        回调里**只置一个 volatile 标志**，不做任何耗时操作。
 *
 * @note  读取即清零（取走语义），避免同一请求被处理两次。
 */
bool ui_pages_take_capture_request(void);

/**
 * @brief 照片控件（`lv_image`）。交给 ui_lvgl 设置图像源。
 * @note  只应在 ui_lvgl 内部使用；直接调用者须已持 LVGL 锁。
 */
lv_obj_t *ui_pages_ai_photo(void);

/**
 * @brief 显示/隐藏照片区的占位提示。
 * @note  调用者须已持 LVGL 锁。拍成第一张照片后应隐藏。
 */
void ui_pages_ai_hint_visible(bool visible);

/**
 * @brief 更新拍照区的状态文字与按钮可用性。
 *
 * @param text           显示在照片区中央的提示（NULL 或空串 = 不显示）。
 *                       拍照前可写「拍摄中…」，失败时写原因。
 * @param button_enabled false = 按钮置灰且点击无效（摄像头不可用时用）。
 *
 * @note 调用者须已持 LVGL 锁。
 */
void ui_pages_set_ai_status(const char *text, bool button_enabled);

#ifdef __cplusplus
}
#endif

