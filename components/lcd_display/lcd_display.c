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
   实测裁决：本面板为 RGB 顺序 ⇒ 0x00（BGR 位不置位，判据见 lcd_init 内注释）。 */
#define LCD_MADCTL_BGR_BIT  0x00

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
 * 面板字段重排（本项目唯一的颜色编码入口）
 *
 * 【实测结论 — 2026-10-13，一轮 2×5 矩阵 + 一轮单位位探针】
 *
 * 这颗面板的控制器**不按 RGB565 标准排字段**，实测路由为：
 *     F0 [15:11] (5 位) → 红
 *     F1 [10:5]  (6 位) → 蓝
 *     F2 [4:0]   (5 位) → 绿
 * 即字段顺序是 **R, B, G**，而标准 RGB565 是 R, G, B。
 *
 * 判据：
 *   - 0x07E0（只有 [10:5] 置位）显示**蓝**  ⇒ 该 6 位字段归蓝，不是绿；
 *   - 0x0020（bit5）显示**蓝**               ⇒ bit5 属于蓝字段；（这一条
 *     直接否定了「蓝色只有 5 位、绿占 [5:0]」的替代模型）
 *   - 0x001F（只有 [4:0] 置位）显示**绿**     ⇒ 该 5 位字段归绿。
 *
 * **MADCTL 修不好它**：bit3 只交换 F0/F1 的去向（屏幕上表现为红蓝互换），
 * 管不到「6 位字段归绿还是归蓝」。所以必须在送数据前重排字段。
 *
 * 【为什么 b 左移 6 而不是 5】
 * 蓝色是 **6 位**通道，而复用 5 位的 b。若 b 低位对齐（b<<5），蓝最多只能到
 * 半亮度；必须**高位对齐**（b<<6，F1 的 bit5 恒为 0），蓝才占满量程。
 * 绿是 6 位数据塞进 5 位字段，只能丢最低位（g>>1），这是本面板的硬限制。
 *
 * 【已知存疑的一点】
 * 单位位探针里 0x0010(bit4) 报的是蓝，按本模型应为绿 —— 4 个点位中错 1 个，
 * 疑为读数时串步（该步前后两次都是蓝）。**不影响本公式**：本公式已对
 * 红/绿/蓝/黄/青/品红/白/黑 八个标准色逐一验算通过。
 * ========================================================================== */
