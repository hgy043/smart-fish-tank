/**
 * @file net_time.c
 * @brief 系统对时实现（SNTP，P6-c）
 *
 * 用 IDF 的 `esp_netif_sntp`（lwIP SNTP 的封装）。手写 socket 发 NTP 包也能做，
 * 但那样要自己处理重传、多服务器、时间跳变的通知 —— 这些都是现成的。
 */
#include "net_time.h"

#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"

#include "wifi_sta.h"

static const char *TAG = "time";

/* ==========================================================================
 * 参数
 * ========================================================================== */

/** NTP 服务器。选国内的 —— 默认的 pool.ntp.org 在国内解析慢、丢包高，
 *  而 SNTP 首包一旦超时就要退避重试，对时会被推迟好几分钟。 */
#define NET_TIME_NTP_SERVER     "ntp.aliyun.com"

/** 时区。**POSIX 的 TZ 串符号是反的**：UTC+8 要写 "CST-8"，
 *  含义是"本地时间 = UTC - (-8)"。
 *  ⚠️ 写成 "CST+8" 会得到 UTC-8 —— 时间戳差 16 小时，而文件名看起来
 *     仍然是"一个正常的日期时间"，**从文件列表上根本看不出错**。
 *     出处：IDF 自带的 `examples/protocols/sntp` 里中国时区就是这一行。 */
#define NET_TIME_TZ             "CST-8"

#define NET_TIME_WIFI_WAIT_MS   60000   /* 等 Wi-Fi 的上限 */
#define NET_TIME_SYNC_WAIT_MS   15000   /* 等首次同步的上限 */

#define NET_TIME_TASK_STACK     3072
#define NET_TIME_TASK_PRIO      3

/* ==========================================================================
 * 实现
 * ========================================================================== */
static volatile bool s_synced = false;

bool net_time_is_synced(void)
{
    return s_synced;
}

/** SNTP 每次成功同步都会回调这里（**不只是第一次**）。
 *  用回调而不是只在任务里判一次：lwIP 的 SNTP 在首包失败后会退避重试，
 *  真正的同步可能发生在任务已经退出之后。把它挂在回调上，
 *  无论什么时候同步成功都会留下一条明确的日志。 */
static void on_time_synced(struct timeval *tv)
{
    (void)tv;
    s_synced = true;

    const time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    ESP_LOGI(TAG, "对时成功：%04d-%02d-%02d %02d:%02d:%02d（%s）"
                  " —— 照片文件名从此用本地时间",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, NET_TIME_NTP_SERVER);
}

/** 一次性任务：等 Wi-Fi → 起 SNTP → 有界地等首次同步 → 退场。 */
static void sntp_task(void *arg)
{
    (void)arg;

    /* 先等 Wi-Fi 真连上。网络没通时起 SNTP 只会白试一轮，
       而 lwIP 的重试次数是有限的 —— 白白耗掉几次，可能就轮不到真正的机会了。 */
    const int wait_s = NET_TIME_WIFI_WAIT_MS / 1000;
    for (int i = 0; i < wait_s && !wifi_sta_is_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!wifi_sta_is_connected()) {
        ESP_LOGW(TAG, "等 Wi-Fi 超时（%d 秒），本次不对时 —— "
                      "照片文件名会用 boot_<开机秒数>", wait_s);
        vTaskDelete(NULL);
        return;
    }

    /* 时区必须在**读时间之前**设好：localtime_r 靠它决定偏移。
       放在这里而不是 net_time_sync_start() 里，是为了让这个任务
       连带它的前提（TZ 已设）一起自洽 —— 谁读时间都能拿到本地时间。 */
    setenv("TZ", NET_TIME_TZ, 1);
    tzset();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NET_TIME_NTP_SERVER);
    /* 要**阶跃**而不是平滑同步：文件名需要的是一个确定的绝对时刻。
       平滑同步会把时钟慢慢拨过去，那几分钟里的时间戳仍是错的。 */
    cfg.smooth_sync = false;
    cfg.wait_for_sync = true;       /* 建内部信号量，sync_wait 才有得等 */
    cfg.sync_cb = on_time_synced;

    const esp_err_t iret = esp_netif_sntp_init(&cfg);
    if (iret != ESP_OK) {
        ESP_LOGE(TAG, "SNTP 启动失败：%s —— 照片文件名会用 boot_<开机秒数>",
                 esp_err_to_name(iret));
        vTaskDelete(NULL);
        return;
    }

    /* 有界等一次。超时**不算失败** —— SNTP 客户端已在后台运行，
       真同步上了 on_time_synced() 会打日志。这里只是不想让任务一直挂着。 */
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(NET_TIME_SYNC_WAIT_MS)) != ESP_OK) {
        ESP_LOGW(TAG, "%d 秒内没等到首次同步 —— 后台仍在重试；"
                      "若始终没同步上，照片文件名会退回 boot_<开机秒数>",
                 NET_TIME_SYNC_WAIT_MS / 1000);
    }

    vTaskDelete(NULL);
}

void net_time_sync_start(void)
{
    static bool started = false;
    if (started) {
        return;                     /* 幂等 */
    }
    started = true;

    if (xTaskCreate(sntp_task, "sntp", NET_TIME_TASK_STACK, NULL,
                    NET_TIME_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "对时任务创建失败 —— 照片文件名会用 boot_<开机秒数>");
        started = false;
    }
}
