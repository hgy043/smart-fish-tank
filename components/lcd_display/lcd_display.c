/**
 * @file lcd_display.c
 * @brief LCD 显示服务 —— **移植自粤嵌官方已验证 BSP**
 *
 * ============================================================================
 * 来源：06-tcp_client_bafa_mqtt/main/esp32_s3_gec.c 的
 *       bsp_display_brightness_init() / bsp_display_new() / bsp_lcd_init() /
 *       lcd_set_color() / lcd_cs()
 *
 * 【本文件的设计原则：照搬，不优化】
 *
 * 前十几轮我在这个 LCD 上反复失败，根本原因是**自己重写**而不是**复用**：
 * 自己选 SPI 模式、自己定 max_transfer_sz、自己写条带绘制、自己删掉片选步骤、
 * 自己造测试框架——每一步都引入了新变量，而每次失败都无法区分是哪个变量的错。
 *
 * 现在严格遵守：**凡是已验证代码里有的，一律照搬；不理解的先不动。**
 * 所有从原实现偏离的地方都在注释里标出原因，没有标注的就是原样。
 * ============================================================================
 */
#include "lcd_display.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7796.h"

#include "gec_board.h"

static const char *TAG = "lcd";

/* ---- 关键参数：以**实测能出图**的参考为准 -------------------------------
 *
 * 事实（用户实测）：D:\ESP32\06-tcp_client_bafa_mqtt （粤嵌官方 BSP）
 * 在同一块板上**屏幕能正常出图**，其配置为：
 *     spi_bus_config_t  : sclk=41, mosi=40, miso/quadwp/quadhd=NC,
 *                         max_transfer_sz = 480*320*2 = 307200
 *     panel_io_spi_cfg  : dc=39, cs=NC, pclk=80MHz, mode=**3**,
 *                         cmd/param bits=8/8, queue_depth=10
 *     panel_dev_cfg     : reset=NC, BGR, 16bpp, vendor_config=NULL
 *     顺序              : reset → lcd_cs(0) → init → invert(true)
 *                         → swap_xy(true) → mirror(true,true)
 *     然后              : 全屏填白 → disp_on_off(true) → 背光开
 *
 * 因此**一律以它为基准**。此前在模式 0/3 之间反复摇摆是错误的做法：
 * 手上有能出图的实现，就不该去猜。
 * ------------------------------------------------------------------------- */
#define LCD_SPI_HOST        GEC_LCD_SPI_HOST          /* SPI3_HOST       */
#define LCD_CMD_BITS        8
#define LCD_PARAM_BITS      8
#define LCD_SPI_MODE        3                         /* 06 BSP 实测可出图 */

/* MADCTL 的方向位 MV|MX|MY（= 0xE0，由 swap_xy(true) + mirror(true,true) 累加）。
   方向已实测裁决正确（彩条能被逐条读出），故作为固定基值。 */
#define LCD_MADCTL_BASE     0xE0

/* MADCTL 的颜色顺序**位本身**（bit3）。
   IDF 的内部头文件里叫 LCD_CMD_BGR_BIT，但它不对外暴露，故在此复写其定义。
   语义：0 = RGB，1 = BGR。 */
#define LCD_MADCTL_BGR_MASK 0x08

/* MADCTL 的颜色顺序位。**必须与 lcd_init() 里 panel_config.rgb_ele_order
   保持一致**，否则方向枚举试验跑完会把颜色顺序改回去。
   2026-10-13 定案：本面板为 **BGR** ⇒ 0x08（与两套厂家参考一致，
   并已由自标注色卡逐格验证）。 */
#define LCD_MADCTL_BGR_BIT  0x08

/* ---- 像素时钟 ----------------------------------------------------------
 * 【2026-10-13 单变量对照：40 → 80 MHz】
 *
 * 40MHz 是早期排查的遗留物：当时观察到「填白能显示、之后就不输出」，怀疑
 * 80MHz 对这颗面板过高，于是「先降一半做正交验证」。**屏好了之后没有改回去**，
 * 于是它成了本项目与「能出图的参考工程」之间**最后一个未解释的差异** ——
 * 06 BSP 用的是 80MHz（esp32_s3_gec.h:252 `(80 * 1000 * 1000)`）。
 *
 * 本轮把两个可能的干扰源先钉死，再单独改这一项：
 *   - 背光极性已裁决为高电平点亮（已改）；
 *   - 颜色字段重排已接入并验证通过（八色全对）。
 * 因此**本轮只有像素时钟一个变量**。
 *
 * ⛔ **上面第二条描述的是当时的现场，不是结论**：`panel_encode()` 在同日稍后
 *    即被推翻并从代码中删除，而「八色全对」恰恰是**纯色对字段错位不敏感的
 *    假阳性** —— 真因（`rgb_ele_order` 写反 + 缺字节交换）直到中性灰出错才
 *    暴露，见文件顶部的完整说明与 architecture.md 7.7。
 *    保留原文，是为了记住**那一轮是拿一个未经自证的测试结果当"已钉死"用的**。
 *
 * 【为什么要往 80MHz 走，而不是留在 40MHz】
 * 不只是「对齐参考」。SPI 时钟直接决定刷新耗时：
 *     整帧 480×320×2 = 307200 字节
 *     40MHz → 307200×8/40e6 ≈ 61 ms
 *     80MHz →                  ≈ 31 ms
 * P2 要接 LVGL，这个差别直接影响界面流畅度，值得现在就定下来。
 *
 * 【判据】改后屏幕应仍然正常出图（彩条 + 八色探针）。
 * 若出现花屏/撕裂/闪烁/偏色 ⇒ 80MHz 对这颗面板过高，改回 40MHz 并在此记录。
 * ⚠️ 若失败，回退时**只改这一个宏**，不要再顺手动别的参数。
 *
 * 【实测结果 2026-10-13：✅ 通过】
 *   80MHz 下屏幕正常出图、颜色正常。至此**本项目与能出图的参考工程之间
 *   已无未解释的差异**（背光极性、颜色字段、SPI 模式、时钟、缓冲方式全部对齐或已裁决）。
 *   保留 40MHz 的历史记录仅为说明这个值当初为什么被改过。
 * -------------------------------------------------------------------- */
