/**
 * @file ui_lvgl.c
 * @brief LVGL 移植层实现（P2）
 *
 * 设计理由与那条不可违反的铁律，见 include/ui_lvgl.h 顶部注释。
 * 这里只补充实现层面的几处关键决定：
 *
 * ── 1. 绘制缓冲放内部 DMA RAM，不放 PSRAM ──────────────────────────────
 *    IDF 的 SPI 驱动在源缓冲位于 PSRAM 时，会在**每次事务**于内部 RAM 分配
 *    一块等长缓冲再 memcpy（spi_master.c 的 setup_dma_priv_buffer），而这段
 *    代码跑在 SPI 中断路径上。19200 字节的反复 malloc/free + ISR 内拷贝
 *    是不可接受的。放内部 RAM 且 64 字节对齐时（19200 = 64×300，地址与长度
 *    双对齐），该分配路径被完全跳过。
 *    参考工程 xiaozhi 对同类屏同样配 buff_dma=1 / buff_spiram=0。
 *
 * ── 2. 显示对象手工建，不用 lvgl_port_add_disp() ───────────────────────
 *    理由见 ui_lvgl.h。先例是 xiaozhi 自己的 custom_lcd_display.cc。
 *
 * ── 3. 完成信号来自 DMA 完成中断，不来自 flush 回调 ────────────────────
 *    这是全篇最重要的一条，理由见 ui_lvgl.h 的「铁律」段。
 *    配套加了一个中断计数器 s_flush_done_cnt —— 中断里不能打日志，
 *    累加计数是排查「LVGL 卡死」最直接的证据。
 */
#include "ui_lvgl.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "lcd_display.h"
#include "touch_input.h"
#include "ui_pages.h"
#include "ui_lvgl_config.h"

/* 中文字体（来自 78/xiaozhi-fonts 组件）。
 * LVGL 字体就是一个 const lv_font_t 全局符号，用这个宏在使用处声明即可 ——
 * 该组件的 include/font_puhui.h 是**空文件**，这不是它的疏忽，而是既定用法：
 * 字体以符号形式导出，由使用方按需声明（xiaozhi 也是这样用的）。 */
LV_FONT_DECLARE(font_puhui_20_4);

static const char *TAG = "ui_lvgl";

static lv_display_t *s_disp  = NULL;
static uint16_t     *s_buf   = NULL;
static bool          s_ready = false;

/* DMA 完成中断计数（中断里不能打日志，只能累加；由外部读取）。
 * ⚠️ 它同时是排查「LVGL 首次刷新后卡死」的关键证据：
 *    若界面停在首帧且该计数为 0 ⇒ 完成链断了（钩子没接上 / 没真正排队）。 */
static volatile uint32_t s_flush_done_cnt = 0;

uint32_t ui_lvgl_get_flush_done_count(void)
{
    return s_flush_done_cnt;
}

/* ==========================================================================
 * DMA 完成中断 → 通知 LVGL
 * ========================================================================== */
static void ui_lvgl_flush_done_isr(void *user)
{
    /* 这里在**中断上下文**：只允许写标志，不得日志/阻塞/分配内存。
       lv_display_flush_ready() 的实现就是 `disp->flushing = 0;` 一句，安全。 */
    s_flush_done_cnt++;
    lv_display_flush_ready((lv_display_t *)user);
}

/* ==========================================================================
 * LVGL flush 回调：排队 + 完成信号由下面的中断给出。
 * ========================================================================== */

/* 送往面板的像素抽样日志（仅自检期间有预算，平时恒为 0，零开销）。
 * 它的价值：把「LVGL 说它画了什么」和「我们真往面板送了什么」分开。
 * 若色卡读数与预期不符，先看这里 —— 若此处已是期望值，问题就在面板/接线，
 * 不在这条链路上；否则问题在 LVGL 渲染或色卡本身。
 * ⚠️ 只在**第一次** flush 里打印，且必须等 DMA 完成后才能读那块缓冲 ——
 *    故只在缓冲被复用前的首帧打印（12 秒静止画面，足够安全）。 */
