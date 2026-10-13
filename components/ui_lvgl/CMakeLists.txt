# ============================================================================
# ui_lvgl — LVGL 移植层与界面（P2）
#
# 设计要点（详见 include/ui_lvgl.h 顶部注释）：
#   · 只用 esp_lvgl_port 的「渲染任务 + 递归锁 + tick」三样基础设施，
#     **不使用**它的 lvgl_port_add_disp() —— 那个会绕过本面板必需的颜色
#     字段重排（panel_encode），并改写 MADCTL 破坏方向。
#   · 显示对象手工创建，flush 走 lcd_display 的 lcd_draw_bitmap()，
#     完成信号走 lcd_display 的 DMA 完成中断钩子。
#
# 依赖可见性（遵循工程既有约定：组件之间不互相依赖，公共类型走 app_core）：
#   app_core            公开头 ui_dashboard.h / ui_lvgl.h 用到 tank_ecosystem_t
#   lvgl                LVGL 本体
#   esp_lvgl_port       渲染任务 / 锁 / tick
#   lcd_display         绘制接口 + 完成钩子
#   78__xiaozhi-fonts   中文字体 font_puhui_20_4
#   以上除 app_core 外都只在 .c 内出现，故放 PRIV_REQUIRES，
#   避免把 lvgl.h 之类的重头文件泄漏给 main。
# ============================================================================
idf_component_register(
    SRCS        "ui_lvgl.c" "ui_pages.c" "ui_dashboard.c"
    INCLUDE_DIRS "include"
    REQUIRES     app_core
    PRIV_REQUIRES lvgl esp_lvgl_port lcd_display touch_input 78__xiaozhi-fonts
)

# ============================================================================
# 编译期质量门禁（与工程内其它组件一致）
# ============================================================================
set_source_files_properties(
    ui_lvgl.c
    ui_pages.c
    ui_dashboard.c
    PROPERTIES COMPILE_OPTIONS
    "-Werror=unused-variable;-Werror=unused-function;-Werror=unused-but-set-variable;-Werror=return-type;-Werror=implicit-function-declaration"
)
