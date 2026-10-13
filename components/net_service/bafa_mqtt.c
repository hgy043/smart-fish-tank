/**
 * @file bafa_mqtt.c
 * @brief 巴法云 MQTT 服务实现
 *
 * 架构约束：本模块**不在事件回调里发消息**，也不允许其它任务直接调用
 * esp_mqtt_client_publish()。所有上报都经发送队列，由 mqtt_task 调用
 * bafa_mqtt_pump() 取出后发布——这样串口日志、QoS、失败重试都集中在一处。
 *
 * 重连策略：关闭 ESP-IDF 内建自动重连，改由本模块的事件回调驱动指数退避。
 * 原因见 s_bafa_event_handler 中 MQTT_EVENT_DISCONNECTED 分支的注释。
 */
#include "bafa_mqtt.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"

#include "app_secrets.h"
#include "app_events.h"
#include "wifi_sta.h"

static const char *TAG = "bafa_mqtt";

/* ==========================================================================
 * 内部状态
 * ========================================================================== */
typedef struct {
    bafa_msg_kind_t kind;
    char            payload[BAFA_TX_PAYLOAD_MAX];
} bafa_tx_msg_t;

static QueueHandle_t             s_tx_queue    = NULL;
static esp_mqtt_client_handle_t  s_client      = NULL;
static esp_timer_handle_t        s_backoff_timer = NULL;

static volatile bool s_connected = false;
static volatile bool s_ready     = false;   /* 队列与客户端是否已初始化完成 */
static uint32_t      s_backoff_ms = BAFA_BACKOFF_MIN_MS;

/* 主题名映射：索引与 bafa_msg_kind_t 对齐 */
static const char *const k_topics[BAFA_MSG_MAX] = {
    [BAFA_MSG_SENSOR] = BAFA_TOPIC_SENSOR,
    [BAFA_MSG_ALERT]  = BAFA_TOPIC_ALERT,
    [BAFA_MSG_AI]     = BAFA_TOPIC_AI,
    [BAFA_MSG_STATUS] = BAFA_TOPIC_STATUS,
    [BAFA_MSG_RAW]    = BAFA_TOPIC_STATUS,   /* 调试用，落到 status 主题 */
};

/* 是否需要 /set 或 /up 后缀 */
static const bool k_use_set_suffix[BAFA_MSG_MAX] = {
    [BAFA_MSG_SENSOR] = false,   /* 高频上报，用 /up 只更新云端不推送，省流量 */
    [BAFA_MSG_ALERT]  = true,
    [BAFA_MSG_AI]     = true,
    [BAFA_MSG_STATUS] = true,
    [BAFA_MSG_RAW]    = false,
};

/* ==========================================================================
 * 状态广播
 * ========================================================================== */
static void publish_mqtt_event(app_event_t type, bool connected)
{
    app_event_msg_t msg = { 0 };
    msg.type      = type;
    msg.data.flag = connected;

    esp_err_t ret = app_events_publish(&msg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MQTT 状态事件 %d 广播失败: %s", (int)type, esp_err_to_name(ret));
    }
}

/* ==========================================================================
 * 主题组装
 * ========================================================================== */
static void build_topic(bafa_msg_kind_t kind, char *out, size_t out_len)
{
    if (kind >= BAFA_MSG_MAX) {
        kind = BAFA_MSG_STATUS;
    }
    if (k_use_set_suffix[kind]) {
        snprintf(out, out_len, "%s/set", k_topics[kind]);
    } else if (kind == BAFA_MSG_SENSOR) {
        snprintf(out, out_len, "%s/up", k_topics[kind]);
    } else {
        snprintf(out, out_len, "%s", k_topics[kind]);
    }
}

/* ==========================================================================
 * 退避重连
 * ========================================================================== */
static void backoff_timer_cb(void *arg)
{
    (void)arg;
    if (s_client == NULL) {
        return;
    }
    ESP_LOGI(TAG, "重连巴法云（退避 %"PRIu32" ms）", s_backoff_ms);
    esp_err_t ret = esp_mqtt_client_reconnect(s_client);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "发起重连失败: %s", esp_err_to_name(ret));
    }
    s_backoff_ms *= 2;
    if (s_backoff_ms > BAFA_BACKOFF_MAX_MS) {
        s_backoff_ms = BAFA_BACKOFF_MAX_MS;
    }
}

static void schedule_backoff(void)
{
    if (s_backoff_timer == NULL) {
        return;
    }
    esp_timer_stop(s_backoff_timer);
    esp_err_t ret = esp_timer_start_once(s_backoff_timer, (uint64_t)s_backoff_ms * 1000ULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动退避定时器失败: %s", esp_err_to_name(ret));
    }
}

/* ==========================================================================
 * 下行命令处理（收到 <topic>/set 的消息）
 * ========================================================================== */