#define LCD_PIXEL_CLOCK_HZ  (80 * 1000 * 1000)

/* max_transfer_sz：参考工程显式设为整屏字节数 307200
   （06 esp32_s3_gec.c:517 实测读取确认）。照抄此值。 */
#define LCD_MAX_TRANSFER_SZ (GEC_LCD_H_RES * GEC_LCD_V_RES * sizeof(uint16_t))

/* ---- 背光极性开关 ----------------------------------------------------
 * 1 = 高电平点亮（参考工程 06 BSP 的写法：duty=1023 为"开"）
 * 0 = 低电平点亮（依据早期实测：IO42 拉高熄屏、拉低亮屏）
 *
 * 【2026-10-13 本轮：0 → 1，单变量试验】
 *
 * 两项实证互相矛盾，只能实测裁决：
 *   - 06 BSP 在同板**能出图**，它点屏的最后一步是 bsp_display_backlight_on()
 *     → duty=1023 → 引脚恒**高**。  ⇒ 高电平点亮
 *   - 早前对 IO42 做的 A/B（拉高熄屏、拉低亮屏）        ⇒ 低电平点亮
 *
 * 本轮**只改这一个变量**，像素时钟仍保持 40MHz、其余参数一概不动，
 * 以免重蹈"一次改多个变量无法定位"的覆辙（architecture.md 7.5）。
 *
 * 判读：
 *   - 屏亮 ⇒ 极性定案为**高电平点亮**，早前的 A/B 结论作废；
 *   - 仍黑 ⇒ 极性不是瓶颈，回退此宏，转向下一个变量（像素时钟 40→80MHz）。 */
#define LCD_BL_INVERT       1

static esp_lcd_panel_io_handle_t s_panel_io = NULL;
static esp_lcd_panel_handle_t    s_panel    = NULL;
static bool                      s_ready    = false;
static int                       s_cs_gpio  = -1;

/* ==========================================================================
 * 绘制完成钩子（P2 新增，供 LVGL 端口使用）
 *
 * 【为什么必须有这个钩子】
 * LVGL 单缓冲部分刷新模式下，**必须**等 DMA 真正读完这块缓冲才能复用它。
 * LVGL 源码原文（lv_refr.c:851-854）：
 *     "In single buffered mode wait here until the buffer is freed.
 *      Else we would draw into the buffer while it's still being
 *      transferred to the display"
 * 它靠一个 flushing 标志来等 —— lv_display_flush_ready() 的实现就是
 * `disp->flushing = 0;`（lv_display.c:609-612）。
 *
 * ⇒ **完成信号只能来自 DMA 完成中断，绝不能在 flush 回调里提前给。**
 *   提前给会让 LVGL 立刻把下一条带画进 DMA 正在读的同一块缓冲，
 *   表现为随机的花屏/撕裂。
 *
 * 【本组件承担的角色】
 * 持有 panel IO 的 on_color_trans_done，并转达给注册者（LVGL 的 flush_ready
 * 就接在这上面）。这样 LVGL 侧不需要拿到 panel 句柄 —— 本组件继续把
 * 「面板句柄」和「非标准色序」这两件事封在内部。
 *
 * ⚠️ panel IO 上该回调**只有一个槽**，注册即整体覆盖（IDF 会打
 *    "already set and now it was overwritten!" 警告），没有链式回调。
 *    本工程内只有本文件注册它，不存在竞争者。
 * ========================================================================== */
static lcd_flush_done_cb_t s_flush_done_cb  = NULL;
static void               *s_flush_done_ctx = NULL;

/** panel IO 的色彩数据发送完成回调（**SPI 中断上下文**）。
 *  SPI 分支下 edata 恒为 NULL（IDF 定义如此，见 esp_lcd_panel_io_spi.c）。
 *  这里只做一件事：把事件转达给注册者。**不得**日志/阻塞/分配内存。 */
static bool lcd_on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    (void)io;
    (void)edata;
    (void)user_ctx;

    if (s_flush_done_cb != NULL) {
        s_flush_done_cb(s_flush_done_ctx);
    }
    return false;   /* 无需唤醒被阻塞的高优先级任务 */
}

void lcd_set_flush_done_cb(lcd_flush_done_cb_t cb, void *user)
{
    s_flush_done_cb  = cb;
    s_flush_done_ctx = user;

    if (s_panel_io != NULL) {
        /* 已初始化 → 立即接线，支持「先 lcd_init 后注册」的顺序。
           注：lcd_init() 自己也注册同一个函数，故 lcd_reinit() 重建 panel IO
           后钩子自动继续有效，调用方无需重新注册。 */
        const esp_lcd_panel_io_callbacks_t cbs = {
            .on_color_trans_done = lcd_on_color_trans_done,
        };
        (void)esp_lcd_panel_io_register_event_callbacks(s_panel_io, &cbs, NULL);
    }
}

