# ============================================================================
# ui_lvgl — LVGL 移植层与界面（P2 / P6-a）
#
# 设计要点（详见 include/ui_lvgl.h 顶部注释）：
#   · 只用 esp_lvgl_port 的「渲染任务 + 递归锁 + tick」三样基础设施，
#     **不使用**它的 lvgl_port_add_disp()。
#   · 显示对象手工创建，flush 走 lcd_display 的 lcd_draw_bitmap()，
#     完成信号走 lcd_display 的 DMA 完成中断钩子。
#     （⚠️ 早期注释说"因为 add_disp 会绕过 panel_encode"—— panel_encode
#      已被证实是错的并删除，那条理由作废。真实理由见 ui_lvgl.h。）
#   · 拍照显示用**双缓冲**，锁外拷贝、锁内只切指针（见 ui_lvgl.c）。
#
# 依赖可见性（遵循工程既有约定：组件之间不互相依赖，公共类型走 app_core）：
#   app_core            公开头 ui_dashboard.h / ui_lvgl.h 用到 tank_ecosystem_t
#                       （含 ECO_ALERT_* 告警位定义）
#   lvgl                LVGL 本体
#   esp_lvgl_port       渲染任务 / 锁 / tick
#   lcd_display         绘制接口 + 完成钩子
#   78__xiaozhi-fonts   中文字体 font_puhui_20_4
#   以上除 app_core 外都只在 .c 内出现，故放 PRIV_REQUIRES，
#   避免把 lvgl.h 之类的重头文件泄漏给 main。
#
#   ⚠️ 本组件**不认识** camera_service —— 它只接受"一帧标准 RGB565"。
#      摄像头与本组件的接线由 main 做（main 同时依赖两者）。
#      这样 ui_lvgl 可以脱离摄像头单独测试。
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
