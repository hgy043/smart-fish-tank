idf_component_register(
    SRCS        "wifi_sta.c" "bafa_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES     app_core esp_wifi esp_event esp_netif esp_timer mqtt
    PRIV_REQUIRES nvs_flash
)

# ============================================================================
# 编译期质量门禁（针对本组件源码）
# 理由与做法见 main/CMakeLists.txt 中的同段说明。
# ============================================================================
set_source_files_properties(
    wifi_sta.c
    bafa_mqtt.c
    PROPERTIES COMPILE_OPTIONS
    "-Werror=unused-variable;-Werror=unused-function;-Werror=unused-but-set-variable;-Werror=return-type;-Werror=implicit-function-declaration"
)