static uint32_t s_flush_probe_budget = 0;

void ui_lvgl_flush_probe(uint32_t frames)
{
    s_flush_probe_budget = frames;
}

static void ui_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (s_flush_probe_budget > 0) {
        s_flush_probe_budget--;
        const uint16_t *px = (const uint16_t *)px_map;
        ESP_LOGI(TAG, "flush(%d,%d)-(%d,%d) 首像素 0x%04X 0x%04X 0x%04X",
                 (int)area->x1, (int)area->y1, (int)area->x2, (int)area->y2,
                 px[0], px[1], px[2]);
    }

    const esp_err_t ret = lcd_draw_bitmap(area->x1, area->y1,
                                          area->x2 + 1, area->y2 + 1,
                                          (uint16_t *)px_map);
    if (ret != ESP_OK) {
        /* 失败路径**必须**补一次 flush_ready，否则 LVGL 会在
           wait_for_flushing 里永久自旋（直到任务看门狗报错）。
           只报一次，避免与错误同时刷屏。 */
        static bool logged = false;
        if (!logged) {
            logged = true;
            ESP_LOGE(TAG, "lcd_draw_bitmap 失败: %s（同类错误不再重复打印）",
                     esp_err_to_name(ret));
        }
        lv_display_flush_ready(disp);
    }

    /* ⚠️⚠️ 成功路径**绝对不要**在这里调用 lv_display_flush_ready()。
       SPI 传输是异步的（排队即返回）。提前把 flushing 置 0，LVGL 会立刻
       把下一条带画进 DMA 正在读的**同一块缓冲**，表现为随机的花屏/撕裂。
       见 lv_refr.c:851-854 的原文注释。完成信号只由上面的中断给出。 */
}

/* ==========================================================================
 * 开屏色卡自检 —— 走**真实的 LVGL 渲染路径**
 *
 * 【设计取舍一：一屏静态色卡，而不是逐个全屏刷色】
 * 本项目为「逐个全屏刷色 + 人工读数」吃过不少苦头：串步、反光、与屏幕边框
 * 混淆都发生过，同一份固件能被读出互相矛盾的结论。
 * 一屏色卡把全部判据压进**同一张照片**，每格自带文字标注，读数不再依赖
 * 记忆和顺序 —— 这正是此前出错的两个地方。
 *
 * 【设计取舍二：下半屏放灰阶，不放第二个彩色】
 * 纯色只能证明「通道没接反」，**证不了字段有没有错位** —— 每个通道取极值时，
 * 错位一比特后仍是个饱和的近似色，肉眼容易放过（本项目正是栽在这里）。
 * 灰阶要求 R=G=B，任何通道拿到错误权重都会立刻显出色调，且**每一级都偏**。
 * 而且灰阶不需要判断者知道「某个 16 位值应该长什么样」—— 只要它不中性就是错。
 *
 * 【设计取舍三：颜色值必须可自证】
 * ⚠️ 第一版色卡在这里翻过车：直接写了 `lv_color_hex(0xF800)`，以为传的是
 *    RGB565。而 `lv_color_hex()` 收的是 **24 位 RGB888**（lv_color.c 明确是
 *    `(c>>16)/(c>>8)/c` 三段拆分），0xF800 于是被当成 (0,248,0) —— 纯绿。
 *    结果八个格子的**红色通道全是 0**，色卡从来没有请求过红/白/灰。
 *    照片如实反映了这一点（0xFFFF 那格显示青色，正是 (0,255,255)），
 *    但当时的结论却是「驱动有问题」—— 测错了对象。
 *
 *    现在的做法：用 `c565()` 把 RGB565 **无损展开**成 RGB888
 *    （`(x<<n)|(x>>(m-n))` 是精确的双向映射，右移回来正好还原），
 *    再用 `lv_color_to_u16()` **回读**一次并打日志，
 *    用串口证明「请求的值 == LVGL 实际发出的值」。色卡自己证明自己。
 *
 * ⚠️ 调用者须已持有 LVGL 锁。停 12 秒供拍照，然后清屏交给界面。
 * ========================================================================== */
