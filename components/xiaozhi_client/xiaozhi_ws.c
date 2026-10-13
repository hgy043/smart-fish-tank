/**
 * @file xiaozhi_ws.c
 * @brief 与小智服务器的 WebSocket 传输层（P7）
 *
 * 照搬 xiaozhi `main/protocols/websocket_protocol.cc` 的报文，但**换掉传输实现**：
 *   · xiaozhi 用自研客户端（`78/esp-ml307`，链接进固件 152KB）
 *   · 本工程用 IDF 的 `esp_websocket_client`
 * 已逐项核对过没有能力缺口：握手需要的四个自定义头（`Authorization` /
 * `Protocol-Version` / `Device-Id` / `Client-Id`）、二进制帧、wss(TLS) 它都支持。
 *
 * ── 帧格式（**这一条最省事，也最容易搞错**）────────────────────────────────
 * 协议**版本 1（默认）= 裸 Opus，没有任何帧头**。
 * 版本 2/3 才需要 `BinaryProtocol2`（16 字节头）/ `BinaryProtocol3`（4 字节头），
 * 那是服务器在 OTA 的 `websocket.version` 里明确要求时才用。
 * 见 `xz_ws_send_audio()` 的分支与 `xiaozhi_internal.h` 的说明。
 */
#include "xiaozhi_internal.h"
#include "xiaozhi_config.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"

#include "cJSON.h"

static const char *TAG = "xz_ws";

/* 服务器 hello 里的参数（protocol.h:86-87 的缺省值） */
#define XZ_SID_MAX 80

static esp_websocket_client_handle_t s_client = NULL;
static SemaphoreHandle_t             s_hello_sem = NULL;   /* 服务器 hello 到达 */
static volatile bool                 s_open = false;

static char s_session_id[XZ_SID_MAX] = "";
static int  s_server_rate   = XZ_DOWN_SAMPLE_RATE_DEF;
static int  s_proto_version = 1;

/* 接收分片重组缓冲。服务器下行的 JSON 有的一两 KB，音频帧则很小。 */
#define XZ_RX_MAX 6144
static char   s_rx[XZ_RX_MAX];
static size_t s_rx_len = 0;
static int    s_rx_opcode = 0;

/* ==========================================================================
 * 服务器 hello（websocket_protocol.cc:227-253 的等价物）
 * ========================================================================== */
static void parse_server_hello(const cJSON *root)
{
    /* transport 必须是 "websocket"，否则丢弃（原文如此） */
    const cJSON *transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport) || strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGW(TAG, "服务器 hello 的 transport 不是 websocket，忽略");
        return;
    }

    const cJSON *sid = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(sid)) {
        snprintf(s_session_id, sizeof(s_session_id), "%s", sid->valuestring);
    }

    const cJSON *ap = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(ap)) {
        const cJSON *sr = cJSON_GetObjectItem(ap, "sample_rate");
        if (cJSON_IsNumber(sr)) {
            s_server_rate = sr->valueint;
        }
        /* frame_duration 服务器也会给，但本工程解码侧按实际帧长处理，不单独记 */
    }

    ESP_LOGI(TAG, "服务器 hello: session_id=%s, 下行采样率=%d Hz",
             (s_session_id[0] != '\0') ? s_session_id : "(空)", s_server_rate);

    s_open = true;
    if (s_hello_sem != NULL) {
        xSemaphoreGive(s_hello_sem);
    }
}

/* ==========================================================================
 * 发 hello（websocket_protocol.cc:202-225 的等价物）
 * ========================================================================== */
static void send_hello(void)
{
    /* ⚠️ 本工程不宣告 aec（xiaozhi 的 GEC 板同样没开 CONFIG_USE_SERVER_AEC）。
       宣告了却没做 AEC，服务器会期待回采参考信号而对不齐。 */
    char json[256];
    snprintf(json, sizeof(json),
             "{\"type\":\"hello\",\"version\":%d,"
             "\"features\":{\"mcp\":true},"
             "\"transport\":\"websocket\","
             "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":%d,"
             "\"channels\":%d,\"frame_duration\":%d}}",
             s_proto_version, XZ_UP_SAMPLE_RATE, XZ_UP_CHANNELS, XZ_UP_FRAME_MS);

    ESP_LOGI(TAG, "→ hello: %s", json);
    (void)esp_websocket_client_send_text(s_client, json, (int)strlen(json),
                                         pdMS_TO_TICKS(5000));
}

