/**
 * @file xiaozhi_client.c
 * @brief 小智客户端的编排层：状态机 + 连接维护（P7）
 *
 * 对外只暴露 xiaozhi_client.h 里的接口。链路细节在 xiaozhi_ota.c / xiaozhi_ws.c。
 *
 * ── 与 xiaozhi 的对应关系 ──────────────────────────────────────────────────
 * xiaozhi 把这套逻辑放在 `application.cc` 的 `CheckNewVersion()` +
 * `OpenAudioChannel()` 里，由一个 `MainEventLoop` 任务驱动。
 * 本工程改成一个**专用任务内的线性流程** —— 因为我们的状态比它简单
 * （不做 MCP 之外的设备控制、不做 OTA 升级、不做配网）。
 *
 * ── 一个刻意的设计：这里**不阻塞开机 ──────────────────────────────────────
 * `xz_client_start()` 只创建任务就返回。网络交互（可能耗时几十秒，含激活等待）
 * 全部在任务里做。这样即使小智服务器不可达，鱼缸本地的界面/传感器/巴法云
 * 都不受影响 —— 与摄像头"失败不致命"是同一条原则。
 */
#include "xiaozhi_internal.h"
#include "xiaozhi_config.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "wifi_sta.h"

static const char *TAG = "xz";

static xz_state_t     s_state = XZ_STATE_IDLE;
static char           s_detail[96] = "未启动";
static xz_callbacks_t s_cbs;                 /* 回调（可能全为 NULL） */
static TaskHandle_t   s_task = NULL;

/* ==========================================================================
 * 状态
 * ========================================================================== */
void xz_set_state(xz_state_t st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_detail, sizeof(s_detail), fmt, ap);
    va_end(ap);

    if (st != s_state) {
        ESP_LOGI(TAG, "状态 %d → %d：%s", (int)s_state, (int)st, s_detail);
        s_state = st;
    } else {
        ESP_LOGD(TAG, "状态 %d：%s", (int)st, s_detail);
    }

    if (s_cbs.on_state != NULL) {
        s_cbs.on_state(st, s_detail);
    }
}

xz_state_t xz_state(void)
{
    return s_state;
}

const char *xz_state_detail(void)
{
    return s_detail;
}

const xz_callbacks_t *xz_cbs(void)
{
    return &s_cbs;
}

void xz_set_callbacks(const xz_callbacks_t *cbs)
{
    if (cbs != NULL) {
        s_cbs = *cbs;
    } else {
        memset(&s_cbs, 0, sizeof(s_cbs));
    }
}

bool xz_is_ready(void)
{
    return (s_state == XZ_STATE_READY) && xz_ws_is_open();
}

int xz_down_sample_rate(void)
{
    /* 还没握手时 xz_ws 的那个静态量是初值（XZ_DOWN_SAMPLE_RATE_DEF），
       返回它是安全的 —— 调用方按"连上后再取"使用即可。 */
    return xz_ws_server_sample_rate();
}

/* ==========================================================================
 * 对话控制
 * ========================================================================== */
static esp_err_t send_listen(const char *state, const char *mode)
{
    if (!xz_ws_is_open()) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *sid = xz_ws_session_id();

    char json[192];
    if (mode != NULL) {
        snprintf(json, sizeof(json),
                 "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"%s\","
                 "\"mode\":\"%s\"}", sid, state, mode);
    } else {
        snprintf(json, sizeof(json),
                 "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"%s\"}",
                 sid, state);
    }
    return xz_ws_send_text(json);
}

