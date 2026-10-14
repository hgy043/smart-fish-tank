/**
 * @file ui_pages.c
 * @brief 页面管理、导航，以及主菜单 / AI 互动 / 相册三页
 *
 * 鱼缸情况页在 ui_dashboard.c（它是唯一有真实数据源的页面，单独成文件）。
 * 页面结构、实现方式与线程约定见 include/ui_pages.h。
 *
 * ── 关于 AI 互动页与相册页的内容 ──────────────────────────────────────
 * 这两页目前**没有数据源**：AI 互动要等 P7（小智接入），相册要等 TF 卡驱动。
 * 按用户要求先画出**完整的视觉假数据**，把布局定下来；接真实数据时只需替换
 * 数据来源，布局本身不用重做。假数据一律带明显标记（占位灰块、"待接入"字样），
 * 避免日后误把占位内容当成真实读数。
 * ============================================================================
 */
#include "ui_pages.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "ui_dashboard.h"
#include "ui_theme.h"

#include "font_awesome.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_20_4);

static const char *TAG = "ui_pages";

/* ==========================================================================
 * 页面状态
 * ========================================================================== */
static lv_obj_t *s_page[UI_PAGE_CNT];
static ui_page_t s_cur = UI_PAGE_MENU;

ui_page_t ui_pages_current(void)
{
    return s_cur;
}

/* ==========================================================================
 * 公共构件
 * ========================================================================== */

/** 页面根容器：整屏、铺底色、初始隐藏 */
static lv_obj_t *make_root(void)
{
    lv_obj_t *p = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(p);                    /* 只要一个纯色底板 */
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_size(p, TH_W, TH_H);
    lv_obj_set_style_bg_color(p, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);        /* 建完由 ui_pages_show 点亮 */
    return p;
}

/** 返回键回调 */
static void on_back_clicked(lv_event_t *e)
{
    (void)e;
    ui_pages_show(UI_PAGE_MENU);
}

