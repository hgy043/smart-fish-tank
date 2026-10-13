idf_component_register(
    SRCS        "gec_board.c"
    INCLUDE_DIRS "include"
    REQUIRES     esp_driver_i2c esp_driver_gpio esp_driver_spi
)

# ============================================================================
# 编译期质量门禁（针对本组件源码）
# 理由与做法见 main/CMakeLists.txt 中的同段说明：
# ESP-IDF 无条件豁免了一批常见警告，须逐文件强制才能真正中断构建。
# ============================================================================
set_source_files_properties(
    gec_board.c
    PROPERTIES COMPILE_OPTIONS
    "-Werror=unused-variable;-Werror=unused-function;-Werror=unused-but-set-variable;-Werror=return-type;-Werror=implicit-function-declaration"
)
