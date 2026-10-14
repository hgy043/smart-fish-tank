/**
 * @file font_service.h
 * @brief 中文字体服务：给子集化的普惠体挂上覆盖面更大的 Noto 回退字体
 *
 * ============================================================================
 * 要解决的问题
 * ============================================================================
 * `78/xiaozhi-fonts` 里的 `font_puhui_16_4` / `font_puhui_20_4` 是**子集化**
 * 字体：只有 6649 个码位、其中 CJK 仅 **5258** 个。它按
 * 「DeepSeek-R1 ∪ Qwen3 分词器字符」这份字表裁剪，**有 766 个字符普惠体里
 * 根本没有、被 lv_font_conv 静默丢弃**。
 *
 * 我们的界面文案是固定的（都在字集内），但**小智回复的文字不是** ——
 * 偶尔出现字集外的汉字时，LVGL 报
 * `lv_draw_letter: glyph dsc. not found for U+xxxx` 并画一个占位方框。
 *
 * 同一个组件里另有一个覆盖大得多的字体 `cbin/font_noto_qwen_16_4.bin`
 * （Noto Sans SC/TC/JP/KR 合并，**18129 码位 / CJK 10335**）。
 * 本组件把它从 flash 的 `fonts` 分区 mmap 出来，用上游自带的
 * `cbin_font_create()` 建成 `lv_font_t`，挂在普惠体后面当 **fallback**。
 *
 * > **为什么是 fallback 而不是直接换字体**：普惠体只用来画常规文字（好看、
 * > 已经在链接里、零额外成本），Noto 只在**缺字时**被命中。字形风格会略有
 * > 差异，但比一个空方框强得多。
 *
 * ============================================================================
 * ⚠️ 两个必须知道的约束
 * ============================================================================
 * 1. **`font_puhui_*` 是 `const lv_font_t`**（在 flash 里），
 *    **不能运行时写 `.fallback`** —— 那会往只读数据段写，直接 fault。
 *    所以本组件先 `memcpy` 出两份 RAM 副本，往副本上挂 fallback，
 *    再把副本的**指针**给出去。
 *    ⇒ **调用方一律用 `font_service_small()` / `font_service_large()`，
 *      不要再直接 `&font_puhui_16_4`。**
 *
 * 2. **cbin 的 ABI 必须与本工程 sdkconfig 一致**（`LV_FONT_FMT_TXT_LARGE`）。
 *    不一致时**渲染出乱码而不是报错**。本组件用 `_Static_assert` 在编译期
 *    盯着这件事，判据与实测过程记在 `fonts/README.md`。
 *
 * ============================================================================
 * 失效时的行为
 * ============================================================================
 * **分区不存在 / 没烧 / 内容不像字体** —— 一律**不致命**：
 * 打一条 WARN，两个字体照常返回（只是没有 fallback，缺字仍是方框）。
 * 字体属于"锦上添花"，不该让界面起不来。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 装载字体：复制普惠体 → 找 `fonts` 分区 → mmap → 建 Noto font → 挂 fallback。
 *
 * 幂等；**必须在创建任何文字控件之前或之后都可以调**（LVGL 是绘制时才查字形，
 * 但为了少一个"为什么第一次刷新是对的"的疑问，还是放在 UI 初始化前面调）。
 *
 * @return `ESP_OK` —— **包括分区缺失/内容不对这些"降级"情况**（会打 WARN）。
 *         只有真正意外的错误才返回非 OK。
 */
esp_err_t font_service_init(void);

/** @brief 16px 字体（普惠体 + Noto 回退）。**任何情况下都非 NULL。** */
const lv_font_t *font_service_small(void);

/** @brief 20px 字体（普惠体 + Noto 回退）。**任何情况下都非 NULL。** */
const lv_font_t *font_service_large(void);

/** @brief 回退字体是否已挂上（供串口/自检打印用）。 */
bool font_service_fallback_ready(void);

/** @brief 当前状态的一句话说明，给日志和自检用。**任何情况下都非 NULL。** */
const char *font_service_status(void);

#ifdef __cplusplus
}
#endif
