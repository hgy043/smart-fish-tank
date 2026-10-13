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
 * 小智 AI 互动页 —— 拍照与照片显示（P6-a 接入真实摄像头）
 *
 * 【布局】内容区是 480 × (320-44) = 480 × 276
 *     照片区   320 × 240   居中靠上（x = 80，y = 0）
 *     拍照按钮 452 × 32    整宽靠下（y = 242）
 *   320×240 是**原尺寸直显**，不做任何缩放 —— 本工程 CONFIG_LV_CACHE_DEF_SIZE=0
 *   （没有图像缓存），LVGL 缩放要每帧软件重算，既费 CPU 又糊。
 *
 * 【为什么按钮不做成小圆钮】整宽按钮的命中面积大得多，也不用管对中线。
 *   状态文字（拍摄中/失败原因）直接写在照片区中央，不额外占纵向空间。
 * ========================================================================== */
#define AI_PHOTO_W   320
#define AI_PHOTO_H   240
#define AI_BTN_H     32

static lv_obj_t *s_ai_photo   = NULL;   /* lv_image：照片本体 */
static lv_obj_t *s_ai_hint    = NULL;   /* 照片区中央的提示/状态文字 */
static lv_obj_t *s_ai_btn     = NULL;
static lv_obj_t *s_ai_btn_lbl = NULL;

/* 拍照请求标志。
 * ⚠️ 只允许**置位**（在 LVGL 回调里）与**取走**（在 ui_task 锁外轮询），
 *    任何一侧都不做别的事 —— 耗时动作一律留在锁外。 */
static volatile bool s_capture_req = false;

static void on_capture_clicked(lv_event_t *e)
{
    (void)e;
    /* 这里跑在 taskLVGL 上下文、**且已持 LVGL 锁**。
       所以只能置个标志就返回 —— 一旦在这里调 camera_service_capture()，
       最长 4 秒的阻塞会把整个界面（含触摸）冻住。 */
    s_capture_req = true;
}

bool ui_pages_take_capture_request(void)
{
    if (!s_capture_req) {
        return false;
    }
    s_capture_req = false;
    return true;
}

lv_obj_t *ui_pages_ai_photo(void)
{
    return s_ai_photo;
}

void ui_pages_ai_hint_visible(bool visible)
{
    if (s_ai_hint == NULL) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(s_ai_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ai_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_pages_set_ai_status(const char *text, bool button_enabled)
{
    if (s_ai_hint != NULL) {
        if (text != NULL && text[0] != '\0') {
            lv_label_set_text(s_ai_hint, text);
            lv_obj_remove_flag(s_ai_hint, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ai_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_ai_btn != NULL) {
        /* 置灰但仍可见 —— 比直接隐藏更能说明"功能存在但不可用"。
           用 LVGL 的 DISABLED 状态而不是自己记标志：处于该状态的对象
           **不会**再派发 CLICKED，所以连点/拍摄中再点都不会进回调。
           （若只改透明度而不设状态，回调照样触发，得在回调里再判一次。） */
        if (button_enabled) {
            lv_obj_remove_state(s_ai_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_ai_btn, LV_STATE_DISABLED);
        }
        lv_obj_set_style_bg_opa(s_ai_btn, button_enabled ? LV_OPA_COVER : LV_OPA_50, 0);

        if (s_ai_btn_lbl != NULL) {
            lv_label_set_text(s_ai_btn_lbl, button_enabled ? "拍照" : "摄像头不可用");
        }
    }
}

static void build_ai_page(lv_obj_t *root)
{
    lv_obj_t *content = ui_page_make_shell(root, "小智AI互动", true);

    /* ---- 照片区外框：即使还没拍过照，也能看出"这里会出画面" ---- */
    lv_obj_t *frame = lv_obj_create(content);
    lv_obj_remove_style_all(frame);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(frame, (TH_W - AI_PHOTO_W) / 2, 0);
    lv_obj_set_size(frame, AI_PHOTO_W, AI_PHOTO_H);
    lv_obj_set_style_bg_color(frame, lv_color_hex(TH_CARD), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(frame, lv_color_hex(TH_EDGE), 0);
    lv_obj_set_style_border_width(frame, 1, 0);

    /* 照片控件：尺寸与 source 一致（320×240），故定位于 (0,0) 即可铺满 */
    s_ai_photo = lv_image_create(frame);
    lv_obj_set_pos(s_ai_photo, 0, 0);

    /* 中央提示 / 状态文字。多行 + 居中。 */
    s_ai_hint = lv_label_create(frame);
    lv_label_set_text(s_ai_hint, "点下方按钮拍照");
    lv_obj_set_style_text_font(s_ai_hint, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(s_ai_hint, lv_color_hex(TH_LABEL), 0);
    lv_obj_set_style_text_align(s_ai_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_ai_hint, AI_PHOTO_W - 24);
    lv_label_set_long_mode(s_ai_hint, LV_LABEL_LONG_WRAP);
    lv_obj_center(s_ai_hint);

    /* ---- 底部拍照按钮（整宽）---- */
    s_ai_btn = lv_obj_create(content);
    lv_obj_remove_style_all(s_ai_btn);
    lv_obj_remove_flag(s_ai_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_ai_btn, TH_MARGIN, AI_PHOTO_H + 2);
    lv_obj_set_size(s_ai_btn, TH_W - 2 * TH_MARGIN, AI_BTN_H);
    lv_obj_set_style_bg_color(s_ai_btn, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_ai_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_ai_btn, 8, 0);
    lv_obj_add_event_cb(s_ai_btn, on_capture_clicked, LV_EVENT_CLICKED, NULL);

    s_ai_btn_lbl = lv_label_create(s_ai_btn);
    lv_label_set_text(s_ai_btn_lbl, "拍照");
    lv_obj_set_style_text_font(s_ai_btn_lbl, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(s_ai_btn_lbl, lv_color_hex(TH_CARD), 0);
    lv_obj_center(s_ai_btn_lbl);
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
