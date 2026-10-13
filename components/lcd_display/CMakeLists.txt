idf_component_register(
    SRCS        "lcd_display.c"
    INCLUDE_DIRS "include"
    REQUIRES     board_gec esp_lcd esp_lcd_st7796 esp_driver_spi
    PRIV_REQUIRES esp_driver_gpio
)

# ============================================================================
# 编译期质量门禁（针对本组件源码）
# 理由与做法见 main/CMakeLists.txt 中的同段说明。
# ============================================================================
set_source_files_properties(
    lcd_display.c
    PROPERTIES COMPILE_OPTIONS
    "-Werror=unused-variable;-Werror=unused-function;-Werror=unused-but-set-variable;-Werror=return-type;-Werror=implicit-function-declaration"
)
