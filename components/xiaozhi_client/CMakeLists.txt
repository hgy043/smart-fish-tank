# ============================================================================
# xiaozhi_client — 小智语音对话客户端（P7）
#
# 职责边界：只负责「设备 ⇄ 小智服务器」这条链路（OTA 取配置 / 激活 /
# WebSocket 协议）。**不碰界面、不碰音频编解码** —— 界面与音频由 main 通过
# 回调接线。这样本组件可以独立验证（先把连接跑通，再谈音频）。
#
# 依赖可见性：
#   xiaozhi_client.h 对外只用到 stdint/stdbool/esp_err，
#   故把其它依赖全放 PRIV_REQUIRES，不把 cJSON/websocket 头泄漏给 main。
#
# ⚠️ 本组件**暂不依赖 esp-sr / esp-opus** —— 那两个是音频侧（阶段 2/3）的事。
#    先把 OTA + 握手跑通，再引入吃内存的语音组件，出问题时才能分辨是哪一层的错。
# ============================================================================
idf_component_register(
    SRCS        "xiaozhi_ota.c" "xiaozhi_ws.c" "xiaozhi_client.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_client esp_websocket_client esp-tls mbedtls json
                  nvs_flash net_service spi_flash esp_app_format esp_hw_support
)

# ============================================================================
# 编译期质量门禁（与工程内其它组件一致）
# ============================================================================
set_source_files_properties(
    xiaozhi_ota.c
    xiaozhi_ws.c
    xiaozhi_client.c
    PROPERTIES COMPILE_OPTIONS
    "-Werror=unused-variable;-Werror=unused-function;-Werror=unused-but-set-variable;-Werror=return-type;-Werror=implicit-function-declaration"
)
