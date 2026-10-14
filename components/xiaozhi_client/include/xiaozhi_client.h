/**
 * @file xiaozhi_client.h
 * @brief 小智语音对话客户端（P7）
 *
 * ============================================================================
 * 【本组件做什么】
 *
 * 把「设备 ⇄ 小智服务器」这条链路封起来，上层只看到三件事：
 *   ① 连接状态（含"需要激活"与 6 位激活码）
 *   ② 收文本（对话内容，交给聊天流显示）
 *   ③ 收/发音频（Opus 帧，交给 audio_service 播放 / 从麦克风取）
 *
 * 协议细节 **照搬 xiaozhi-esp32-1.9.4-gec**（参见各 .c 文件顶部的出处标注）。
 * xiaozhi 本身就是用 ESP-IDF v5.5.3 构建的，与本工程同版本。
 *
 * ============================================================================
 * 【链路概览】（每一步的报文格式都在 xiaozhi_ota.c / xiaozhi_ws.c 里逐字标注）
 *
 *   开机 ──POST──▶ api.tenclass.net/xiaozhi/ota/
 *                     │  请求头带 Device-Id(MAC) / Client-Id(UUID)
 *                     ▼
 *                  响应 { websocket{url,token,version}, activation{code,...} }
 *                     │
 *          ┌──────────┴──────────┐
 *     有 activation          没有 activation
 *          │                      │
 *     屏幕显示 6 位码            直接连
 *     用户在 App 输入            │
 *     轮询 activate 接口 ────────┘
 *                                │
 *                    ──wss──▶ <websocket.url>
 *                     │  握手头：Authorization: Bearer <token>
 *                     │         Protocol-Version / Device-Id / Client-Id
 *                     ▼
 *                   发 hello ──▶ 服务器回 hello{session_id, audio_params}
 *                                │
 *                          可以对话了
 *
 * ============================================================================
 * 【字节序 / 格式约定】
 *   · 上行音频：Opus，**16 kHz 单声道 60ms/帧**（协议契约，不要改）
 *   · 下行音频：Opus，采样率以服务器 hello 的 audio_params 为准（通常 24 kHz）
 *   · 音频二进制帧：协议版本 **1（默认）= 裸 Opus，无帧头**。
 *     只有服务器在 OTA 的 websocket.version 里明确要求 2/3 时才需要加头，
 *     见 `xz_ws_send_audio()` 的说明。
 * ============================================================================
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* tank_ecosystem_t —— MCP 工具的数据源用的就是它。
   本组件因此依赖 app_core（纯数据模型组件，无外部依赖），
   与 ui_lvgl 依赖 app_core 是同一条既有分层。 */
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 状态
 * ========================================================================== */
typedef enum {
    XZ_STATE_IDLE = 0,          /* 未启动 */
    XZ_STATE_OTA,               /* 正在向服务器取配置 */
    XZ_STATE_NEED_ACTIVATION,   /* **需要激活**：界面上要显示 6 位码 */
    XZ_STATE_ACTIVATING,        /* 激活请求已发出，等待用户在 App 确认 */
    XZ_STATE_CONNECTING,        /* 正在建立 WebSocket */
    XZ_STATE_READY,             /* 已连上，可以说话 */
    XZ_STATE_ERROR,             /* 出错（见 xz_state_detail() 的原因串） */
} xz_state_t;

/** 状态变化 / 收到文本 的回调。**在 client 自己的任务上下文调用**
 *  （不是中断），实现方不要在里面做长时间阻塞的事。 */
typedef struct {
    void (*on_state)(xz_state_t st, const char *detail);
    /** role: "user"（识别出的用户话）/ "assistant"（AI 回复）/ "system"（提示） */
    void (*on_chat)(const char *role, const char *text);
    /** 收到一段 Opus 音频（服务器下行的 TTS） */
    void (*on_audio)(const uint8_t *opus, size_t len);
} xz_callbacks_t;

/* ==========================================================================
 * 生命周期
 * ========================================================================== */

/**
 * @brief 读 NVS 里的凭据并启动客户端任务。
 * @note  幂等。**不会**阻塞等待连接成功 —— 连接是异步的，
 *        结果通过 on_state 回调报告。
 * @note  需要 Wi-Fi 已连接；未连接时会等待（内部轮询），不报错。
 */
esp_err_t xz_client_start(void);

/** @brief 当前状态 */
xz_state_t xz_state(void);

/** @brief 状态文字（含失败原因）。始终返回非 NULL。 */
const char *xz_state_detail(void);

/**
 * @brief 需要激活时的 6 位激活码；不需要激活则返回 NULL。
 * @note  界面上应把它显示出来，用户在小智 App 里输入以完成绑定。
 *        激活成功后本函数返回 NULL。
 */
const char *xz_activation_code(void);

