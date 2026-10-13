# ============================================================================
# touch_input — 触摸输入（FT6X36，I2C 轮询）
#
# 依赖：
#   board_gec          复用其 I2C 总线与引脚真源
#   esp_lcd_touch      FT6X36 驱动的公共层（已 vendored 进 components/）
#   esp_lcd_touch_ft6x36  具体驱动（已 vendored）
#   esp_lcd            esp_lcd_new_panel_io_i2c
#
# 刻意**不依赖 LVGL** —— 本组件只负责读数，接到 LVGL 上是 ui_lvgl 的事。
# 这样将来 P5 语音交互要复用触摸时，不必被 LVGL 绑住。
# ============================================================================
idf_component_register(
    SRCS        "touch_input.c"
    INCLUDE_DIRS "include"
    REQUIRES     board_gec esp_lcd_touch esp_lcd_touch_ft6x36 esp_lcd
)

# ============================================================================
# 编译期质量门禁（与工程内其它组件一致）
# ============================================================================
set_source_files_properties(
    touch_input.c
    PROPERTIES COMPILE_OPTIONS
    "-Werror=unused-variable;-Werror=unused-function;-Werror=unused-but-set-variable;-Werror=return-type;-Werror=implicit-function-declaration"
)
