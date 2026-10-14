/**
 * @file xiaozhi_internal.h
 * @brief xiaozhi_client 组件内部共享的东西（不对外）
 *
 * 对外接口只有 xiaozhi_client.h 一个。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "xiaozhi_client.h"

/* xz_mcp_handle() 的参数是解析好的 cJSON 子树，所以这里要 cJSON 的类型。
   本头文件只在组件内部使用（不对外），而 json 已在 PRIV_REQUIRES 里。 */
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 设备标识 —— 服务器用它认设备，**格式必须与 xiaozhi 逐字节一致**
 * ========================================================================== */

/**
 * @brief 设备 ID = Wi-Fi STA MAC，**全小写、冒号分隔**（如 `7c:df:a1:12:34:56`）。
 *
 * 出处：xiaozhi `main/system_info.cc:34-44` 的 `GetMacAddress()`，
 * `snprintf("%02x:%02x:...")`。大小写/分隔符写错会导致服务器把它当成新设备、
 * 要求重新激活。
 */
const char *xz_device_id(void);

/**
 * @brief 客户端 ID = 标准 UUIDv4（小写带连字符），首次生成后存 NVS 复用。
 *
 * 出处：xiaozhi `main/boards/common/board.cc:14-45`。
 * NVS 存于 namespace `"board"` / 键 `"uuid"`（与 xiaozhi 一致，
 * 换回 xiaozhi 固件时不会变成新设备）。
 */
const char *xz_client_id(void);

/* ==========================================================================
 * 状态机（实现在 xiaozhi_client.c）
 * ========================================================================== */
void xz_set_state(xz_state_t st, const char *fmt, ...);

/** 当前注册的回调（可能为 NULL 字段） */
const xz_callbacks_t *xz_cbs(void);

/* ==========================================================================
 * OTA（实现在 xiaozhi_ota.c）
 * ========================================================================== */

/** 服务器下发的 WebSocket 配置 */
#define XZ_URL_MAX    192
#define XZ_TOKEN_MAX  128

typedef struct {
    char url[XZ_URL_MAX];
    char token[XZ_TOKEN_MAX];
    int  version;       /* 协议版本，默认 1 = 裸 Opus */
} xz_ws_cfg_t;

/**
 * @brief 向 OTA 接口取配置（含 websocket url/token/version）与激活信息。
 *
 * @param[out] out 取到的 websocket 配置
 * @return ESP_OK；ESP_ERR_NOT_FINISHED（**需要激活**，激活码见 xz_activation_code()）；
 *         其余为网络/解析错误。
 *
 * @note 需要 Wi-Fi 已连接。
 */
esp_err_t xz_ota_fetch(xz_ws_cfg_t *out);

/**
 * @brief 发起一次激活请求（用户在 App 里输入激活码后才会成功）。
 * @return ESP_OK 激活成功；ESP_ERR_TIMEOUT 服务器返回 202（等用户确认，应重试）；
 *         其余为失败。
 */
esp_err_t xz_ota_activate(void);

/** 无条件重新向服务器取一次配置（丢弃 NVS 里的缓存）。 */
esp_err_t xz_ota_refresh(void);

/* ==========================================================================
 * WebSocket（实现在 xiaozhi_ws.c）
 * ========================================================================== */

/** 建立连接并完成 hello 握手。阻塞至握手完成或超时。 */
esp_err_t xz_ws_open(const xz_ws_cfg_t *cfg);

/** 关闭连接。 */
void xz_ws_close(void);

/** 是否已连接且 hello 握手完成。 */
bool xz_ws_is_open(void);

/** 发一条文本 JSON（内部会补 session_id 之外的东西由调用方负责）。 */
esp_err_t xz_ws_send_text(const char *json);

/** 发一段 Opus 音频。协议版本 1 = 裸帧，2/3 = 加对应帧头（见 .c 内说明）。 */
esp_err_t xz_ws_send_audio(const uint8_t *opus, size_t len);

/** 服务器给出的 session_id（hello 之前为空串）。之后每条上行都要带。 */
const char *xz_ws_session_id(void);

/** 服务器下行的采样率（服务器 hello 的 audio_params.sample_rate，通常 24000）。 */
int xz_ws_server_sample_rate(void);

/* ==========================================================================
 * MCP 工具服务端（实现在 xiaozhi_mcp.c，P7 阶段 4）
 * ========================================================================== */

/**
 * @brief 处理一条 MCP 请求并回包。
 *
 * @param payload 服务器消息里 `payload` 那个子树（**不含**外层信封），
 *                已由调用方 cJSON_Parse 出来，本函数不改动它。
 *
 * ⚠️ 在 **esp_websocket_client 自己的任务**上被调用，且回包也在这条任务上发出。
 *    工具实现必须快（见 xiaozhi_mcp.c 的文件头）。
 *    **慢工具（拍照）不走这里同步执行** —— 它们由 xz_mcp_start_worker()
 *    建的工具任务执行，本函数只负责入队后立即返回。
 */
void xz_mcp_handle(const cJSON *payload);

/**
 * @brief 创建慢工具队列与执行任务（幂等）。由 xz_client_start() 调用。
 * @return ESP_OK；任务/队列创建失败时返回 ESP_ERR_NO_MEM。
 */
esp_err_t xz_mcp_start_worker(void);

/* ==========================================================================
 * 视觉上传（实现在 xiaozhi_vision.c）
 *
 * 服务器在 MCP initialize 里下发 vision 地址；xiaozhi_mcp.c 收到后就存进这里。
 * ========================================================================== */

/** 记下本次会话的 vision 地址与 token（token 就是 session_id，每会话一变）。 */
void xz_vision_set(const char *url, const char *token);

/** 本次会话是否有 vision 地址可用。 */
bool xz_vision_is_ready(void);

/** 上传一张 JPEG 取回解释文本。失败时 out 里仍会写入一句给 LLM 看的说明。 */
esp_err_t xz_vision_upload(const uint8_t *jpeg, size_t jpeg_len,
                           const char *question, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