static void handle_command(const char *data, int data_len)
{
    /* 先按长度安全比较，避免 data 非 '\0' 结尾时越界 */
    if (data_len == 2 && strncmp(data, "on", 2) == 0) {
        ESP_LOGI(TAG, "收到命令: on");
    } else if (data_len == 3 && strncmp(data, "off", 3) == 0) {
        ESP_LOGI(TAG, "收到命令: off");
    } else {
        ESP_LOGI(TAG, "收到未识别命令: %.*s", data_len, data);
        return;
    }

    /* P0 阶段仅记录；P4（音频）/P2（UI）/继电器控制在此挂接。
       解析出的命令应经事件总线投递，不要在事件回调里直接操作外设。 */
}

/* ==========================================================================
 * MQTT 事件处理
 *
 * 运行在 MQTT 客户端任务上下文。禁止阻塞、禁止耗时操作。
 * ========================================================================== */
static void bafa_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;

    const esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    if (event == NULL) {
        return;
    }

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        s_connected  = true;
        s_backoff_ms = BAFA_BACKOFF_MIN_MS;     /* 连上就重置退避 */

        ESP_LOGI(TAG, "已连接 %s:%d", BAFA_MQTT_SERVER, BAFA_MQTT_PORT);

        int msg_id = esp_mqtt_client_subscribe(event->client, BAFA_SUBSCRIBE_TOPIC, 0);
        ESP_LOGI(TAG, "订阅 %s, msg_id=%d", BAFA_SUBSCRIBE_TOPIC, msg_id);

        /* 上线通告 */
        const char *online = "{\"state\":\"online\"}";
        esp_mqtt_client_publish(event->client, BAFA_TOPIC_STATUS "/set",
                                online, 0, 0, 0);

        publish_mqtt_event(EVENT_MQTT_CONNECTED, true);
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "连接断开，%.1f 秒后重连", (double)s_backoff_ms / 1000.0);
        /* ESP-IDF 内建的 reconnect_timeout_ms 是指数退避且只在初始连接阶段
           生效，断线后的重连间隔不可控（实测会漂移到数分钟），无法满足
           「断线后 30s 内重连」的验收指标。故显式关闭自动重连，
           改由本模块的定时器控制间隔。 */
        publish_mqtt_event(EVENT_MQTT_DISCONNECTED, false);
        schedule_backoff();
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "收到下行: topic=%.*s len=%d",
                 event->topic_len, event->topic, event->data_len);
        /* 只在是本设备命令主题时才解析 */
        if (event->topic_len == (int)strlen(BAFA_SUBSCRIBE_TOPIC) &&
            strncmp(event->topic, BAFA_SUBSCRIBE_TOPIC, event->topic_len) == 0) {
            handle_command(event->data, event->data_len);
        }
        break;

    case MQTT_EVENT_ERROR:
        if (event->error_handle != NULL &&
            event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "TCP 传输错误, errno=%d, tls=%d",
                     event->error_handle->esp_transport_sock_errno,
                     (int)event->error_handle->esp_tls_last_esp_err);
        } else {
            ESP_LOGE(TAG, "MQTT 错误事件");
        }
        break;

    default:
        break;
    }
}

/* ==========================================================================
 * 对外接口
 * ========================================================================== */
esp_err_t bafa_mqtt_start(void)
{
    if (s_client != NULL) {
        return ESP_OK;
    }

    s_tx_queue = xQueueCreate(BAFA_TX_QUEUE_LEN, sizeof(bafa_tx_msg_t));
    if (s_tx_queue == NULL) {
        ESP_LOGE(TAG, "创建发送队列失败（内存不足）");
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = backoff_timer_cb,
        .arg      = NULL,
        .name     = "bafa_backoff",
    };
    esp_err_t ret = esp_timer_create(&timer_args, &s_backoff_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建退避定时器失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 客户端 ID = 巴法云用户私钥；用户名/密码留空 */
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .hostname = BAFA_MQTT_SERVER,
                .port     = BAFA_MQTT_PORT,
                .transport = MQTT_TRANSPORT_OVER_TCP,
            },
        },
        .credentials = {
            .client_id = BAFA_UID,
        },
        .session = {
            .keepalive = 60,
        },
        .network = {
            /* 显式关闭内建自动重连，由本模块的退避定时器接管 */
            .disable_auto_reconnect = true,
            .reconnect_timeout_ms   = BAFA_BACKOFF_MIN_MS,
            .timeout_ms             = 10000,
        },
        /* 这两项的 Kconfig 版本（CONFIG_MQTT_BUFFER_SIZE / CONFIG_MQTT_TASK_STACK_SIZE）
           依赖 CONFIG_MQTT_USE_CUSTOM_CONFIG，默认关闭时改 sdkconfig 是无效的，
           必须在运行时结构体里设置。 */
        .buffer = {
            .size = 2048,   /* 收发缓冲：上报 JSON 上限 512 字节，留足余量 */
        },
        .task = {
            .stack_size = 8192,     /* MQTT 客户端任务栈 */
        },
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "创建 MQTT 客户端失败");
        return ESP_FAIL;
    }

    ret = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                         bafa_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 MQTT 事件处理器失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_mqtt_client_start(s_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动 MQTT 客户端失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "MQTT 客户端已启动（异步连接 %s:%d）", BAFA_MQTT_SERVER, BAFA_MQTT_PORT);

    /* 置位必须放在最后：调用方看到 s_ready 为真即意味着队列与客户端都可用 */
    s_ready = true;
    return ESP_OK;
}

