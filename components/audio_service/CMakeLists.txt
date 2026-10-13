idf_component_register(
    SRCS        "audio_service.c"
    INCLUDE_DIRS "include"
    REQUIRES     board_gec esp_driver_i2s esp_driver_gpio espressif__esp_codec_dev
)

# ============================================================================
# 编译期质量门禁（针对本组件源码）
# 理由与做法见 main/CMakeLists.txt 中的同段说明。
# ============================================================================
set_source_files_properties(
    audio_service.c
    PROPERTIES COMPILE_OPTIONS
    "-Werror=unused-variable;-Werror=unused-function;-Werror=unused-but-set-variable;-Werror=return-type;-Werror=implicit-function-declaration"
)