/* ==========================================================================
 * 收到音频帧 → 交给上层
 * ========================================================================== */
static void deliver_audio(const uint8_t *data, size_t len)
{
    const uint8_t *payload = data;
    size_t         plen    = len;

    /* 版本 2/3 才需要剥帧头；版本 1 是裸 Opus（见文件头说明） */
    if (s_proto_version == 2 && len > 16) {
        /* BinaryProtocol2：u16 version; u16 type; u32 reserved;
                             u32 timestamp; u32 payload_size; payload
           注意 payload_size 是 **u32**（16 字节头）—— 别写成 u16。 */
        const uint32_t psz = ((uint32_t)data[12] << 24) | ((uint32_t)data[13] << 16) |
                             ((uint32_t)data[14] << 8)  |  (uint32_t)data[15];
        if (psz <= len - 16) {
            payload = data + 16;
            plen    = psz;
        }
    } else if (s_proto_version == 3 && len > 4) {
        /* BinaryProtocol3：u8 type; u8 reserved; u16 payload_size; payload */
        const uint16_t psz = (uint16_t)(((uint16_t)data[2] << 8) | data[3]);
        if (psz <= len - 4) {
            payload = data + 4;
            plen    = psz;
        }
    }

    const xz_callbacks_t *cb = xz_cbs();
    if (cb != NULL && cb->on_audio != NULL && plen > 0) {
        cb->on_audio(payload, plen);
    }
}

/* ==========================================================================
 * 下行 JSON 分发（application.cc:679-766 的等价物）
 * ========================================================================== */
static void handle_json(const cJSON *root)
{
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        return;
    }

    const xz_callbacks_t *cb = xz_cbs();

    if (strcmp(type->valuestring, "hello") == 0) {
        parse_server_hello(root);

    } else if (strcmp(type->valuestring, "tts") == 0) {
        const cJSON *state = cJSON_GetObjectItem(root, "state");
        if (!cJSON_IsString(state)) {
            return;
        }
        if (strcmp(state->valuestring, "sentence_start") == 0) {
            /* 只有 sentence_start 才带 text（原文如此） */
            const cJSON *text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text) && cb != NULL && cb->on_chat != NULL) {
                cb->on_chat("assistant", text->valuestring);
            }
        } else {
            ESP_LOGD(TAG, "← tts/%s", state->valuestring);
        }

    } else if (strcmp(type->valuestring, "stt") == 0) {
        /* 用户说的话被识别出来了 */
        const cJSON *text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text) && cb != NULL && cb->on_chat != NULL) {
            cb->on_chat("user", text->valuestring);
        }

    } else if (strcmp(type->valuestring, "llm") == 0) {
        const cJSON *emo = cJSON_GetObjectItem(root, "emotion");
        ESP_LOGD(TAG, "← llm/emotion=%s",
                 cJSON_IsString(emo) ? emo->valuestring : "(无)");

    } else if (strcmp(type->valuestring, "mcp") == 0) {
        /* 工具调用。本工程暂未实现 MCP 工具（阶段 4），先记日志。 */
        const cJSON *payload = cJSON_GetObjectItem(root, "payload");
        ESP_LOGI(TAG, "← mcp 工具调用（尚未实现，忽略）");
        (void)payload;

    } else if (strcmp(type->valuestring, "alert") == 0) {
        const cJSON *msg = cJSON_GetObjectItem(root, "message");
        if (cJSON_IsString(msg)) {
            ESP_LOGW(TAG, "← 服务器提示: %s", msg->valuestring);
            if (cb != NULL && cb->on_chat != NULL) {
                cb->on_chat("system", msg->valuestring);
            }
        }

    } else {
        /* ⚠️ 未识别的 type **只记日志、不断链** —— 这是 xiaozhi 的行为
           （application.cc:763-765），服务器加新消息类型不该让老设备掉线。 */
        ESP_LOGD(TAG, "← 未识别的消息类型: %s", type->valuestring);
    }
}

/* ==========================================================================
 * WebSocket 事件
 * ========================================================================== */