#if UI_LVGL_COLOR_SELFTEST

/* RGB565 → RGB888 的无损展开。
   低位复制到高位，使得 (v<<3)|(v>>2) 右移 3 位正好还原 v。
   这正是 LVGL 内部 RGB888→RGB565 的逆运算，故回读值必然与入参一致。 */
static lv_color_t c565(uint16_t v)
{
    const uint8_t r5 = (uint8_t)((v >> 11) & 0x1F);
    const uint8_t g6 = (uint8_t)((v >>  5) & 0x3F);
    const uint8_t b5 = (uint8_t)( v        & 0x1F);

    return lv_color_make((uint8_t)((r5 << 3) | (r5 >> 2)),
                         (uint8_t)((g6 << 2) | (g6 >> 4)),
                         (uint8_t)((b5 << 3) | (b5 >> 2)));
}

static void ui_lvgl_color_selftest(void)
{
    /* 上排：三原色 + 白（判定通道有没有接反 —— 一眼可判，无需思考）
       下排：四级灰阶（**主判据** —— 任何色调都是错，且不依赖"本该是什么色"）
       文字后跟数值，既便于串口核对，也防字体缺字时无法判读。 */
    static const struct {
        uint16_t    rgb565;
        const char *text;
        bool        black_label;   /* true = 标签用黑字（画在亮底上） */
    } k_cards[] = {
        { 0xF800, "红\nF800",   false },
        { 0x07E0, "绿\n07E0",   true  },
        { 0x001F, "蓝\n001F",   false },
        { 0xFFFF, "白\nFFFF",   true  },
        { 0x4208, "灰25%\n4208", true  },   /* ← 主判据 */
        { 0x8410, "灰50%\n8410", true  },   /* ← 主判据 */
        { 0x9CF3, "灰75%\n9CF3", true  },   /* ← 主判据 */
        { 0xCE79, "灰81%\nCE79", true  },   /* ← 主判据 */
    };
    const int cnt = (int)(sizeof(k_cards) / sizeof(k_cards[0]));

    lv_obj_t *scr = lv_display_get_screen_active(s_disp);
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    const int title_h = 32;
    const int cell_w  = LCD_H_RES / 4;
    const int cell_h  = (LCD_V_RES - title_h) / 2;

    ESP_LOGW(TAG, "========== 色卡自检（真实 LVGL 路径）==========");
    ESP_LOGW(TAG, "LVGL 交出的值如下；lcd_display 会在出口做唯一的字节交换后送面板");

    for (int i = 0; i < cnt; i++) {
        const lv_color_t c = c565(k_cards[i].rgb565);
        const uint16_t   back = lv_color_to_u16(c);

        /* 自证：请求值必须与 LVGL 实际发出的 565 值一致。
           不一致 ⇒ 色卡本身就画错了，屏幕上的结果不能用来判定驱动。 */
        if (back != k_cards[i].rgb565) {
            ESP_LOGE(TAG, "  第 %d 格自证失败：请求 0x%04X，LVGL 发出 0x%04X —— "
                          "色卡有误，本次结果不可用于判定驱动",
                     i + 1, k_cards[i].rgb565, back);
        } else {
            ESP_LOGI(TAG, "  第 %d 格 0x%04X ✓（LVGL 实际发出 0x%04X）",
                     i + 1, k_cards[i].rgb565, back);
        }

        lv_obj_t *cell = lv_obj_create(scr);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(cell, cell_w, cell_h);
        lv_obj_set_pos(cell, (i % 4) * cell_w, title_h + (i / 4) * cell_h);
        lv_obj_set_style_radius(cell, 0, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(cell, c, 0);

        lv_obj_t *lab = lv_label_create(cell);
        lv_obj_set_style_text_font(lab, &font_puhui_20_4, 0);
        lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(lab,
            k_cards[i].black_label ? lv_color_black() : lv_color_white(), 0);
        lv_label_set_text(lab, k_cards[i].text);
        lv_obj_center(lab);
    }

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &font_puhui_20_4, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "色卡：每格应等于格内文字所述颜色");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    ESP_LOGW(TAG, "上排应为：红 绿 蓝 白");
    ESP_LOGW(TAG, "下排应为：四级**中性**灰阶（无任何色调），一级比一级亮");
    ESP_LOGW(TAG, "**下排出现任何色调 ⇒ 通道权重错，即失败**");
    ESP_LOGW(TAG, "停 12 秒供拍照……");

    /* 记下**真正送往面板**的前几帧首像素 —— 串口与照片互为交叉证据。
       12 秒静止画面 = 32 条带（20 行/条），给 8 帧预算够看清每格起始值。 */
    ui_lvgl_flush_probe(8);
    lv_refr_now(s_disp);

    vTaskDelay(pdMS_TO_TICKS(12000));

    /* 清干净再交给界面 —— 否则色卡会残留在页面底下 */
    lv_obj_clean(scr);
    ESP_LOGW(TAG, "========== 色卡自检结束，即将进入界面 ==========");
}
#endif

