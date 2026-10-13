/**
 * @file app_events.h
 * @brief 应用事件定义与事件总线接口
 *
 * 设计约束（Agent.md 第 6 节）：
 *   - 跨任务通信一律走事件总线 / 队列，禁止跨任务直接操作外设或 LVGL。
 *   - 事件负载必须是小而可拷贝的值类型，禁止在事件里传堆指针。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 事件基 */
#define APP_EVENT_BASE  "APP"

/** 事件类型 */
typedef enum {
    EVENT_SENSOR_UPDATED = 0,
    EVENT_WATER_QUALITY_ABNORMAL,
    EVENT_WATER_TEMP_ABNORMAL,
    EVENT_AIR_TEMP_ABNORMAL,
    EVENT_LIGHT_ABNORMAL,
    EVENT_WATER_MUDDY,
    EVENT_FISH_ACTIVITY_ABNORMAL,
    EVENT_VOICE_QUERY,
    EVENT_MQTT_CONNECTED,
    EVENT_MQTT_DISCONNECTED,
    EVENT_AI_SUGGESTION_READY,
    EVENT_VISION_RESULT_READY,
    EVENT_WIFI_CONNECTED,
    EVENT_WIFI_DISCONNECTED,
    EVENT_MAX,
} app_event_t;

/** 统一事件负载：用 union 承载，保持定长可拷贝 */
typedef struct {
    app_event_t type;
    union {
        tank_ecosystem_t sensor;
        vision_result_t  vision;
        bool             flag;
    } data;
} app_event_msg_t;

/**
 * @brief 初始化应用事件总线（底层使用 esp_event 默认循环）。
 */
esp_err_t app_events_init(void);

/**
 * @brief 发布一个已构造好的事件消息（线程安全，可从任意任务调用）。
 */
esp_err_t app_events_publish(const app_event_msg_t *msg);

/**
 * @brief 订阅指定事件类型。
 * @param type     事件类型
 * @param handler  处理函数
 * @param arg      用户参数
 */
esp_err_t app_events_subscribe(app_event_t type,
                               void (*handler)(void *arg, const app_event_msg_t *msg),
                               void *arg);

#ifdef __cplusplus
}
#endif
