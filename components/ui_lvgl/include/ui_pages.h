/**
 * @file ui_pages.h
 * @brief 页面管理与导航（P2）
 *
 * ============================================================================
 * 【页面结构】
 *
 *     主菜单 (UI_PAGE_MENU)
 *       ├─ 鱼缸情况 (UI_PAGE_TANK)   —— 六张数据卡片
 *       ├─ 小智AI互动 (UI_PAGE_AI)   —— 聊天流 + 圆形语音键（P7 接入小智）
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
 * **唯一的例外是 `ui_pages_talk_held()`**：给工作任务在**锁外**轮询用，
 * 它只读一个 volatile 标志，不碰任何 LVGL 对象。
 * ============================================================================
 */
#pragma once

#include "lvgl.h"
#include "app_types.h"
#include "ui_lvgl.h"    /* 只为一个类型：ui_chat_role_t（公共 API 的一部分，只定义一份）*/

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

/* ==========================================================================
 * SD 卡相册（P6-c）
 *
 * 刷新次序（**这个次序是设计的一部分，不要打乱**）：
 *
 *   ui_task:
 *     if (ui_pages_album_refresh_requested()) {   // ① 锁外轮询
 *         buf = ui_lvgl_album_begin(...);         // ② 内部上锁：摘掉所有 src
 *         for (i...) sd_service_load_thumb(...);  // ③ **锁外**解码，几百毫秒
 *         ui_lvgl_album_commit(...);              // ④ 内部上锁：挂上新 src
 *     }
 *
 * 关键是 ② 与 ④ 之间 LVGL **不持有**那块缓冲的任何引用，所以 ③ 在锁外
 * 改写它是安全的 —— 这也正是这里**不需要双缓冲**的原因。
 * （AI 页那张照片气泡要双缓冲，是因为它换图没有这个空窗。）
 * ========================================================================== */

/**
 * @brief 【**不加锁**】取走"相册需要刷新"的请求。true = 刚进入相册页。
 *
 * @note 与 `ui_pages_talk_held()` 同一条纪律：给 ui_task 在**锁外**轮询用，
 *       只读一个 volatile 标志，不碰任何 LVGL 对象。读取即清零。
 */
bool ui_pages_album_refresh_requested(void);

/** @brief 把 6 个格子的图**全部摘掉** —— 之后外部就可以安全地改写缓冲。
 *  **调用者须已持 LVGL 锁。** */
void ui_pages_album_clear(void);

/** @brief 把缓冲里前 count 张挂上显示。
 *  @param buf 连续一整块，第 i 张在 buf + i*w*h（标准 native RGB565）
 *  **调用者须已持 LVGL 锁。** */
void ui_pages_album_show(const uint16_t *buf, int count, int w, int h);

/** @brief 相册页脚注文字（"共 N 张" / "卡不可用：原因"）。
 *  **调用者须已持 LVGL 锁。** */
void ui_pages_album_note(const char *text);

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
 * 「小智AI互动」页：聊天流（WeChat 风格，P6-b）
 *
 * 【为什么是聊天流而不是"照片区 + 按钮"】
 * xiaozhi 的 WeChat 风格里，**图片本身就是一条消息气泡**（它的
 * `SetPreviewImage()` 在对话流里建一个 `img_bubble`）。所以照片不必另开区域，
 * 作为一条消息出现在聊天里即可 —— 两条需求（WeChat 风格、拍照显示）合流。
 *
 * 【复用的部分】
 * 布局与配色搬自 xiaozhi `main/display/lcd_display.cc` 的 WeChat 分支：
 *   · 气泡三态：用户右对齐绿底 / 助手左对齐白底 / 系统居中灰底
 *   · 气泡宽度按**实测文本宽度**自适应（不是固定宽度），上限屏宽 85%
 *   · 最多保留 N 条，超出删最早的（避免控件无限增长）
 *   · 新消息自动滚到底
 * **不依赖** xiaozhi 的 Display 基类 / Board 单例 / Settings —— 是纯 LVGL 代码。
 *
 * ⚠️ `ui_chat_role_t` 定义在 ui_lvgl.h —— 那是 main 侧唯一可见的公共头，
 *    角色枚举属于公共 API 的一部分，只定义一份。
 * ========================================================================== */

/**
 * @brief 往聊天流里追加一条文字消息。**调用者须已持 LVGL 锁。**
 * @note  内部会拷贝文本，不持有调用方的指针。
 */
void ui_pages_chat_add(ui_chat_role_t role, const char *text);

/**
 * @brief 往聊天流里追加一条**照片**消息。**调用者须已持 LVGL 锁。**
 *
 * @param dsc 指向 `lv_image_dsc_t`（标准 native RGB565）。图片会按
 *            **屏宽70% × 屏高50%** 缩放后放进气泡（照搬 xiaozhi 的
 *            `SetPreviewImage()`）—— QVGA 图 320×240 在 480×224 的聊天区里
 *            **1:1 放不下**，不缩放会被裁掉一半。
 *            ⚠️ **LVGL 会长期引用这块内存**，调用方必须保证它在气泡
 *            被删除前一直有效且不被改写。
 *
 * @note  **同一时刻只保留一张照片**：新照片会先删掉上一张照片气泡。
 *        这是内存约束下的取舍 —— 本工程的照片缓冲只有两块
 *        （见 ui_lvgl_config.h 的 UI_LVGL_PHOTO_*），留多了会互相覆盖。
 *        历史照片属于「SD卡相册」页的职责，不是聊天流的。
 */
void ui_pages_chat_add_photo(const lv_image_dsc_t *dsc);

/* ==========================================================================
 * 语音按钮（P7 按键说话）
 * ========================================================================== */

/**
 * @brief 【**不加锁**】麦克风按钮当前是否**正被按住**。
 *
 * @note 这条纪律见文件头的「锁外轮询」说明：给 `ui_task` / `voice_task` 在
 *       **锁外**读，读侧一律不碰 LVGL。
 *       关键在于它返回的是**状态**而非**一次性请求** ——
 *       语音任务取帧一次要阻塞 60ms，请求式的标志会漏掉按下-松开这个事件；
 *       状态式的不会。
 *
 * @note **读取不清零**（每次调用都反映当下真实状态）。
 * @note 按钮不可用时恒返回 false。
 */
bool ui_pages_talk_held(void);

/**
 * @brief 设置语音按钮的可用性。**调用者须已持 LVGL 锁。**
 * @param ready 连上服务器后才为 true；false 时置灰且按不动。
 */
void ui_pages_set_talk_ready(bool ready);

/**
 * @brief 设置语音按钮的显示状态。**调用者须已持 LVGL 锁。**
 * @note 传 UI_TALK_IDLE 会按"当前是否可用"自动选择文案，
 *       不会把"语音（未连接）"覆盖成"按住说话"。
 */
void ui_pages_set_talk_state(ui_talk_state_t st);

#ifdef __cplusplus
}
#endif