/* --------------------------------------------------------------------------
 * 【已删除】灰阶对照自检 / 逐位权重探针
 *
 * 这里曾有两代诊断探针：
 *   1) 上下半屏对照（经 panel_encode vs 原样送出）
 *   2) 9 步「逐位权重探针」（每次只置一位，要求用户报「颜色 + 亮/暗」）
 *
 * 两者都发端于同一个错误前提 —— 「面板的字段顺序不是标准 RGB565」。
 * 而那个前提本身，是 `panel_encode()` 的产物而非观测事实：编码把字段整体
 * 错位了 1 个比特，于是低频位跑到了高频位上，单个低位比特才会「很亮」。
 * 探针绕了一大圈，真正要问的问题其实在更前面一层：
 * **那个编码函数本身是不是对的**。
 *
 * 结论（2026-10-13）：它就是错的。删掉它，颜色即正确。
 * 见 lcd_display.c 的废除说明。
 *
 * 两个探针一并删除。它们的读数方式（逐个全屏刷色 + 人工记忆顺序）在本项目
 * 已多次产生互相矛盾的结论，不适合作为长期手段保留；现在的替代品是上面
 * 那张**一屏自标注色卡**：一次拍照、每格自带文字、判据是硬性的中性灰。
 * -------------------------------------------------------------------------- */

/* ==========================================================================
 * 触摸输入 → LVGL indev
 *
 * LVGL 的 indev 是**轮询**式的：它按需调用本回调，回调内部去读一次触摸。
 * 触摸硬件本身也是轮询的（FT6X36 无中断脚），两边对得上。
 *
 * ⚠️ 本回调在 **LVGL 渲染任务（taskLVGL）上下文**执行，且此时已持有 LVGL 递归锁。
 *    它内部只做一次 I2C 事务（不阻塞等待、不打日志），不会拖住渲染。
 * ========================================================================== */
static void ui_lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    /* 只在「按下」瞬间打一条日志 —— 本回调每秒被调约 30 次，
       无条件打印会把串口刷爆。这条日志是验证触摸是否有效的直接证据。 */
    static bool s_prev_pressed = false;

    uint16_t x = 0, y = 0;
    const bool pressed = touch_input_is_ready() && touch_input_read(&x, &y);

    if (pressed) {
        if (!s_prev_pressed) {
            ESP_LOGI(TAG, "触摸按下 (%u, %u)", (unsigned)x, (unsigned)y);
        }
        data->point.x = x;
        data->point.y = y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state   = LV_INDEV_STATE_RELEASED;
    }
    s_prev_pressed = pressed;
}

/* ==========================================================================
 * 初始化
 * ========================================================================== */
