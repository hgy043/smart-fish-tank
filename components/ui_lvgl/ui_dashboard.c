/**
 * @file ui_dashboard.c
 * @brief 鱼缸情况页 —— 六张数据卡片（浅色主题）
 *
 * 布局与调用约定见 include/ui_dashboard.h。
 *
 * ============================================================================
 * 【两处刻意的设计】
 *
 * ① **文本缓存**：lv_label_set_text() 即使内容完全相同也会把对象标脏并触发
 *    重绘。ui_task 以 200ms 节拍刷新，若无条件 set，界面会一直处于「被标脏」
 *    状态，白白消耗 CPU 与 SPI 带宽。故每个标签配一份缓存，只在内容真变时才写。
 *
 * ② **数值按数据类别着色**，而不是一律黑字：水质（青）/ 温度（橙）/ 湿度（青绿）
 *    / 光照（琥珀）。这样扫一眼就能分清「这条是水的还是空气的」，不必逐个读标签。
 *    标签用灰、数值用彩色，层级由**字号 + 颜色**共同承担。
 * ============================================================================
 */
#include "ui_dashboard.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "ui_pages.h"       /* ui_page_make_shell */
#include "ui_theme.h"

#include "font_awesome.h"

LV_FONT_DECLARE(font_puhui_20_4);   /* 数值 */
LV_FONT_DECLARE(font_puhui_16_4);   /* 标签 */
LV_FONT_DECLARE(font_awesome_20_4); /* 顶栏链路图标 */

static const char *TAG = "ui_tank";

/* ==========================================================================
 * 布局（内容区 = 480 × (320-44) = 480 × 276）
 * ========================================================================== */
#define GRID_M     14                                   /* 内容区留白 */
#define GAP_X      12
#define GAP_Y      12
#define COLS       2
#define ROWS       3
#define CARD_W     ((TH_W - 2 * GRID_M - GAP_X) / COLS)  /* = 220 */
#define CARD_H     ((TH_H - TH_HEADER_H - 2 * GRID_M - (ROWS - 1) * GAP_Y) / ROWS) /* = 74 */

/* ==========================================================================
 * 卡片定义表 —— 建界面与刷新共用，避免两处各写一份列表而在加字段时漏改
 * ========================================================================== */
typedef struct {
    const char *name;
    const char *fmt;
    uint32_t    color;      /* 正常时的数值颜色 */
    uint32_t    alert_bit;  /* 对应 tank_ecosystem_t.alert_mask 的位；0 = 无告警位 */
    uint8_t     col, row;
} card_def_t;

/* 卡片定义表 —— 建界面与刷新共用，避免两处各写一份列表而在加字段时漏改。
   alert_bit 取自 app_core/app_types.h 的 ECO_ALERT_*（位定义在那里，
   阈值仍在 main/app_config.h —— 组件不能依赖 main，分层如此）。 */
static const card_def_t k_cards[] = {
    { "pH",   "%.2f",     TH_WATER, ECO_ALERT_PH,        0, 0 },
    { "水温", "%.1f ℃",   TH_TEMP,  ECO_ALERT_WATER_TMP, 1, 0 },
    { "气温", "%.1f ℃",   TH_TEMP,  ECO_ALERT_AIR_TMP,   0, 1 },
    { "湿度", "%.0f %%",  TH_HUM,   ECO_ALERT_HUMIDITY,  1, 1 },
    { "光照", "%.0f lx",  TH_LUX,   ECO_ALERT_LUX,       0, 2 },
    { "浊度", "%.2f NTU", TH_WATER, ECO_ALERT_TURBIDITY, 1, 2 },
};
#define CARD_CNT ((int)(sizeof(k_cards) / sizeof(k_cards[0])))

/* ==========================================================================
 * 控件与缓存
 * ========================================================================== */
static lv_obj_t *s_val[CARD_CNT];
static char      s_val_cache[CARD_CNT][24];

/* 告警着色状态缓存：-1 = 尚未写入过颜色。
   与 s_wifi_on / s_mqtt_on 同款做法 —— **只在状态翻转时**才调
   lv_obj_set_style_text_color()。若每 200ms 无条件 set 一次，
   LVGL 会把标签标脏并重绘，白白占用 SPI 带宽（本工程单缓冲部分刷新，
   重绘代价是实打实的）。 */
static int8_t    s_val_alert[CARD_CNT] = { -1, -1, -1, -1, -1, -1 };

static lv_obj_t *s_ic_wifi = NULL;
static lv_obj_t *s_ic_mqtt = NULL;
static int8_t    s_wifi_on  = -1;    /* -1 = 尚未写入过 */
static int8_t    s_mqtt_on  = -1;

static float card_value(int idx, const tank_ecosystem_t *s)
{
    switch (idx) {
    case 0:  return s->ph;
    case 1:  return s->water_temp_c;
    case 2:  return s->air_temp_c;
    case 3:  return s->air_humidity;
    case 4:  return s->lux;
    case 5:  return s->turbidity_ntu;
    default: return 0.0f;
    }
}