static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket 已连接，发送 hello");
        /* 连接成功后**立即**发 hello —— 服务器只等 10 秒 */
        send_hello();
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket 断开");
        s_open = false;
        break;

    case WEBSOCKET_EVENT_DATA: {
        if (d->op_code == 0x08) {       /* close */
            ESP_LOGW(TAG, "服务器关闭连接");
            s_open = false;
            break;
        }
        if (d->op_code == 0x09 || d->op_code == 0x0A) {
            break;                       /* ping/pong 由客户端库处理 */
        }

        /* ---- 分片重组 ----
           esp_websocket_client 在分片时把一整条消息分多次回调，
           通过 payload_offset 指示位置。data_len 只是本片的长度。 */
        if (d->payload_offset == 0) {
            s_rx_len = 0;
            s_rx_opcode = d->op_code;
        }
        if (d->data_len > 0 && s_rx_len + (size_t)d->data_len < sizeof(s_rx)) {
            memcpy(s_rx + s_rx_len, d->data_ptr, (size_t)d->data_len);
            s_rx_len += (size_t)d->data_len;
        }

        /* 还没收完就继续等下一片 */
        if (d->payload_offset + d->data_len < d->payload_len) {
            break;
        }

        if (s_rx_opcode == 0x02) {              /* 二进制 = Opus 音频 */
            deliver_audio((const uint8_t *)s_rx, s_rx_len);
        } else if (s_rx_opcode == 0x01) {       /* 文本 = JSON */
            s_rx[s_rx_len] = '\0';
            cJSON *root = cJSON_Parse(s_rx);
            if (root != NULL) {
                handle_json(root);
                cJSON_Delete(root);
            } else {
                ESP_LOGW(TAG, "收到非法 JSON（%u 字节）", (unsigned)s_rx_len);
            }
        }
        s_rx_len = 0;
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket 错误");
        s_open = false;
        break;

    default:
        break;
    }
}

/* ==========================================================================
 * 对外
 * ========================================================================== */
const char *xz_ws_session_id(void)
{
    return s_session_id;
}

int xz_ws_server_sample_rate(void)
{
    return s_server_rate;
}

bool xz_ws_is_open(void)
{
    return s_open;
}