/* ==========================================================================
 * 【已废除】panel_encode() —— 此处曾有一个「面板字段重排」函数，2026-10-13 删除
 *
 * 原函数声称：这颗面板的控制器不按 RGB565 标准排字段，实际路由为
 * R,B,G（[15:11]→红、[10:5]→蓝(6位)、[4:0]→绿(5位)），故送数据前必须重排：
 *
 *     return (r << 11) | (b << 6) | (g >> 1);
 *
 * ── 为什么它是错的 ──────────────────────────────────────────────────────
 *
 * 把那个式子展开，就能看出它实际做的事：**把三个字段整体错位了 1 个比特**。
 *     r → [15:11]    （正确）
 *     b → [10:6]     （标准是 [4:0]）
 *     g → [ 5:1]     （标准是 [10:5]）
 * 也就是说，它假设面板要的是 R5 B6 G5，而面板要的是标准的 R5 G6 B5。
 *
 * ── 用户实测如何反推出面板其实是标准的 ──────────────────────────────────
 *
 * 现象：彩条「红|绿|蓝|白|黑」显示成「红|蓝|绿|白|黑」。
 * 反推路由（已知编码把 R 放 [15:11]、B 放 [10:6]、G 放 [5:1]）：
 *     红输入 → [15:11] 全亮 → 显示红 ⇒ [15:11] → 红
 *     蓝输入 → [10:6] 全亮  → 显示绿 ⇒ [10:6] → 绿
 *     绿输入 → [ 5:1] 全亮  → 显示蓝 ⇒ [ 5:1] → 蓝
 * 这三条合起来正是 **[15:11]→R、[10:5]→G、[4:0]→B，即标准 RGB565**。
 *
 * 用这个模型逐条复算彩条，三条全中：
 *     红 0xF800 → 编码 0xF800 → R=100%     → 红 ✓
 *     绿 0x07E0 → 编码 0x001F → G=2% B=48% → 暗蓝 ✓（观察：蓝）
 *     蓝 0x001F → 编码 0x07C0 → G=98%      → 亮绿 ✓（观察：绿）
 *
 * ── 教训 ────────────────────────────────────────────────────────────────
 *
 * 「字段归属」（哪个字段归哪个通道）和「字段宽度」（字段各占几位）是两件事。
 * 当初证明前者用的是**单比特探针**，那种数据天然推不出后者；宽度假设从未被
 * 独立验证，就被写进了注释并当成定案。后来中间调（中性灰显示成紫色）出错，
 * 正是错位一个比特的必然结果 —— **低位跑到了高位上**。
 *
 * 两套厂家参考（06 BSP 与 xiaozhi）**都没有任何字段重排步骤**。本文件此前的
 * 偏离是唯一的异常。
 *
 * ── 但"删掉它就对了"这个结论只对了一半（同日稍后修正） ──────────────────
 *
 * 删掉重排后，纯色确实正确了（红/绿/蓝的三个字段各自归位），但**灰阶仍然
 * 全错**：自标注色卡上，两块中性灰显示成品红。继续反推才发现还有第二个、
 * 独立的问题：**16 位像素的字节序**（见下方 panel_send()）。
 *
 * 正确的收尾状态是：标准 RGB565 → **字节交换** → 面板（BGR 色序）。
 * 也就是说 —— 数据在送面板前**确实需要一次变换**，只是那次变换是**字节
 * 交换**，不是字段重排。当初的直觉（"面板要的东西和标准 RGB565 不一样"）
 * 方向没错，但把变换认成了字段重排，而且那个重排公式恰好是个错位 1 比特的
 * 东西，于是又叠了一层新错误。
 *
 * 【教训】"这个变换是多余的"和"这个变换是错的那种变换"是两回事。
 * 删掉一个错误变换之后，如果症状只是**部分**好转，说明底下还有第二个问题，
 * 不要就此收工 —— 而应该像这次一样，用能自证的测试（自标注色卡 + LVGL 侧
 * 回读 + flush 侧回读）把每一段链路分别钉死。
 * ========================================================================== */

/* 前置声明：lcd_init() 需要用它做全屏填白（清 GRAM），
   而定义在文件后部。 */
esp_err_t lcd_fill(uint16_t color);

/* ==========================================================================
 * 片选绑定（保留本项目原有的可配置能力，默认 NC 与两套厂家代码一致）
 * ========================================================================== */
void lcd_set_cs_gpio(int gpio_num)
{
    s_cs_gpio = gpio_num;
    ESP_LOGW(TAG, "LCD 片选：%s", gpio_num < 0 ? "NC（不绑定 GPIO）" : "已绑定");
}

int lcd_get_cs_gpio(void)
{
    return s_cs_gpio;
}

/* ==========================================================================
 * 背光 —— 原样照搬 06 BSP 的 bsp_display_brightness_init()
 *        （用户已提供该函数原文，逐字段对齐）
 * ========================================================================== */
static bool s_bl_inited = false;

static esp_err_t backlight_pwm_init(void)
{
    if (s_bl_inited) {
        return ESP_OK;
    }

    const ledc_channel_config_t lcd_backlight_channel = {
        .gpio_num   = GEC_LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = GEC_LCD_BL_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = GEC_LCD_BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
        .flags = { .output_invert = false },
    };
    const ledc_timer_config_t lcd_backlight_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = GEC_LCD_BL_LEDC_RES,
        .timer_num       = GEC_LCD_BL_LEDC_TIMER,
        .freq_hz         = GEC_LCD_BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };

    ESP_ERROR_CHECK(ledc_timer_config(&lcd_backlight_timer));
    ESP_ERROR_CHECK(ledc_channel_config(&lcd_backlight_channel));

    s_bl_inited = true;
    ESP_LOGI(TAG, "背光 PWM 就绪: IO%d %dHz 10bit",
             GEC_LCD_BACKLIGHT_GPIO, GEC_LCD_BL_LEDC_FREQ_HZ);
    return ESP_OK;
}

/* 直接写 LEDC 占空比，本身不含极性逻辑 —— 极性判断在 lcd_backlight() /
   lcd_backlight_set_percent() 内完成，依据见上方 LCD_BL_INVERT。
   参考实现 bsp_display_brightness_set() 用 output_invert=false + 高占空比=亮；
   本项目在 LCD_BL_INVERT=1 时与之逐字一致，=0 时才对占空比取反。 */
static esp_err_t backlight_set_duty_raw(uint32_t duty_10bit)
{
    esp_err_t ret = ledc_set_duty(LEDC_LOW_SPEED_MODE, GEC_LCD_BL_LEDC_CHANNEL, duty_10bit);
    if (ret == ESP_OK) {
        ret = ledc_update_duty(LEDC_LOW_SPEED_MODE, GEC_LCD_BL_LEDC_CHANNEL);
    }
    return ret;
}