esp_err_t ui_lvgl_init(void)
{
    ESP_RETURN_ON_FALSE(!s_ready, ESP_ERR_INVALID_STATE, TAG, "ui_lvgl 已初始化");
    ESP_RETURN_ON_FALSE(lcd_is_ready(), ESP_ERR_INVALID_STATE, TAG,
                        "LCD 未初始化 —— 请先调用 lcd_init()");

    /* ---- 绘制缓冲：显式要求内部 DMA RAM ----
       绝不允许落到 PSRAM（理由见文件头第 1 条）。
       64 字节对齐使地址与长度双双对齐，SPI 驱动因而跳过内部等长拷贝。 */
    s_buf = heap_caps_aligned_alloc(UI_LVGL_BUF_ALIGN, UI_LVGL_BUF_BYTES,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(s_buf != NULL, ESP_ERR_NO_MEM, TAG,
                        "绘制缓冲分配失败（需内部 DMA RAM %u 字节）",
                        (unsigned)UI_LVGL_BUF_BYTES);

    /* ---- LVGL 本体与端口基础设施 ----
       lv_init() 幂等：端口任务内部还会再调一次（LVGL 9 有重入保护）。 */
    lv_init();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = UI_LVGL_TASK_PRIO;
    port_cfg.task_stack      = UI_LVGL_TASK_STACK;
    port_cfg.task_affinity   = UI_LVGL_TASK_CORE;
    port_cfg.timer_period_ms = UI_LVGL_TICK_PERIOD_MS;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init 失败");

    /* 建显示对象期间必须持锁，否则端口任务可能先开始渲染 */
    if (!lvgl_port_lock(0)) {          /* 0 = 无限等待 */
        return ESP_ERR_TIMEOUT;
    }

    s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    if (s_disp == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }

    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, ui_lvgl_flush_cb);
    /* 单缓冲 + 部分刷新：缓冲被当作「满宽 × N 行」的条带使用。
       传 NULL 作为 buf2 即单缓冲 —— 这正是那个完成钩子必须存在的原因。 */
    lv_display_set_buffers(s_disp, s_buf, NULL, UI_LVGL_BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_default(s_disp);

    /* 完成钩子必须在**第一次 flush 之前**接好。
       此刻仍持锁，端口任务不会开画，顺序安全。 */
    lcd_set_flush_done_cb(ui_lvgl_flush_done_isr, s_disp);

    /* ---- 触摸：初始化硬件并注册为 LVGL 输入设备 ----
       失败**不致命**：界面照样显示，只是点不动，故只告警、不返回错误。
       这样即使触摸硬件有问题，也能先把界面看清楚（排障时很重要）。 */
    const esp_err_t tret = touch_input_init();
    if (tret == ESP_OK) {
        lv_indev_t *indev = lv_indev_create();
        if (indev != NULL) {
            lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(indev, ui_lvgl_touch_read_cb);
            lv_indev_set_display(indev, s_disp);
            ESP_LOGI(TAG, "触摸已接入 LVGL indev");
        } else {
            ESP_LOGW(TAG, "创建 LVGL indev 失败");
        }
    } else {
        ESP_LOGW(TAG, "触摸初始化失败: %s —— 界面仍会显示，但无法点击",
                 esp_err_to_name(tret));
    }

    /* ---- 开屏自检（按需开启，见 ui_lvgl_config.h）---- */
#if UI_LVGL_COLOR_SELFTEST
    ui_lvgl_color_selftest();
#endif

    ui_pages_init();

    lvgl_port_unlock();

    s_ready = true;
    ESP_LOGI(TAG, "LVGL 就绪: %dx%d，缓冲 %u 字节（内部 DMA RAM，%d 行/条带）",
             LCD_H_RES, LCD_V_RES, (unsigned)UI_LVGL_BUF_BYTES, UI_LVGL_BUF_LINES);
    ESP_LOGI(TAG, "渲染任务 taskLVGL: 优先级 %d 核心 %d 栈 %d；tick %d ms",
             UI_LVGL_TASK_PRIO, UI_LVGL_TASK_CORE,
             UI_LVGL_TASK_STACK, UI_LVGL_TICK_PERIOD_MS);
    return ESP_OK;
}

bool ui_lvgl_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void ui_lvgl_unlock(void)
{
    lvgl_port_unlock();
}

bool ui_lvgl_update(const tank_ecosystem_t *snap)
{
    if (!s_ready || snap == NULL) {
        return false;
    }
    if (!lvgl_port_lock(UI_LVGL_LOCK_TIMEOUT_MS)) {
        return false;   /* 取锁超时不报错：宁可少刷一帧，也不拖住调用方节拍 */
    }

    ui_pages_update_tank(snap);

    lvgl_port_unlock();

    /* 端口渲染任务平时按 LVGL 的 33ms 刷新定时器走。
       主动唤醒一次可把「数据变化 → 上屏」的延迟压到最小。 */
    (void)lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, NULL);
    return true;
}