lv_obj_t *ui_page_make_shell(lv_obj_t *parent, const char *title, bool with_back)
{
    if (with_back) {
        /* 返回键：放在左上角，标题保持居中 —— 比「返回键+标题」左对齐更规整 */
        lv_obj_t *btn = lv_obj_create(parent);
        lv_obj_remove_style_all(btn);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(btn, 8, 8);
        lv_obj_set_size(btn, 60, 32);
        lv_obj_set_style_bg_color(btn, lv_color_hex(TH_CARD), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(TH_EDGE), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_add_event_cb(btn, on_back_clicked, LV_EVENT_CLICKED, NULL);

        lv_obj_t *arrow = lv_label_create(btn);
        lv_label_set_text(arrow, FONT_AWESOME_ANGLE_LEFT);
        lv_obj_set_style_text_font(arrow, &font_awesome_20_4, 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(TH_LABEL), 0);
        lv_obj_center(arrow);
    }

    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(TH_TITLE), 0);
    lv_obj_set_style_text_font(t, &font_puhui_20_4, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 12);

    /* 顶栏下的分隔线 */
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_pos(line, 0, TH_HEADER_H - 1);
    lv_obj_set_size(line, TH_W, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(TH_DIVIDER), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    /* 内容容器：已经按顶栏高度下移，各页直接往里放控件 */
    lv_obj_t *content = lv_obj_create(parent);
    lv_obj_remove_style_all(content);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(content, 0, TH_HEADER_H);
    lv_obj_set_size(content, TH_W, TH_H - TH_HEADER_H);
    return content;
}

/* ==========================================================================
 * 主菜单页
 * ========================================================================== */
typedef struct {
    const char *title;
    const char *icon;       /* FontAwesome 字形 */
    const char *desc;
    ui_page_t   page;
    uint32_t    color;
} menu_item_t;

static const menu_item_t k_menu[] = {
    { "鱼缸情况",   FONT_AWESOME_TEMPERATURE_HALF, "水质与环境实时数据", UI_PAGE_TANK,  0x0277BD },
    { "小智AI互动", FONT_AWESOME_MICROPHONE,       "语音对话与养护建议", UI_PAGE_AI,    0x6A1B9A },
    { "SD卡相册",   FONT_AWESOME_IMAGE,            "抓拍照片浏览",       UI_PAGE_ALBUM, 0x2E7D32 },
};
#define MENU_CNT ((int)(sizeof(k_menu) / sizeof(k_menu[0])))

static void on_menu_clicked(lv_event_t *e)
{
    const ui_page_t page = (ui_page_t)(intptr_t)lv_event_get_user_data(e);
    ui_pages_show(page);
}

static void build_menu_page(lv_obj_t *root)
{
    lv_obj_t *content = ui_page_make_shell(root, "智能鱼缸", false);

    const int m      = 18;                              /* 内容区内留白 */
    const int gap    = 12;
    const int avail  = TH_H - TH_HEADER_H - 2 * m;       /* 可用高度 */
    const int item_h = (avail - (MENU_CNT - 1) * gap) / MENU_CNT;

    for (int i = 0; i < MENU_CNT; i++) {
        const menu_item_t *it = &k_menu[i];

        lv_obj_t *btn = lv_obj_create(content);
        lv_obj_remove_style_all(btn);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(btn, m, m + i * (item_h + gap));
        lv_obj_set_size(btn, TH_W - 2 * m, item_h);
        lv_obj_set_style_bg_color(btn, lv_color_hex(TH_CARD), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(TH_EDGE), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_add_event_cb(btn, on_menu_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)it->page);

        /* 图标：用该项的主色，作为视觉锚点 */
        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, it->icon);
        lv_obj_set_style_text_font(icon, &font_awesome_20_4, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(it->color), 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 18, 0);

        lv_obj_t *title = lv_label_create(btn);
        lv_label_set_text(title, it->title);
        lv_obj_set_style_text_font(title, &font_puhui_20_4, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 58, -10);

        lv_obj_t *desc = lv_label_create(btn);
        lv_label_set_text(desc, it->desc);
        lv_obj_set_style_text_font(desc, &font_puhui_16_4, 0);
        lv_obj_set_style_text_color(desc, lv_color_hex(TH_LABEL), 0);
        lv_obj_align(desc, LV_ALIGN_LEFT_MID, 58, 12);

        /* 右侧箭头，提示「可进入」 */
        lv_obj_t *chev = lv_label_create(btn);
        lv_label_set_text(chev, FONT_AWESOME_ANGLE_RIGHT);
        lv_obj_set_style_text_font(chev, &font_awesome_20_4, 0);
        lv_obj_set_style_text_color(chev, lv_color_hex(TH_LABEL), 0);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -16, 0);
    }
}

/* ==========================================================================
 * 小智 AI 互动页 —— 聊天流（WeChat 风格，P6-b）
 *
 * 【布局】内容区 480 × (320 - 44) = 480 × 276
 *     聊天区   480 × 224   可垂直滚动，气泡自上而下堆叠
 *     操作栏   480 ×  52   底部：居中一个**圆形语音键**（麦克风图标 + 状态文字）
 *
 * 【为什么只剩一个按钮】
 *   原本还有一个「拍照」矩形键（P6-a）。2026-10-14 用户要求去掉 —— 拍照现在
 *   由 AI 侧驱动（`self.camera.take_photo` MCP 工具，见 xiaozhi_mcp.c），
 *   而且**拍到的照片照样会作为聊天气泡出现在本页**（ui_lvgl_show_photo），
 *   所以手动按钮是重复入口。随之把 `ui_pages_take_capture_request()` /
 *   `ui_pages_set_camera_ready()` 一并删掉 —— 按钮没了，那条请求链
 *   就再也不会被触发，留着就是永远不会执行的代码。
 *
 * 【气泡三态】搬自 xiaozhi `main/display/lcd_display.cc` 的 WeChat 分支：
 *     用户   右对齐，微信绿 0x95EC69
 *     助手   左对齐，白底 + 描边
 *     系统   居中，浅灰底
 *
 * 【为什么每行要套一个"整宽行容器"】
 *   LVGL 的 flex 布局里，**主轴对齐是容器级的，不能逐个孩子设**。
 *   而右对齐用户气泡、左对齐助手气泡必须在同一个纵向流里共存。
 *   解法（xiaozhi 用的也是这个）：每个消息放进一个 `LV_PCT(100)` 宽的**行容器**，
 *   行容器自己是一个横向 flex，按角色取 `FLEX_ALIGN_END / START / CENTER`。
 *   比"整宽行 + lv_obj_align"更一致 —— 全程只用 flex，不混两套定位机制。
 *
 * 【气泡宽度】按**实测文本宽度**自适应，上限屏宽 85%。
 *   短消息不该撑满整行（那样看着像公告不像对话），长消息自动换行。
 * ========================================================================== */
