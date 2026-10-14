/**
 * @file font_service.c
 * @brief 中文字体服务 —— 见 include/font_service.h 顶部的完整说明
 */
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"

#include "lvgl.h"
#include "font/lv_font_fmt_txt.h"   /* lv_font_fmt_txt_dsc_t 的真定义，用于校验 bin */
#include "cbin_font.h"              /* 78/xiaozhi-fonts 自带的 cbin 加载器 */
#include "font_service.h"

static const char *TAG = "font";

/* 子集化的普惠体（CJK 只有 5258 个）。
 * ⚠️ 都是 **const**，在 flash 里 —— 见 font_service.h 的约束 1，不能直接写 .fallback。 */
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);

#define FONT_PARTITION_LABEL "fonts"

/* ==========================================================================
 * ⚠️ 编译期 ABI 守卫
 *
 * `fonts/font_noto_qwen_16_4.bin` 是按 **16 字节/条** 的字形描述符生成的
 * （即生成端 LV_FONT_FMT_TXT_LARGE=1）。本工程 sdkconfig 也是 =y。
 * 一旦有人把它关掉，字形描述符变成 8 字节 ⇒ **渲染出乱码而不是报错**，
 * 属于最难查的那类故障。所以把它钉在这里，改了编译就过不去。
 *
 * 判据与实测过程见 fonts/README.md「与本工程 sdkconfig 的 ABI 约定」。
 * ========================================================================== */
_Static_assert(sizeof(lv_font_fmt_txt_glyph_dsc_t) == 16,
               "LV_FONT_FMT_TXT_LARGE 与本工程字体 bin 不匹配 —— "
               "fonts/font_noto_qwen_16_4.bin 按 16 字节/条生成，见 fonts/README.md");

/* 16px + fallback / 20px + fallback 的 RAM 副本 */
static lv_font_t s_small;
static lv_font_t s_large;

static lv_font_t *s_fallback        = NULL;
static bool       s_inited          = false;
static char       s_status[112]     = "未初始化";

/* mmap 的基址与句柄。
 * ⚠️ **绝不能 munmap** —— cbin_font_create() 建出来的字体里，
 *    cmap 的 unicode_list / glyph_id_ofs_list 都直接指向这块映射区。 */
static const void                *s_map        = NULL;
static esp_partition_mmap_handle_t s_map_handle = 0;

/* ==========================================================================
 * bin 合法性校验
 * ========================================================================== */
/**
 * @brief 判断 mmap 出来的内容是不是一个能用的 cbin 字体。
 *
 * ⚠️ **这一步不能省。** 分区没烧过时整片是 0xFF，而 cbin 的第一个字段就是
 *    `lv_font_t.dsc`（一个**偏移量**）。0xFF…FF 会被解读成 0xFFFFFFFF，
 *    `cbin_font_create()` 随即去 `bin + 0xFFFFFFFF` 读结构体 —— 野指针直接崩。
 *    它自己没有任何入参校验，所以守卫必须在我们这边。
 *
 * 这里**只读标量字段**，函数指针字段一律不读、更不调用。
 *
 * @param why 失败时写入一句人话
 */