/* 运行期背光极性覆盖：扫描试验用，正常路径保持编译期 LCD_BL_INVERT */
static int s_bl_invert_override = -1;   /* -1 = 用编译期值 */

void lcd_set_backlight_invert(bool invert)
{
    s_bl_invert_override = invert ? 1 : 0;
}

static bool bl_is_inverted(void)
{
    if (s_bl_invert_override >= 0) {
        return s_bl_invert_override != 0;
    }
    return LCD_BL_INVERT != 0;
}

esp_err_t lcd_backlight(bool on)
{
    esp_err_t ret = backlight_pwm_init();
    if (ret != ESP_OK) {
        return ret;
    }
    if (bl_is_inverted()) {
        /* 高电平点亮：全亮 = duty 1023 */
        return backlight_set_duty_raw(on ? 1023U : 0U);
    }
    /* 低电平点亮：全亮 = duty 0（引脚恒低） */
    return backlight_set_duty_raw(on ? 0U : 1023U);
}

esp_err_t lcd_backlight_set_percent(int percent)
{
    if (percent < 0)   { percent = 0; }
    if (percent > 100) { percent = 100; }

    esp_err_t ret = backlight_pwm_init();
    if (ret != ESP_OK) {
        return ret;
    }
    const uint32_t duty = bl_is_inverted()
                        ? (uint32_t)(1023U * percent / 100)
                        : (uint32_t)(1023U * (100 - percent) / 100);
    ESP_LOGI(TAG, "背光亮度 %d%% (duty=%u, invert=%d)", percent, (unsigned)duty,
             (int)bl_is_inverted());
    return backlight_set_duty_raw(duty);
}

/* ==========================================================================
 * 初始化 —— 原样照搬 06 BSP 的 bsp_display_new()
 * ========================================================================== */
