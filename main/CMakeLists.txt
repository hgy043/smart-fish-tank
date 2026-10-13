idf_component_register(
    SRCS        "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES     app_core board_gec net_service lcd_display audio_service ui_lvgl camera_service nvs_flash esp_timer esp_driver_gpio
)

# ============================================================================
# 编译期质量门禁（针对本组件源码）
#
# ESP-IDF 无条件把一批常见警告排除在 -Werror 之外（见 sdkconfig.defaults 中的
# 实测记录），因此依赖全局配置无法捕获「定义了未使用的变量/函数」这类问题。
# 这里对本组件源码逐文件强制，使这类问题直接中断构建。
#
# 只作用于 main 组件自身的文件，不影响 IDF 与第三方组件。
# ============================================================================
set_source_files_properties(
    app_main.c
    PROPERTIES COMPILE_OPTIONS
    "-Werror=unused-variable;-Werror=unused-function;-Werror=unused-but-set-variable;-Werror=return-type;-Werror=implicit-function-declaration"
)