/* ==========================================================================
 * 摄像头照片显示（P6-a）
 *
 * 调用次序见 ui_lvgl.h。这里只强调两条不可动摇的约束：
 *
 *   ① **拷贝在锁外**。153 KB 的 memcpy 在 PSRAM 上约几毫秒，
 *      放在锁内会白白占着 LVGL 锁。本函数把拷贝放在取锁**之前**。
 *   ② **双缓冲**。交替使用两块缓冲，于是"taskLVGL 正在渲染的那块"
 *      永远不会被下一帧写到 —— 撕裂从设计上就不可能发生，不靠运气。
 *      （对照：xiaozhi 的 `Esp32Camera::Capture()` 是单缓冲、在锁外改写
 *      正在显示的缓冲，它靠的是"不发生"而非"不可能"。）
 * ========================================================================== */

/* 双缓冲。**按需分配** —— 第一次显示照片时才要这 300 KB PSRAM，
 * 不拍照的场景不该先占着。 */
static uint16_t      *s_photo_buf[2] = { NULL, NULL };
static lv_image_dsc_t s_photo_dsc[2];
static int            s_photo_cur = -1;     /* -1 = 还没显示过任何照片 */

bool ui_lvgl_capture_requested(void)
{
    /* 纯读标志，不碰 LVGL，故**不加锁** —— 见 ui_lvgl.h 的调用次序 */
    return ui_pages_take_capture_request();
}

void ui_lvgl_chat_add(ui_chat_role_t role, const char *text)
{
    if (!s_ready || text == NULL || text[0] == '\0') {
        return;
    }
    if (!lvgl_port_lock(UI_LVGL_LOCK_TIMEOUT_MS)) {
        return;
    }
    ui_pages_chat_add(role, text);
    lvgl_port_unlock();
    (void)lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, NULL);
}

void ui_lvgl_set_camera_ready(bool ready)
{
    if (!s_ready) {
        return;
    }
    if (!lvgl_port_lock(UI_LVGL_LOCK_TIMEOUT_MS)) {
        return;
    }
    ui_pages_set_camera_ready(ready);
    lvgl_port_unlock();
    (void)lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, NULL);
}

bool ui_lvgl_talk_held(void)
{
    /* **不加锁** —— 与 ui_lvgl_capture_requested 同理：
       它只读一个 volatile 标志，目的是让 voice_task 能在锁外轮询。
       若在这里加锁，voice_task 每 60ms 抢一次 LVGL 锁，
       会和渲染任务互相拖慢。 */
    if (!s_ready) {
        return false;
    }
    return ui_pages_talk_held();
}

void ui_lvgl_set_talk_ready(bool ready)
{
    if (!s_ready) {
        return;
    }
    if (!lvgl_port_lock(UI_LVGL_LOCK_TIMEOUT_MS)) {
        return;
    }
    ui_pages_set_talk_ready(ready);
    lvgl_port_unlock();
    (void)lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, NULL);
}