static bool bin_plausible(const void *base, size_t size, const char **why)
{
    const uint8_t *p = (const uint8_t *)base;

    if (size < 1024) { *why = "分区太小"; return false; }

    /* 擦除态快筛：未烧录的 flash 是 0xFF */
    if (p[0] == 0xFF && p[1] == 0xFF && p[2] == 0xFF && p[3] == 0xFF) {
        *why = "分区是空的（没烧过？）";
        return false;
    }

    const lv_font_t *f = (const lv_font_t *)base;
    if (f->line_height < 8 || f->line_height > 64) { *why = "line_height 越界"; return false; }

    const size_t dsc_off = (size_t)(uintptr_t)f->dsc;
    if (dsc_off == 0 || dsc_off + sizeof(lv_font_fmt_txt_dsc_t) > size) {
        *why = "dsc 偏移越界";
        return false;
    }

    const lv_font_fmt_txt_dsc_t *d =
        (const lv_font_fmt_txt_dsc_t *)(const void *)(p + dsc_off);

    /* cmap_num 是 9 位位域（上限 511），所以这里只能查"是不是 0" ——
     * 写 `> 1024` 编译器会判定恒假并报 -Werror=type-limits。 */
    if (d->cmap_num == 0) { *why = "cmap_num 为 0"; return false; }
    switch (d->bpp) {
    case 1: case 2: case 3: case 4: case 8: break;
    default: *why = "bpp 不是 1/2/3/4/8"; return false;
    }
    if (d->bitmap_format > LV_FONT_FMT_TXT_COMPRESSED_NO_PREFILTER) {
        *why = "bitmap_format 不认识（本工程未开 LV_USE_FONT_COMPRESSED）";
        return false;
    }

    /* 位图区 / 字形描述符区 / cmap 区的偏移都应落在分区内，且依次递增 */
    const size_t gb_off = (size_t)(uintptr_t)d->glyph_bitmap;
    const size_t gd_off = (size_t)(uintptr_t)d->glyph_dsc;
    const size_t cm_off = (size_t)(uintptr_t)d->cmaps;
    if (!(gb_off < gd_off && gd_off < cm_off && cm_off < size)) {
        *why = "bitmap/glyph_dsc/cmaps 偏移顺序不对";
        return false;
    }

    /* 字形条目数 = 字形描述符区长度 / 每条大小。本 bin 是 16186 条；
     * 取个宽松的下限就够抓「整个区域被截断/损坏」这类问题。 */
    const size_t n_glyph = (cm_off - gd_off) / sizeof(lv_font_fmt_txt_glyph_dsc_t);
    if (n_glyph < 100) { *why = "字形条目数少得不合理"; return false; }

    return true;
}

/* ==========================================================================
 * 自检 —— 为什么需要它
 *
 * 「缺字形」只在**真的渲染到那个字**时才报错，而屏幕上的中文大部分来自 AI，
 * **说什么不受控** —— 光靠"看着像好使了"没法验收。
 * 所以这里主动拿一个**已知普惠体里没有**的码位去问两个字体，当场得到判据。
 *
 * 探针取 U+6B38（欸）：它既不在普惠体字集里（见 fonts/README.md 的实测记录），
 * 也正是现场报过的那条 `glyph dsc. not found for U+6B38` 的主角。
 * ========================================================================== */
#define FONT_SELFTEST_PROBE 0x6B38u

static void font_selftest(void)
{
    const uint32_t probe = FONT_SELFTEST_PROBE;
    lv_font_glyph_dsc_t d;

    /* 不挂 fallback 的原始普惠体：这里**应当取不到**（否则探针失去区分度）。 */
    const bool in_puhui = lv_font_get_glyph_dsc(&font_puhui_16_4, &d, probe, 0);
    /* 界面实际用的字体：这里**应当取得到**。 */
    const bool in_ui    = lv_font_get_glyph_dsc(font_service_small(), &d, probe, 0);

    if (in_puhui) {
        ESP_LOGW(TAG, "自检失效：U+%04X 竟然在普惠体里 —— 探针不再有区分度，"
                      "请换一个码位（见 fonts/README.md）", (unsigned)probe);
        return;
    }
    if (!in_ui) {
        ESP_LOGW(TAG, "自检不通过：U+%04X 在界面字体里仍取不到字形（%s）"
                      "—— 该字仍会画成占位方框", (unsigned)probe, s_status);
        return;
    }

    ESP_LOGI(TAG, "自检通过：U+%04X 普惠体里没有、回退后取到了（%dx%d，来自%s）"
                  "—— 缺字回退确实生效",
             (unsigned)probe, (int)d.box_w, (int)d.box_h,
             (d.resolved_font == s_fallback) ? "Noto 回退" : "普惠体");
}

/* ==========================================================================
 * 公共接口
 * ========================================================================== */