bool bafa_mqtt_is_ready(void)
{
    return s_ready;
}

bool bafa_mqtt_is_connected(void)
{
    return s_connected;
}

esp_err_t bafa_mqtt_enqueue(bafa_msg_kind_t kind, const char *payload)
{
    if (s_tx_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bafa_tx_msg_t msg = { .kind = kind };
    /* snprintf 保证截断而非溢出 */
    snprintf(msg.payload, sizeof(msg.payload), "%s", payload);

    /* 队列满时不阻塞调用者（可能是高优先级传感器任务）——直接丢弃并告警。
       频上报场景下丢一帧可接受，阻塞传感器任务则会造成整机节拍失真。 */
    if (xQueueSend(s_tx_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "发送队列已满，丢弃一条 (kind=%d)", (int)kind);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t bafa_mqtt_pump(uint32_t timeout_ms)
{
    /* 服务尚未初始化（app_main 可能还没跑到 bafa_mqtt_start）。
       此时**必须仍然吃满超时**：如果立刻返回，调用方 mqtt_task 会全速空转，
       实测表现为每 10ms 刷一条 "pump 返回 ESP_ERR_INVALID_STATE"，
       既淹没有效日志，又白烧 CPU。 */
    if (!s_ready || s_tx_queue == NULL) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return ESP_ERR_INVALID_STATE;
    }

    bafa_tx_msg_t msg;
    if (xQueueReceive(s_tx_queue, &msg, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_connected || s_client == NULL) {
        /* 未连接：把消息**放回队列**而不是丢弃。
           P0 阶段的周期上报是幂等的，保留一条待发消息有意义；
           这里不重试投递（避免与其它生产者竞争队列顺序），
           把消息塞回尾部即可，下次 pump 会再取到。 */
        if (xQueueSendToFront(s_tx_queue, &msg, 0) != pdTRUE) {
            ESP_LOGW(TAG, "未连接且队列已满，丢弃一条 (kind=%d)", (int)msg.kind);
        }
        return ESP_ERR_INVALID_STATE;
    }

    char topic[64];
    build_topic(msg.kind, topic, sizeof(topic));

    /* QoS 0：巴法云不支持 QoS 2；QoS 0 足够且不会因重发造成数据重复 */
    int msg_id = esp_mqtt_client_publish(s_client, topic, msg.payload, 0, 0, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "发布失败 topic=%s", topic);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "已发布 → %s : %s", topic, msg.payload);
    return ESP_OK;
}

/* ==========================================================================
 * 便捷封装
 * ========================================================================== */
esp_err_t bafa_mqtt_post_status(const tank_ecosystem_t *data)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char buf[BAFA_TX_PAYLOAD_MAX];
    /* 字段名与 Agent.md 第 9 节的协议约定保持一致 */
    int n = snprintf(buf, sizeof(buf),
        "{\"ts\":%"PRIu32","
        "\"water\":{\"ph\":%.2f,\"tds\":%.1f,\"turbidity\":%.2f,\"temp\":%.2f},"
        "\"env\":{\"air_temp\":%.2f,\"hum\":%.1f,\"lux\":%.1f},"
        "\"vision\":{\"fish_count\":%d,\"muddy\":%s},"
        "\"status\":{\"wifi\":%s,\"mqtt\":%s,\"alert\":%s}}",
        data->timestamp,
        data->ph, data->tds_ppm, data->turbidity_ntu, data->water_temp_c,
        data->air_temp_c, data->air_humidity, data->lux,
        data->fish_count, data->water_muddy ? "true" : "false",
        data->wifi_connected ? "true" : "false",
        data->mqtt_connected ? "true" : "false",
        data->alert_active ? "true" : "false");

    if (n < 0 || n >= (int)sizeof(buf)) {
        ESP_LOGE(TAG, "状态 JSON 被截断（需要 %d 字节，缓冲 %u）", n, (unsigned)sizeof(buf));
        return ESP_ERR_INVALID_SIZE;
    }

    return bafa_mqtt_enqueue(BAFA_MSG_SENSOR, buf);
}

esp_err_t bafa_mqtt_post_alert(const char *alert)
{
    if (alert == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char buf[BAFA_TX_PAYLOAD_MAX];
    snprintf(buf, sizeof(buf), "{\"alert\":\"%s\"}", alert);
    return bafa_mqtt_enqueue(BAFA_MSG_ALERT, buf);
}

esp_err_t bafa_mqtt_post_ai(const char *suggestion)
{
    if (suggestion == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char buf[BAFA_TX_PAYLOAD_MAX];
    snprintf(buf, sizeof(buf), "{\"ai\":\"%s\"}", suggestion);
    return bafa_mqtt_enqueue(BAFA_MSG_AI, buf);
}

esp_err_t bafa_mqtt_post(bafa_msg_kind_t kind, const char *payload)
{
    return bafa_mqtt_enqueue(kind, payload);
}