esp_err_t lcd_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_lcd_panel_io_handle_t io_handle = NULL;

    /* 背光初始化（06 BSP 在 SPI 之前调用 bsp_display_brightness_init） */
    ESP_RETURN_ON_ERROR(backlight_pwm_init(), TAG, "Brightness init failed");

    /* ---- SPI 总线 ----
       ⚠️ 必须显式设置 max_transfer_sz：06 BSP 的 buscfg 未加 `= {}`，
       未列出字段为栈上未初始化值，照抄会让行为不可复现。
       这里采用 xiaozhi 的显式值（整屏字节数），并对其余字段显式清零。 */
    const spi_bus_config_t buscfg = {
        .mosi_io_num     = GEC_LCD_MOSI_GPIO,
        .miso_io_num     = GPIO_NUM_NC,
        .sclk_io_num     = GEC_LCD_SCLK_GPIO,
        .quadwp_io_num   = GPIO_NUM_NC,
        .quadhd_io_num   = GPIO_NUM_NC,
        .max_transfer_sz = LCD_MAX_TRANSFER_SZ,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI init failed");

    /* ---- 面板 IO ---- */
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num       = GEC_LCD_DC_GPIO,
        .cs_gpio_num       = (s_cs_gpio < 0) ? GPIO_NUM_NC : s_cs_gpio,
        .pclk_hz           = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits      = LCD_CMD_BITS,
        .lcd_param_bits    = LCD_PARAM_BITS,
        .spi_mode          = LCD_SPI_MODE,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                 &io_config, &io_handle),
        TAG, "New panel IO failed");

    /* ---- 接线：色彩数据发送完成 → lcd_on_color_trans_done → 注册者 ----
     * 必须在这里注册（而不是等 lcd_set_flush_done_cb 被调用），因为：
     *   ① 调用方可能「先 lcd_init() 后注册钩子」——那种顺序下靠
     *      lcd_set_flush_done_cb() 里的补注册也能接上；
     *   ② 但 lcd_reinit() 会重建 panel IO，旧的注册随旧 io 销毁。
     *      在每次 lcd_init() 里注册，钩子就天然跨 reinit 存活。
     * 未注册任何钩子时，该回调只是多一次判空 —— 对既有绘制路径零影响。 */
    const esp_lcd_panel_io_callbacks_t io_cbs = {
        .on_color_trans_done = lcd_on_color_trans_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(io_handle, &io_cbs, NULL),
                        TAG, "register panel io callbacks failed");

    /* ---- ST7796 面板 ----
     * rgb_ele_order：**BGR**，与两套厂家参考（06 BSP、xiaozhi）一致。
     *
     * 【2026-10-13 定案：此前写 RGB 是错的，且这个错误与「缺字节交换」互相掩护】
     *
     * 曾经的理由是「配 BGR 时彩条红蓝互换 ⇒ 本面板 bit3 极性与 IDF 假设相反」。
     * 该结论作废 —— 当时的观测是在**同时缺字节交换**的前提下做的，两个错误
     * 叠在一起，读数自然指向了错误的方向。
     *
     * 完整的真相（对着一张自标注色卡逐格反推得出，8/8 全中，推演见文件顶部）：
     *   ① 本面板**需要** bit3 = 1（即 `LCD_RGB_ELEMENT_ORDER_BGR`）；
     *   ② ESP32-S3 的 SPI LCD 驱动发 16 位像素时**低字节先出**，面板收到的
     *      是字节序颠倒的值，必须由软件预交换（见 `panel_send()`）。
     *   两者缺一不可，且**必须同时正确**：
     *     只有 ① ：红蓝互换
     *     只有 ② ：绿字段被彻底打散（6 位且跨字节边界）
     *     都没有 ：红和蓝碰巧看着对，绿全错 —— 本项目就卡在这个状态里
     *
     * 为什么这个状态极难识破：R 与 B 都是 5 位且在字中位置对称，两个错误对
     * 它们几乎完全抵消，纯色一直"看起来没问题"；而**绿是 6 位、跨字节边界**，
     * 字节交换会把它拆到乱七八糟的位置。绿又是亮度主通道，于是中间调、灰阶
     * 全线崩塌 —— 这就是"纯色勉强过、灰阶全错"的真正机制。
     *
     * 终值 MADCTL 随之由 0xE0 变为 **0xE8**（见下方初始化注释）。 */
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,               /* 06 BSP: BSP_LCD_RST = NC */
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7796(io_handle, &panel_config, &s_panel),
                        TAG, "New panel failed");

    /* ---- 复位 + 片选 ---- */
    /* 注意：RST=NC 时驱动会发软复位 SWRESET(0x01)+120ms。
       这一步若失败必须看得见 —— 此前忽略了返回值。 */
    const esp_err_t rret = esp_lcd_panel_reset(s_panel);
    ESP_LOGI(TAG, "面板复位（RST=NC → 软复位 SWRESET）-> %s", esp_err_to_name(rret));
    ESP_RETURN_ON_ERROR(rret, TAG, "panel reset failed");

    /* 06 BSP 在此调用 lcd_cs(0) —— 经 PCA9557 拉低片选。
       这一步本项目初版曾误删，导致连续多轮全黑排查。 */
    const esp_err_t csret = gec_lcd_cs(GEC_PCA9557_LEVEL_LCD_CS_ACTIVE);
    ESP_LOGI(TAG, "拉低 LCD 片选（PCA9557 P0=0）-> %s", esp_err_to_name(csret));
    if (csret != ESP_OK) {
        ESP_LOGW(TAG, "片选拉低失败: %s（面板将不响应命令）", esp_err_to_name(csret));
    }

    /* ---- 面板初始化序列（照搬 xiaozhi 顺序）----
     * 注意 MADCTL 是**驱动累加**出来的终值 0xE8：
     *   BGR 位由 rgb_ele_order 决定 —— BGR ⇒ 加 0x08；
     *   swap_xy(true) 加 MV(0x20) → 0x28；
     *   mirror(true,true) 再加 MX|MY(0x40|0x80) → 0xE8。
     * 手写 0x00 或 0x08 会黑屏/花屏，必须靠这三个 API 逐步设置。 */
    ESP_LOGI(TAG, "面板初始化序列开始（SLPOUT → MADCTL → COLMOD → 14 条厂商表）");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    ESP_LOGI(TAG, "面板初始化序列完成");

    /* invert_color 是**幂等设置**：发 INVON(0x21)。
       厂商初始化表末尾已经发过一次 0x21，这里再发一次不会"翻转两次"——
       0x21 是"进入反色模式"的设定命令，不是切换命令。
       保留它是为了与参考实现逐项一致。 */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG, "swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, true), TAG, "mirror failed");

    s_panel_io = io_handle;
    s_ready    = true;

    /* ---- 关键补齐①：先全屏填白，再打开显示 ----
     *
     * xiaozhi 的 SpiLcdDisplay 构造里做了这件事，而板级 InitializeSt7796Display()
     * 里**没有**（见 main/display/lcd_display.cc:139-146）：
     *   for (y...) esp_lcd_panel_draw_bitmap(panel, 0, y, 480, y+1, 0xFFFF);
     *   esp_lcd_panel_disp_on_off(panel, true);
     *
     * 只抄板级函数就会漏掉这两步 —— 面板 GRAM 里是上电随机值，
     * 表现为花屏/规律色带。 */
    ESP_LOGI(TAG, "全屏填白（清掉 GRAM 随机值）");
    const esp_err_t fret = lcd_fill(0xFFFF);
    ESP_LOGI(TAG, "填白 -> %s", esp_err_to_name(fret));
    if (fret != ESP_OK) {
        ESP_LOGE(TAG, "填白失败 —— 说明 SPI 发送通路有问题，需先解决这个");
    }

    /* ---- 关键补齐②：打开显示 ---- */
    const esp_err_t dret = esp_lcd_panel_disp_on_off(s_panel, true);
    ESP_LOGI(TAG, "打开显示（DISPON 0x29）-> %s", esp_err_to_name(dret));

    ESP_LOGI(TAG, "ST7796 就绪: %dx%d SPI3 MOSI=IO%d SCLK=IO%d DC=IO%d "
                  "模式%d @%uMHz",
             GEC_LCD_H_RES, GEC_LCD_V_RES, GEC_LCD_MOSI_GPIO, GEC_LCD_SCLK_GPIO,
             GEC_LCD_DC_GPIO, LCD_SPI_MODE, LCD_PIXEL_CLOCK_HZ / 1000000U);
    return ESP_OK;
}

bool lcd_is_ready(void)
{
    return s_ready;
}

