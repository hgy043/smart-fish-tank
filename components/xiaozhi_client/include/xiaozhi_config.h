/**
 * @file xiaozhi_config.h
 * @brief xiaozhi_client 的可调参数（集中于此，禁止在 .c 里散落魔法数字）
 *
 * 与 xiaozhi-esp32-1.9.4-gec 的取值逐项对齐；偏离之处都写了理由。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 服务器
 * ========================================================================== */

/** OTA 接口。取自 xiaozhi 的 `CONFIG_OTA_URL`（其 sdkconfig:603）。
 *  客户端 POST 到它取 WebSocket 地址/token，以及（首次）激活码。
 *  自建 xiaozhi-server 时改这一行即可。 */
#define XZ_OTA_URL              "https://api.tenclass.net/xiaozhi/ota/"

/** 上报给服务器的 User-Agent：`<board>/<version>`（xiaozhi 的 `SystemInfo::GetUserAgent()`）。
 *  服务器可能据此判断设备型号，保持与 xiaozhi 同格式。 */
#define XZ_BOARD_NAME           "smart-fish-tank"

/** Accept-Language。xiaozhi 传 `Lang::CODE`，其 zh-CN 版即此值。 */
#define XZ_LANGUAGE             "zh-CN"

/* ==========================================================================
 * 超时（逐项照搬 xiaozhi 的取值，见括号内的出处）
 * ========================================================================== */

#define XZ_HTTP_TIMEOUT_MS      10000   /* OTA/激活的 HTTP 超时 */
#define XZ_WS_HANDSHAKE_MS      10000   /* 等 WebSocket 101 响应（web_socket.cc:173-179） */
#define XZ_WS_HELLO_TIMEOUT_MS  10000   /* 等服务器 hello（websocket_protocol.cc:188-193） */

/** 无任何入站数据多久算通道超时、需要重连。
 *  出处：`protocol.cc:81-90` 的 `kTimeoutSeconds = 120`。
 *  注意 xiaozhi **不发应用层心跳**（它的 `Ping()` 全工程无调用者），
 *  超时靠"有没有收到数据"判断。 */
#define XZ_WS_IDLE_TIMEOUT_MS   (120 * 1000)

/** 激活重试间隔（`application.cc:257-271`）：
 *  服务器返回 202（等用户在 App 确认）→ 3 秒；其它失败 → 10 秒。 */
#define XZ_ACTIVATE_WAIT_MS     3000
#define XZ_ACTIVATE_RETRY_MS    10000
#define XZ_ACTIVATE_MAX_TRIES   10

/* ==========================================================================
 * 音频（协议契约，**不要改**）
 * ========================================================================== */

/** 上行 Opus：16 kHz 单声道 60ms/帧。
 *  出处：`audio_service.cc:38-39` 的 `OpusEncoderWrapper(16000, 1, 60)`，
 *  以及上行 hello 里的 `audio_params`。服务器按这个契约收音频，改了两端对不上。 */
#define XZ_UP_SAMPLE_RATE       16000
#define XZ_UP_CHANNELS          1
#define XZ_UP_FRAME_MS          60
#define XZ_UP_FRAME_SAMPLES     ((XZ_UP_SAMPLE_RATE) * (XZ_UP_FRAME_MS) / 1000)   /* 960 */

/** 下行 Opus 的缺省参数（服务器 hello 里会给真值，这里是兜底）。
 *  出处：`protocol.h:86-87`。
 *  ⚠️ 别拿 XZ_UP_FRAME_MS 顶替下行帧长 —— 上下行是**两个独立**的契约，
 *     只是恰好都是 60ms。将来服务器改成别的值时，混用会让人查错方向。 */
#define XZ_DOWN_SAMPLE_RATE_DEF 24000
#define XZ_DOWN_FRAME_MS_DEF    60

/* ==========================================================================
 * 任务
 * ========================================================================== */

/** 栈 6144：本任务要跑 **TLS 握手**（esp_http_client + wss），
 *  那是这个组件里最吃栈的一步。
 *  实测（2026-10-13 上板，monitor_task 的栈高水位）：余 4792 / 8192，
 *  即峰值只用 3.4KB —— 8192 是过量分配，已收到 6144，仍留 2.7KB 余量。
 *  改动后请复核水位读数。 */
#define XZ_TASK_STACK           6144
#define XZ_TASK_PRIO            3       /* 与 mqtt_task 同级：网络类，不该压过 UI */
#define XZ_TASK_CORE            0       /* 与 Wi-Fi/MQTT 同核；core1 已被 UI/LVGL/传感器占满 */

#ifdef __cplusplus
}
#endif
