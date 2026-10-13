/**
 * @file wifi_sta.h
 * @brief Wi-Fi 站点服务（非阻塞、带指数退避重连）
 *
 * 设计要点：
 *   - **不阻塞启动**：原参考实现 wifi_sta_init() 会 portMAX_DELAY 等待连上，
 *     在单任务 demo 里可行，但在多任务系统里会把 app_main 挂死。本实现改为
 *     事件驱动，立刻返回，连接状态经事件总线广播。
 *   - 重连采用**指数退避**，上限受 APP_WIFI_BACKOFF_MAX_MS 约束，
 *     满足「断线后 30s 内自动重连」的验收指标。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif_ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 退避参数 */
#define APP_WIFI_BACKOFF_MIN_MS     1000    /* 首次重连间隔           */
#define APP_WIFI_BACKOFF_MAX_MS     30000   /* 退避上限（＝验收指标） */
#define APP_WIFI_MAX_RETRY_LOG      20      /* 超过后只降频打日志     */

/**
 * @brief 初始化并启动 Wi-Fi 站点连接（非阻塞，立即返回）。
 * @note  依赖 NVS 已初始化。
 */
esp_err_t wifi_sta_start(void);

/** @brief 当前是否已连接且拿到 IP。 */
bool wifi_sta_is_connected(void);

/** @brief 获取当前 IP（未连接时返回 0.0.0.0）。 */
esp_ip4_addr_t wifi_sta_get_ip(void);

/** @brief 获取当前信号强度 dBm（未连接时返回 0）。 */
int8_t wifi_sta_get_rssi(void);

/** @brief 主动断开并停止重连（用于配网切换等场景）。 */
esp_err_t wifi_sta_stop(void);

#ifdef __cplusplus
}
#endif