esp_err_t lcd_reinit(void)
{
    /* 重建顺序（实测要求，见 docs/architecture.md 7.x）：
       必须先摘设备再释放总线，否则 DMA 不会重新绑定。

       ⚠️ P2 起（LVGL 接入后）本函数**只允许在 ui_lvgl_init() 之前调用**。
       它会 esp_lcd_panel_del() 并重建句柄；若 LVGL 的渲染任务正在 flush，
       就会拿已释放的句柄发 DMA。完成钩子本身跨 reinit 存活（lcd_init() 会
       重新注册），但**并发调用**依然非法。 */
    ESP_LOGW(TAG, "重新初始化 LCD");

    if (s_panel != NULL) {
        (void)esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
    if (s_panel_io != NULL) {
        (void)esp_lcd_panel_io_del(s_panel_io);
        s_panel_io = NULL;
    }
    (void)spi_bus_free(LCD_SPI_HOST);

    s_ready = false;
    return lcd_init();
}

/* ==========================================================================
 * 送往面板的**唯一出口** —— 标准 RGB565 → 面板
 *
 * 全项目只有这里调用 esp_lcd_panel_draw_bitmap。所有绘制路径（LVGL flush、
 * lcd_fill、彩条、棋盘格、进度条、各探针）都经过这里，颜色相关的变换只需
 * 维护一处。这是**刻意收口**：此前各路径各写各的，直接导致「LVGL 画的」与
 * 「自检画的」颜色不一致，排障时无法互相印证。
 *
 * ── 这里做的变换：字节交换。**它不是可选项。** ──────────────────────────
 *
 * ESP32-S3 的 SPI LCD 驱动发送 16 位像素时**低字节先出**，面板收到的是字节
 * 序颠倒的值。不补这一步，**绿字段（6 位且横跨字节边界）会被彻底打散**，
 * 而红/蓝字段（各 5 位、在字中位置对称）几乎不受影响 —— 外在表现正是
 * 「纯色勉强看着对、灰阶与中间调全错」。本项目曾长期困在这个状态。
 *
 * xiaozhi 的 `flags.swap_bytes = 1` 补的是同一个东西
 * （它在 LVGL 端口里逐像素调 `lv_draw_sw_rgb565_swap()`）。
 * 本项目放在这里，好处是**所有**绘制路径都自动带上，
 * 不会出现"某条路径忘了交换、于是两条路径互相矛盾"的情况。
 *
 * ⚠️ 本函数**就地改写** buf ⇒ 调用方在 DMA 完成前不得复用该缓冲；
 *    逐行调用的调用方**必须每行重新填缓冲**（连续两次调用等于没交换）。
 * ========================================================================== */
static esp_err_t panel_send(int x_start, int y_start, int x_end, int y_end,
                            uint16_t *buf, size_t n_px)
{
    for (size_t i = 0; i < n_px; i++) {
        buf[i] = (uint16_t)((buf[i] >> 8) | (buf[i] << 8));
    }
    return esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, buf);
}

/* ==========================================================================
 * LVGL 端口用的位图写入接口（P2 新增）
 *
 * 存在的意义：把「面板句柄」与「非标准色序」这两件事**继续封在组件内部**。
 * LVGL 侧只交出标准 RGB565 位图，不需要、也拿不到 panel 句柄。
 *
 * 这样做的直接收益：将来若换面板/换驱动芯片/色序结论被修正，
 * 影响范围都锁在本文件内，LVGL 那层一字不用改。
 * ========================================================================== */
esp_err_t lcd_draw_bitmap(int x_start, int y_start, int x_end, int y_end,
                          uint16_t *pixels)
{
    if (!s_ready || s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels == NULL ||
        x_start < 0 || y_start < 0 ||
        x_end > GEC_LCD_H_RES || y_end > GEC_LCD_V_RES ||
        x_end <= x_start || y_end <= y_start) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 标准 RGB565 交给 panel_send() —— 它负责唯一的传输前变换（字节交换）
       与硬件写入。本函数不再自己碰颜色，理由见 panel_send() 的说明。

       就地改写：panel_send 会**原地**交换 pixels 的字节，故调用方在 DMA
       完成前不得复用该缓冲 —— 这正是完成钩子存在的理由。 */
    const size_t n_px = (size_t)(x_end - x_start) * (size_t)(y_end - y_start);
    return panel_send(x_start, y_start, x_end, y_end, pixels, n_px);
}

/* lcd_draw_bitmap_raw() 已于 2026-10-13 **删除**。
 *
 * 它当初存在的唯一理由，是给「面板收到某个 16 位值后显示成什么」和
 * 「panel_encode 的换算对不对」造一个对照臂 —— 一个编码、一个不编码。
 * 既然 panel_encode() 已被证实是错误的（它把字段整体错位了 1 比特，见文件
 * 前部废除说明），两条路径现在**逐字节相同**，对照臂也就没有了存在意义。
 *
 * 再保留它反而有害：它的注释会写着「本函数画出来的颜色是错的」，
 * 而事实已不再如此，后人照着注释理解会被带偏。 */

/* ==========================================================================
 * 绘制 —— 原样照搬 06 BSP 的 lcd_set_color()
 *
 * 【为什么要照搬这个"低效"写法】
 * 06 BSP 逐行发送：每次 esp_lcd_panel_draw_bitmap(0, y, W, y+1, buffer)，
 * 一行 = 480×2 = 960 字节，缓冲区用 MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM。
 *
 * 本项目此前自作聪明改成「307KB 整帧」和「15KB 条带」，两次都出事：
 *   - 整帧：SPI 驱动的 setup_dma_priv_buffer 会在内部 RAM 分配等长拷贝 → 必然失败
 *   - 条带：绕过了一行路径，行为与已验证实现不同，无法与它对照
 * 逐行发送是**已验证工作**的写法，960 字节的传输在任何配置下都安全。
 * 性能差异在点屏/自检场景下可忽略；真要提速应在确认通路后再单独优化。
 * ========================================================================== */
esp_err_t lcd_fill(uint16_t color)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t *buffer = (uint16_t *)heap_caps_malloc(
        GEC_LCD_H_RES * sizeof(uint16_t), MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "行缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    for (int y = 0; y < GEC_LCD_V_RES; y++) {
        /* ⚠️ 缓冲必须在**每行发送前**重填：panel_send() 会就地交换字节，
           若只在循环外填一次，第二行会被"交换回去"，颜色又错了。 */
        for (size_t i = 0; i < GEC_LCD_H_RES; i++) {
            buffer[i] = color;
        }
        ret = panel_send(0, y, GEC_LCD_H_RES, y + 1, buffer, GEC_LCD_H_RES);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "第 %d 行绘制失败: %s", y, esp_err_to_name(ret));
            break;
        }
    }

    free(buffer);
    return ret;
}

/** 用同一套逐行路径画任意图案（回调给出每像素颜色） */
static uint16_t (*s_pixel_at)(int x, int y);

