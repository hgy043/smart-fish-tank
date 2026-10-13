# ============================================================================
# esp_lcd_touch_ft6x36 — FT6X36 触摸驱动（自包含副本）
#
# 来源：D:\ESP32\project\06-tcp_client_bafa_mqtt\components\esp_lcd_touch_ft6x36
#       （粤嵌官方 BSP，用户已验证可编译烧录）
#
# 该驱动**不在 ESP 组件注册表里** —— 它来自 github.com/cfscn/esp_lcd_touch_ft6x36，
# 故只能 vendor 进 components/，与 esp_lcd_st7796 的处理方式一致。
#
# 对原版做了**一处改动**：把依赖从 idf_component.yml 移到 CMakeLists。
#   原 CMakeLists 只写 REQUIRES "esp_lcd"，靠 idf_component.yml 里的
#       esp_lcd_touch: { public: true, version: ^1.0.4 }
#   传递依赖。本项目为自包含已删除 idf_component.yml，故必须在此显式声明，
#   否则编译期找不到 esp_lcd_touch.h。
#
# 升级时：从 06BSP 重新取该目录，重做上述一步。
# ============================================================================
idf_component_register(
    SRCS "esp_lcd_touch_ft6x36.c"
    INCLUDE_DIRS "include"
    REQUIRES "esp_lcd" "esp_lcd_touch"
)
