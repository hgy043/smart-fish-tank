/**
 * @file app_events.c
 * @brief 应用事件总线实现（封装 esp_event）
 */
#include "app_events.h"

#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_events";

/* 事件类型 -> 可读名字，仅用于日志 */
static const char *const k_event_names[EVENT_MAX] = {
    "SENSOR_UPDATED",
    "WATER_QUALITY_ABNORMAL",
    "WATER_TEMP_ABNORMAL",
    "AIR_TEMP_ABNORMAL",
    "LIGHT_ABNORMAL",
    "WATER_MUDDY",
    "FISH_ACTIVITY_ABNORMAL",
    "VOICE_QUERY",
    "MQTT_CONNECTED",
    "MQTT_DISCONNECTED",
    "AI_SUGGESTION_READY",
    "VISION_RESULT_READY",
    "WIFI_CONNECTED",
    "WIFI_DISCONNECTED",
};

/* ==========================================================================
 * 订阅表
 * ========================================================================== */
#define APP_EVENT_MAX_SUBS 16

typedef struct {
    app_event_t type;
    void      (*handler)(void *arg, const app_event_msg_t *msg);
    void       *arg;
    bool        used;
} app_sub_t;

static app_sub_t s_subs[APP_EVENT_MAX_SUBS];
static bool      s_inited = false;

/* ==========================================================================
 * 内部：esp_event 分发回调
 *
 * 注意：esp_event 的 handler 在事件循环任务上下文执行。
 *      这里只做「查表 + 调用订阅者」，耗时应由订阅者自行保证；
 *      订阅者若需耗时处理，必须把数据投递到自己的队列后立即返回。
 * ========================================================================== */
static void app_event_dispatch(void *arg,
                               esp_event_base_t base,
                               int32_t id,
                               void *event_data)
{
    (void)arg;
    (void)base;

    const app_event_msg_t *msg = (const app_event_msg_t *)event_data;
    if (msg == NULL) {
        return;
    }

    for (int i = 0; i < APP_EVENT_MAX_SUBS; i++) {
        if (s_subs[i].used && s_subs[i].type == msg->type) {
            s_subs[i].handler(s_subs[i].arg, msg);
        }
    }

    (void)id;
}

esp_err_t app_events_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "创建默认事件循环失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 只注册一次通配 handler，所有事件类型都经此分发 */
    ret = esp_event_handler_instance_register(ESP_EVENT_ANY_BASE,
                                              ESP_EVENT_ANY_ID,
                                              app_event_dispatch,
                                              NULL,
                                              NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册事件分发器失败: %s", esp_err_to_name(ret));
        return ret;
    }

    memset(s_subs, 0, sizeof(s_subs));
    s_inited = true;

    ESP_LOGI(TAG, "事件总线就绪（支持 %d 个订阅，%d 种事件）",
             APP_EVENT_MAX_SUBS, (int)EVENT_MAX);
    return ESP_OK;
}

esp_err_t app_events_subscribe(app_event_t type,
                               void (*handler)(void *arg, const app_event_msg_t *msg),
                               void *arg)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (handler == NULL || type < 0 || type >= EVENT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < APP_EVENT_MAX_SUBS; i++) {
        if (!s_subs[i].used) {
            s_subs[i].used    = true;
            s_subs[i].type    = type;
            s_subs[i].handler = handler;
            s_subs[i].arg     = arg;
            ESP_LOGD(TAG, "订阅 %s 成功 (slot %d)",
                     k_event_names[type], i);
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "订阅表已满，无法订阅 %s", k_event_names[type]);
    return ESP_ERR_NO_MEM;
}

esp_err_t app_events_publish(const app_event_msg_t *msg)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (msg == NULL || msg->type < 0 || msg->type >= EVENT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = esp_event_post(APP_EVENT_BASE,
                                   (int32_t)msg->type,
                                   msg,
                                   sizeof(*msg),
                                   /* 等待上限 100ms：禁止无限阻塞，避免发布者被事件循环拖死 */
                                   pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        /* 事件队列满时明确告警，绝不静默丢弃 */
        ESP_LOGW(TAG, "发布事件 %s 失败: %s（事件队列可能已满）",
                 k_event_names[msg->type], esp_err_to_name(ret));
    }
    return ret;
}

const char *app_event_name(app_event_t type)
{
    if (type < 0 || type >= EVENT_MAX) {
        return "INVALID";
    }
    return k_event_names[type];
}