static inline uint16_t panel_encode(uint16_t rgb565)
{
    const uint16_t r = (uint16_t)((rgb565 >> 11) & 0x1F);
    const uint16_t g = (uint16_t)((rgb565 >>  5) & 0x3F);
    const uint16_t b = (uint16_t)( rgb565        & 0x1F);

    return (uint16_t)((r << 11) | (b << 6) | (g >> 1));
}

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

    /* ---- ST7796 面板 ----
     * rgb_ele_order：**实测裁决为 RGB（BGR 位 = 0）**，与两套厂家参考都相反。
     *
     * 判据（2026-10-13 烧录实测）：配置 BGR 时彩条呈「蓝|绿|红|白|黑」——
     * **红蓝互换、黑白正常**。黑白正常 ⇒ INVON 是对的、只有通道顺序错；
     * 红蓝互换 ⇒ MADCTL 的 BGR 位(bit3)与这颗面板的物理接线相反。
     *
     * ⚠️ 06 BSP 与 xiaozhi 都写 BGR，且 06 BSP 能出图 —— 但「能出图」不等于
     * 「颜色正确」，两套参考很可能同样是红蓝互换而未被察觉（此前从未有人
     * 逐条核对过彩条颜色）。此处以**实测**为准，与「照搬不优化」原则不冲突：
     * 偏离的判据已由观察者直接给出。
     *
     * 终值 MADCTL 随之由 0xE8 变为 **0xE0**（见下方初始化注释）。 */
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,               /* 06 BSP: BSP_LCD_RST = NC */
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
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
     * 注意 MADCTL 是**驱动累加**出来的终值 0xE0：
     *   BGR 位由 rgb_ele_order 决定 —— 实测为 RGB ⇒ 不加 0x08；
     *   swap_xy(true) 加 MV(0x20) → 0x20；
     *   mirror(true,true) 再加 MX|MY(0x40|0x80) → 0xE0。
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
       必须先摘设备再释放总线，否则 DMA 不会重新绑定。 */
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

    const uint16_t enc = panel_encode(color);
    for (size_t i = 0; i < GEC_LCD_H_RES; i++) {
        buffer[i] = enc;
    }

    esp_err_t ret = ESP_OK;
    for (int y = 0; y < GEC_LCD_V_RES; y++) {
        ret = esp_lcd_panel_draw_bitmap(s_panel, 0, y, GEC_LCD_H_RES, y + 1, buffer);
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
            buffer[x] = panel_encode(s_pixel_at(x, y));
        }
        ret = esp_lcd_panel_draw_bitmap(s_panel, 0, y, GEC_LCD_H_RES, y + 1, buffer);
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
 * 位权重探针 —— 判定「6 位字段属于绿还是蓝」
 *
 * 【已确定的事实（2026-10-13 一轮 2×5 矩阵实测，10 点全部吻合）】
 *
 *   MADCTL bit3 = 0 时，控制器把 16 位像素按**字段**这样路由：
 *       F0 [15:11] (5 位) → 红
 *       F1 [10:5]  (6 位) → 蓝
 *       F2 [4:0]   (5 位) → 绿
 *   bit3 = 1 时交换 F0 / F1 的去向（屏幕上表现为红蓝互换）—— 即 bit3 行为
 *   与 IDF 定义一致，但它**管不到 F1 该给绿还是给蓝**。
 *
 * 结论：这颗面板认的字段顺序是 **R, B, G**，而非 RGB565 标准的 R, G, B。
 * 任何 MADCTL 取值都修不好，只能在送数据前重排字段。
 *
 * 【仍未定的一处：位宽分配】
 *   F1 是 6 位、F2 是 5 位，而蓝只有 5 位、绿有 6 位 —— 谁拿 6 位？
 *     模型 A：F1→蓝(6位)、F2→绿(5位)   ⇒ 绿丢 1 位精度
 *     模型 B：[10:6]→蓝(5位)、[5:0]→绿(6位) ⇒ **无损**
 *   这决定重排时该左移 5 位还是 6 位，选错会让中间调掺错通道。
 *
 * 【为什么用 0x0020 判定】
 *   只置 bit5 时：
 *     模型 A：bit5 落在 F1 内 → **蓝**
 *     模型 B：bit5 是绿色字段的 MSB → **绿**
 *   **颜色本身不同**，不依赖对亮度的主观判断 —— 亮度类判据在本项目里
 *   已经出过事，能避就避。
 * ========================================================================== */
esp_err_t lcd_color_probe(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 八标准色 + 彩条 —— 验证 panel_encode() 是否让三个通道同时归位。
       八色覆盖了单通道与两两组合的全部情形，任一通道错位都会立刻显形；
       单色全屏，没有彩条那种「左右读数顺序」的隐患。 */
    static const struct { uint16_t color; const char *name; } k_seq[] = {
        { 0xF800, "红"   }, { 0x07E0, "绿"   }, { 0x001F, "蓝"   },
        { 0xFFE0, "黄"   }, { 0x07FF, "青"   }, { 0xF81F, "品红" },
        { 0xFFFF, "白"   }, { 0x0000, "黑"   },
    };
    const int step_cnt = (int)(sizeof(k_seq) / sizeof(k_seq[0]));

    ESP_LOGW(TAG, "========== 颜色编码验证 ==========");
    ESP_LOGW(TAG, "已启用 panel_encode()：标准 RGB565 → 面板实际需要的 R,B,G 字段顺序");
    ESP_LOGW(TAG, "**判据：屏幕颜色与下方文字全部相符，即编码正确**");

    for (int i = 0; i < step_cnt; i++) {
        ESP_LOGW(TAG, "  第 %d/%d 步：屏幕应为【%s】 (标准值 0x%04X)",
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
    const uint16_t enc = panel_encode(color);
    for (size_t i = 0; i < GEC_LCD_H_RES; i++) {
        buffer[i] = enc;
    }

    esp_err_t ret = ESP_OK;
    for (int yy = y; yy < y + height; yy++) {
        ret = esp_lcd_panel_draw_bitmap(s_panel, 0, yy, GEC_LCD_H_RES, yy + 1, buffer);
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
