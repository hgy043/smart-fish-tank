/**
 * @file touch_input.c
 * @brief 触摸输入实现（FT6X36，I2C 轮询）
 *
 * 硬件与设计说明见 include/touch_input.h。
 *
 * ── 一处与参考实现的**故意偏离**（务必知道） ──────────────────────────────
 * 粤嵌 06BSP 的 bsp_touch_init() 是这样创建 panel IO 的：
 *
 *     esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)BSP_I2C_NUM, ...);
 *
 * 它把**端口号**（0）强转成了总线句柄。在 C 语言下，IDF 5.5 的
 * esp_lcd_new_panel_io_i2c() 只有一个原型，接收的是 `i2c_master_bus_handle_t`
 * （见 esp_lcd_io_i2c.h：C++ 下才有按类型重载的 v1/v2 双版本）。
 * 强转端口号得到的是一个无效句柄 —— 那样建出来的触摸 IO 实际不可用。
 *
 * 这与 pinmap 里「xiaozhi 的 InitializeTouch() 被注释未启用」相互印证：
 * 这块板子的触摸很可能在厂家例程里**本来就没真正跑起来过**。
 *
 * 本实现改为传**真实的 bus 句柄**（gec_board_get_i2c_bus()），是正确用法。
 * 若实测触摸仍无效，应优先怀疑硬件（触摸排线未接 / 面板无触摸层），
 * 而不是这段代码的 API 用法。
 * ============================================================================
 */
#include "touch_input.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_ft6x36.h"

#include "gec_board.h"      /* 板级引脚真源 + I2C 总线句柄 */

static const char *TAG = "touch";

static esp_lcd_touch_handle_t    s_tp    = NULL;
static esp_lcd_panel_io_handle_t s_tp_io = NULL;

bool touch_input_is_ready(void)
{
    return s_tp != NULL;
}

esp_err_t touch_input_init(void)
{
    if (s_tp != NULL) {
        return ESP_OK;      /* 幂等 */
    }

    i2c_master_bus_handle_t bus = gec_board_get_i2c_bus();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "I2C 未初始化 —— 请先调用 gec_board_i2c_init()");

    /* ---- 触摸的 I2C panel IO ---- */
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_FT6x36_CONFIG();
    io_cfg.scl_speed_hz = GEC_I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &s_tp_io),
                        TAG, "创建触摸 I2C IO 失败");

    /* ---- FT6X36 驱动实例 ----
       坐标参数**逐字段沿用 06BSP**（它在本板上验证过）：
         注意 x_max / y_max 与屏幕分辨率是**对调**的 —— 因为 swap_xy 打开，
         驱动内部先做轴交换，再按 x_max/y_max 做镜像标定。
       故 x_max = 屏高(320)、y_max = 屏宽(480)，这不是笔误。 */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = GEC_LCD_V_RES,             /* 320，见上方说明 */
        .y_max = GEC_LCD_H_RES,             /* 480 */
        .rst_gpio_num = GPIO_NUM_NC,        /* 触摸复位与 LCD 共享，无独立脚 */
        .int_gpio_num = GPIO_NUM_NC,        /* 无中断脚 ⇒ 纯轮询 */
        .levels = {
            .reset     = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy  = 1,
            .mirror_x = 0,
            .mirror_y = 1,
        },
    };

    const esp_err_t ret = esp_lcd_touch_new_i2c_ft6x36(s_tp_io, &tp_cfg, &s_tp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FT6X36 初始化失败: %s", esp_err_to_name(ret));
        (void)esp_lcd_panel_io_del(s_tp_io);
        s_tp_io = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "FT6X36 就绪 @0x%02X（轮询模式，无中断/复位脚，swap_xy=1 mirror=(0,1)）",
             GEC_I2C_ADDR_FT6X36);
    return ESP_OK;
}

bool touch_input_read(uint16_t *x, uint16_t *y)
{
    if (s_tp == NULL) {
        return false;
    }

    /* 读一次硬件寄存器。无中断脚 ⇒ 这一次调用就是一次 I2C 事务 */
    if (esp_lcd_touch_read_data(s_tp) != ESP_OK) {
        return false;
    }

    /* 用 esp_lcd_touch_get_data() 而非 esp_lcd_touch_get_coordinates()：
       后者在 esp_lcd_touch 1.2.1 里已标记 deprecated（2.0 将移除）。
       本项目要求零警告，故直接用新接口。 */
    esp_lcd_touch_point_data_t pts[1];
    uint8_t cnt = 0;

    if (esp_lcd_touch_get_data(s_tp, pts, &cnt, 1) != ESP_OK || cnt == 0) {
        return false;
    }

    if (x != NULL) { *x = pts[0].x; }
    if (y != NULL) { *y = pts[0].y; }
    return true;
}
