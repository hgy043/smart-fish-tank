# ============================================================================
# camera_service — 摄像头服务（P6-a）
#
# 职责边界：只负责「把一帧画面变成标准 native RGB565」。不碰 LVGL、不碰界面 ——
# 由 main 把结果转交给 ui_lvgl，这样本组件可以独立测试（例如只打日志不上屏）。
#
# 依赖可见性（遵循工程既有约定）：
#   board_gec             摄像头 DVP 引脚宏 + PCA9557 上电（P2）
#   espressif__esp32-camera  esp_camera_init / fb_get / bswap 的来源
#   两者都只在 .c 内出现，公共头只用到 stdint/stdbool/esp_err，
#   故放 PRIV_REQUIRES —— 不把 esp_camera.h 泄漏给 main。
# ============================================================================
idf_component_register(
    SRCS        "camera_service.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES board_gec espressif__esp32-camera
)

# ============================================================================
# 编译期质量门禁（与工程内其它组件一致）
# ============================================================================
set_source_files_properties(
    camera_service.c
    PROPERTIES COMPILE_OPTIONS
    "-Werror=unused-variable;-Werror=unused-function;-Werror=unused-but-set-variable;-Werror=return-type;-Werror=implicit-function-declaration"
)