static esp_err_t draw_pattern(uint16_t (*pixel_at)(int x, int y))
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    s_pixel_at = pixel_at;

    uint16_t *buffer = (uint16_t *)heap_caps_malloc(
        GEC_LCD_H_RES * sizeof(uint16_t), MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    for (int y = 0; y < GEC_LCD_V_RES; y++) {
        for (int x = 0; x < GEC_LCD_H_RES; x++) {
            buffer[x] = s_pixel_at(x, y);   /* 每行重填：panel_send 会就地交换字节 */
        }
        ret = panel_send(0, y, GEC_LCD_H_RES, y + 1, buffer, GEC_LCD_H_RES);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "第 %d 行绘制失败: %s", y, esp_err_to_name(ret));
            break;
        }
    }

    free(buffer);
    return ret;
}

static uint16_t bars_pixel_at(int x, int y)
{
    (void)y;
    const int seg = GEC_LCD_H_RES / 5;
    switch (x / seg) {
    case 0:  return 0xF800;   /* 红 */
    case 1:  return 0x07E0;   /* 绿 */
    case 2:  return 0x001F;   /* 蓝 */
    case 3:  return 0xFFFF;   /* 白 */
    default: return 0x0000;   /* 黑 */
    }
}

esp_err_t lcd_draw_color_bars(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "绘制彩条：红|绿|蓝|白|黑（左→右）");
    return draw_pattern(bars_pixel_at);
}

/* ==========================================================================
 * 颜色验证探针
 *
 * 【为什么要加灰阶，而不再只测八色】
 * 纯色（红/绿/蓝）只能验证「通道有没有接反」，它**对字段错位天然不敏感** ——
 * 本项目的教训就在这里：面板其实一直是标准 RGB565，八色在错误的编码下也曾
 * 一度「看起来通过」，直到中间调（中性灰显示成紫色）才暴露。
 *
 * 原因很直白：纯色是每个通道取极值。错位一个比特后，极值挪到相邻字段，
 * 看上去仍然是一个饱和的近似色，肉眼很容易放过；而中间调里每一位都在
 * 参与配比，错位立刻变成明显的偏色。
 *
 * 故本探针的**主判据是那两级中性灰**：
 *   0x8410 与 0xCE79 是标准的中性灰（R、G、B 三通道各自约 50% / 80%）。
 *   只要屏幕上它们呈现**任何色调**（偏绿/偏蓝/偏红/发紫），即为错误。
 * 这一条比八色更硬 —— 中性灰在正确的实现里没有任何理由偏色。
 * ========================================================================== */
esp_err_t lcd_color_probe(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 八标准色 + 两级中性灰 + 彩条。
       八色覆盖单通道与两两组合的全部情形；灰阶作为中间调的主判据。 */
    static const struct { uint16_t color; const char *name; } k_seq[] = {
        { 0xF800, "红"   }, { 0x07E0, "绿"   }, { 0x001F, "蓝"   },
        { 0xFFE0, "黄"   }, { 0x07FF, "青"   }, { 0xF81F, "品红" },
        { 0xFFFF, "白"   }, { 0x0000, "黑"   },
        { 0x8410, "中性灰（约50%）" }, { 0xCE79, "浅灰（约80%）" },
    };
    const int step_cnt = (int)(sizeof(k_seq) / sizeof(k_seq[0]));

    ESP_LOGW(TAG, "========== 颜色验证 ==========");
    ESP_LOGW(TAG, "标准 RGB565 **直通**（无任何字段重排）—— 与两套厂家参考一致");
    ESP_LOGW(TAG, "判据：屏幕颜色与下方文字全部相符");
    ESP_LOGW(TAG, "**重点看最后两级灰：必须是不带任何色调的中性灰**");

    for (int i = 0; i < step_cnt; i++) {
        ESP_LOGW(TAG, "  第 %d/%d 步：屏幕应为【%s】 (值 0x%04X)",
                 i + 1, step_cnt, k_seq[i].name, k_seq[i].color);
        const esp_err_t ret = lcd_fill(k_seq[i].color);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "  填充失败: %s", esp_err_to_name(ret));
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    ESP_LOGW(TAG, "  最后：彩条，应为 红|绿|蓝|白|黑（左→右）");
    (void)lcd_draw_color_bars();
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGW(TAG, "========== 验证结束 ==========");
    ESP_LOGW(TAG, "请回报：**哪一步的颜色与文字不符**（全部相符则颜色一项收尾）");
    return ESP_OK;
}

static int s_cell;

static uint16_t checker_pixel_at(int x, int y)
{
    const bool odd = (((x / s_cell) + (y / s_cell)) & 1) != 0;
    return odd ? 0xFFFF : 0x0000;
}