void ui_lvgl_set_talk_state(ui_talk_state_t st)
{
    if (!s_ready) {
        return;
    }
    if (!lvgl_port_lock(UI_LVGL_LOCK_TIMEOUT_MS)) {
        return;
    }
    ui_pages_set_talk_state(st);
    lvgl_port_unlock();
    (void)lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, NULL);
}

esp_err_t ui_lvgl_show_photo(const uint16_t *rgb565, int w, int h)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG,
                        "ui_lvgl 未初始化");
    ESP_RETURN_ON_FALSE(rgb565 != NULL && w > 0 && h > 0, ESP_ERR_INVALID_ARG,
                        TAG, "照片参数非法");
    ESP_RETURN_ON_FALSE(w <= UI_LVGL_PHOTO_MAX_W && h <= UI_LVGL_PHOTO_MAX_H,
                        ESP_ERR_INVALID_SIZE, TAG,
                        "照片 %dx%d 超过缓冲容量 %dx%d（改摄像头分辨率时"
                        "记得同步 ui_lvgl_config.h）",
                        w, h, UI_LVGL_PHOTO_MAX_W, UI_LVGL_PHOTO_MAX_H);

    const size_t bytes = (size_t)UI_LVGL_PHOTO_MAX_W * UI_LVGL_PHOTO_MAX_H
                         * sizeof(uint16_t);

    /* ---- 按需分配两块缓冲（**锁外**）---- */
    if (s_photo_buf[0] == NULL) {
        for (int i = 0; i < 2; i++) {
            s_photo_buf[i] = (uint16_t *)heap_caps_malloc(
                                 bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (s_photo_buf[i] == NULL) {
                ESP_LOGE(TAG, "照片缓冲 %d 分配失败（需 %u 字节 PSRAM）",
                         i, (unsigned)bytes);
                return ESP_ERR_NO_MEM;
            }
        }
        ESP_LOGI(TAG, "照片双缓冲就绪：2 × %u 字节（PSRAM）", (unsigned)bytes);
    }

    /* ---- 写入**另一块**缓冲（**锁外**）---- */
    const int next = (s_photo_cur + 1) & 1;     /* -1 → 0，0 ↔ 1 交替 */

    memcpy(s_photo_buf[next], rgb565, (size_t)w * h * sizeof(uint16_t));

    /* 描述符也写"另一块"的 —— 它同样不在显示中 */
    memset(&s_photo_dsc[next], 0, sizeof(s_photo_dsc[next]));
    s_photo_dsc[next].header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_photo_dsc[next].header.cf     = LV_COLOR_FORMAT_RGB565;
    s_photo_dsc[next].header.w      = (uint32_t)w;
    s_photo_dsc[next].header.h      = (uint32_t)h;
    s_photo_dsc[next].header.stride = (uint32_t)w * 2;
    s_photo_dsc[next].data_size     = (uint32_t)((size_t)w * h * 2);
    s_photo_dsc[next].data          = (const uint8_t *)s_photo_buf[next];

    /* ---- 只在这里持锁：切一次指针（微秒级）---- */
    if (!lvgl_port_lock(UI_LVGL_LOCK_TIMEOUT_MS)) {
        /* 缓冲已经写好了，只是没切过去。**不推进 s_photo_cur** ——
           下一次会重写同一块，仍不会碰正在显示的那块。 */
        ESP_LOGW(TAG, "照片已就绪但取锁超时，本轮未切换显示");
        return ESP_ERR_TIMEOUT;
    }
    /* 把照片作为一条**聊天消息**追加进去。
       `ui_pages_chat_add_photo()` 会先删掉上一张照片的气泡 ——
       这正是双缓冲能只留两块的前提：同一时刻 LVGL 只引用其中一个描述符。 */
    ui_pages_chat_add_photo(&s_photo_dsc[next]);
    lvgl_port_unlock();

    s_photo_cur = next;         /* 只在切换成功后推进 */
    (void)lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, NULL);
    ESP_LOGI(TAG, "照片已作为消息显示：%dx%d（缓冲 %d）", w, h, next);
    return ESP_OK;
}
