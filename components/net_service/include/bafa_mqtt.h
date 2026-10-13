/**
 * @file bafa_mqtt.h
 * @brief 巴法云 MQTT 服务
 *
 * 职责边界（硬性）：**所有 esp_mqtt_client_publish 调用只能出现在 mqtt_task 内。**
 * 本模块只提供「投递待发消息」的队列接口，由 mqtt_task 取出后实际发布。
 * 其它任务（sensor/ai）要上报数据，必须调用 bafa_mqtt_post_* 系列函数。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 重连指数退避参数 */
#define BAFA_BACKOFF_MIN_MS     1000
#define BAFA_BACKOFF_MAX_MS     30000   /* 验收指标：断线后 30s 内重连 */
#define BAFA_TX_QUEUE_LEN       8       /* 待发消息队列深度 */
#define BAFA_TX_PAYLOAD_MAX     512     /* 单条消息最大长度（含 '\0'） */

/** 消息投递目的主题 */
typedef enum {
    BAFA_MSG_SENSOR = 0,    /* 生态状态 JSON        → <topic_sensor>/up  */
    BAFA_MSG_ALERT,         /* 告警文本             → <topic_alert>/set  */
    BAFA_MSG_AI,            /* AI 建议              → <topic_ai>/set     */
    BAFA_MSG_STATUS,        /* 设备状态             → <topic_status>/set */
    BAFA_MSG_RAW,           /* 原样发布（调试用）                        */
    BAFA_MSG_MAX,
} bafa_msg_kind_t;

/** 投递一条待发消息到发送队列（线程安全，可从任意任务调用） */
esp_err_t bafa_mqtt_post(bafa_msg_kind_t kind, const char *payload);

/** 便捷封装：把生态数据结构序列化成 JSON 后投递 */
esp_err_t bafa_mqtt_post_status(const tank_ecosystem_t *data);

/** 投递告警 */
esp_err_t bafa_mqtt_post_alert(const char *alert);

/** 投递 AI 建议 */
esp_err_t bafa_mqtt_post_ai(const char *suggestion);

/**
 * @brief 创建发送队列并启动 MQTT 客户端（非阻塞）。
 * @note  必须由 app_main 调用一次。实际发布动作发生在 mqtt_task 中。
 */
esp_err_t bafa_mqtt_start(void);

/** @brief 当前 MQTT 是否已连接。 */
bool bafa_mqtt_is_connected(void);

/** @brief 服务是否已完成初始化（队列与客户端可用）。未就绪时 pump 会等待。 */
bool bafa_mqtt_is_ready(void);

/** @brief 把消息放入队列（供 mqtt_task 之外的任务使用） */
esp_err_t bafa_mqtt_enqueue(bafa_msg_kind_t kind, const char *payload);

/**
 * @brief 由 mqtt_task 调用：阻塞等待队列中的消息并发布。
 * @param timeout_ms 等待超时（毫秒）
 * @return ESP_OK 发布了一条；ESP_ERR_TIMEOUT 超时无消息
 */
esp_err_t bafa_mqtt_pump(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