/** 仅在文本变化时才写入标签 */
static void set_if_changed(lv_obj_t *lbl, char *cache, size_t cap, const char *text)
{
    if (strncmp(cache, text, cap) == 0) {
        return;
    }
    snprintf(cache, cap, "%s", text);   /* 先入缓存再交给 LVGL，明确生命周期 */
    lv_label_set_text(lbl, cache);
}

/* ==========================================================================
 * 建界面
 * ========================================================================== */

/** 顶栏右上角的链路图标：WiFi 用扇形、MQTT 用云朵，断线换斜杠版并变红 */
static void create_link_icons(lv_obj_t *page_root)
{
    s_ic_mqtt = lv_label_create(page_root);
    lv_label_set_text(s_ic_mqtt, "");
    lv_obj_set_style_text_font(s_ic_mqtt, &font_awesome_20_4, 0);
    lv_obj_align(s_ic_mqtt, LV_ALIGN_TOP_RIGHT, -TH_MARGIN, 12);

    s_ic_wifi = lv_label_create(page_root);
    lv_label_set_text(s_ic_wifi, "");
    lv_obj_set_style_text_font(s_ic_wifi, &font_awesome_20_4, 0);
    lv_obj_align(s_ic_wifi, LV_ALIGN_TOP_RIGHT, -TH_MARGIN - 32, 12);
}

static void create_card(lv_obj_t *content, int idx)
{
    const card_def_t *d = &k_cards[idx];

    lv_obj_t *card = lv_obj_create(content);
    lv_obj_remove_style_all(card);      /* 去掉默认内边距/边框，逐项自定 */
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(card, GRID_M + d->col * (CARD_W + GAP_X),
                          GRID_M + d->row * (CARD_H + GAP_Y));
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(TH_EDGE), 0);
    lv_obj_set_style_border_width(card, 1, 0);

    lv_obj_t *name = lv_label_create(card);
    lv_label_set_text(name, d->name);
    lv_obj_set_style_text_color(name, lv_color_hex(TH_LABEL), 0);
    lv_obj_set_style_text_font(name, &font_puhui_16_4, 0);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 14, 6);

    s_val[idx] = lv_label_create(card);
    lv_label_set_text(s_val[idx], "--");
    lv_obj_set_style_text_color(s_val[idx], lv_color_hex(d->color), 0);
    lv_obj_set_style_text_font(s_val[idx], &font_puhui_20_4, 0);
    lv_obj_align(s_val[idx], LV_ALIGN_TOP_LEFT, 14, 32);
}

void ui_dashboard_create(lv_obj_t *page_root)
{
    create_link_icons(page_root);

    /* 顶栏由公共构件生成（标题居中、带返回键），返回值即内容容器 */
    lv_obj_t *content = ui_page_make_shell(page_root, "鱼缸情况", true);

    for (int i = 0; i < CARD_CNT; i++) {
        create_card(content, i);
    }
    ESP_LOGI(TAG, "鱼缸情况页就绪（%d 张卡片）", CARD_CNT);
}

/* ==========================================================================
 * 刷新
 * ========================================================================== */
static void update_link_icon(lv_obj_t *icon, int8_t *state,
                             const char *glyph_on, const char *glyph_off, bool ok)
{
    if (*state == (int8_t)ok) {
        return;     /* 状态没变就不动，避免无谓重绘 */
    }
    *state = (int8_t)ok;
    lv_label_set_text(icon, ok ? glyph_on : glyph_off);
    lv_obj_set_style_text_color(icon, lv_color_hex(ok ? TH_OK : TH_BAD), 0);
}

void ui_dashboard_update(const tank_ecosystem_t *snap)
{
    if (snap == NULL || s_val[0] == NULL) {
        return;     /* 界面尚未创建 */
    }

    if (s_ic_wifi != NULL) {
        update_link_icon(s_ic_wifi, &s_wifi_on,
                         FONT_AWESOME_WIFI, FONT_AWESOME_WIFI_SLASH,
                         snap->wifi_connected);
    }
    if (s_ic_mqtt != NULL) {
        update_link_icon(s_ic_mqtt, &s_mqtt_on,
                         FONT_AWESOME_CLOUD, FONT_AWESOME_CLOUD_SLASH,
                         snap->mqtt_connected);
    }

    char buf[24];
    for (int i = 0; i < CARD_CNT; i++) {
        snprintf(buf, sizeof(buf), k_cards[i].fmt, (double)card_value(i, snap));
        set_if_changed(s_val[i], s_val_cache[i], sizeof(s_val_cache[i]), buf);

        /* 越界则把数值改成红色。
           只在**翻转时**写颜色 —— 理由见 s_val_alert 的声明处。 */
        const bool alerting = (snap->alert_mask & k_cards[i].alert_bit) != 0;
        if (s_val_alert[i] != (int8_t)alerting) {
            s_val_alert[i] = (int8_t)alerting;
            lv_obj_set_style_text_color(
                s_val[i],
                lv_color_hex(alerting ? TH_BAD : k_cards[i].color), 0);
            ESP_LOGD(TAG, "卡片「%s」%s", k_cards[i].name,
                     alerting ? "进入告警" : "恢复正常");
        }
    }
}