esp_err_t font_service_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    /* 记下装入前后的内部堆 —— 见文件末尾那条日志的说明。
     * 必须在 memcpy/找分区**之前**取。
     * 用 heap_caps_get_free_size(MALLOC_CAP_INTERNAL) 而不是
     * esp_get_free_internal_heap_size()：两者**等价**（后者就是前者的薄封装），
     * 但前者在已包含的 esp_heap_caps.h 里，不必为此多引一个 esp_system。 */
    const uint32_t heap_before =
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    /* 先无条件把两份 RAM 副本建好并拷贝内容 —— 无论分区能不能用，
     * font_service_small()/large() 都必须返回有效指针（UI 直接拿它们去设字体）。 */
    memcpy(&s_small, &font_puhui_16_4, sizeof(lv_font_t));
    memcpy(&s_large, &font_puhui_20_4, sizeof(lv_font_t));
    s_small.fallback = NULL;
    s_large.fallback = NULL;
    s_inited = true;

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, FONT_PARTITION_LABEL);
    if (part == NULL) {
        snprintf(s_status, sizeof(s_status),
                 "没有 %s 分区 —— 缺字回退不生效（表现：生僻字画成方框）",
                 FONT_PARTITION_LABEL);
        ESP_LOGW(TAG, "%s", s_status);
        goto done;
    }

    /* 整个分区映射进来。offset=0、size=分区大小 都是 64KB 对齐的，
     * 不需要 esp_partition_mmap 做额外对齐处理。 */
    esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA,
                                       &s_map, &s_map_handle);
    if (err != ESP_OK) {
        snprintf(s_status, sizeof(s_status), "mmap %s 失败：%s",
                 FONT_PARTITION_LABEL, esp_err_to_name(err));
        ESP_LOGW(TAG, "%s", s_status);
        s_map = NULL;
        goto done;
    }

    const char *why = NULL;
    if (!bin_plausible(s_map, part->size, &why)) {
        snprintf(s_status, sizeof(s_status),
                 "%s 内容不像 cbin 字体（%s）—— 缺字回退不生效", FONT_PARTITION_LABEL, why);
        ESP_LOGW(TAG, "%s", s_status);
        goto done;
    }

    s_fallback = cbin_font_create((uint8_t *)s_map);
    if (s_fallback == NULL) {
        snprintf(s_status, sizeof(s_status), "cbin_font_create() 返回 NULL");
        ESP_LOGW(TAG, "%s", s_status);
        goto done;
    }

    s_small.fallback = s_fallback;
    s_large.fallback = s_fallback;

    const lv_font_fmt_txt_dsc_t *fd = (const lv_font_fmt_txt_dsc_t *)s_fallback->dsc;
    snprintf(s_status, sizeof(s_status),
             "Noto 回退已挂上（分区 %s %u 字节；%d 条 cmap，%d bpp）",
             FONT_PARTITION_LABEL, (unsigned)part->size,
             (int)fd->cmap_num, (int)fd->bpp);

    /* 把"实高"一并打出来是故意的：
     *   普惠体 16px 实高 20、20px 实高 25；回退字体（Noto 16px）实高 27。
     * 回退字形**坐在基础字体的基线上**（`lv_draw_label.c:587` 用的是
     * `font->line_height - font->base_line`，取自基础字体）—— 所以不会串行，
     * 但比同行字**略大一号**。这是 fallback 的固有代价，不是 bug。
     * 写进日志，省得下次有人当故障查。
     *
     * ⚠️ **内部堆增量也一并打出来**：本工程内部堆余量是 15.1 节的待决策项，
     *    任何新增的常驻占用都必须有实测数字，不能靠估。 */
    const uint32_t heap_after =
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "%s；实高：回退 %d，普惠体 16px=%d / 20px=%d"
                  "（回退只在缺字时命中，字形略大是预期）",
             s_status, (int)s_fallback->line_height,
             (int)font_puhui_16_4.line_height, (int)font_puhui_20_4.line_height);
    ESP_LOGI(TAG, "内部堆 %u → %u（本组件净占 %d 字节；字体位图在 mmap 区，不计）",
             (unsigned)heap_before, (unsigned)heap_after,
             (int)heap_before - (int)heap_after);

done:
    font_selftest();
    return ESP_OK;
}

const lv_font_t *font_service_small(void)
{
    return s_inited ? &s_small : &font_puhui_16_4;
}

const lv_font_t *font_service_large(void)
{
    return s_inited ? &s_large : &font_puhui_20_4;
}

bool font_service_fallback_ready(void)
{
    return s_fallback != NULL;
}

const char *font_service_status(void)
{
    return s_status;
}
