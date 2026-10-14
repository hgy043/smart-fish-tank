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

/** vision 上传（拍照给 AI 看）的超时。
 *  比 OTA 那 10 秒宽得多：服务器要在那边跑一个视觉模型才回得来。
 *  这条路径跑在**工具任务**上，不是 WebSocket 任务，所以占着不碍事。 */
#define XZ_VISION_TIMEOUT_MS    30000

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

/* ==========================================================================
 * MCP 工具任务（跑"慢工具"—— 目前就是拍照上传）
 * ========================================================================== */

/** 跑慢工具的任务栈。**初值照搬 xiaozhi**：
 *  `application.cc:610-613` 的 `main_event_loop` 是 `2048 * 4 = 8192`，
 *  而那正是它执行 `camera->Capture()` / `Explain()` 的任务 —— 同一职责、
 *  同一量级的调用链（JPEG 编码器 + esp_http_client），所以从它的数字起步，
 *  比拍脑袋估一个靠谱。
 *
 *  ⚠️ 这是**初值，不是结论**：跑过一轮拍照后按 monitor_task 的实测水位收，
 *     收到"峰值 + 一倍余量"为止（工程里 XZ_TASK_STACK 就是这么从
 *     8192 收到 6144 的）。**不要在没跑过拍照的情况下凭开机水位去收它** ——
 *     开机时这条任务还没干过活，水位读数是假的。 */
#define XZ_TOOL_TASK_STACK      8192
#define XZ_TOOL_TASK_PRIO       3       /* 与 xz_client/mqtt 同级 */
#define XZ_TOOL_TASK_CORE       0       /* 与 Wi-Fi 同核，但优先级不高于它 */

/** 任务名。main 侧要用它取任务句柄做栈水位监控，故不能在两处各写一份字面量。 */
#define XZ_TOOL_TASK_NAME       "xz_tool"

/** 待执行工具请求的队列深度。
 *  深度 2 而不是 1：偶发的连续两次调用（用户追问）不该被拒。
 *  满了两条还在来，说明模型在乱调，回 busy 比无限排队好。 */
#define XZ_MCP_QUEUE_LEN        2

/** 工具调用里字符串参数的存放上限（`question`）。 */
#define XZ_MCP_QUESTION_MAX     256

/** 工具结果文本的上限。服务器回的视觉描述可能不短，给宽一点。 */
#define XZ_MCP_RESULT_MAX       512

#ifdef __cplusplus
}
#endif