/** @brief 注册回调。必须在 xz_client_start() 之前调用才保证不漏事件。 */
void xz_set_callbacks(const xz_callbacks_t *cbs);

/* ==========================================================================
 * MCP 工具（让 AI 能读鱼缸，P7 阶段 4）
 *
 * 连上服务器后，服务器会发来 JSON-RPC 的 `initialize` / `tools/list`，
 * 之后 LLM 就能在对话中调用我们注册的工具 —— 比如用户问「鱼缸现在怎么样」。
 * 工具的注册表在 xiaozhi_mcp.c，**上层不需要、也无法在本组件之外加工具**。
 * ========================================================================== */

/**
 * @brief MCP 工具读鱼缸状态的数据源。由 main 注册。
 *
 * 【为什么用回调，而不是本组件自己去取数据】
 * 状态快照 `tank_ecosystem_t` 由 main 持有（它才是**唯一的写者**），本组件
 * 不该反向依赖 main。这与 xiaozhi 的做法是同构的 —— 它的 MCP 工具也是回调进
 * `board.GetDeviceStatusJson()`（mcp_server.cc:117-119），而不是自己翻数据。
 *
 * @param[out] out 写入当前快照
 * @return true 表示 out 已填好；false 表示数据还没准备好
 *         （工具会照实回一条 `{"success": false, ...}`，而不是编一个数）。
 *
 * ⚠️ **在 esp_websocket_client 自己的任务上被调用**（栈不归我们管）。
 *    实现必须快、且不能阻塞 —— 典型做法是「加锁 + 结构体拷贝」。
 */
typedef bool (*xz_tank_status_fn)(tank_ecosystem_t *out);

/**
 * @brief 注册鱼缸状态数据源。
 * @note  必须在 xz_client_start() 之前调用：服务器在 hello 之后**立刻**就会
 *        发工具请求，注册晚了那一次只能回「取不到数据」。
 */
void xz_set_tank_status_provider(xz_tank_status_fn fn);

/**
 * @brief 拍照数据源（`self.camera.take_photo` 用）。由 main 注册。
 *
 * 与上面的鱼缸数据源同一个套路：`camera_service` 是本组件不该依赖的东西，
 * 所以由 main 把它接起来。
 *
 * @param[out] jpeg     JPEG 数据起点
 * @param[out] out_len  JPEG 字节数
 * @return ESP_OK；其余为失败（本组件会把它变成一条给 LLM 看的失败说明，
 *         而不是让这次对话卡住）。
 *
 * ⚠️ **在工具任务上被调用，不是在 WebSocket 任务上** —— 这里可以（也应该）
 *    阻塞：抓帧最长 4 秒、编码若干百毫秒。这是本组件唯一一条允许慢的路径。
 * @note  返回的缓冲归**实现方**所有，本组件只在本次上传期间读它，不 free、
 *        也不长期持有。
 */
typedef esp_err_t (*xz_camera_capture_fn)(uint8_t **jpeg, size_t *out_len);

/**
 * @brief 注册拍照数据源。
 * @note  同样要在 xz_client_start() 之前注册。不注册的话
 *        `self.camera.take_photo` 会在工具表里**照常出现**（模型能看见），
 *        但一调就回失败 —— 如果本工程根本不需要这个能力，宁可不注册工具，
 *        也不要让它变成一个只会说"失败"的工具。
 */
void xz_set_camera_capture_provider(xz_camera_capture_fn fn);

/* ==========================================================================
 * 对话控制
 * ========================================================================== */

/** @brief 开始听（发 listen/start）。 */
esp_err_t xz_listen_start(void);

/** @brief 停止听（发 listen/stop），服务器随后开始回复。 */
esp_err_t xz_listen_stop(void);

/** @brief 打断当前回复（发 abort）。 */
esp_err_t xz_abort(void);

/**
 * @brief 发一帧上行音频（Opus，16kHz 单声道 60ms）。
 *
 * @note 由**音频侧**驱动节奏（它才知道麦克风的取帧速率），本组件只负责打包发送。
 *       只在 `xz_listen_start()` 与 `xz_listen_stop()` 之间调用才有意义。
 */
esp_err_t xz_send_audio(const uint8_t *opus, size_t len);

/** @brief 当前是否已经连上服务器、可以对话。 */
bool xz_is_ready(void);

/**
 * @brief 服务器在 hello 里指定的**下行音频采样率**（Hz）。
 *
 * @note 服务器用它告诉我"我的 TTS 是多大采样率的"，解码器必须按这个值创建，
 *       否则解出来是噪声。连接建立前返回 XZ_DOWN_SAMPLE_RATE_DEF。
 * @note 请在收到 XZ_STATE_READY 之后再取 —— 握手完成后这个值才是真的。
 */
int xz_down_sample_rate(void);

#ifdef __cplusplus
}
#endif
