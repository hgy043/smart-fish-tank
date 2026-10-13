# ============================================================================
# esp_lcd_st7796 — LCD 驱动（自包含副本）
#
# 来源：06-tcp_client_bafa_mqtt/components/esp_lcd_st7796 （esp-bsp v1.4.0，
#       用户已验证可编译烧录）。
#
# 做了三处调整，让组件**自包含、不依赖组件管理器**：
#   1. 删掉原 CMakeLists 里的 `include(package_manager)` —— 那需要额外的
#      cmake_utilities 组件。
#   2. 删掉 idf_component.yml —— 避免构建时联网拉取。
#   3. `cu_pkg_define_version()` 原本会生成 ESP_LCD_ST7796_VER_MAJOR/
#      MINOR/PATCH 三个宏，驱动源码里要用；这里用 target_compile_definitions
#      手工补上，版本号与 idf_component.yml 记录的 1.4.0 一致。
#      ⚠️ 升级驱动时这三个宏必须同步改，否则打印的版本号会骗人。
#
# 升级驱动时：从 esp-bsp 重新取 components/lcd/esp_lcd_st7796 并重做上述三步。
# ============================================================================
set(srcs "esp_lcd_st7796.c"
         "esp_lcd_st7796_general.c")
if(CONFIG_SOC_MIPI_DSI_SUPPORTED)
    list(APPEND srcs "esp_lcd_st7796_mipi.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "priv_include"
                       REQUIRES "esp_lcd"
                       PRIV_REQUIRES "driver")

# 版本宏（对应 esp-bsp esp_lcd_st7796 v1.4.0）
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    ESP_LCD_ST7796_VER_MAJOR=1
    ESP_LCD_ST7796_VER_MINOR=4
    ESP_LCD_ST7796_VER_PATCH=0
)