esp_err_t xz_ws_open(const xz_ws_cfg_t *cfg)
{
    if (cfg == NULL || cfg->url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_client != NULL) {
        xz_ws_close();
    }

    s_proto_version = cfg->version;
    s_session_id[0] = '\0';
    s_server_rate   = XZ_DOWN_SAMPLE_RATE_DEF;
    s_rx_len        = 0;
    s_open          = false;

    /* ---- 握手头（websocket_protocol.cc:100-109）----
       只带这四个；协议头（Upgrade/Connection/Sec-WebSocket-*）由客户端库自己补。
       esp_websocket_client 的 headers 是**以 \r\n 结尾的一整串**。 */
    static char headers[512];
    int n = snprintf(headers, sizeof(headers),
                     "Protocol-Version: %d\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",
                     cfg->version, xz_device_id(), xz_client_id());

    if (cfg->token[0] != '\0' && n > 0 && n < (int)sizeof(headers)) {
        /* xiaozhi 的规则：token 里没有空格就自动补 "Bearer "（:100-106） */
        const char *prefix = (strchr(cfg->token, ' ') == NULL) ? "Bearer " : "";
        snprintf(headers + n, sizeof(headers) - (size_t)n,
                 "Authorization: %s%s\r\n", prefix, cfg->token);
    }

    const esp_websocket_client_config_t ws_cfg = {
        .uri                = cfg->url,
        .headers            = headers,
        /* wss 走 TLS，用 IDF 的证书包校验（不跳过） */
        .crt_bundle_attach  = esp_crt_bundle_attach,
        /* 自动重连交给我们自己管（与 xiaozhi 一致）：它有自己的退避与
           "空闲 120 秒判超时"逻辑，库的自动重连会与之打架。 */
        .disable_auto_reconnect = true,
        /* 下行 JSON 可能一两 KB。**不要**为它把缓冲开大 ——
           内部 RAM 是本工程最稀缺的资源（实测接入本组件后余量从 38% 掉到 29%）。
           分片重组的分支已实现在事件处理器里，缓冲开小只是多几次回调。 */
        .buffer_size        = 2048,
        .task_stack         = 4096,
        .task_prio          = 5,
        /* xiaozhi **不发应用层心跳**（它的 Ping() 全工程无调用者）。
           库的默认 10 秒会自动发 ping 帧 —— 服务器按 RFC 应答即可，
           但为了与参考行为一致、也少占空中时间，这里调到 0（关闭）。 */
        .ping_interval_sec  = 0,
        .network_timeout_ms = XZ_WS_HANDSHAKE_MS,
    };

    if (s_hello_sem == NULL) {
        s_hello_sem = xSemaphoreCreateBinary();
        if (s_hello_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        xSemaphoreTake(s_hello_sem, 0);     /* 清掉上一次的残留 */
    }

    ESP_LOGI(TAG, "连接 %s", cfg->url);
    s_client = esp_websocket_client_init(&ws_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_websocket_client_init 失败");
        return ESP_FAIL;
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, NULL);

    const esp_err_t ret = esp_websocket_client_start(s_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start 失败: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return ret;
    }

    /* 等服务器 hello。整条链有两个 10 秒：握手 + hello，
       这里给一个合并的上限即可（握手失败会先触发 DISCONNECTED）。 */
    if (xSemaphoreTake(s_hello_sem, pdMS_TO_TICKS(XZ_WS_HANDSHAKE_MS +
                                                  XZ_WS_HELLO_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "等待服务器 hello 超时");
        xz_ws_close();
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "会话就绪");
    return ESP_OK;
}

void xz_ws_close(void)
{
    s_open = false;
    if (s_client != NULL) {
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
}

esp_err_t xz_ws_send_text(const char *json)
{
    if (s_client == NULL || json == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const int sent = esp_websocket_client_send_text(s_client, json, (int)strlen(json),
                                                    pdMS_TO_TICKS(3000));
    if (sent < 0) {
        ESP_LOGE(TAG, "发送文本失败");
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "→ %s", json);
    return ESP_OK;
}

esp_err_t xz_ws_send_audio(const uint8_t *opus, size_t len)
{
    if (s_client == NULL || opus == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 版本 1：裸 Opus 直接发，**没有帧头**（websocket_protocol.cc:55-57）*/
    if (s_proto_version == 1) {
        return (esp_websocket_client_send_bin(s_client, (const char *)opus, (int)len,
                                              pdMS_TO_TICKS(3000)) < 0)
               ? ESP_FAIL : ESP_OK;
    }

    uint8_t hdr[16];
    size_t  hlen;

    if (s_proto_version == 2) {
        /* BinaryProtocol2，16 字节头，多字节字段**大端** */
        hdr[0] = 0; hdr[1] = 2;                     /* version = 2 */
        hdr[2] = 0; hdr[3] = 0;                     /* type = 0（OPUS） */
        hdr[4] = hdr[5] = hdr[6] = hdr[7] = 0;      /* reserved */
        hdr[8] = hdr[9] = hdr[10] = hdr[11] = 0;    /* timestamp（无 server AEC 时用不到）*/
        hdr[12] = (uint8_t)((len >> 24) & 0xFF);
        hdr[13] = (uint8_t)((len >> 16) & 0xFF);
        hdr[14] = (uint8_t)((len >> 8)  & 0xFF);
        hdr[15] = (uint8_t)( len        & 0xFF);
        hlen = 16;
    } else {
        /* BinaryProtocol3，4 字节头，payload_size 大端 u16 */
        hdr[0] = 0;                                 /* type = 0（OPUS） */
        hdr[1] = 0;                                 /* reserved */
        hdr[2] = (uint8_t)((len >> 8) & 0xFF);
        hdr[3] = (uint8_t)( len       & 0xFF);
        hlen = 4;
    }

    /* 拼一帧发出去。Opus 帧很小（几十~160 字节），栈上拼没问题。 */
    uint8_t frame[16 + 400];
    if (len > sizeof(frame) - hlen) {
        ESP_LOGE(TAG, "Opus 帧过大: %u 字节", (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(frame, hdr, hlen);
    memcpy(frame + hlen, opus, len);

    return (esp_websocket_client_send_bin(s_client, (const char *)frame,
                                          (int)(hlen + len), pdMS_TO_TICKS(3000)) < 0)
           ? ESP_FAIL : ESP_OK;
}