#define AI_CHAT_MAX_MSG   20      /* 最多保留的消息条数（与 xiaozhi MAX_MESSAGES 一致）*/
#define AI_MIC_D          40      /* 圆形语音键直径 */
#define AI_ICON_W         18      /* 麦克风图标的画布尺寸 */
#define AI_ICON_H         22

static lv_obj_t *s_chat_list   = NULL;  /* 聊天区（flex column，可滚动）*/
static lv_obj_t *s_chat_photo  = NULL;  /* 当前那张照片的 lv_image；同刻只留一张 */
static lv_obj_t *s_mic_btn     = NULL;  /* 圆形语音键 */
static lv_obj_t *s_mic_icon    = NULL;  /* 键里的麦克风图标（整体调透明度用）*/
static lv_obj_t *s_mic_btn_lbl = NULL;  /* 圆钮右侧的状态文字 */

/* 麦克风按钮是否**正被按住**（P7 按键说话）。
 *
 * 【为什么是"按住状态"而不是"按下/松开两个请求"】
 * 语音任务在 rec_read() 里一次阻塞 60ms，两次轮询之间可能跨过整个
 * 按下-松开。请求式的标志会漏事件；而"当前是否按住"是**状态**，
 * 只要轮询还存在就一定读得到正确值（最多晚 60ms）。
 *
 * ⚠️ 只在 LVGL 回调里**写**，只在锁外**读**。读侧不碰 LVGL。 */
static volatile bool s_talk_held = false;

/* --------------------------------------------------------------------------
 * 语音按钮（按键说话）
 * -------------------------------------------------------------------------- */

static void on_mic_pressed(lv_event_t *e)
{
    (void)e;
    s_talk_held = true;
}

static void on_mic_released(lv_event_t *e)
{
    /* RELEASED 与 PRESS_LOST 都走这里。
       PRESS_LOST 是"按住时手指滑出按钮"—— 对用户来说等同于松手，
       必须也置 false，否则会一直录下去。 */
    (void)e;
    s_talk_held = false;
}

bool ui_pages_talk_held(void)
{
    /* 纯读标志，不碰 LVGL —— 调用方可在任意任务、锁外调用 */
    return s_talk_held;
}

void ui_pages_set_talk_ready(bool ready)
{
    if (s_mic_btn == NULL) {
        return;
    }
    /* 用 LVGL 的 DISABLED 状态而不是自己记标志 ——
       处于该状态的对象不会再派发 PRESSED，所以"没连上就按不动"是天然成立的，
       不需要在回调里再判一次。 */
    if (ready) {
        lv_obj_remove_state(s_mic_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_mic_btn, LV_STATE_DISABLED);
        s_talk_held = false;    /* 变成不可用的瞬间必须清掉，否则会卡在"一直按住" */
    }
    lv_obj_set_style_bg_opa(s_mic_btn, ready ? LV_OPA_COVER : LV_OPA_50, 0);

    /* 图标要跟着一起淡 —— 它是**独立的对象**，不受按钮 bg_opa 影响。
       只调按钮不调图标的话，会出现"淡色圆底上顶着一个亮白麦克风"的怪样子。
       用 opa（而非 bg_opa）是因为它会带上孩子一起变。 */
    if (s_mic_icon != NULL) {
        lv_obj_set_style_opa(s_mic_icon, ready ? LV_OPA_COVER : LV_OPA_50, 0);
    }

    if (s_mic_btn_lbl != NULL) {
        lv_label_set_text(s_mic_btn_lbl, ready ? "按住说话" : "语音（未连接）");
    }
}

