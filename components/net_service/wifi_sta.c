/**
 * @file wifi_sta.c
 * @brief Wi-Fi 站点服务实现
 *
 * 与参考实现 (06-tcp_client_bafa_mqtt/main/wifi_config.c) 的差异：
 *   - 改为**非阻塞**：原实现用 xEventGroupWaitBits(portMAX_DELAY) 在
 *     app_main 里死等连上，多任务系统下会拖死启动流程。本实现立刻返回。
 *   - 重连改为**软定时器驱动的指数退避**，而不是在事件回调里循环调用
 *     esp_wifi_connect()（后者会在回调上下文里制造忙等）。
 *   - 状态变化经应用事件总线广播，UI / MQTT / 音频各自订阅。
 */
#include "wifi_sta.h"

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/inet.h"

#include "app_secrets.h"
#include "app_events.h"

static const char *TAG = "wifi_sta";

/* 失败即返回，并打印出错位置。不可用 ESP_ERROR_CHECK——那会 abort 整个系统，
   网络连不上不该导致设备重启。 */
#define CHECK_RET(expr, what)                                              \
    do {                                                                   \
        esp_err_t err_ = (expr);                                           \
        if (err_ != ESP_OK) {                                              \
            ESP_LOGE(TAG, "%s 失败: %s", (what), esp_err_to_name(err_));    \
            return err_;                                                   \
        }                                                                  \
    } while (0)

#define WIFI_BIT_CONNECTED  BIT0
#define WIFI_BIT_GOT_IP     BIT1

static EventGroupHandle_t s_wifi_events  = NULL;
static esp_netif_t       *s_sta_netif    = NULL;
static esp_timer_handle_t s_retry_timer  = NULL;

static uint32_t s_backoff_ms       = APP_WIFI_BACKOFF_MIN_MS;
static uint32_t s_retry_count      = 0;
static bool     s_started          = false;
static bool     s_stop_requested   = false;
static esp_ip4_addr_t s_ip         = { 0 };
static int8_t   s_rssi             = 0;

/* ==========================================================================
 * 事件广播
 * ========================================================================== */
static void publish_net_event(app_event_t type, bool connected)
{
    app_event_msg_t msg = { 0 };
    msg.type      = type;
    msg.data.flag = connected;

    esp_err_t ret = app_events_publish(&msg);
    if (ret != ESP_OK) {
        /* 事件队列满不该影响网络主流程，只记录 */
        ESP_LOGW(TAG, "网络事件 %d 广播失败: %s", (int)type, esp_err_to_name(ret));
    }
}

/* ==========================================================================
 * 重连退避
 * ========================================================================== */
static void retry_timer_cb(void *arg)
{
    (void)arg;

    if (s_stop_requested) {
        return;
    }
    if (xEventGroupGetBits(s_wifi_events) & WIFI_BIT_GOT_IP) {
        return;     /* 已连上，无需重试 */
    }

    ESP_LOGI(TAG, "第 %"PRIu32" 次重连尝试（退避 %"PRIu32" ms）",
             s_retry_count + 1, s_backoff_ms);

    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect 失败: %s", esp_err_to_name(ret));
    }

    /* 指数退避：翻倍，上限 APP_WIFI_BACKOFF_MAX_MS */
    s_retry_count++;
    s_backoff_ms *= 2;
    if (s_backoff_ms > APP_WIFI_BACKOFF_MAX_MS) {
        s_backoff_ms = APP_WIFI_BACKOFF_MAX_MS;
    }
}

static void schedule_retry(void)
{
    if (s_retry_timer == NULL || s_stop_requested) {
        return;
    }
    esp_timer_stop(s_retry_timer);      /* 允许重入：先停掉未触发的 */
    esp_err_t ret = esp_timer_start_once(s_retry_timer, (uint64_t)s_backoff_ms * 1000ULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动重连定时器失败: %s", esp_err_to_name(ret));
    }
}

