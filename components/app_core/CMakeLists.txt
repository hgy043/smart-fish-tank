idf_component_register(
    SRCS        "app_events.c"
    INCLUDE_DIRS "."
    REQUIRES     esp_event
)

# ============================================================================
# 编译期质量门禁（针对本组件源码）
# 理由与做法见 main/CMakeLists.txt 中的同段说明。
# ============================================================================
set_source_files_properties(
    app_events.c
    PROPERTIES COMPILE_OPTIONS
    "-Werror=unused-variable;-Werror=unused-function;-Werror=unused-but-set-variable;-Werror=return-type;-Werror=implicit-function-declaration"
)