void ui_pages_set_talk_state(ui_talk_state_t st)
{
    if (s_mic_btn == NULL || s_mic_btn_lbl == NULL) {
        return;
    }

    /* 标签文字**留在界面层** —— 与其它页面文案一致，业务代码不该拼 UI 字符串。
       配色只区分"是否正在进行中"：进行中用暖色，静默用主题强调色。 */
    switch (st) {
    case UI_TALK_LISTENING:
        lv_label_set_text(s_mic_btn_lbl, "正在听…");
        lv_obj_set_style_bg_color(s_mic_btn, lv_color_hex(TH_BUSY), 0);
        break;
    case UI_TALK_WORKING:
        lv_label_set_text(s_mic_btn_lbl, "识别中…");
        lv_obj_set_style_bg_color(s_mic_btn, lv_color_hex(TH_BUSY), 0);
        break;
    case UI_TALK_SPEAKING:
        lv_label_set_text(s_mic_btn_lbl, "回复中…");
        lv_obj_set_style_bg_color(s_mic_btn, lv_color_hex(TH_BUSY), 0);
        break;
    case UI_TALK_IDLE:
    default:
        /* 回到 IDLE 时按当前是否可用决定文案 —— 别覆盖掉"未连接" */
        lv_label_set_text(s_mic_btn_lbl,
                          lv_obj_has_state(s_mic_btn, LV_STATE_DISABLED)
                              ? "语音（未连接）" : "按住说话");
        lv_obj_set_style_bg_color(s_mic_btn, lv_color_hex(TH_ACCENT), 0);
        break;
    }
}

/* --------------------------------------------------------------------------
 * 内部小工具
 * -------------------------------------------------------------------------- */

/** 多行文本里**最宽一行**的像素宽度。
 *
 *  为什么逐行量而不是整串量：`lv_text_get_width()` 会把 '\n' 也算进宽度，
 *  多行消息会因此偏宽、气泡留出多余的右边距。逐行取最大值更贴合内容。
 *  （xiaozhi 是整串量的 —— 它那边偏宽一点看不出来。这里顺手做对。） */
static lv_coord_t chat_text_width(const char *text, const lv_font_t *font)
{
    lv_coord_t max_w = 0;
    const char *p = text;

    while (p != NULL && *p != '\0') {
        const char *nl = strchr(p, '\n');
        const size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
        if (len > 0) {
            const lv_coord_t w = lv_text_get_width(p, (uint32_t)len, font, 0);
            if (w > max_w) {
                max_w = w;
            }
        }
        p = (nl != NULL) ? (nl + 1) : NULL;
    }
    return max_w;
}

/** 删掉最早的消息，直到条数低于上限。
 *  上限存在的理由：控件数无限增长会让每帧的布局遍历变慢（本工程是单缓冲
 *  部分刷新，控件越多重绘代价越大）。 */
static void chat_trim(void)
{
    /* ⚠️ s_chat_list 的直接子对象是**行容器**，不是气泡 —— 见文件头的说明。 */
    while (lv_obj_get_child_count(s_chat_list) >= AI_CHAT_MAX_MSG) {
        lv_obj_t *row = lv_obj_get_child(s_chat_list, 0);
        if (row == NULL) {
            break;
        }
        /* 被删的这行可能就是照片所在行 —— 指针要跟着清，
           否则后面 ui_pages_chat_add_photo() 会对已删除对象操作。 */
        if (s_chat_photo != NULL && lv_obj_get_parent(s_chat_photo) == row) {
            s_chat_photo = NULL;
        }
        lv_obj_del(row);
    }
}

/** 建一个整宽行容器，内部按 main_align 横向对齐。
 *
 *  ⚠️ **不调 `lv_obj_remove_style_all()`** —— 照 xiaozhi 的写法：
 *  只覆盖需要改的样式（背景透明、无边框、无内边距），其余保留主题默认值。
 *  默认值里有 `width/height = LV_SIZE_CONTENT` 这类关键项，全抹掉会让
 *  尺寸解析失去起点。 */