/* ==========================================================================
 * Wi-Fi / IP 事件处理
 *
 * 注意：本回调运行在系统事件循环任务上下文，禁止阻塞、禁止耗时操作。
 * ========================================================================== */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "开始连接 SSID: %s", APP_WIFI_SSID);
            if (!s_stop_requested) {
                esp_wifi_connect();
            }
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "已连上 AP，等待分配 IP");
            xEventGroupSetBits(s_wifi_events, WIFI_BIT_CONNECTED);
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *d =
                (const wifi_event_sta_disconnected_t *)event_data;

            xEventGroupClearBits(s_wifi_events, WIFI_BIT_CONNECTED | WIFI_BIT_GOT_IP);
            s_ip.addr = 0;
            s_rssi    = 0;

            ESP_LOGW(TAG, "连接断开 (reason=%d)，%.1f 秒后重连",
                     d ? d->reason : -1, (double)s_backoff_ms / 1000.0);

            publish_net_event(EVENT_WIFI_DISCONNECTED, false);
            schedule_retry();
            break;
        }

        default:
            break;
        }
        return;
    }

    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)event_data;
        s_ip = ev->ip_info.ip;

        /* 连接成功：重置退避，避免下次断线继承过长的间隔 */
        s_backoff_ms  = APP_WIFI_BACKOFF_MIN_MS;
        s_retry_count = 0;

        xEventGroupSetBits(s_wifi_events, WIFI_BIT_GOT_IP);
        ESP_LOGI(TAG, "获取到 IP: " IPSTR "  网关: " IPSTR,
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));

        publish_net_event(EVENT_WIFI_CONNECTED, true);
    }
}

/* ==========================================================================
 * 对外接口
 * ========================================================================== */
esp_err_t wifi_sta_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        ESP_LOGE(TAG, "创建事件组失败（内存不足）");
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = retry_timer_cb,
        .arg      = NULL,
        .name     = "wifi_retry",
    };
    esp_err_t ret = esp_timer_create(&timer_args, &s_retry_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建重连定时器失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* esp_netif_init 与 esp_event_loop_create_default 都可能已被调用过。
       app_events_init() 已经创建了默认事件循环，因此后者必然返回
       ESP_ERR_INVALID_STATE —— 那是幂等成功，不是错误。 */
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "创建默认事件循环失败: %s", esp_err_to_name(ret));
        return ret;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "创建默认 STA 网卡失败");
        return ESP_FAIL;
    }

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    CHECK_RET(esp_wifi_init(&init_cfg), "esp_wifi_init");
    CHECK_RET(esp_event_handler_instance_register(
                  WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL),
              "注册 WIFI_EVENT 处理器");
    CHECK_RET(esp_event_handler_instance_register(
                  IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL),
              "注册 IP_EVENT 处理器");

    wifi_config_t wifi_cfg = { 0 };
    /* 用 strncpy 而非 strcpy：结构体字段长度固定，避免越界 */
    strncpy((char *)wifi_cfg.sta.ssid, APP_WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, APP_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = APP_WIFI_AUTHMODE;

    CHECK_RET(esp_wifi_set_mode(WIFI_MODE_STA), "设置 Wi-Fi 模式");
    CHECK_RET(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), "设置 Wi-Fi 配置");
    CHECK_RET(esp_wifi_start(), "启动 Wi-Fi");

    s_started        = true;
    s_stop_requested = false;

    ESP_LOGI(TAG, "Wi-Fi 已启动（非阻塞），SSID=%s", APP_WIFI_SSID);
    return ESP_OK;
}

bool wifi_sta_is_connected(void)
{
    if (s_wifi_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_wifi_events) & WIFI_BIT_GOT_IP) != 0;
}

esp_ip4_addr_t wifi_sta_get_ip(void)
{
    return s_ip;
}

int8_t wifi_sta_get_rssi(void)
{
    if (!wifi_sta_is_connected()) {
        return 0;
    }
    wifi_ap_record_t ap = { 0 };
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_rssi = ap.rssi;
    }
    return s_rssi;
}

esp_err_t wifi_sta_stop(void)
{
    if (!s_started) {
        return ESP_OK;
    }
    s_stop_requested = true;
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
    esp_err_t ret = esp_wifi_disconnect();
    ESP_LOGI(TAG, "Wi-Fi 已请求断开: %s", esp_err_to_name(ret));
    return ret;
}
