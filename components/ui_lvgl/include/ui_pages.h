/**
 * @file ui_pages.h
 * @brief 页面管理与导航（P2）
 *
 * ============================================================================
 * 【页面结构】
 *
 *     主菜单 (UI_PAGE_MENU)
 *       ├─ 鱼缸情况 (UI_PAGE_TANK)   —— 六张数据卡片
 *       ├─ 小智AI互动 (UI_PAGE_AI)   —— 对话气泡（假数据）
 *       └─ SD卡相册 (UI_PAGE_ALBUM)  —— 九宫格缩略图（假数据）
 *
 * 【实现方式：预建 + 显隐切换】
 * 四个页面在初始化时一次建齐，切页只切 LV_OBJ_FLAG_HIDDEN。
 * 不采用「销毁/重建」的理由：本工程的 LVGL 是**单缓冲部分刷新**，
 * 反复创建销毁会产生大量重绘区域与堆碎片；而四个页面的控件总数很小
 * （约 40 个对象），常驻内存完全可接受。
 *
 * 【线程约定】
 * 本模块所有函数都在 **LVGL 锁内**调用 —— 由 ui_lvgl 负责加解锁。
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

#ifdef __cplusplus
}
#endif