static lv_obj_t *chat_make_row(lv_flex_align_t main_align)
{
    lv_obj_t *row = lv_obj_create(s_chat_list);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, main_align,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

/** 滚到底。先强制布局一次，否则刚加进去的行还没有尺寸，滚不到底。 */
static void chat_scroll_to_bottom(void)
{
    lv_obj_update_layout(s_chat_list);
    lv_obj_scroll_to_y(s_chat_list, LV_COORD_MAX, LV_ANIM_OFF);
}

/* --------------------------------------------------------------------------
 * 追加消息
 * -------------------------------------------------------------------------- */
void ui_pages_chat_add(ui_chat_role_t role, const char *text)
{
    if (s_chat_list == NULL || text == NULL || text[0] == '\0') {
        return;
    }

    /* ---- 系统消息折叠（照搬 xiaozhi :533-548）----
       连续多条系统消息时**删掉上一条**而不是追加 —— 系统消息是状态提示，
       堆一屏会淹没真正的对话内容。 */
    const uint32_t n_child = lv_obj_get_child_count(s_chat_list);
    if (role == UI_CHAT_SYSTEM && n_child > 0) {
        lv_obj_t *last = lv_obj_get_child(s_chat_list, (int32_t)n_child - 1);
        if (last != NULL && lv_obj_get_child_count(last) > 0) {
            lv_obj_t *last_bubble = lv_obj_get_child(last, 0);
            if (last_bubble != NULL &&
                lv_obj_get_user_data(last_bubble) == (void *)(intptr_t)UI_CHAT_SYSTEM) {
                lv_obj_del(last);
            }
        }
    }

    chat_trim();

    /* 行容器：整宽透明 —— 照搬 xiaozhi 的做法（:640-677）。
       用户/系统气泡要靠它做右对齐/居中；助手气泡直接挂 chat_list（靠列的
       cross-align START 自然左对齐），xiaozhi 也是这样分两种放法的。 */
    lv_obj_t *row = chat_make_row(
        (role == UI_CHAT_USER)   ? LV_FLEX_ALIGN_END
      : (role == UI_CHAT_SYSTEM) ? LV_FLEX_ALIGN_CENTER
      :                            LV_FLEX_ALIGN_START);

    /* ---- 气泡本体 ----
       ⚠️ **不要**对气泡调 lv_obj_remove_style_all()！
       那是本工程其它页面（卡片等）的惯例，但会连默认的
       width/height = LV_SIZE_CONTENT 一起抹掉，使尺寸解析失去起点 ——
       表现就是"文字与气泡对不上""图片只显示一半"。xiaozhi 的气泡是
       "只改需要的样式、保留主题默认值"，这里照它写。 */
    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_style_radius(bubble, TH_CHAT_RADIUS, 0);
    lv_obj_set_style_pad_all(bubble, TH_CHAT_PAD, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_flex_grow(bubble, 0, 0);
    lv_obj_set_user_data(bubble, (void *)(intptr_t)role);

    lv_obj_t *label = lv_label_create(bubble);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &font_puhui_16_4, 0);
    lv_obj_set_style_text_line_space(label, 6, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

    /* 宽度按**实测文本宽度**自适应，上限屏宽 85%（照搬 xiaozhi :563-580）。
       ⚠️ 气泡宽要**留出内边距**（+2×pad）—— xiaozhi 那边把气泡设成与文字同宽，
       而气泡有 8px padding，文字实际比可用区宽 16px，会被挤着换行。
       这里补上这 16px，是本实现对参考的唯一修正。 */
    lv_coord_t w = chat_text_width(text, &font_puhui_16_4);
    if (w < 20)            { w = 20; }
    if (w > TH_CHAT_MAX_W) { w = TH_CHAT_MAX_W; }
    lv_obj_set_width(label, w);
    lv_obj_set_width(bubble, w + 2 * TH_CHAT_PAD);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);

    switch (role) {
    case UI_CHAT_USER:
        lv_obj_set_style_bg_color(bubble, lv_color_hex(TH_BUBBLE_USER), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(TH_TEXT), 0);
        break;
    case UI_CHAT_ASSISTANT:
        lv_obj_set_style_bg_color(bubble, lv_color_hex(TH_BUBBLE_AI), 0);
        lv_obj_set_style_border_color(bubble, lv_color_hex(TH_EDGE), 0);
        lv_obj_set_style_border_width(bubble, 1, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(TH_TEXT), 0);
        break;
    default:    /* UI_CHAT_SYSTEM */
        lv_obj_set_style_bg_color(bubble, lv_color_hex(TH_BUBBLE_SYS), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(TH_TEXT_SYS), 0);
        break;
    }

    chat_scroll_to_bottom();
}

void ui_pages_chat_add_photo(const lv_image_dsc_t *dsc)
{
    if (s_chat_list == NULL || dsc == NULL ||
        dsc->header.w == 0 || dsc->header.h == 0) {
        return;
    }

    /* ---- 同一时刻只留一张照片：先删旧的整行 ----
       原因见 ui_pages.h —— 照片缓冲只有两块，留多了会互相覆盖。 */
    if (s_chat_photo != NULL) {
        lv_obj_t *old_row = lv_obj_get_parent(s_chat_photo);
        lv_obj_del((old_row != NULL) ? old_row : s_chat_photo);
        s_chat_photo = NULL;
    }

    chat_trim();

    /* ---- 缩放因子（照搬 xiaozhi `SetPreviewImage()` :744-760）----
       图片限制在 **屏宽70% × 屏高50%** 之内。
       为什么必须缩放而不能 1:1 直显：QVGA 图是 320×240，而聊天区只有
       480 × (320-44-52) = 480×224 —— **1:1 根本放不下，会被裁掉一半**。

       LVGL 的缩放单位是 1/256（256 = 100%）；取宽高两个限制里更小的那个，
       保证整图都装得下；图本来就比限制小时不放大（钳到 256）。
       缩放开销：LVGL 只在对象标脏时重算，静态气泡不会每帧重绘，可接受。 */
    const lv_coord_t max_w = TH_W * 70 / 100;
    const lv_coord_t max_h = TH_H * 50 / 100;
    const lv_coord_t img_w = (lv_coord_t)dsc->header.w;
    const lv_coord_t img_h = (lv_coord_t)dsc->header.h;

    lv_coord_t zoom_w = (max_w * 256) / img_w;
    lv_coord_t zoom_h = (max_h * 256) / img_h;
    lv_coord_t zoom   = (zoom_w < zoom_h) ? zoom_w : zoom_h;
    if (zoom > 256) {
        zoom = 256;
    }

    const lv_coord_t sw = (img_w * zoom) / 256;
    const lv_coord_t sh = (img_h * zoom) / 256;

    /* 照片与助手消息一样左对齐、白底 —— 它是"给你看的东西"，
       不是用户自己说的话。xiaozhi 的 img_bubble 也是这么摆的。 */
    lv_obj_t *row = chat_make_row(LV_FLEX_ALIGN_START);

    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_style_radius(bubble, TH_CHAT_RADIUS, 0);
    lv_obj_set_style_pad_all(bubble, TH_CHAT_PAD, 0);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(TH_BUBBLE_AI), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bubble, lv_color_hex(TH_EDGE), 0);
    lv_obj_set_style_border_width(bubble, 1, 0);
    lv_obj_set_scrollbar_mode(bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_flex_grow(bubble, 0, 0);

    /* ⚠️ 气泡尺寸**必须显式给**：`lv_image_set_scale()` 只改**绘制**缩放，
       不改对象自身尺寸（对象尺寸仍是源的 w×h）。所以是按缩放后的尺寸
       加内边距来定气泡大小 —— 与 xiaozhi 的 `scaled_width + 16` 同款。 */
    lv_obj_set_size(bubble, sw + 2 * TH_CHAT_PAD, sh + 2 * TH_CHAT_PAD);

    s_chat_photo = lv_image_create(bubble);
    lv_image_set_src(s_chat_photo, dsc);
    lv_image_set_scale(s_chat_photo, zoom);
    lv_obj_center(s_chat_photo);

    chat_scroll_to_bottom();
}

/* --------------------------------------------------------------------------
 * 麦克风图标
 *
 * 【为什么是画出来的，不是字符】
 *   · LVGL 内置符号里**没有麦克风**。最接近的 `LV_SYMBOL_AUDIO` 是
 *     "带声波的喇叭"（Font Awesome 0xF001），语义是音量不是录音。
 *   · 而且本工程标签用的是普惠体（`font_puhui_*`），**不含** Font Awesome
 *     那段私用区码位 —— 硬塞 LV_SYMBOL_AUDIO 只会画出一个缺字方块。
 *   故用图元拼：胶囊机身 + 竖杆 + 基座横线，是最经典的麦克风造型，
 *   且完全不依赖字体。
 * -------------------------------------------------------------------------- */

/**
 * @brief 画一个实心圆角矩形，当图标的零件。
 *
 * ⚠️ **必须去掉 CLICKABLE**：`lv_obj` 默认可点击，零件会把手势从按钮上吃掉 ——
 *    现象是"图标画得挺好看，但按住没反应"。因为视觉完全正常，
 *    这种问题很难往"零件把事件挡了"上想，所以在这里写死。
 */
static lv_obj_t *icon_part(lv_obj_t *parent, int x, int y, int w, int h,
                           int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);   /* ★ 见函数头 */
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(TH_CARD), 0);   /* 白色图标 */
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

