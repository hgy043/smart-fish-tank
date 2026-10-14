# ============================================================================
# sd_service — TF 卡存储服务（1-bit SD 模式，P6-c）
#
# 职责边界：只管「挂载 / 存照片 / 列照片 / 解成缩略图」。
# **不碰界面、不碰摄像头** —— 照片从哪来、显示到哪去，都是 main 的事。
#
# 依赖可见性：
#   sd_service.h 对外只用到 stdint/stdbool/stddef/esp_err，
#   故所有实现依赖放 PRIV_REQUIRES，不把 fatfs / sdmmc / esp_jpeg 的头
#   泄漏给 main。
#
# ⚠️ fatfs 的公开头 `esp_vfs_fat.h` **无条件** `#include "driver/sdspi_host.h"`，
#    所以即使我们只用 SDMMC 模式，`esp_driver_sdspi` 也必须能被解析 ——
#    它由 fatfs 的公开依赖传递带出，不必单列。
# ============================================================================
idf_component_register(
    SRCS        "sd_service.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES fatfs sdmmc esp_driver_sdmmc
                  espressif__esp_jpeg board_gec
                  esp_timer esp_common
)

# ============================================================================
# 编译期质量门禁（与工程内其它组件一致）
# ============================================================================
set_source_files_properties(
    sd_service.c
    PROPERTIES COMPILE_OPTIONS
    "-Werror=unused-variable;-Werror=unused-function;-Werror=unused-but-set-variable;-Werror=return-type;-Werror=implicit-function-declaration"
)