esp_err_t xz_listen_start(void)
{
    /* mode = "manual"：本机靠**松手**结束录音，不由服务器 VAD 断句。
       本工程没有 VAD（那是 AFE 的职责，见 xiaozhi_config.h 的说明），
       manual 是唯一正确的选择。 */
    const esp_err_t ret = send_listen("start", "manual");
    ESP_LOGI(TAG, "→ listen/start (manual) -> %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t xz_listen_stop(void)
{
    const esp_err_t ret = send_listen("stop", NULL);
    ESP_LOGI(TAG, "→ listen/stop -> %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t xz_abort(void)
{
    if (!xz_ws_is_open()) {
        return ESP_ERR_INVALID_STATE;
    }
    char json[160];
    snprintf(json, sizeof(json), "{\"session_id\":\"%s\",\"type\":\"abort\"}",
             xz_ws_session_id());
    return xz_ws_send_text(json);
}

esp_err_t xz_send_audio(const uint8_t *opus, size_t len)
{
    if (!xz_ws_is_open()) {
        return ESP_ERR_INVALID_STATE;
    }
    return xz_ws_send_audio(opus, len);
}

/* ==========================================================================
 * 主流程（专用任务）
 * ========================================================================== */
static void xz_client_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "小智客户端任务启动（栈 %u 字节）",
             (unsigned)(XZ_TASK_STACK));

    for (;;) {
        /* ---- 1. 等 Wi-Fi。没有网就没有后面的一切 ---- */
        if (!wifi_sta_is_connected()) {
            xz_set_state(XZ_STATE_IDLE, "等待 Wi-Fi 连接");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        /* ---- 2. 取配置（WebSocket 地址/token）---- */
        xz_set_state(XZ_STATE_OTA, "正在向服务器取配置");
        xz_ws_cfg_t cfg;
        esp_err_t ret = xz_ota_fetch(&cfg);

        if (ret == ESP_ERR_NOT_FINISHED) {
            /* 设备未被绑定：服务器要我们先激活。
               激活码要显示在屏幕上，用户在小智 App 里输入。 */
            const char *code = xz_activation_code();
            xz_set_state(XZ_STATE_NEED_ACTIVATION,
                         "请在小智 App 输入激活码 %s", (code != NULL) ? code : "??????");

            bool activated = false;
            for (int i = 0; i < XZ_ACTIVATE_MAX_TRIES && !activated; i++) {
                xz_set_state(XZ_STATE_ACTIVATING,
                             "等待 App 确认（第 %d/%d 次）", i + 1, XZ_ACTIVATE_MAX_TRIES);
                const esp_err_t ar = xz_ota_activate();
                if (ar == ESP_OK) {
                    activated = true;
                    break;
                }
                /* 202 = 已受理、等用户操作 → 3 秒后重试；
                   其它失败 → 10 秒后重试（application.cc:257-271） */
                vTaskDelay(pdMS_TO_TICKS((ar == ESP_ERR_TIMEOUT)
                                         ? XZ_ACTIVATE_WAIT_MS
                                         : XZ_ACTIVATE_RETRY_MS));
            }

            if (!activated) {
                xz_set_state(XZ_STATE_ERROR,
                             "激活未完成（已尝试 %d 次）", XZ_ACTIVATE_MAX_TRIES);
                vTaskDelay(pdMS_TO_TICKS(30000));
            }
            continue;   /* 回到循环开头重新取配置 */
        }

        if (ret != ESP_OK) {
            xz_set_state(XZ_STATE_ERROR, "取配置失败：%s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        /* ---- 3. 建立 WebSocket 并完成 hello 握手 ---- */
        xz_set_state(XZ_STATE_CONNECTING, "正在连接服务器");
        ret = xz_ws_open(&cfg);
        if (ret != ESP_OK) {
            xz_set_state(XZ_STATE_ERROR, "连接失败：%s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        xz_set_state(XZ_STATE_READY, "已连接，可以说话");

        /* ---- 4. 保持连接。socket 断了就退出去重连 ---- */
        while (xz_ws_is_open()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        xz_ws_close();
        xz_set_state(XZ_STATE_IDLE, "连接已断开，稍后重连");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/* ==========================================================================
 * 启动
 * ========================================================================== */
esp_err_t xz_client_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;      /* 幂等 */
    }

    /* 先取一次设备标识 —— 它们要读写 NVS，放在创建任务之前做，
       这样失败能立刻返回错误（而不是在任务里静默失败）。 */
    (void)xz_device_id();
    (void)xz_client_id();

    xz_set_state(XZ_STATE_IDLE, "客户端已启动，等待连接");

    const BaseType_t ok = xTaskCreatePinnedToCore(xz_client_task, "xz_client",
                                                  XZ_TASK_STACK, NULL,
                                                  XZ_TASK_PRIO, &s_task,
                                                  XZ_TASK_CORE);
    if (ok != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "已启动。设备 ID=%s，客户端 ID=%s",
             xz_device_id(), xz_client_id());
    return ESP_OK;
}