/** 在圆钮里居中地画一个麦克风。坐标相对 18 × 22 的画布。 */
static void build_mic_icon(lv_obj_t *btn)
{
    s_mic_icon = lv_obj_create(btn);
    lv_obj_remove_style_all(s_mic_icon);
    lv_obj_remove_flag(s_mic_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_mic_icon, LV_OBJ_FLAG_CLICKABLE);   /* 同上 */
    lv_obj_set_size(s_mic_icon, AI_ICON_W, AI_ICON_H);
    lv_obj_center(s_mic_icon);

    /* ① 机身：竖胶囊（半径取宽度的一半，两端正好是半圆）*/
    icon_part(s_mic_icon, 4, 0, 10, 15, 5);
    /* ② 竖杆：从机身底连到基座 */
    icon_part(s_mic_icon, 8, 15, 2, 5, 1);
    /* ③ 基座横线 */
    icon_part(s_mic_icon, 1, 20, 16, 2, 1);
}

/* --------------------------------------------------------------------------
 * 建页
 * -------------------------------------------------------------------------- */
static void build_ai_page(lv_obj_t *root)
{
    lv_obj_t *content = ui_page_make_shell(root, "小智AI互动", true);

    /* ---- 聊天区：纵向 flex + 滚动 ---- */
    s_chat_list = lv_obj_create(content);
    lv_obj_remove_style_all(s_chat_list);
    lv_obj_set_pos(s_chat_list, 0, 0);
    lv_obj_set_size(s_chat_list, TH_W, TH_H - TH_HEADER_H - TH_CHAT_BAR_H);
    lv_obj_set_style_bg_color(s_chat_list, lv_color_hex(TH_CHAT_BG), 0);
    lv_obj_set_style_bg_opa(s_chat_list, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_chat_list, 8, 0);
    lv_obj_set_style_pad_row(s_chat_list, TH_CHAT_GAP_Y, 0);
    lv_obj_set_scrollbar_mode(s_chat_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_chat_list, LV_DIR_VER);
    lv_obj_set_flex_flow(s_chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_chat_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* ---- 底部操作栏 ----
       本页其它地方一律绝对定位，这里改用 flex：要让「圆钮 + 状态文字」
       整体居中，而文字的宽度随状态变（"按住说话"/"识别中…"…），
       绝对定位算不出这个宽度。 */
    lv_obj_t *bar = lv_obj_create(content);
    lv_obj_remove_style_all(bar);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(bar, 0, TH_H - TH_HEADER_H - TH_CHAT_BAR_H);
    lv_obj_set_size(bar, TH_W, TH_CHAT_BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(TH_CARD), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_column(bar, 14, 0);     /* 圆钮与文字之间的间距 */
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* ---- 圆形语音键（唯一的一个按钮）----
       按住 = 开始录并上行；松开 = 停止上行，等服务器回复。
       选"按住说话"而不是"点一下开始、再点一下结束"，是因为前者
       松手即停，用户不需要记住"现在是不是在录"这个状态。 */
    s_mic_btn = lv_obj_create(bar);
    lv_obj_remove_style_all(s_mic_btn);
    lv_obj_remove_flag(s_mic_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_mic_btn, AI_MIC_D, AI_MIC_D);
    lv_obj_set_style_radius(s_mic_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_mic_btn, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_mic_btn, LV_OPA_COVER, 0);
    /* 初始不可用，等 xz_client 连上服务器后由 ui_lvgl_set_talk_ready(true) 打开 */
    lv_obj_add_state(s_mic_btn, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_mic_btn, on_mic_pressed,   LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(s_mic_btn, on_mic_released,  LV_EVENT_RELEASED,   NULL);
    lv_obj_add_event_cb(s_mic_btn, on_mic_released,  LV_EVENT_PRESS_LOST, NULL);

    build_mic_icon(s_mic_btn);

    /* ---- 状态文字 ----
       **不是按钮**，只是提示。保留它是因为圆钮上只有一个图标，而
       "按住"这个动作光看麦克风图标猜不出来（图标通常意味着"点一下"）。
       文字随状态变，见 ui_pages_set_talk_state()。 */
    s_mic_btn_lbl = lv_label_create(bar);
    lv_label_set_text(s_mic_btn_lbl, "语音（未连接）");
    lv_obj_set_style_text_font(s_mic_btn_lbl, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(s_mic_btn_lbl, lv_color_hex(TH_TEXT), 0);
}

/* ==========================================================================
 * SD 卡相册页（视觉假数据）
 * ========================================================================== */
#define ALBUM_COLS  3
#define ALBUM_ROWS  2
#define ALBUM_CNT   (ALBUM_COLS * ALBUM_ROWS)

static void build_album_page(lv_obj_t *root)
{
    lv_obj_t *content = ui_page_make_shell(root, "SD卡相册", true);

    const int m    = 16;
    const int gap  = 10;
    const int cell_w = (TH_W - 2 * m - (ALBUM_COLS - 1) * gap) / ALBUM_COLS;
    const int cell_h = (TH_H - TH_HEADER_H - 2 * m - 26 - (ALBUM_ROWS - 1) * gap) / ALBUM_ROWS;

    for (int i = 0; i < ALBUM_CNT; i++) {
        const int r = i / ALBUM_COLS;
        const int c = i % ALBUM_COLS;

        lv_obj_t *cell = lv_obj_create(content);
        lv_obj_remove_style_all(cell);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(cell, m + c * (cell_w + gap), m + r * (cell_h + gap));
        lv_obj_set_size(cell, cell_w, cell_h);
        lv_obj_set_style_bg_color(cell, lv_color_hex(TH_CARD), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(cell, 10, 0);
        lv_obj_set_style_border_color(cell, lv_color_hex(TH_EDGE), 0);
        lv_obj_set_style_border_width(cell, 1, 0);

        /* 占位图标：明确是「还没有图」，不是加载失败 */
        lv_obj_t *ic = lv_label_create(cell);
        lv_label_set_text(ic, FONT_AWESOME_IMAGE);
        lv_obj_set_style_text_font(ic, &font_awesome_20_4, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(TH_EDGE), 0);
        lv_obj_center(ic);
    }

    lv_obj_t *note = lv_label_create(content);
    lv_label_set_text(note, "占位缩略图 —— TF 卡驱动接入后显示真实抓拍");
    lv_obj_set_style_text_font(note, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(note, lv_color_hex(TH_LABEL), 0);
    lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, m, -8);
}

/* ==========================================================================
 * 初始化与切页
 * ========================================================================== */
void ui_pages_init(void)
{
    s_page[UI_PAGE_MENU]  = make_root();
    s_page[UI_PAGE_TANK]  = make_root();
    s_page[UI_PAGE_AI]    = make_root();
    s_page[UI_PAGE_ALBUM] = make_root();

    build_menu_page(s_page[UI_PAGE_MENU]);

    /* 鱼缸页：它自己建顶栏（还要在右上角放链路图标），故传页面根容器 */
    ui_dashboard_create(s_page[UI_PAGE_TANK]);

    build_ai_page(s_page[UI_PAGE_AI]);
    build_album_page(s_page[UI_PAGE_ALBUM]);

    ui_pages_show(UI_PAGE_MENU);
    ESP_LOGI(TAG, "四个页面就绪，当前显示：主菜单");
}

void ui_pages_show(ui_page_t page)
{
    if (page >= UI_PAGE_CNT || s_page[page] == NULL) {
        ESP_LOGW(TAG, "切页请求非法（page=%d），忽略", (int)page);
        return;
    }
    if (page == s_cur && !lv_obj_has_flag(s_page[page], LV_OBJ_FLAG_HIDDEN)) {
        return;     /* 已在该页 */
    }

    for (int i = 0; i < UI_PAGE_CNT; i++) {
        if (s_page[i] == NULL) {
            continue;
        }
        if ((ui_page_t)i == page) {
            lv_obj_remove_flag(s_page[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_page[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_cur = page;
}

void ui_pages_update_tank(const tank_ecosystem_t *snap)
{
    ui_dashboard_update(snap);
}
