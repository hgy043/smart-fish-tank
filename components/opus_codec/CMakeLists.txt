# ============================================================================
# opus_codec — Opus 编解码（P7）
#
# 依赖：78__esp-opus（由 main/idf_component.yml 的 78/esp-opus-encoder 间接带入，
#       两者都已解析进 dependencies.lock）。
#       这里**直接依赖 78__esp-opus 本体**，不依赖 C++ 的 esp-opus-encoder ——
#       理由见 opus_codec.h。
#
# opus.h 用在 .c 里而不出现在头文件，故放 PRIV_REQUIRES，
# 不把头文件依赖泄漏给调用方。
# ============================================================================
idf_component_register(
    SRCS        "opus_codec.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES 78__esp-opus
)

# ============================================================================
# 编译期质量门禁（与工程内其它组件一致）
# ============================================================================
set_source_files_properties(
    opus_codec.c
    PROPERTIES COMPILE_OPTIONS
    "-Werror=unused-variable;-Werror=unused-function;-Werror=unused-but-set-variable;-Werror=return-type;-Werror=implicit-function-declaration"
)
