# ============================================================================
# font_service —— 中文字体服务（详见 include/font_service.h）
#
# 为什么是独立组件，而不是塞进 ui_lvgl：
#   1) 它要 esp_partition + mmap + 78/xiaozhi-fonts 的 cbin_font.h，
#      ui_lvgl 不该背这些依赖；
#   2) 下面那句「把 fonts/*.bin 挂到 idf.py flash 上」需要放在**组件**的
#      CMakeLists 里 —— 那时分区表已经就绪，partition_table_get_partition_info
#      才拿得到 offset。esp-sr 挂它自己的 srmodels.bin 用的也是这个位置。
# ============================================================================

idf_component_register(
    SRCS         "font_service.c"
    INCLUDE_DIRS "include"
    # 78__xiaozhi-fonts 提供 cbin_font.h（头文件要走 include path，必须显式声明）。
    # 另外它也是 font_puhui_16_4 / font_puhui_20_4 这两个符号的提供方 ——
    # 那两个符号靠链接期解析，本工程历来没写这条 requires 也能链上。
    PRIV_REQUIRES lvgl esp_partition heap 78__xiaozhi-fonts
    # 公开头用了 lv_font_t，所以 lvgl 也要对使用方可见。
    REQUIRES      lvgl
)

# ----------------------------------------------------------------------------
# 把 fonts/font_noto_qwen_16_4.bin 挂到 `idf.py flash` 上，写进 `fonts` 分区。
#
# 机制与 espressif__esp-sr/CMakeLists.txt:96 挂 srmodels.bin 完全相同：
# esptool_py_flash_to_partition() 会在配置期用 parttool.py 查出分区偏移，
# 然后往 flash 目标上追加一条镜像。**不需要手工 esptool 命令。**
#
# 判据（首次烧录时看这两条）：
#   · 烧录日志里出现 "fonts.bin" / 0xd00000 字样；
#   · 串口出现 [font] Noto 回退已挂上…
# 没烧上/内容不对时**不致命** —— 会打 [font] 的 WARN，界面照常，只是缺字仍是方框。
# ----------------------------------------------------------------------------
set(FONT_BIN "${CMAKE_CURRENT_LIST_DIR}/../../fonts/font_noto_qwen_16_4.bin")

if(EXISTS "${FONT_BIN}")
    esptool_py_flash_to_partition(flash "fonts" "${FONT_BIN}")
else()
    # 不 FATAL_ERROR：字体是锦上添花，不该拦住整个工程。
    message(WARNING
        "找不到字体文件: ${FONT_BIN}\n"
        "  → 缺字形修复不会生效（其余功能不受影响）。\n"
        "  → 恢复办法见 fonts/README.md。")
endif()

# ============================================================================
# 编译期质量门禁（与工程内其它组件一致）
# ============================================================================
set_source_files_properties(
    font_service.c
    PROPERTIES COMPILE_OPTIONS
    "-Werror=unused-variable;-Werror=unused-function;-Werror=unused-but-set-variable;-Werror=return-type;-Werror=implicit-function-declaration"
)