esp_err_t lcd_draw_checkerboard(int cell)
{
    if (!s_ready || cell <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cell = cell;
    ESP_LOGI(TAG, "绘制 %d 像素棋盘格", cell);
    return draw_pattern(checker_pixel_at);
}

esp_err_t lcd_draw_progress_bar(int y, int height, uint16_t color)
{
    if (!s_ready || y < 0 || height <= 0 || (y + height) > GEC_LCD_V_RES) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t *buffer = (uint16_t *)heap_caps_malloc(
        GEC_LCD_H_RES * sizeof(uint16_t), MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = ESP_OK;
    for (int yy = y; yy < y + height; yy++) {
        /* ⚠️ 每行重填：panel_send() 会就地交换字节，只填一次会互相抵消 */
        for (size_t i = 0; i < GEC_LCD_H_RES; i++) {
            buffer[i] = color;
        }
        ret = panel_send(0, yy, GEC_LCD_H_RES, yy + 1, buffer, GEC_LCD_H_RES);
        if (ret != ESP_OK) {
            break;
        }
    }
    free(buffer);
    return ret;
}

/* ==========================================================================
 * 诊断辅助（本项目自加，不影响已验证路径）
 * ========================================================================== */
esp_err_t lcd_read_id(uint8_t id_out[3])
{
    /* ⚠️ 本探针不可信：面板 IO 在构造时固定传输参数，缺少读操作所需的
       DUMMY 相位，实测在**彩条正常显示时**仍读回 00 00 00。 */
    if (s_panel_io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t id[3] = { 0, 0, 0 };
    const esp_err_t ret = esp_lcd_panel_io_rx_param(s_panel_io, 0x04, id, sizeof(id));
    if (ret == ESP_OK && id_out != NULL) {
        id_out[0] = id[0];
        id_out[1] = id[1];
        id_out[2] = id[2];
    }
    ESP_LOGW(TAG, "读面板 ID(0x04): %s 返回 %02X %02X %02X（缺 DUMMY 相位，不可信）",
             esp_err_to_name(ret), id[0], id[1], id[2]);
    return ret;
}

void lcd_dump_pin_levels(void)
{
    const struct { const char *name; int gpio; } pins[] = {
        { "MOSI", GEC_LCD_MOSI_GPIO      },
        { "SCLK", GEC_LCD_SCLK_GPIO      },
        { "DC",   GEC_LCD_DC_GPIO        },
        { "背光", GEC_LCD_BACKLIGHT_GPIO },
    };
    ESP_LOGW(TAG, "===== SPI 引脚快照 =====");
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        ESP_LOGW(TAG, "  %-4s IO%-2d = %d", pins[i].name, pins[i].gpio,
                 gpio_get_level((gpio_num_t)pins[i].gpio));
    }
    ESP_LOGW(TAG, "  CS   = %s", (s_cs_gpio < 0) ? "NC（由 PCA9557 P0 控制）" : "GPIO");
    ESP_LOGW(TAG, "========================");
}

void lcd_set_spi_mode(int mode)
{
    (void)mode;
    /* 本文件严格照搬 06 BSP，SPI 模式固定为 3（LCD_SPI_MODE）。
       保留此接口仅为兼容既有调用点；如需改模式请直接改 LCD_SPI_MODE 并重新编译，
       因为运行期改模式需要完整重建总线，容易再引入变量。 */
    ESP_LOGW(TAG, "lcd_set_spi_mode(%d) 被忽略：本文件固定使用 06 BSP 的模式 %d",
             mode, LCD_SPI_MODE);
}

void lcd_set_pclk_hz(unsigned int hz)
{
    (void)hz;
    ESP_LOGW(TAG, "lcd_set_pclk_hz(%u) 被忽略：本文件固定使用 06 BSP 的 %u MHz",
             hz, LCD_PIXEL_CLOCK_HZ / 1000000U);
}

void lcd_set_panel_params(unsigned int colmod, bool bgr, bool invert_color)
{
    (void)colmod;
    (void)bgr;
    (void)invert_color;
    ESP_LOGW(TAG, "lcd_set_panel_params 被忽略：本文件固定使用参考实现的参数");
}

/* ==========================================================================
 * 枚举试验：方向 × 背光极性
 *
 * 依据（用户实测）：填白能显示、复位见规律色带 ⇒ 面板/背光/SPI 全正常，
 * 问题在"初始化后画面不对"。方向位只有 3 位，穷举 8 种远比反复试错快。
 * ========================================================================== */
esp_err_t lcd_orientation_bl_sweep(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* MV=0x20 MX=0x40 MY=0x80 —— 三位的全部 8 种组合 */
    static const struct { unsigned bit; const char *desc; } k_dirs[] = {
        { 0x00, "MV=0 MX=0 MY=0" },
        { 0x80, "MV=0 MX=0 MY=1" },
        { 0x40, "MV=0 MX=1 MY=0" },
        { 0xC0, "MV=0 MX=1 MY=1" },
        { 0x20, "MV=1 MX=0 MY=0" },
        { 0xA0, "MV=1 MX=0 MY=1" },
        { 0x60, "MV=1 MX=1 MY=0" },
        { 0xE0, "MV=1 MX=1 MY=1" },
    };
    const int dir_cnt = 8;

    ESP_LOGW(TAG, "========== 方向 × 背光极性 枚举试验开始 ==========");
    ESP_LOGW(TAG, "共 %d 组（8 方向 × 2 背光极性），每组 4 秒：先红后绿", dir_cnt * 2);
    ESP_LOGW(TAG, "**判据：出现名副其实的纯色（红就是红）即该组正确**");

    int round = 0;
    for (int d = 0; d < dir_cnt; d++) {
        for (int b = 0; b < 2; b++) {
            round++;
            lcd_set_backlight_invert(b != 0);

            ESP_LOGW(TAG, "--- 第 %d/%d 组: 方向[%s] 背光[%s] ---",
                     round, dir_cnt * 2, k_dirs[d].desc,
                     (b != 0) ? "高电平点亮" : "低电平点亮");

            /* 直接改 MADCTL 的 MV/MX/MY 三位，只保留颜色顺序位。
               ⚠️ 这里的 BGR 位必须与 panel_config.rgb_ele_order 保持一致 ——
               实测裁决为 RGB（见 lcd_init 内注释），故为 0x00 而非 0x08。
               若只改 rgb_ele_order 而漏改此处，本试验跑完会把颜色顺序改回去。 */
            const uint8_t madctl = (uint8_t)(LCD_MADCTL_BGR_BIT | k_dirs[d].bit);
            esp_err_t ret = esp_lcd_panel_io_tx_param(s_panel_io, 0x36,
                                                      (uint8_t[]){ madctl }, 1);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "    写 MADCTL=0x%02X 失败: %s", madctl, esp_err_to_name(ret));
                continue;
            }
            ESP_LOGW(TAG, "    MADCTL=0x%02X", madctl);

            (void)lcd_backlight(true);

            ESP_LOGW(TAG, "    屏幕现在应该是【纯红】");
            (void)lcd_fill(0xF800);
            vTaskDelay(pdMS_TO_TICKS(2000));

            ESP_LOGW(TAG, "    屏幕现在应该是【纯绿】");
            (void)lcd_fill(0x07E0);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    lcd_set_backlight_invert(LCD_BL_INVERT != 0);

    ESP_LOGW(TAG, "========== 枚举试验结束 ==========");
    ESP_LOGW(TAG, "请回报：**第几组出现了正确的纯色？**（并说明红/绿是否相符）");
    return ESP_OK;
}
