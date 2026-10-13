/**
 * @file app_secrets.example.h
 * @brief 设备凭据模板 —— **本文件可入库，真实凭据文件不可入库**
 *
 * ============================================================================
 * 【使用方法】
 *
 *   1. 复制本文件为 app_secrets.h：
 *        cp app_secrets.example.h app_secrets.h
 *      （Windows PowerShell：Copy-Item app_secrets.example.h app_secrets.h）
 *
 *   2. 把下面的占位符改成你自己的真实凭据。
 *
 *   app_secrets.h 已在 .gitignore 中排除，**不会被提交**。
 *   本模板文件（.example.h）则应当入库，供他人知道需要哪些配置项。
 *
 * 【为什么这样拆】
 *   真实凭据（Wi-Fi 密码、巴法云私钥）一旦进入 git 历史，即使后续删除
 *   也仍可从历史中检出；若推送到远端则等同泄露。故凭据与代码分离。
 * ============================================================================
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * Wi-Fi 站点配置
 * ========================================================================== */
#define APP_WIFI_SSID           "你的Wi-Fi名称"
#define APP_WIFI_PASSWORD       "你的Wi-Fi密码"

/* 认证模式。WPA2_PSK 对 WPA/WPA2 混合热点同样有效 */
#define APP_WIFI_AUTHMODE       WIFI_AUTH_WPA2_PSK

/* ==========================================================================
 * 巴法云 MQTT 配置
 * ========================================================================== */

/* 服务器与端口（9501=普通，9503=TLS，9504=WebSocket） */
#define BAFA_MQTT_SERVER        "bemfa.com"
#define BAFA_MQTT_PORT          9501

/* 用户私钥，作为 MQTT 客户端 ID 完成鉴权（用户名/密码留空即可）
   在巴法云控制台「我的控制台」页面获取 */
#define BAFA_UID                "在此填入你的巴法云私钥(32位十六进制)"

/* ==========================================================================
 * 设备主题（设备编码）
 *
 * 巴法云主题规则：
 *   - 发布 <topic>          → 正常推送，订阅该主题的设备都能收到
 *   - 发布 <topic>/set      → 向所有订阅者推送（发布者自己不收，防自循环）
 *   - 发布 <topic>/up       → 只更新云端数据，不推送（省流量，适合高频上报）
 *
 * ⚠️ 这些主题名必须先在巴法云控制台创建，否则发布无效。
 * ========================================================================== */
#define BAFA_TOPIC_SENSOR       "fishsensor"   /* 上报完整生态状态 JSON → 用 /up */
#define BAFA_TOPIC_ALERT        "fishalert"    /* 上报告警              → 用 /set */
#define BAFA_TOPIC_AI           "fishai"       /* 上报 AI 建议          → 用 /set */
#define BAFA_TOPIC_STATUS       "fishstatus"   /* 设备在线状态          → 用 /set */
#define BAFA_TOPIC_CMD          "fishcmd"      /* 订阅控制命令          ← 订阅本主题 */

/* 订阅用主题（接收手机下发的控制命令） */
#define BAFA_SUBSCRIBE_TOPIC    BAFA_TOPIC_CMD

#ifdef __cplusplus
}
#endif
