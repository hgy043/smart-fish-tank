/**
 * @file app_types.h
 * @brief 全局生态数据结构
 *
 * 与 Agent.md 第 5.1 节保持一致；新增字段必须同步更新本文件与文档。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 水质 + 环境的完整快照 */
typedef struct {
    uint32_t timestamp;             /* 采样时刻，秒（单调时钟） */

    /* --- 水质（经 ADS1115 采集） --- */
    float ph;                       /* pH 值           */
    float tds_ppm;                  /* 溶解性总固体 ppm */
    float turbidity_ntu;            /* 浊度 NTU        */
    float water_temp_c;             /* 水温 ℃          */

    /* --- 环境 --- */
    float air_temp_c;               /* 空气温度 ℃      */
    float air_humidity;             /* 空气湿度 %      */
    float lux;                      /* 光照 lx         */

    /* --- 视觉 --- */
    bool  fish_detected;
    int   fish_count;
    bool  water_muddy;
    char  snapshot_path[64];

    /* --- 链路状态 --- */
    bool  wifi_connected;
    bool  mqtt_connected;
    bool  alert_active;
} tank_ecosystem_t;

/** 视觉识别结果 */
typedef struct {
    uint32_t timestamp;
    bool     fish_detected;
    int      fish_count;
    bool     water_muddy;
    char     snapshot_path[64];
} vision_result_t;

/** 链路状态快照（由 wifi_sta / bafa_mqtt 更新，UI 与上报读取）
 *
 *  注意：IP 存为 uint32_t（网络字节序，与 esp_ip4_addr_t.addr 同布局），
 *  而不是直接用 esp_ip4_addr_t——否则 app_core 会背上对 esp_netif 的依赖，
 *  而事件总线组件不应该依赖网络栈。 */
typedef struct {
    bool     wifi_connected;
    uint32_t ip_addr;       /* 0 表示 0.0.0.0 */
    int8_t   rssi_dbm;
    bool     mqtt_connected;
} net_status_t;

#ifdef __cplusplus
}
#endif
