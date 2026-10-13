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

    /* --- 水质（经 ADS1115 采集；P1 未接入前由模拟数据填充） --- */
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

    /* --- 告警 ---
     * alert_mask：每项一位，见下方 ECO_ALERT_*。**界面靠它决定哪张卡片变红**。
     * alert_active：= (alert_mask != 0)，保留给"有没有告警"这类粗判用途
     *               （MQTT 上报、将来的语音播报）。
     *
     * ⚠️ 2026-10-13：`alert_active` 此前被 `eco_refresh_links()` 里一句
     *    `s_eco.alert_active = false;` 每次调用都清零，而该函数被 mqtt_task
     *    每 10 秒调一次 —— 告警活不过 10 秒。那行已删除，现在**只有一个写者**：
     *    传感器采样处（按阈值计算）。凡是要改 alert_* 的代码，请先确认这一点。 */
    uint32_t alert_mask;
    bool  alert_active;
} tank_ecosystem_t;

/* ==========================================================================
 * 告警位定义 —— 对应 tank_ecosystem_t.alert_mask
 *
 * 【为什么用位掩码而不是一个 bool】
 * 界面上需要知道**是哪一项**超了，才能只把那条卡片标红。一个全局 bool 只能
 * 让整页变红，对用户没有信息量（"有东西不对"但不知道是什么）。
 *
 * 【阈值不在这里】
 * 这里是 app_core（共享数据模型），**不能**依赖 main/app_config.h ——
 * 组件之间不互相依赖是本工程的既有分层。故阈值留在 main 侧，
 * 由采样任务算好后把结果按位填进 alert_mask。
 *
 * 【哪一项对应哪张卡片】
 * 见 ui_lvgl/ui_dashboard.c 的 k_cards[].alert_bit；
 * ECO_ALERT_TDS **没有对应卡片**（鱼缸页只放六张卡），但仍会在 MQTT 上报里体现。
 * ========================================================================== */
#define ECO_ALERT_PH         (1u << 0)  /* pH 越界            */
#define ECO_ALERT_WATER_TMP  (1u << 1)  /* 水温越界（卡片）    */
#define ECO_ALERT_AIR_TMP    (1u << 2)  /* 气温越界（卡片）    */
#define ECO_ALERT_HUMIDITY   (1u << 3)  /* 湿度越界（卡片）    */
#define ECO_ALERT_LUX        (1u << 4)  /* 光照越界（卡片）    */
#define ECO_ALERT_TURBIDITY  (1u << 5)  /* 浊度越界（卡片）    */
#define ECO_ALERT_TDS        (1u << 6)  /* TDS 越界（无卡片）  */

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
