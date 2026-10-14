/**
 * @file app_main.c
 * @brief 应用入口 — P0 基础框架阶段
 *
 * P0 验收目标：**可编译、可联网**。
 *   可编译 → 已达成（全清重建零错误零警告）
 *   可联网 → 本次接入：Wi-Fi 连接 + 巴法云 MQTT 上下行
 *
 * 本文件已建立的线程边界（后续阶段不得破坏）：
 *   - LVGL 的所有调用只能出现在 ui_task 内（P2 接入）。
 *   - MQTT 的所有发布只能出现在 mqtt_task 内（经 bafa_mqtt_pump）。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "lwip/inet.h"
#include "nvs_flash.h"

#include "gec_board.h"
#include "lcd_display.h"
#include "audio_service.h"
#include "ui_lvgl.h"
#include "font_service.h"   /* 中文字体：普惠体 + Noto 回退（见该头文件顶部） */
#include "camera_service.h"
#include "sd_service.h"
#include "ui_lvgl_config.h"     /* 只为断言缩略图参数与 sd_service 一致 */
#include "xiaozhi_client.h"
#include "xiaozhi_config.h"
#include "opus_codec.h"
#include "app_config.h"
#include "app_events.h"
#include "app_types.h"
#include "app_secrets.h"
#include "wifi_sta.h"
#include "bafa_mqtt.h"
#include "net_time.h"

static const char *TAG = "app_main";

/* ==========================================================================
 * 全局生态状态（唯一实例）
 *
 * 由 ui_task 与 mqtt_task 共享读取。写操作只发生在：
 *   · sensor_task        —— 传感器数值 + alert_mask（见 eco_sensor_sample）
 *   · eco_refresh_links  —— 链路状态（wifi/mqtt/timestamp），**不碰告警**
 * 用互斥锁保护，避免读到「写了一半」的结构体。
 * ========================================================================== */
static tank_ecosystem_t s_eco;
static SemaphoreHandle_t s_eco_mutex = NULL;

static esp_err_t eco_init(void)
{
    s_eco_mutex = xSemaphoreCreateMutex();
    if (s_eco_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_eco, 0, sizeof(s_eco));

    /* 初值取各自游走量程的中点 —— 与 sensor_task 的随机游走衔接自然，
       不会出现"开机瞬间从某个数猛跳到量程里"的观感。 */
    s_eco.ph             = (SIM_PH_MIN    + SIM_PH_MAX)    * 0.5f;
    s_eco.water_temp_c   = (SIM_WTEMP_MIN + SIM_WTEMP_MAX) * 0.5f;
    s_eco.air_temp_c     = (SIM_ATEMP_MIN + SIM_ATEMP_MAX) * 0.5f;
    s_eco.air_humidity   = (SIM_HUM_MIN   + SIM_HUM_MAX)   * 0.5f;
    s_eco.lux            = (SIM_LUX_MIN   + SIM_LUX_MAX)   * 0.5f;
    s_eco.turbidity_ntu  = (SIM_TURB_MIN  + SIM_TURB_MAX)  * 0.5f;
    s_eco.tds_ppm        = (SIM_TDS_MIN   + SIM_TDS_MAX)   * 0.5f;
    return ESP_OK;
}

static void eco_snapshot(tank_ecosystem_t *out)
{
    if (out == NULL || s_eco_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_eco_mutex, portMAX_DELAY);
    *out = s_eco;
    xSemaphoreGive(s_eco_mutex);
}

/**
 * @brief MCP 工具的数据源（注册给 xiaozhi_client，见其头文件里的说明）。
 *
 * xz_mcp_handle() 在**小智服务器问「鱼缸现在怎么样」**时调用这里，
 * 拿到的结构体由 xiaozhi_mcp.c 摊成 JSON 交给 LLM。
 *
 * ⚠️ 运行在 **esp_websocket_client 自己的任务**上，那个任务的栈不归我们管。
 *    这里只做「加锁 + 结构体拷贝」（微秒级），不阻塞 —— 这是它能被这样调用的前提。
 *    将来若往 MCP 工具里加耗时的东西（比如拍照），**必须先把执行挪到别的任务**，
 *    否则会堵死 WebSocket 的接收。P7 已经因为在这条任务上解码 Opus 撞过一次
 *    整机复位（见 s_dl_q 上方那段说明），同一个坑不要踩第二次。
 *
 * @return false = 传感器任务还没初始化，没有可信数据。
 *         **不要**在这种情况下返回 true 并给一个零值快照 —— 那会让 AI 笃定地
 *         说"水温 0 度"，比说"读不到"糟得多。
 */
static bool tank_status_provider(tank_ecosystem_t *out)
{
    if (out == NULL || s_eco_mutex == NULL) {
        return false;
    }
    eco_snapshot(out);
    return true;
}

/**
 * @brief MCP 拍照工具的数据源（注册给 xiaozhi_client）。
 *
 * 与上面的 tank_status_provider 同一个套路，但**运行上下文完全不同**：
 * 它在 xiaozhi 的**工具任务**上跑，不是在 WebSocket 任务上。所以这里可以慢 ——
 * 抓帧最长 4 秒、JPEG 编码若干百毫秒，都允许。这条区别是刻意的：
 * 慢工具压在那条 WebSocket 任务上会堵死接收（xz_on_audio 上方有血泪说明）。
 *
 * @param[out] jpeg    JPEG 数据起点。**归 camera_service 所有，不要 free**，
 *                     下一次拍照前有效（本函数发完就返回，调用方随即用掉）。
 * @param[out] out_len JPEG 字节数
 */
static esp_err_t camera_capture_provider(uint8_t **jpeg, size_t *out_len)
{
    /* ① 拍 + 编码成 JPEG。失败原因已在 camera_service 里记了日志。 */
    const esp_err_t ret = camera_service_capture_jpeg(jpeg, out_len);
    if (ret != ESP_OK) {
        return ret;
    }

    /* ② 顺带存一份到 TF 卡。
       ⚠️ **存盘失败不影响这次调用** —— 卡满、没插卡、卡是 exFAT，
       都不该让「AI 看图」这个主功能挂掉。只记日志、继续往下走。
       （存盘放在上传之前：上传要几秒，先落盘就不怕那些秒里出岔子。） */
    char name[SD_PHOTO_NAME_MAX];
    const esp_err_t sret = sd_service_save_photo(*jpeg, *out_len, name, sizeof(name));
    if (sret != ESP_OK) {
        ESP_LOGW(TAG, "照片存卡失败：%s（不影响本次看图）", sd_service_status());
    } else {
        ESP_LOGI(TAG, "照片已存卡：%s（%u 字节）", name, (unsigned)*out_len);
    }

    /* ③ 顺带上屏 —— 让用户看得见 **AI 到底看到了什么**。
       这不是装饰：出问题时它能把两种完全不同的故障当场分开 ——
       屏幕上的照片糊了 ⇒ 是拍摄问题；照片清楚但 AI 答得不对 ⇒ 是识别/提示问题。
       少了这一眼，两种故障在日志里长得一模一样。

       ⚠️ 这一步不检查返回值、也不因此判失败：照片已经在上传路上了，
          上屏失败（取锁超时等）不该把整次工具调用拖垮。 */
    int w = 0, h = 0;
    const uint16_t *px = camera_service_frame(&w, &h);
    if (px != NULL) {
        (void)ui_lvgl_show_photo(px, w, h);
    }
    return ESP_OK;
}

/**
 * @brief 开机验一遍「从卡里读一张照片 → JPEG 解码 → 得到缩略图」这条路。
 *
 * 【为什么值得在开机做】
 * `sd_service_selftest()` 只验了**裸的字节读写**；而相册真正依赖的是
 * 「列目录 → 读文件 → 解码 → 尺寸正好是缩略图那么大」。这条链后两步失败时，
 * 界面上只显示"无法显示"四个字，**看不出是哪一步断的**；而且那是"人主动点进
 * 相册页"才会走到的路径 —— 排查时得指望人恰好点进去、而且串口恰好在抓。
 *
 * 2026-10-14 首次实测就撞上了这个：卡里两张 JPEG，电脑上都能打开，
 * 相册页却说"最近 0 张（2 张无法显示）"。当时手上没有那条路径的日志。
 * 把它提到开机日志里，这条路从此每次启动都留下判据。
 */
static void sd_probe_newest_thumb(void)
{
    char names[1][SD_PHOTO_NAME_MAX];
    int  count = 0;
    if (sd_service_list_photos(names, 1, &count) != ESP_OK || count == 0) {
        ESP_LOGI(TAG, "[SD] 卡上还没有照片，跳过缩略图自检");
        return;
    }

    /* 缓冲放 PSRAM 并**用完即还** —— 它不是常驻需求，没必要占着内部堆
       （内部堆是本工程最紧的资源，见 monitor_task 的读数）。 */
    uint16_t *buf = (uint16_t *)heap_caps_malloc(
                        SD_THUMB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGW(TAG, "[SD] 缩略图自检缓冲分配失败，跳过");
        return;
    }

    int w = 0, h = 0;
    const esp_err_t ret = sd_service_load_thumb(names[0], buf,
                                                (size_t)SD_THUMB_W * SD_THUMB_H,
                                                &w, &h);
    heap_caps_free(buf);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[SD] 缩略图自检**失败**：%s 解码不出来（%s）",
                 names[0], esp_err_to_name(ret));
    } else if (w != SD_THUMB_W || h != SD_THUMB_H) {
        ESP_LOGE(TAG, "[SD] 缩略图自检**失败**：%s 解出 %dx%d，期望 %dx%d",
                 names[0], w, h, SD_THUMB_W, SD_THUMB_H);
    } else {
        ESP_LOGI(TAG, "[SD] 缩略图自检通过：%s → %dx%d RGB565", names[0], w, h);
    }
}

/* ==========================================================================
 * 相册刷新（P6-c）
 *
 * ⚠️ 在 **ui_task 内、LVGL 锁外**执行。扫卡 + 解码 6 张 JPEG 要几百毫秒，
 *    持锁做会把界面连同触摸一起冻住。
 *    次序（见 ui_lvgl.h）：锁内摘 src → **这里解码** → 锁内挂 src。
 *    中间这段安全的前提是 LVGL 已经不持有那块缓冲了 —— 也正因如此
 *    相册**不需要双缓冲**（而 AI 页那张照片气泡需要，它换图没有这个空窗）。
 * ========================================================================== */

/* 缩略图参数在 sd_service 与 ui_lvgl 两处各定义一份（组件之间不互相依赖），
   必须一致：不一致的话 ui_lvgl 的缓冲装不下解码结果，表现为相册永远空白，
   而日志上只会说"参数超限"，离原因很远。故钉成编译期断言。 */
_Static_assert(SD_THUMB_MAX == UI_LVGL_ALBUM_MAX &&
               SD_THUMB_W   == UI_LVGL_ALBUM_W   &&
               SD_THUMB_H   == UI_LVGL_ALBUM_H,
               "sd_service 与 ui_lvgl 的缩略图参数不一致 —— 改一个必须改另一个"
               "（分别在 sd_service.h 与 ui_lvgl_config.h）");

static void ui_album_refresh(void)
{
    /* 无论下面走到哪一步，都先把画面上旧的图清掉 ——
       卡拔了、照片删了，就不该还留着上一次的内容。 */
    uint16_t *buf = ui_lvgl_album_begin(SD_THUMB_MAX, SD_THUMB_W, SD_THUMB_H);

    if (!sd_service_is_ready()) {
        /* 卡不可用（没插/exFAT/接线问题）。**这不是错误弹窗**，
           而是设备的一种正常状态：把原因写在页脚上就够了。 */
        ui_lvgl_album_commit(0, SD_THUMB_W, SD_THUMB_H);
        ui_lvgl_album_note(sd_service_status());
        return;
    }

    char names[SD_THUMB_MAX][SD_PHOTO_NAME_MAX];
    int  count = 0;
    if (sd_service_list_photos(names, SD_THUMB_MAX, &count) != ESP_OK) {
        ui_lvgl_album_commit(0, SD_THUMB_W, SD_THUMB_H);
        ui_lvgl_album_note("读取 TF 卡失败");
        return;
    }
    if (count == 0) {
        ui_lvgl_album_commit(0, SD_THUMB_W, SD_THUMB_H);
        ui_lvgl_album_note("卡上还没有照片 —— 让小智拍一张试试");
        return;
    }
    if (buf == NULL) {
        return;             /* 分配失败/取锁超时，里面已经记过日志 */
    }

    /* ---- ★ 锁外：解码。慢没关系 ---- */
    int ok = 0;
    for (int i = 0; i < count; i++) {
        int w = 0, h = 0;
        uint16_t *slot = buf + (size_t)ok * SD_THUMB_W * SD_THUMB_H;

        if (sd_service_load_thumb(names[i], slot,
                                  (size_t)SD_THUMB_W * SD_THUMB_H,
                                  &w, &h) != ESP_OK) {
            continue;       /* 单张坏掉不该让整页空着 */
        }
        if (w != SD_THUMB_W || h != SD_THUMB_H) {
            /* 尺寸不是预期值就**跳过**，不能将就显示。
               整页的图必须同尺寸 —— 描述符里只有一个 stride，
               尺寸不齐会让后一张读到前一张的像素。
               本机拍的照片恒为 QVGA→1/2→160×120，走到这里的是从电脑拷进来的。 */
            ESP_LOGW(TAG, "%s 解出 %dx%d，非 %dx%d，跳过",
                     names[i], w, h, SD_THUMB_W, SD_THUMB_H);
            continue;
        }
        ok++;
    }

    ui_lvgl_album_commit(ok, SD_THUMB_W, SD_THUMB_H);

    char note[80];
    if (ok == count) {
        snprintf(note, sizeof(note), "最近 %d 张", ok);
    } else {
        snprintf(note, sizeof(note), "最近 %d 张（%d 张无法显示）", ok, count - ok);
    }
    ui_lvgl_album_note(note);
    ESP_LOGI(TAG, "相册已刷新：列出 %d 张，显示 %d 张", count, ok);
}

/** 刷新链路状态字段并更新时间戳 */
static void eco_refresh_links(void)
{
    if (s_eco_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_eco_mutex, portMAX_DELAY);
    s_eco.timestamp      = (uint32_t)(esp_timer_get_time() / 1000000LL);
    s_eco.wifi_connected = wifi_sta_is_connected();
    s_eco.mqtt_connected = bafa_mqtt_is_connected();

    /* ⚠️ 2026-10-13 删掉了这里原本的一句 `s_eco.alert_active = false;`。
     *
     * 它是个真 bug：本函数被 mqtt_task **每 10 秒**调一次（还有网络事件回调），
     * 于是任何由采样算出的告警**活不过 10 秒**就被清掉 ——
     * 现象会非常迷惑："告警偶尔闪一下就没了，像是随机的"。
     *
     * 现在 alert_mask / alert_active **只有一个写者**：eco_sensor_sample()
     * 里的阈值判定。本函数只管链路状态，不碰告警。 */
    xSemaphoreGive(s_eco_mutex);
}

/* ==========================================================================
 * 传感器数据模拟（P1 接入 ADS1115 之前的替代）
 *
 * ADS1115 不打算购买了（用户决定），故 pH / TDS / 浊度等一律由随机游走生成。
 * 参数见 app_config.h 的 SIM_* 段 —— 那里解释了为什么是"游走"而不是"重掷"、
 * 为什么量程要略超出阈值、以及为什么步长用量程的比例。
 *
 * ⚠️ 接真传感器时：把本段整体删掉，改成在 eco_sensor_sample() 里读实际值即可。
 *    阈值判定与 alert_mask 的填充逻辑可以原样保留。
 * ========================================================================== */

/* --------------------------------------------------------------------------
 * 告警 → 聊天消息
 *
 * 告警集合变化时，往「小智AI互动」页的聊天流里追加一条助手消息。
 * 这样那个页面**不需要假数据**就有真实内容 —— 数据变了它就说一句。
 *
 * 线程边界：`eco_sensor_sample()` 跑在 sensor_task 里，**不能**直接调 LVGL
 * （本工程约定所有 LVGL 调用只在 ui_task 内）。所以这里只把待发的文本写进
 * 缓冲区并置标志，由 ui_task 取走发送。
 * -------------------------------------------------------------------------- */
static const struct {
    uint32_t    bit;
    const char *name;
} k_alert_names[] = {
    { ECO_ALERT_PH,        "pH"   },
    { ECO_ALERT_WATER_TMP, "水温" },
    { ECO_ALERT_AIR_TMP,   "气温" },
    { ECO_ALERT_HUMIDITY,  "湿度" },
    { ECO_ALERT_LUX,       "光照" },
    { ECO_ALERT_TURBIDITY, "浊度" },
    { ECO_ALERT_TDS,       "TDS"  },
};
#define ALERT_NAME_CNT ((int)(sizeof(k_alert_names) / sizeof(k_alert_names[0])))

static char              s_alert_msg[96];
static volatile bool     s_alert_msg_ready = false;

/** 拼一条给用户看的告警消息。
 *  只列**哪几项**越界，不逐个报数值 —— 数值「鱼缸情况」页本来就有，
 *  聊天里再铺一遍反而看不清重点。 */
static void alert_msg_build(uint32_t mask)
{
    if (mask == 0) {
        snprintf(s_alert_msg, sizeof(s_alert_msg), "各项数据已恢复正常。");
        return;
    }

    size_t used = (size_t)snprintf(s_alert_msg, sizeof(s_alert_msg), "⚠ ");
    for (int i = 0; i < ALERT_NAME_CNT && used < sizeof(s_alert_msg) - 8; i++) {
        if ((mask & k_alert_names[i].bit) == 0) {
            continue;
        }
        const int n = snprintf(s_alert_msg + used, sizeof(s_alert_msg) - used,
                               "%s%s", (used > 4) ? "、" : "",
                               k_alert_names[i].name);
        if (n < 0) {
            break;
        }
        used += (size_t)n;
    }
    snprintf(s_alert_msg + used, sizeof(s_alert_msg) - used, " 数据越界");
}

/** 上一次的告警状态 —— 滞回判定要用。只有 sensor_task 读写，且在锁内。 */
static bool s_alert_ph = false, s_alert_wtemp = false, s_alert_atemp = false;
static bool s_alert_hum = false, s_alert_lux = false, s_alert_turb = false;
static bool s_alert_tds = false;

/** [-1, 1] 上的均匀随机数。
 *  用 esp_random()（硬件 RNG）而不是 rand()：后者需要种子，且多个任务同时调
 *  会竞争同一份全局状态。esp_random 无需播种、线程安全。 */
static float sim_rand_sym(void)
{
    return ((float)(esp_random() % 2001u) - 1000.0f) / 1000.0f;
}

/** 随机游走一步：上次值 + 一个小随机步，夹在 [lo, hi] 内 */
static float sim_walk(float prev, float lo, float hi)
{
    const float step = (hi - lo) * SIM_WALK_STEP_FRAC;
    float v = prev + sim_rand_sym() * step;
    if (v < lo) { v = lo; }
    if (v > hi) { v = hi; }
    return v;
}

/** 带滞回的越界判断。
 *  进入：超出 [min, max]。
 *  退出：回到 [min + hyst, max - hyst] 之内（比进入条件更严）。
 *  was = 上一次是否处于告警。 */
static bool sim_alert(float v, float min, float max, float hyst, bool was)
{
    if (was) {
        return !(v >= (min + hyst) && v <= (max - hyst));
    }
    return (v < min || v > max);
}

/** 滞回幅度：取该量量程的一个比例（见 app_config.h 的说明） */
#define SIM_HYST(lo, hi)  (((hi) - (lo)) * SIM_ALERT_HYST_FRAC)

/** 模拟一次采样：游走全部量 → 按阈值算 alert_mask → 写回 s_eco */
static void eco_sensor_sample(void)
{
    if (s_eco_mutex == NULL) {
        return;
    }

    /* 「读旧值 → 游走 → 写回」整段持锁：与 eco_snapshot() 的读者互斥，
       不会让界面读到"改了一半"的结构体。 */
    xSemaphoreTake(s_eco_mutex, portMAX_DELAY);

    s_eco.ph           = sim_walk(s_eco.ph,           SIM_PH_MIN,    SIM_PH_MAX);
    s_eco.water_temp_c = sim_walk(s_eco.water_temp_c, SIM_WTEMP_MIN, SIM_WTEMP_MAX);
    s_eco.air_temp_c   = sim_walk(s_eco.air_temp_c,   SIM_ATEMP_MIN, SIM_ATEMP_MAX);
    s_eco.air_humidity = sim_walk(s_eco.air_humidity, SIM_HUM_MIN,   SIM_HUM_MAX);
    s_eco.lux          = sim_walk(s_eco.lux,          SIM_LUX_MIN,   SIM_LUX_MAX);
    s_eco.turbidity_ntu = sim_walk(s_eco.turbidity_ntu, SIM_TURB_MIN, SIM_TURB_MAX);
    s_eco.tds_ppm      = sim_walk(s_eco.tds_ppm,      SIM_TDS_MIN,   SIM_TDS_MAX);

    s_eco.timestamp = (uint32_t)(esp_timer_get_time() / 1000000LL);

    /* 阈值判定。每项一步一算，顺带更新滞回状态。
       位定义在 app_core/app_types.h 的 ECO_ALERT_*（那里也解释了为什么用位掩码）。 */
    uint32_t mask = 0;

    s_alert_ph = sim_alert(s_eco.ph, THRESHOLD_PH_MIN, THRESHOLD_PH_MAX,
                           SIM_HYST(SIM_PH_MIN, SIM_PH_MAX), s_alert_ph);
    if (s_alert_ph) { mask |= ECO_ALERT_PH; }

    s_alert_wtemp = sim_alert(s_eco.water_temp_c,
                              THRESHOLD_WATER_TEMP_MIN, THRESHOLD_WATER_TEMP_MAX,
                              SIM_HYST(SIM_WTEMP_MIN, SIM_WTEMP_MAX), s_alert_wtemp);
    if (s_alert_wtemp) { mask |= ECO_ALERT_WATER_TMP; }

    s_alert_atemp = sim_alert(s_eco.air_temp_c,
                              THRESHOLD_AIR_TEMP_MIN, THRESHOLD_AIR_TEMP_MAX,
                              SIM_HYST(SIM_ATEMP_MIN, SIM_ATEMP_MAX), s_alert_atemp);
    if (s_alert_atemp) { mask |= ECO_ALERT_AIR_TMP; }

    s_alert_hum = sim_alert(s_eco.air_humidity,
                            THRESHOLD_HUMIDITY_MIN, THRESHOLD_HUMIDITY_MAX,
                            SIM_HYST(SIM_HUM_MIN, SIM_HUM_MAX), s_alert_hum);
    if (s_alert_hum) { mask |= ECO_ALERT_HUMIDITY; }

    s_alert_lux = sim_alert(s_eco.lux, THRESHOLD_LUX_MIN, THRESHOLD_LUX_MAX,
                            SIM_HYST(SIM_LUX_MIN, SIM_LUX_MAX), s_alert_lux);
    if (s_alert_lux) { mask |= ECO_ALERT_LUX; }

    /* 浊度只有上限 —— 用 0 当虚拟下限，滞回逻辑照常工作 */
    s_alert_turb = sim_alert(s_eco.turbidity_ntu, 0.0f, THRESHOLD_TURBIDITY_MAX,
                             SIM_HYST(SIM_TURB_MIN, SIM_TURB_MAX), s_alert_turb);
    if (s_alert_turb) { mask |= ECO_ALERT_TURBIDITY; }

    s_alert_tds = sim_alert(s_eco.tds_ppm, THRESHOLD_TDS_MIN, THRESHOLD_TDS_MAX,
                            SIM_HYST(SIM_TDS_MIN, SIM_TDS_MAX), s_alert_tds);
    if (s_alert_tds) { mask |= ECO_ALERT_TDS; }

    s_eco.alert_mask   = mask;
    s_eco.alert_active = (mask != 0);

    xSemaphoreGive(s_eco_mutex);

    /* 只在告警集合变化时打日志 + 生成聊天消息 —— 每秒一条会把串口刷爆，
       而"告警从无到有/从有到无"正是最需要留下痕迹的时刻。 */
    static uint32_t s_last_mask = 0;
    static uint32_t s_last_msg_s = 0;       /* 0 = 还没发过告警消息 */

    if (mask != s_last_mask) {
        ESP_LOGW(TAG, "告警状态变化: 0x%02"PRIx32" -> 0x%02"PRIx32"（%s）",
                 s_last_mask, mask, mask ? "有越界" : "全部正常");
        s_last_mask = mask;

        /* ---- 冷却：距上一条告警消息不足 APP_ALERT_MSG_MIN_INTERVAL_S 就不发 ----
           ⚠️ **只抑制消息，不抑制 alert_mask** ——
              卡片该变红就得立刻变红，那是实时状态；而"要不要说话"是另一回事。
              随机游走会反复穿越阈值，不加冷却的话聊天流会被告警刷满。 */
        const uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
        if (s_last_msg_s == 0 ||
            (now_s - s_last_msg_s) >= APP_ALERT_MSG_MIN_INTERVAL_S) {

            s_last_msg_s = now_s;

            /* 先把文本写好，**再**置标志 —— 消费者看到标志时内容必然已就绪。
               单生产者（sensor_task）+ 单消费者（ui_task），无需更多同步。 */
            alert_msg_build(mask);
            s_alert_msg_ready = true;
        }
    }
}

/* ==========================================================================
 * P0.5 硬件验证
 *
 * 设计原则：**一次只测一件事**，避免一个失败污染另一个的判断。
 * 每个试验都打一条带 [P05] 前缀的结论日志，可直接从串口读答案。
 *
 * 已裁决完毕、因此已删除代码的试验（结论留在 docs/pinmap.md）：
 *   - 背光引脚排除 → 背光在 IO42，低电平点亮
 *   - LED 引脚排除  → IO10 同时关联 LED 与屏，不得作普通 GPIO
 *   - 面板 ID 读回   → 因缺 DUMMY 相位而不可信，已弃用
 * ========================================================================== */

#if P05_TEST_C1_I2S_COLLISION
/** ⛔【本试验的前提已被推翻 —— 永久关闭，代码仅作历史记录】
 *
 * 2026-10-13 查明：**C1（LCD 与 I2S 争用 IO39/IO40）根本不存在**。
 * 它是引脚表把 I2S 的 BCLK/WS 误抄成 LCD 的 DC/MOSI 造成的。两套厂家已验证
 * 代码一致写 BCLK=IO14、WS=IO13，与 LCD 的 IO39/IO40/IO41 零重叠。
 * 证据见 components/board_gec/include/gec_board.h 第四节。
 *
 * 下面这套「三级取证」测的是一个不存在的冲突 —— **不要启用，也不要据其
 * 结论做任何判断**。保留代码只为记住这段排查史。
 * ---------------------------------------------------------------------------
 * 原设计（已作废）：C1 冲突试验：LCD 与 I2S 争用 IO39/IO40。
 *
 *  这是本项目最大架构风险的裁决实验，采用三级取证：
 *    1) I2S 初始化前先记录 LCD 状态（上一阶段已画好彩条）
 *    2) 初始化音频并播放测试音；播放期间重画彩条 → 观察是否花屏
 *    3) 停止音频后重画彩条 → 区分「运行期争用」与「永久性破坏」
 *
 *  判读：屏幕出现花屏/撕裂/偏色 => C1 成立，两者不能同时工作。 */
static void p05_c1_collision_test(void)
{
    ESP_LOGI(TAG, "[C1] ===== LCD 与 I2S 引脚争用试验开始 =====");

    if (!lcd_is_ready()) {
        ESP_LOGW(TAG, "[C1] LCD 未就绪，试验无意义，跳过");
        return;
    }

    /* --- 阶段 1：基线（此时只有 LCD 在跑） --- */
    ESP_LOGI(TAG, "[C1] 阶段1：只有 LCD。重画彩条作为基线，请记住当前画面");
    lcd_draw_color_bars();
    ESP_LOGI(TAG, "[C1] 阶段1 面板 ID 基线:");
    (void)lcd_read_id(NULL);
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* --- 阶段 2：初始化 I2S ---
       注：本试验的前提（C1）已被推翻，此处文字随引脚修正一并更新：
       I2S 现为 BCLK=IO14 / WS=IO13，与 LCD 的 DC=IO39 / MOSI=IO40 **不再重合**，
       故下面这句「重合」的判断已不成立。 */
    ESP_LOGI(TAG, "[C1] 阶段2：初始化 I2S（BCLK=IO%d, WS=IO%d）",
             GEC_I2S_BCLK_GPIO, GEC_I2S_WS_GPIO);

    const esp_err_t aret = audio_service_init();
    if (aret != ESP_OK) {
        ESP_LOGE(TAG, "[C1] 音频初始化失败: %s", esp_err_to_name(aret));
        ESP_LOGE(TAG, "[C1] 若错误为引脚已被占用，即为 C1 成立的**硬证据**");
        return;
    }
    ESP_LOGI(TAG, "[C1] 音频初始化成功（说明 GPIO 矩阵允许两外设输出同一引脚）");

    /* 诊断点：I2S 通道建好之后、还没开始播放 —— 屏幕是否已受影响？ */
    ESP_LOGI(TAG, "[C1] 阶段2 音后诊断：**尚未播放**，重画彩条。"
                  "若此刻已无显示 => 破坏发生在 I2S 通道创建阶段，与播放无关");
    lcd_draw_color_bars();
    vTaskDelay(pdMS_TO_TICKS(2000));

    esp_err_t ret = audio_service_enable_output(true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[C1] 打开播放通路失败: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "[C1] 播放通路已打开（功放已使能）");
    }

    /* --- 阶段 3：播放测试音的同时重画彩条 --- */
    ESP_LOGI(TAG, "[C1] 阶段3：播放 1kHz 测试音（应能听到声音），"
                  "同时重画彩条——**请观察屏幕是否花屏/撕裂**");
    lcd_draw_color_bars();

    /* 测试音与重画交替进行：每 300ms 重画一次，共 3 秒，
       这样即使某次绘制恰好躲过冲突窗口也能被后续几次抓到 */
    for (int i = 0; i < 10; i++) {
        esp_err_t tr = audio_service_play_tone(1000, 300, 8000);
        if (tr != ESP_OK) {
            ESP_LOGW(TAG, "[C1] 测试音播放失败: %s", esp_err_to_name(tr));
            break;
        }
        lcd_draw_color_bars();
    }

    /* --- 阶段 4：停音频（释放 I2S 引脚）后再画 --- */
    ESP_LOGI(TAG, "[C1] 阶段4：关闭音频通路（这会停用 I2S 通道并释放 IO39/IO40），"
                  "然后再画一次彩条。若此刻恢复 => 属**引脚占用**，可用分时复用规避");
    (void)audio_service_enable_output(false);
    vTaskDelay(pdMS_TO_TICKS(500));
    lcd_draw_color_bars();
    vTaskDelay(pdMS_TO_TICKS(2500));

    /* --- 阶段 5：重新初始化 LCD --- */
    ESP_LOGI(TAG, "[C1] 阶段5：重新初始化 LCD（SPI/面板全部重建）");
    vTaskDelay(pdMS_TO_TICKS(500));

    const esp_err_t rret = lcd_reinit();
    ESP_LOGI(TAG, "[C1] lcd_reinit() 返回: %s", esp_err_to_name(rret));

    if (rret == ESP_OK) {
        const esp_err_t dret = lcd_draw_color_bars();
        ESP_LOGI(TAG, "[C1] 重绘彩条返回: %s", esp_err_to_name(dret));
    }
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* --- 阶段 6：重开音频并播放，同时观察屏幕能否保持 --- */
    ESP_LOGI(TAG, "[C1] 阶段6：重新打开音频播放 1kHz 测试音。"
                  "**① 请听声音；② 请盯住屏幕看内容会不会变花/消失**");
    ESP_LOGI(TAG, "[C1] 本阶段先画一次彩条，再在播放中画第二次——"
                  "若第一次画完后屏幕正常、第二次之后变花，说明 I2S 会持续污染面板");

    const esp_err_t aret2 = audio_service_enable_output(true);
    ESP_LOGI(TAG, "[C1] 重新打开播放通路: %s", esp_err_to_name(aret2));
    if (aret2 == ESP_OK) {
        /* 先落一幅图，再开始放音 */
        lcd_draw_color_bars();
        const esp_err_t tret = audio_service_play_tone(1000, 1500, 8000);
        ESP_LOGI(TAG, "[C1] 测试音播放: %s", esp_err_to_name(tret));

        /* 声音仍在响（或刚响完）时再画一次 —— 这是「音频期间能否刷屏」的判据 */
        vTaskDelay(pdMS_TO_TICKS(500));
        lcd_draw_color_bars();
        ESP_LOGI(TAG, "[C1] 已在 I2S 运行期间重画彩条，请观察屏幕是否异常");
    }
    vTaskDelay(pdMS_TO_TICKS(4000));

    /* --- 阶段 7：关音频后再画一次，确认屏幕可恢复 --- */
    ESP_LOGI(TAG, "[C1] 阶段7：关闭音频后重画彩条（屏应在此时恢复）");
    (void)audio_service_enable_output(false);
    vTaskDelay(pdMS_TO_TICKS(500));
    lcd_draw_color_bars();
    vTaskDelay(pdMS_TO_TICKS(2500));

    ESP_LOGI(TAG, "[C1] ===== 试验结束 =====");
    ESP_LOGI(TAG, "[C1] 已确认的事实：厂家 xiaozhi 固件在本板同样是"
                  "「音频正常、屏幕无显示」=> C1 是硬件级冲突，非移植问题");
    ESP_LOGI(TAG, "[C1] 请回报（决定 UI 方案能否保住）：");
    ESP_LOGI(TAG, "[C1]   ① 阶段3 播放测试音时：**听到声音了吗？**");
    ESP_LOGI(TAG, "[C1]   ② 阶段3（I2S 在用）：屏幕有无显示？");
    ESP_LOGI(TAG, "[C1]   ③ 阶段4（音频已关）：屏幕**恢复显示**了吗？");
    ESP_LOGI(TAG, "[C1]   ④ 阶段5（重初始化）：屏幕恢复显示了吗？");
    ESP_LOGI(TAG, "[C1]   ⑤ 阶段6：听到声音了吗？播放期间屏幕是否变花/消失？");
    ESP_LOGI(TAG, "[C1]   ⑥ 阶段7（关音频后）：屏幕恢复了吗？");
}
#endif

#if P05_TEST_AUDIO_LCD_ORDER
/* ==========================================================================
 * 音频 / LCD 初始化顺序对照试验 —— 判定「喇叭无声」是否由 C1 引起
 *
 * ⛔【结局：本试验写完后尚未上板，真因即已查明，故未启用】
 *   2026-10-13：喇叭无声的真因是 **I2S 的 BCLK/WS 被错写成 IO39/IO40**
 *   （正是 LCD 的 DC/MOSI），改回厂家代码的 **IO14/IO13** 后冲突消失。
 *   C1 本身就不存在。详见 components/board_gec/include/gec_board.h 第四节。
 *   下面的设计因此测的是一个不存在的冲突，**结论以引脚修正为准**。
 *   代码保留，因为它的 A/B 结构对日后任何「两外设争引脚」的怀疑都直接可用。
 * ----------------------------------------------------------------------------
 *
 * 【背景】2026-10-13 实测：LCD 能正常出图后，音频软件链路全绿但**完全无声**。
 *   日志证据（全部返回 OK，无任何报错）：
 *     audio: I2S 双工通道就绪: MCLK=IO38 BCLK=IO39 WS=IO40 DIN=IO12 DOUT=IO45
 *     audio: [步骤1] esp_codec_dev_open(ES8311) -> 0 OK
 *     audio: [步骤2] 设置音量 70 -> 0 OK
 *     audio: [步骤4] PCA9557 P1 功放使能 -> ESP_OK
 *     audio: 测试音播放完成
 *   即：**不是 API 调用失败，是信号没到喇叭**。
 *
 * 【怀疑】C1 —— IO39 与 IO40 同时被两个外设占用：
 *     LCD: DC=IO39、MOSI=IO40        （lcd_init() 里 spi_bus_initialize）
 *     I2S: BCLK=IO39、WS=IO40        （audio_service_init()）
 *   LCD 先初始化占了这两个脚，I2S 后初始化。I2S 究竟有没有真正接管引脚，
 *   无法从代码判定（GPIO 矩阵同一输出脚只有一个信号源，后写者应胜出，
 *   但 ESP-IDF 各驱动的实际行为必须实测）。
 *
 * 【做法】一次烧录内做 A/B：先在不初始化 LCD 的前提下播一次音，再初始化
 *   LCD，然后用**完全相同的参数**再播一次。两次结果一比对，答案自明。
 *
 *   判读：①有声 ③无声 ⇒ C1 成立，喇叭无声由 LCD 占脚引起
 *         ①无声       ⇒ 与 LCD 无关，另有原因（转向硬件/编解码器）
 * ========================================================================== */
static void p05_audio_lcd_order_test(void)
{
    ESP_LOGW(TAG, "[顺序] ===== 音频/LCD 初始化顺序对照试验 =====");

    /* ---------- 阶段 1：LCD 尚未初始化，只有音频 ---------- */
    ESP_LOGW(TAG, "[顺序] ---------- 阶段1：**LCD 未初始化**，只跑音频 ----------");
    esp_err_t aret = audio_service_init();
    if (aret != ESP_OK) {
        ESP_LOGE(TAG, "[顺序] 音频初始化失败: %s", esp_err_to_name(aret));
        ESP_LOGE(TAG, "[顺序] 若为引脚被占用，本身即 C1 的证据");
        return;
    }

    (void)audio_service_enable_output(true);
    ESP_LOGW(TAG, "[顺序] 播放 1000Hz / 1.5s / 幅度8000 —— ★请仔细听");
    const esp_err_t t1 = audio_service_play_tone(1000, 1500, 8000);
    ESP_LOGW(TAG, "[顺序] 阶段1 播放返回: %s", esp_err_to_name(t1));
    (void)audio_service_enable_output(false);
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGW(TAG, "[顺序] 阶段1 结束 —— ★刚才有声音吗？请记住答案");

    /* ---------- 阶段 2：初始化 LCD，抢占 IO39/IO40 ---------- */
    ESP_LOGW(TAG, "[顺序] ---------- 阶段2：初始化 LCD（SPI3 接管 IO39=DC / IO40=MOSI）----------");
    const esp_err_t lret = lcd_init();
    ESP_LOGW(TAG, "[顺序] lcd_init() -> %s", esp_err_to_name(lret));
    if (lret == ESP_OK) {
        (void)lcd_backlight(true);
        vTaskDelay(pdMS_TO_TICKS(300));
        (void)lcd_draw_color_bars();
        ESP_LOGW(TAG, "[顺序] 已画彩条 —— ★屏幕有显示吗？");
    }
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* ---------- 阶段 3：LCD 已占脚，用同样参数再播一次 ---------- */
    ESP_LOGW(TAG, "[顺序] ---------- 阶段3：**LCD 已初始化**，再播一次同样的音 ----------");
    (void)audio_service_enable_output(true);
    ESP_LOGW(TAG, "[顺序] 播放 1000Hz / 1.5s / 幅度8000（参数与阶段1 完全相同）—— ★请仔细听");
    const esp_err_t t3 = audio_service_play_tone(1000, 1500, 8000);
    ESP_LOGW(TAG, "[顺序] 阶段3 播放返回: %s", esp_err_to_name(t3));
    (void)audio_service_enable_output(false);
    vTaskDelay(pdMS_TO_TICKS(1500));

    /* ---------- 阶段 4：LCD 是否被音频反过来破坏了 ---------- */
    ESP_LOGW(TAG, "[顺序] ---------- 阶段4：播放后再画一次彩条（查反向干扰）----------");
    (void)lcd_draw_color_bars();
    ESP_LOGW(TAG, "[顺序] 已重画彩条 —— ★此刻屏幕还正常吗？");
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGW(TAG, "[顺序] ========== 试验结束，请回报四个问题 ==========");
    ESP_LOGW(TAG, "[顺序]   ① 阶段1（只有音频）：**听到声音了吗？**");
    ESP_LOGW(TAG, "[顺序]   ② 阶段2：屏幕**显示出彩条了吗？**");
    ESP_LOGW(TAG, "[顺序]   ③ 阶段3（LCD 已初始化）：**听到声音了吗？**");
    ESP_LOGW(TAG, "[顺序]   ④ 阶段4：彩条**还正常吗？**");
    ESP_LOGW(TAG, "[顺序] 判读：①有声、③无声 ⇒ C1 成立，喇叭无声由 LCD 占脚引起");
    ESP_LOGW(TAG, "[顺序]       ①无声         ⇒ 与 LCD 无关，转向硬件/编解码器排查");
}
#endif

static void p05_run(void)
{
#if P05_TEST_AUDIO_LCD_ORDER
    /* 本试验自带 LCD 初始化（故意放在音频之后），故独占 p05_run 流程 */
    p05_audio_lcd_order_test();
    return;
#endif

#if P05_LCD_INIT
    /* ======================================================================
     * LCD 上电初始化
     *
     * ⚠️ P2 起这是**必需步骤**，不只是自检 —— lcd_init() 就在这里，
     *    而 ui_lvgl_init() 要求 lcd_is_ready() 为真。
     *    下面的绘制自检可以关（P05_TEST_LCD_DRAW），初始化不行。
     *
     * 历史：LCD 的初始化序列是逐行移植粤嵌官方已验证 BSP
     * （06-tcp_client_bafa_mqtt）而来的。此前十几轮失败的根本原因是"重写"
     * 而非"复用"——自己选 SPI 模式、自己定缓冲尺寸、自己删掉片选步骤，
     * 每一步都引入新变量，失败时无法区分是哪个变量的错。
     * ====================================================================== */
    ESP_LOGI(TAG, "[P05] ========== LCD 初始化 ==========");

    esp_err_t ret = lcd_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[P05] lcd_init 失败: %s —— 界面将不可用", esp_err_to_name(ret));
    } else {
        lcd_dump_pin_levels();

        (void)lcd_backlight(true);
        vTaskDelay(pdMS_TO_TICKS(300));

#if P05_TEST_LCD_DRAW
        /* 先按参考实现的参数画一次，若正常则应看到彩条 */
        ret = lcd_draw_color_bars();
        ESP_LOGI(TAG, "[P05] 彩条绘制（参考参数 方向=MV+MX+MY）-> %s",
                 esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(5000));

        /* 全屏单色探针 —— 裁定颜色通道映射。
           彩条报告依赖「左→右」的人为读数顺序，报错一位就会带偏结论；
           本探针全屏一次只出一种颜色，去掉该隐患。 */
#if P05_TEST_LCD_COLOR_PROBE
        ESP_LOGW(TAG, "[P05] 进入全屏单色探针（裁定颜色通道）");
        const esp_err_t pret = lcd_color_probe();
        ESP_LOGI(TAG, "[P05] 单色探针返回: %s", esp_err_to_name(pret));
        /* 探针结束停在黑色上，重画一次彩条，便于继续用彩条交叉核对 */
        (void)lcd_draw_color_bars();
        ESP_LOGI(TAG, "[P05] 已重画彩条（当前应显示 红|绿|蓝|白|黑）");
#endif

        /* 方向 × 背光极性 枚举试验 —— **已裁决，默认关闭**
         *
         * 裁决依据（2026-10-13 实测）：
         *   - 方向：wake 后直接画彩条，观察者能按「左→右」逐条读出颜色，
         *           证明 MV|MX|MY = 1|1|1（终值 MADCTL 0xE0）正确；
         *   - 背光：LCD_BL_INVERT 由 0 改为 1 后屏幕点亮 ⇒ **高电平点亮**。
         *
         * 之所以默认关闭而非保留：该试验会把背光极性来回翻转，并占用 64 秒，
         * 正好会干扰「颜色顺序」这一项正在做的裁决 —— 保留它会引入无关变量。
         * 若日后怀疑方向/极性，把 app_config.h 里的开关置 1 即可复现。 */
#if P05_TEST_LCD_ORIENT_SWEEP
        ESP_LOGW(TAG, "[P05] 进入方向 × 背光极性 枚举试验");
        const esp_err_t sret = lcd_orientation_bl_sweep();
        ESP_LOGI(TAG, "[P05] 枚举试验返回: %s", esp_err_to_name(sret));
#else
        ESP_LOGI(TAG, "[P05] 方向/背光枚举试验已关闭（另有开关）；"
                      "彩条保持显示，便于核对颜色顺序");
#endif
#endif  /* P05_TEST_LCD_DRAW */

        /* 绘制自检全部关闭时，屏幕停在"已初始化但未画内容"的黑屏状态，
           随后交给 ui_task 里的 ui_lvgl_init() 接管。 */
    }
    ESP_LOGI(TAG, "[P05] ========== LCD 初始化结束 ==========");
#endif  /* P05_LCD_INIT */

#if P05_TEST_C1_I2S_COLLISION
    p05_c1_collision_test();
#endif

#if P05_TEST_AUDIO
    /* ======================================================================
     * 音频重复播放（验证「喇叭只响第一次」已修）
     *
     * 真因：esp_codec_dev 的 _i2s_drv_enable() 对通道启停**不做幂等检查**，
     * 而 IDF 的 i2s_channel_disable() 在未启用时返回 ESP_ERR_INVALID_STATE。
     * 修法：codec 通路常开、只切功放使能。上轮已实测三轮全响。
     * ====================================================================== */
    ESP_LOGI(TAG, "[Audio] ========== 音频重复播放测试 ==========");

    esp_err_t aret = audio_service_init();
    if (aret != ESP_OK) {
        ESP_LOGE(TAG, "[Audio] 初始化失败: %s", esp_err_to_name(aret));
        return;
    }

    for (int i = 1; i <= 3; i++) {
        esp_err_t r = audio_service_enable_output(true);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "[Audio] 第 %d 轮打开通路失败: %s", i, esp_err_to_name(r));
            continue;
        }
        const int freq = 800 + i * 200;      /* 1000 / 1200 / 1400 Hz */
        r = audio_service_play_tone(freq, 800, 8000);
        ESP_LOGI(TAG, "[Audio] 第 %d 轮 播放 %dHz -> %s", i, freq, esp_err_to_name(r));
        (void)audio_service_enable_output(false);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    ESP_LOGI(TAG, "[Audio] 请回报：三轮都听到声音了吗？");
#endif
}

/* ==========================================================================
 * NVS
 * ========================================================================== */
static esp_err_t app_nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要格式化: %s", esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS 就绪");
    } else {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(ret));
    }
    return ret;
}

/* ==========================================================================
 * 内存与任务健康检查
 * ========================================================================== */
typedef struct {
    const char  *name;
    TaskHandle_t handle;
} task_reg_t;

#define APP_MAX_MONITORED_TASKS 8
static task_reg_t s_monitored[APP_MAX_MONITORED_TASKS];
static int        s_monitored_cnt = 0;

static void monitor_register(const char *name, TaskHandle_t handle)
{
    if (s_monitored_cnt < APP_MAX_MONITORED_TASKS) {
        s_monitored[s_monitored_cnt].name   = name;
        s_monitored[s_monitored_cnt].handle = handle;
        s_monitored_cnt++;
    } else {
        ESP_LOGW(TAG, "监控表已满，%s 未被纳入健康检查", name);
    }
}

static void log_memory_report(void)
{
    /* ⚠️ 口径必须一致，否则会打印出「空闲 > 总计」这种物理上不可能的数字。
       实测踩坑：初版用 esp_get_free_heap_size() 取"空闲"，却用
       heap_caps_get_total_size(MALLOC_CAP_INTERNAL) 取"总计"，前者包含
       PSRAM（8MB），后者只有内部 RAM（约 360KB），于是打印出
       「空闲 8579952 / 总计 373107」——空闲是总计的 23 倍。

       正确口径：
         esp_get_free_heap_size()  = 全部堆的空闲（内部 + PSRAM）
         esp_get_free_internal_heap_size() = 仅内部堆的空闲
       两者各自与对应的 heap_caps total 配对使用。 */
    const size_t int_free      = esp_get_free_internal_heap_size();
    const size_t int_total     = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const size_t int_min_free  = esp_get_minimum_free_heap_size();

    const size_t all_free      = esp_get_free_heap_size();
    const size_t psram_free    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_total   = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    if (int_total > 0) {
        ESP_LOGI(TAG, "内部堆: 空闲 %u / 总计 %u 字节 (%.0f%% 余量, 历史最低 %u)",
                 (unsigned)int_free, (unsigned)int_total,
                 100.0 * (double)int_free / (double)int_total,
                 (unsigned)int_min_free);
    }

    if (psram_total > 0) {
        ESP_LOGI(TAG, "PSRAM : 空闲 %u / 总计 %u 字节 (%.0f%% 余量)",
                 (unsigned)psram_free, (unsigned)psram_total,
                 100.0 * (double)psram_free / (double)psram_total);
    } else {
        ESP_LOGW(TAG, "PSRAM 未检测到！摄像头与 LVGL 将无法按设计工作，"
                      "请确认 CONFIG_SPIRAM_MODE_OCT");
    }

    ESP_LOGI(TAG, "全堆(含PSRAM): 空闲 %u 字节", (unsigned)all_free);

    /* 验收指标：内部堆余量 >= 30%。
       判据用内部堆而非全堆——内部 RAM 才是真正的稀缺资源，
       算上 8MB PSRAM 的全堆余量永远好看，会掩盖真实压力。 */
    if (int_total > 0) {
        const int pct = (int)((uint64_t)int_free * 100 / int_total);
        if (pct < 30) {
            ESP_LOGW(TAG, "内部堆余量仅 %d%%，低于 30%% 验收线", pct);
        }
    }
}

/** 网络状态播报（含 IP 与信号强度） */
static void log_net_status(void)
{
    if (!wifi_sta_is_connected()) {
        ESP_LOGW(TAG, "网络: 未连接");
        return;
    }
    const esp_ip4_addr_t ip = wifi_sta_get_ip();
    ESP_LOGI(TAG, "网络: 已连接 IP=" IPSTR " RSSI=%d dBm, MQTT=%s",
             IP2STR(&ip), wifi_sta_get_rssi(),
             bafa_mqtt_is_connected() ? "已连接" : "未连接");
}

/* ==========================================================================
 * 事件订阅：网络状态变化
 * ========================================================================== */
static void on_wifi_event(void *arg, const app_event_msg_t *msg)
{
    (void)arg;
    ESP_LOGI(TAG, "[事件] Wi-Fi %s", msg->data.flag ? "已连接" : "已断开");
    eco_refresh_links();
}

static void on_mqtt_event(void *arg, const app_event_msg_t *msg)
{
    (void)arg;
    ESP_LOGI(TAG, "[事件] MQTT %s", msg->data.flag ? "已连接" : "已断开");
    eco_refresh_links();
}

/* ==========================================================================
 * 任务：sensor_task
 *
 * P1 的职责原本是「读 ADS1115 + 温湿度 + 光照」。ADS1115 不购买了，
 * 故当前只做一件事：**按 APP_SAMPLE_PERIOD_MS 走一次随机游走并判定阈值**。
 * 任务骨架、周期、栈都是照 P1 的规划来的，接真传感器时只需换掉取值部分。
 *
 * 为什么单独立一个任务而不是塞进 ui_task：
 *   · 采样节拍（1s）与界面节拍（30ms/200ms）是两件事，耦合在一起以后
 *     任一方调整都会牵动另一方；
 *   · 将来接真实 ADC 时会有 I2C 阻塞，不该拖累界面刷新。
 * ========================================================================== */
static void sensor_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "sensor_task 启动 (优先级 %d, 核心 %d, 栈 %d)",
             APP_TASK_PRIO_SENSOR, APP_TASK_CORE_1, APP_TASK_STACK_SENSOR);
    ESP_LOGW(TAG, "传感器数据为**模拟值**（随机游走）—— ADS1115 未接入，"
                  "参数见 app_config.h 的 SIM_* 段");

    const TickType_t period = pdMS_TO_TICKS(APP_SAMPLE_PERIOD_MS);
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, period);
        eco_sensor_sample();
    }
}

/* ==========================================================================
 * 拍照
 *
 * 【2026-10-14：手动拍照这条路撤了】
 * 原先这里是 `ui_capture_photo()`，配合界面上的「拍照」键做手动拍摄
 * （按钮回调置标志 → ui_task 锁外轮询 → 抓帧 → 上屏）。
 * 用户要求去掉那个键、AI 页只留一个语音键，于是整条链一并删除 ——
 * 键没了，`ui_lvgl_capture_requested()` 永远不会返回 true，留着就是
 * 永远不会执行的代码，还会让后来的人误以为"手动拍照还在"。
 *
 * 现在拍照由 **AI 侧**驱动：`self.camera.take_photo` MCP 工具 →
 * `camera_capture_provider()`（见上）→ 抓帧 + JPEG 编码 + 上传 +
 * **顺带上屏**。照片照样会作为聊天气泡出现在「小智AI互动」页，
 * 只是发起方从"用户按按钮"变成了"AI 决定要看"。
 *
 * 底层的 `camera_service_capture()`（不带 JPEG 的那条）现在没有调用方了，
 * 但**保留**：它是 camera_service 这个通用组件的正经入口，
 * 不只是这条 UI 流程的附属品（调试摄像头、将来加别的触发方式都用得上）。
 * ========================================================================== */


/* ==========================================================================
 * P7 下行语音的播放缓冲
 *
 * 【为什么要一个环形缓冲，而不是在回调里直接播】
 * `xz_on_audio` 跑在 **xz_client 任务**里，那个任务还要收 WebSocket 帧、
 * 跑心跳、处理服务器下发的控制消息。如果在回调里直接调
 * `audio_service_play_pcm()`，它会**按实时速率阻塞**（60ms 的数据就阻塞
 * 60ms）—— 网络侧立刻被拖住，抖动无从吸收，最后表现为断音和连接超时。
 *
 * 所以：回调只把 PCM **放进环形缓冲就返回**（微秒级），
 * 由独立的 play_task 按音频速率慢慢取出并播放。
 * 这也正是 xiaozhi 的分层（解码队列 → 播放队列 → 播放任务）的简化版。
 *
 * 【容量】2 秒 @24kHz。抖动余量给足，PSRAM 里不差这 96KB。
 * 【溢出策略】丢**最旧**的，不是丢最新的 —— TTS 是流式的，
 *   丢新的会让播放永远落后、延迟越积越大；丢旧的只损失一点点音质，
 *   延迟有界。这和实时音频的通行做法一致。
 * ========================================================================== */
#define PLAY_RING_SAMPLES   (24000 * 2)     /* 2 秒 @24kHz */
#define PLAY_CHUNK_SAMPLES  480             /* 每次取出 20ms，与 I2S 缓冲粒度相称 */

static int16_t          *s_play_ring  = NULL;   /* PSRAM */
static size_t            s_play_wr    = 0;
static size_t            s_play_rd    = 0;
static SemaphoreHandle_t s_play_lock  = NULL;
static volatile bool     s_speaking   = false;
static TaskHandle_t      s_play_task_h = NULL;

/* ==========================================================================
 * 下行 Opus 包的转运队列
 *
 * 【为什么不直接在 xz_on_audio 里解码】—— 这是 2026-10-13 实测撞出来的：
 * `xz_on_audio` 跑在 **esp_websocket_client 自己的任务**里，那个任务的栈
 * 是**组件默认值**，不归我们管。在里面调 `opus_decode()` 的结果是
 *     ***ERROR*** A stack overflow in task websocket_task has been detected.
 * 而且它一崩就是整机复位，连"哪一步崩的"都留不下。
 *
 * 参考实现（xiaozhi）正是这么分层的：接收任务只把包塞进 Decode Queue，
 * 由 `opus_codec` 任务（栈 2048×13 = 26624）去解码。
 * 我们照做 —— 本工程的 `voice_task` 就是那个大栈任务。
 *
 * 【队列容量】24 个包 ≈ **1.4 秒**的下行音频。
 *   ⚠️ 首版只给了 3 格，实测**不够**：服务器下发 TTS 是**成批**的，
 *   不是按播放速率匀速吐（日志里出现过"下行包队列已满，丢弃（累计 21 个）"）。
 *   3 格在成批到达时几毫秒就满了，于是整段整段地丢。
 *
 *   正确的做法是**用缓冲解耦**"成批的生产者"与"实时的消费者"，
 *   而不是指望消费者跑得够快。这段缓冲的意义与后面那个 2 秒 PCM 环形
 *   缓冲相同，只是它存的是**压缩后**的包（省 20 倍内存）。
 *
 *   存储放 **PSRAM**：24 × 1002 ≈ 24KB，内部 RAM 现在很紧；
 *   队列只被 CPU 访问、不做 DMA 源，放 PSRAM 没有性能问题。
 *
 * 【溢出策略】满了丢**新**包并立即返回。绝不能让网络接收任务阻塞 ——
 *   丢一包只是跳一下音，卡住接收任务会把整条连接拖死。
 * ========================================================================== */
typedef struct {
    uint16_t len;
    uint8_t  data[OPUS_MAX_PACKET_SIZE];
} opus_pkt_t;

#define DL_QUEUE_LEN    24
static QueueHandle_t s_dl_q = NULL;
/* 静态分配的队列需要调用方提供控制块与存储区。
   存储区放 PSRAM —— 见上面「队列容量」那段。 */
static StaticQueue_t s_dl_q_ctrl;

/** 把解码后的 PCM 放进环形缓冲。**非阻塞**，可从中断以外的任意任务调用。 */
static void play_ring_put(const int16_t *pcm, size_t n)
{
    if (s_play_ring == NULL || s_play_lock == NULL || n == 0 || n > PLAY_RING_SAMPLES) {
        return;
    }
    if (xSemaphoreTake(s_play_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;                     /* 拿不到锁就丢这一帧，绝不阻塞网络任务 */
    }

    for (size_t i = 0; i < n; i++) {
        s_play_ring[s_play_wr] = pcm[i];
        s_play_wr = (s_play_wr + 1) % PLAY_RING_SAMPLES;
        if (s_play_wr == s_play_rd) {
            /* 满了：把读指针往前推一格 = 丢掉最旧的一个样本 */
            s_play_rd = (s_play_rd + 1) % PLAY_RING_SAMPLES;
        }
    }

    if (!s_speaking) {
        s_speaking = true;
        ui_lvgl_set_talk_state(UI_TALK_SPEAKING);
    }
    xSemaphoreGive(s_play_lock);
}

/** 从环形缓冲取出至多 max 个样本。返回实际取到的数量。 */
static size_t play_ring_take(int16_t *out, size_t max)
{
    if (s_play_ring == NULL || s_play_lock == NULL) {
        return 0;
    }
    if (xSemaphoreTake(s_play_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return 0;
    }

    size_t n = 0;
    while (n < max && s_play_rd != s_play_wr) {
        out[n++] = s_play_ring[s_play_rd];
        s_play_rd = (s_play_rd + 1) % PLAY_RING_SAMPLES;
    }

    xSemaphoreGive(s_play_lock);
    return n;
}

/* ==========================================================================
 * 任务：play_task —— 把环形缓冲里的 PCM 按实时速率送给喇叭
 *
 * `audio_service_play_pcm()` 会**阻塞到数据被 I2S 取走**，所以循环的节奏
 * 天然就是音频的节奏，不需要额外的定时。缓冲空了就退化成 20ms 轮询。
 * ========================================================================== */
static void play_task(void *arg)
{
    (void)arg;

    int16_t *chunk = heap_caps_malloc(PLAY_CHUNK_SAMPLES * sizeof(int16_t),
                                      MALLOC_CAP_INTERNAL);
    if (chunk == NULL) {
        ESP_LOGE(TAG, "play_task 缓冲分配失败，语音播放不可用");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "play_task 就绪（环形缓冲 %d 样本 = 2 秒）",
             (int)PLAY_RING_SAMPLES);

    uint32_t played = 0;        /* 本轮播放的样本数，播完打印一次 */

    for (;;) {
        const size_t n = play_ring_take(chunk, PLAY_CHUNK_SAMPLES);
        if (n == 0) {
            /* 播完了 —— 把按钮从"回复中"还原，并报出这一轮**实际播了多少**。
               这条日志是"喇叭到底响没响"的**唯一客观证据**：
               上层只知道收到了包、解码成功了，但数据有没有真的走到 I2S，
               只有这里能证明。 */
            if (s_speaking) {
                s_speaking = false;
                ui_lvgl_set_talk_state(UI_TALK_IDLE);
                ESP_LOGI(TAG, "本轮播放结束：%u 样本 = %.2f 秒 @%dHz",
                         (unsigned)played, (double)played / AUDIO_SAMPLE_RATE_HZ,
                         AUDIO_SAMPLE_RATE_HZ);
                played = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (audio_service_play_pcm(chunk, n) == ESP_OK) {
            played += (uint32_t)n;
        }
    }
}

/* ==========================================================================
 * 任务：voice_task —— 按键说话的上行链路（P7）
 *
 * 轮询麦克风按钮的"按住"状态，按住期间：
 *     麦克风 → rec_read(960@16k) → Opus 编码 → xz_send_audio()
 * 松手即停止上行并告诉服务器"我说完了"。
 *
 * 【为什么是轮询而不是事件】
 * 按钮状态在界面线程里改（LVGL 回调），上行循环却每 60ms 才醒一次。
 * 用事件就要跨任务传信号量、还要处理"按下-松开发生在两次唤醒之间"的丢失，
 * 而轮询一个 volatile 状态位天然没有这个问题。代价是最多 60ms 的响应延迟，
 * 对人按键说话这个场景完全无感。
 * ========================================================================== */
static void voice_task(void *arg)
{
    (void)arg;

    int16_t *pcm  = malloc(OPUS_UP_FRAME_SAMPLES * sizeof(int16_t));
    uint8_t *opus = malloc(OPUS_MAX_PACKET_SIZE);
    /* 下行解码缓冲放 PSRAM：24kHz 的包解出来 2880 字节，
       而且这块内存只被 CPU 读写、不做 DMA 源，放 PSRAM 没有性能问题；
       放内部 RAM 又要吃掉 4KB，而内部堆已经很紧。 */
    int16_t *dec  = heap_caps_malloc(OPUS_DOWN_MAX_SAMPLES * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM);
    /* 收包缓冲放内部：每轮循环都要用，且只有 1KB */
    opus_pkt_t *pkt = malloc(sizeof(opus_pkt_t));

    if (pcm == NULL || opus == NULL || dec == NULL || pkt == NULL) {
        ESP_LOGE(TAG, "voice_task 缓冲分配失败，语音对话不可用");
        free(pcm);
        free(opus);
        free(dec);
        free(pkt);
        vTaskDelete(NULL);
        return;
    }

    bool     listening   = false;
    uint32_t sent_frames = 0;
    uint32_t sent_bytes  = 0;
    uint32_t down_frames = 0;

    for (;;) {
        /* --- 先把下行队列排空 ---
           解码必须在这个任务里做，不能放 xz_on_audio 回调里
           （那跑在 websocket_task 的栈上，装不下 —— 见 s_dl_q 上方）。
           队列空时这里只是一次零超时的 xQueueReceive 尝试，开销可忽略。 */
        while (xQueueReceive(s_dl_q, pkt, 0) == pdTRUE) {
            size_t dn = 0;
            if (opus_codec_decode(pkt->data, pkt->len, dec,
                                  OPUS_DOWN_MAX_SAMPLES, &dn) == ESP_OK) {
                play_ring_put(dec, dn);
                if (++down_frames == 1) {
                    ESP_LOGI(TAG, "收到第一帧下行语音：%u 字节 → 解码 %u 样本",
                             (unsigned)pkt->len, (unsigned)dn);
                }
            }
        }

        /* 按钮被置灰时 ui_lvgl_talk_held() 恒为 false，
           所以"没连上服务器就按不动"这件事不需要在这里再判一次。 */
        const bool held = ui_lvgl_talk_held() && xz_is_ready();

        if (held && !listening) {
            if (audio_service_rec_start() != ESP_OK) {
                ESP_LOGW(TAG, "录音通路打不开，本次说话作废");
                ui_lvgl_set_talk_state(UI_TALK_IDLE);
                while (ui_lvgl_talk_held()) {
                    vTaskDelay(pdMS_TO_TICKS(50));   /* 等松手，别反复重试 */
                }
                continue;
            }
            /* 清掉上一段对话在 Opus 预测器里的残留，否则开头会带上一次的话音 */
            opus_codec_reset();
            sent_frames = 0;
            if (xz_listen_start() != ESP_OK) {
                ESP_LOGW(TAG, "listen/start 发送失败");
            }
            listening = true;
            ui_lvgl_set_talk_state(UI_TALK_LISTENING);
            ESP_LOGI(TAG, "开始上行（按键说话）");
        } else if (!held && listening) {
            listening = false;
            (void)audio_service_rec_stop();
            (void)xz_listen_stop();
            ui_lvgl_set_talk_state(UI_TALK_WORKING);
            /* 平均帧长是**判断上行到底带没带声音**的关键指标：
               Opus 开了 DTX，静音帧只有 2~5 字节，而 16kbps 的语音帧约 120 字节。
               服务器"收到了但不回话"时，看这个数字就知道是音频的问题还是协议的问题。 */
            ESP_LOGI(TAG, "上行结束：%u 帧 / %u 字节，平均 %u 字节/帧%s",
                     (unsigned)sent_frames, (unsigned)sent_bytes,
                     (sent_frames > 0) ? (unsigned)(sent_bytes / sent_frames) : 0,
                     (sent_frames > 0 && (sent_bytes / sent_frames) < 20)
                         ? "  <-- 太小，基本是静音（查麦克风增益）" : "");
        }

        if (!listening) {
            /* 10ms 而不是 30ms：这个任务在待机时也是**下行包的唯一消费者**，
               轮询间隔直接决定 TTS 从"到齐"到"开声"的延迟。
               10ms 空转的开销可以忽略（一次 xQueueReceive 尝试 + 一次读标志）。 */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        size_t got = 0;
        if (audio_service_rec_read(pcm, OPUS_UP_FRAME_SAMPLES, &got) != ESP_OK
                || got == 0) {
            ESP_LOGW(TAG, "取麦克风数据失败，本轮上行中断");
            listening = false;
            (void)audio_service_rec_stop();
            (void)xz_listen_stop();
            ui_lvgl_set_talk_state(UI_TALK_IDLE);
            continue;
        }

        size_t olen = 0;
        if (opus_codec_encode(pcm, opus, OPUS_MAX_PACKET_SIZE, &olen) != ESP_OK) {
            continue;               /* 单帧编码失败不致命，继续下一帧 */
        }
        if (xz_send_audio(opus, olen) == ESP_OK) {
            sent_bytes += (uint32_t)olen;
            sent_frames++;
        }
    }
}

/* ==========================================================================
 * 小智语音对话的回调（P7）
 *
 * ⚠️ 这些回调跑在 **xz_client 自己的任务**上下文（不是 ui_task）。
 *    直接调 `ui_lvgl_chat_add()` 是**安全**的 —— 它是 ui_lvgl 的对外接口，
 *    内部自己取 LVGL 递归锁（见 ui_lvgl.h 的线程约定）。
 *    但**不能**在这里调裸 LVGL 函数，也不能做耗时的事（会拖住 xz 任务）。
 * ========================================================================== */
static void xz_on_state(xz_state_t st, const char *detail)
{
    /* 状态在重连循环里会被反复刷新（每几秒一次），所以只在**进入关键状态**时
       才往聊天流里说一句 —— 否则会把界面刷满"正在取配置"。 */
    static xz_state_t s_last = XZ_STATE_IDLE;

    if (st == s_last) {
        return;
    }
    s_last = st;

    switch (st) {
    case XZ_STATE_NEED_ACTIVATION: {
        /* 激活码要显眼 —— 用户得在小智 App 里把它敲进去 */
        const char *code = xz_activation_code();
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "需要激活\n请在小智 App 里输入：\n【 %s 】",
                 (code != NULL) ? code : "??????");
        ui_lvgl_chat_add(UI_CHAT_SYSTEM, buf);
        break;
    }
    case XZ_STATE_READY:
        ui_lvgl_chat_add(UI_CHAT_SYSTEM, "小智已连接。");
        /* 连上了才让语音按钮按得动 —— 在此之前的按下都无处可发 */
        ui_lvgl_set_talk_ready(true);
        /* 解码器必须按**服务器 hello 给的**采样率建，不能用我们的上行 16kHz 想当然。
           服务器几乎总是回 24kHz，但这是它的选择，不是我们的常量。 */
        (void)opus_codec_set_down(xz_down_sample_rate(), XZ_DOWN_FRAME_MS_DEF);
        break;
    case XZ_STATE_ERROR:
        ui_lvgl_set_talk_ready(false);
        if (detail != NULL) {
            ui_lvgl_chat_add(UI_CHAT_SYSTEM, detail);
        }
        break;
    default:
        /* IDLE/OTA/CONNECTING/ACTIVATING 只打日志，不打扰用户；
           但要**收回语音按钮** —— 离开 READY（含掉线重连）后按下去也没有
           地方可发，留着它按得动只会制造一个更难查的状态。
           本函数顶部有"状态没变就直接返回"的短路，所以这里不会反复刷界面。 */
        ui_lvgl_set_talk_ready(false);
        break;
    }
}

static void xz_on_chat(const char *role, const char *text)
{
    if (role == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    /* 打日志而不只是往界面塞 —— 界面在板子上，排障时看不到。
       这条日志是"服务器到底听懂了什么、回了什么"的唯一可读证据。 */
    ESP_LOGI(TAG, "对话[%s] %s", role, text);

    ui_chat_role_t r = UI_CHAT_ASSISTANT;
    if (strcmp(role, "user") == 0) {
        r = UI_CHAT_USER;
    } else if (strcmp(role, "system") == 0) {
        r = UI_CHAT_SYSTEM;
    }
    ui_lvgl_chat_add(r, text);
}

static void xz_on_audio(const uint8_t *opus, size_t len)
{
    /* ⚠️⚠️ 本回调跑在 **esp_websocket_client 自己的任务**里，那个任务的栈
     * 是组件默认值、不归我们控制，**装不下 Opus 解码**。
     * 在这里解码的实测后果是整机复位（见 s_dl_q 上方的说明）。
     * 所以这里只做一件事：**入队**，几微秒返回。
     * 解码与播放分别由 voice_task（大栈）和 play_task 负责。 */
    if (s_dl_q == NULL || opus == NULL || len == 0 || len > OPUS_MAX_PACKET_SIZE) {
        return;
    }

    opus_pkt_t pkt;
    pkt.len = (uint16_t)len;
    memcpy(pkt.data, opus, len);

    /* 超时 0：队列满就丢**这一个新包**并返回。
       绝不能让网络接收任务因为"播放跟不上"而阻塞 —— 丢一包只是跳一下音，
       卡住接收任务会把整条连接拖死。 */
    if (xQueueSend(s_dl_q, &pkt, 0) != pdTRUE) {
        static uint32_t s_dropped = 0;
        if (++s_dropped % 20 == 1) {
            ESP_LOGW(TAG, "下行包队列已满，丢弃（累计 %u 个）", (unsigned)s_dropped);
        }
    }
}

/* ==========================================================================
 * 任务：ui_task
 *
 * P0 阶段先建立任务骨架与刷新节拍；LVGL 初始化在 P2 接入。
 * 所有 LVGL 调用必须且只能出现在本任务内。
 * ========================================================================== */
static void ui_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "ui_task 启动 (优先级 %d, 核心 %d, 栈 %d)",
             APP_TASK_PRIO_UI, APP_TASK_CORE_1, APP_TASK_STACK_UI);

    /* ======================================================================
     * P2：LVGL 初始化
     *
     * 在本任务内做 —— 本工程约定「LVGL 的初始化与显示对象创建只发生在 ui_task」。
     * 前置条件：lcd_init() 已由 p05_run() 完成（ui_lvgl_init() 内部会检查
     * lcd_is_ready()，不满足直接返回错误）。
     *
     * 注意它会阻塞十余秒（开屏色卡自检，见 ui_lvgl_config.h），这是刻意的：
     * 那一屏自查一次性覆盖「LVGL 渲染 → 标准 RGB565 直通 → SPI → DMA 完成」
     * 整条新路径，且把判据压进**同一张照片**，不依赖人工记忆顺序。
     * ====================================================================== */

    /* ---- 中文字体服务（必须在建任何文字控件之前）----
     * 把 flash 的 `fonts` 分区里那个覆盖面更大的 Noto 字体挂成普惠体的 fallback，
     * 解决 AI 回复里偶尔出现的生僻字画成方框的问题（详见 font_service.h）。
     * **不致命**：分区缺失/内容不对时它只打一条 WARN，界面照常。 */
    (void)font_service_init();
    ESP_LOGI(TAG, "[字体] %s", font_service_status());

    const esp_err_t iret = ui_lvgl_init();
    if (iret != ESP_OK) {
        /* 刻意**不**删除本任务：monitor_task 持有它的句柄，对已删除任务的
           句柄调 uxTaskGetStackHighWaterMark 是未定义行为。
           保留任务空转，代价可忽略。 */
        ESP_LOGE(TAG, "ui_lvgl_init 失败: %s —— 界面停用，其余功能不受影响",
                 esp_err_to_name(iret));
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    /* ---- 开屏往聊天流里放一条系统提示 ----
       只说明**当前能做什么**，不播报数据。

       【2026-10-14 去掉了一条"鱼缸基本情况汇报"】那条原本是这样的：
           ui_lvgl_chat_add(UI_CHAT_ASSISTANT, "水温 25.3℃，pH 7.10，…");
       它读的是真快照（eco_snapshot，不是写死的字符串），但去掉有三个理由：

       ① **它挂着小智的名义，而小智根本没说过这句** —— 用的是助手角色，
          看起来就是 AI 的开场白，实际是本机拼的。这是三条里最要紧的一条：
          等于把没发生过的话记在 AI 头上，比数据不准更难发现。
       ② **数值来自模拟器**。ADS1115 没有采购（I2C 扫描每次都在报
          "[ 缺失 ] ADS1115 水质ADC"），s_eco 里的数全是 sensor_task 随机游走
          生成的。播报它 = 拿模拟值冒充实测。
       ③ **它是开机那一下的快照**，播完模拟器继续游走，几秒后屏幕上那串
          数字就跟当前值不一样了 —— 看起来像"读数"，实际是个历史值。

       数据本身没问题，也不缺地方显示：「鱼缸情况」页六张卡片就是干这个的，
       而且卡片变红比一行字清楚得多。聊天流从此**只出现真实发生过的事**。 */

    /* 按键的用法要写出来：语音键现在只剩一个**图标**，「按住」这个动作
       光看麦克风图标猜不到（图标通常意味着"点一下"）。
       连接状态由 xz_on_state 那条"小智已连接。"负责，不在这里抢话说。 */
    ui_lvgl_chat_add(UI_CHAT_SYSTEM, "按住下方麦克风键说话。");

    /* 摄像头可用性一次性问清并**写进聊天流**。
       P6-a 时这一步是控制「拍照」按钮的置灰；那个按钮 2026-10-14 去掉了，
       但这条信息仍然有用 —— 拍照现在由 AI 驱动，摄像头要是坏的，
       用户会只看到"AI 说它看不到"，在这里先把原因说清楚，省得去翻串口。 */
    if (!camera_service_is_ready()) {
        ESP_LOGW(TAG, "摄像头不可用（AI 拍照会失败）：%s", camera_service_status());
        ui_lvgl_chat_add(UI_CHAT_SYSTEM, camera_service_status());
    }

    const TickType_t period = pdMS_TO_TICKS(APP_UI_REFRESH_PERIOD_MS);
    TickType_t last_wake = xTaskGetTickCount();
    TickType_t last_data = 0;
    uint32_t   loop = 0;

    for (;;) {
        /*
         * 100Hz 时基 => 1 tick = 10ms。APP_UI_REFRESH_PERIOD_MS = 30ms
         * 不是 10ms 的整数倍，pdMS_TO_TICKS 会取整，因此必须用
         * vTaskDelayUntil 的绝对节拍推进，避免误差累积。
         */
        vTaskDelayUntil(&last_wake, period);

        /* 相册刷新：用户进入相册页时由 ui_pages_show() 置位。
           扫卡 + 解码 6 张 JPEG 要几百毫秒，整个动作在 **LVGL 锁外**执行 ——
           持锁做会把界面连同触摸一起冻住。
           按 30ms 节拍检查而不是跟着下面的 200ms 数据节流走：用户按了菜单
           就该尽快看到图，不该再等一个数据周期。 */
        if (ui_lvgl_album_refresh_requested()) {
            ui_album_refresh();
        }

        /* 告警变化 → 往聊天流里发一条**系统**消息。
           为什么用系统角色而不是助手角色：告警是"设备在通知你"，
           不是"助手在回答你"。用助手角色时它前面没有对应的用户提问，
           看起来就像在**自问自答**（用户实测反馈）。
           系统消息居中灰底，语义正确；而且它与上一条系统消息会自动折叠，
           不会堆一屏。
           由本任务代发，因为约定「所有 LVGL 调用只在 ui_task 内」
           （sensor_task 只负责写文本 + 置标志）。 */
        if (s_alert_msg_ready) {
            s_alert_msg_ready = false;
            ui_lvgl_chat_add(UI_CHAT_SYSTEM, s_alert_msg);

            /* 同时上报到巴法云。
               ⚠️ 2026-10-14 之前这里**只喂了屏幕** —— `bafa_mqtt_post_alert()`
               写了却一个调用者都没有，所以"远程查看告警"这件事从来没有真正
               成立过：人不在设备跟前就永远不知道出过告警。
               投递是入队（非阻塞），不会拖住 ui_task。 */
            (void)bafa_mqtt_post_alert(s_alert_msg);
        }

        /* 界面数据不需要 30ms 这么密：读快照本身很便宜，但每次写入标签
           都会让 LVGL 标脏并重绘。按 APP_UI_DATA_PERIOD_MS 节流。 */
        const TickType_t now = xTaskGetTickCount();
        if ((now - last_data) < pdMS_TO_TICKS(APP_UI_DATA_PERIOD_MS)) {
            continue;
        }
        last_data = now;

        tank_ecosystem_t snap;
        eco_snapshot(&snap);          /* 互斥拷贝一份快照，不与写者竞争 */

        /* 内部完成：上锁 → 更新控件 → 解锁 → 唤醒渲染任务。
           取锁超时返回 false，属正常节流，不报错。 */
        (void)ui_lvgl_update(&snap);

        /* 每 10 秒打一次心跳。用 ESP_LOGD 以便需要时用日志等级过滤，
           避免长期运行时刷屏。 */
        if (++loop % (10000 / APP_UI_DATA_PERIOD_MS) == 0) {
            ESP_LOGD(TAG, "ui_task 心跳 %"PRIu32, loop);
        }
    }
}

/* ==========================================================================
 * 任务：mqtt_task
 *
 * **本任务是唯一允许调用 MQTT 发布的地方**（经 bafa_mqtt_pump）。
 * 其它任务要上报数据，调用 bafa_mqtt_post_* 投递到队列即可。
 * ========================================================================== */
static void mqtt_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "mqtt_task 启动 (优先级 %d, 核心 %d, 栈 %d)",
             APP_TASK_PRIO_MQTT, APP_TASK_CORE_0, APP_TASK_STACK_MQTT);

    const TickType_t period      = pdMS_TO_TICKS(APP_MQTT_REPORT_PERIOD_MS);
    TickType_t       last_report = xTaskGetTickCount();
    TickType_t       last_warn   = xTaskGetTickCount();

    for (;;) {
        /* 阻塞等待待发消息，最长 500ms。这样既有事件驱动的低延迟，
           又能周期性回到循环顶部检查「该不该做周期上报了」。 */
        const esp_err_t ret = bafa_mqtt_pump(500);

        /* 只把「真正的意外」当告警，且做 10 秒节流。
           ESP_ERR_TIMEOUT（无消息）是常态；
           ESP_ERR_INVALID_STATE 表示尚未连接或服务未就绪，属于预期状态。
           早期版本对每个非 OK 返回都打日志，在服务未就绪时每秒刷两条，
           把有效日志全淹了——日志必须能反映异常，而不是反映正常波动。 */
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            const TickType_t now = xTaskGetTickCount();
            if ((now - last_warn) >= pdMS_TO_TICKS(10000)) {
                last_warn = now;
                ESP_LOGW(TAG, "pump 返回 %s（MQTT %s）",
                         esp_err_to_name(ret),
                         bafa_mqtt_is_ready() ? "已就绪未连接" : "尚未就绪");
            }
        }

        /* 周期上报：把当前生态状态投递到发送队列（实际发送由下一轮 pump 完成） */
        const TickType_t now = xTaskGetTickCount();
        if ((now - last_report) >= period) {
            last_report = now;

            eco_refresh_links();
            tank_ecosystem_t snapshot;
            eco_snapshot(&snapshot);

            /* 仅在 MQTT 已连接时才入队，避免队列被未连接时的无效消息填满 */
            if (bafa_mqtt_is_connected()) {
                const esp_err_t perr = bafa_mqtt_post_status(&snapshot);
                if (perr != ESP_OK) {
                    ESP_LOGW(TAG, "状态入队失败: %s", esp_err_to_name(perr));
                }
            }
        }
    }
}

/* ==========================================================================
 * 任务：monitor_task
 * ========================================================================== */
static void monitor_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "monitor_task 启动 (优先级 %d, 核心 %d, 栈 %d)",
             APP_TASK_PRIO_MONITOR, APP_TASK_CORE_1, APP_TASK_STACK_MONITOR);

    const TickType_t period = pdMS_TO_TICKS(APP_MONITOR_PERIOD_MS);
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, period);

        log_net_status();
        log_memory_report();

        /* 任务栈高水位：验收指标要求余量 >= 30%。
           ⚠️ uxTaskGetStackHighWaterMark() 在 ESP-IDF 中返回的是**字节**，
           不是标准 FreeRTOS 文档所说的"字"（IDF 的 task.h 里明确写了
           "in bytes (as opposed to words in the standard FreeRTOS documentation)"）。
           初版把它当字打印，读数看似宽裕，实际是 1/4 的余量。 */
        for (int i = 0; i < s_monitored_cnt; i++) {
            if (s_monitored[i].handle == NULL) {
                continue;
            }
            const UBaseType_t hw_bytes = uxTaskGetStackHighWaterMark(s_monitored[i].handle);
            ESP_LOGI(TAG, "任务 %-14s 栈余量 %u 字节%s",
                     s_monitored[i].name, (unsigned)hw_bytes,
                     hw_bytes < 1024 ? "  <-- 不足 1KB，需加大" : "");
            if (hw_bytes < 1024) {
                ESP_LOGW(TAG, "任务 %s 栈余量告急 (%u 字节)！",
                         s_monitored[i].name, (unsigned)hw_bytes);
            }
        }
    }
}

/* ==========================================================================
 * 入口
 * ========================================================================== */
void app_main(void)
{
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, " 智能鱼缸 AI 生态监测系统 — P0 基础框架");
    ESP_LOGI(TAG, " 芯片: %s  IDF: %s", CONFIG_IDF_TARGET, esp_get_idf_version());
    ESP_LOGI(TAG, " FreeRTOS 时基: %d Hz (1 tick = %d ms)",
             CONFIG_FREERTOS_HZ, 1000 / CONFIG_FREERTOS_HZ);
    ESP_LOGI(TAG, "=================================================");

    /* 1. NVS */
    ESP_ERROR_CHECK(app_nvs_init());

    /* 2. 事件总线（必须先于任何任务与外设服务创建） */
    ESP_ERROR_CHECK(app_events_init());

    /* 3. 全局生态状态 */
    ESP_ERROR_CHECK(eco_init());

    /* 4. 网络状态事件订阅 */
    ESP_ERROR_CHECK(app_events_subscribe(EVENT_WIFI_CONNECTED,    on_wifi_event, NULL));
    ESP_ERROR_CHECK(app_events_subscribe(EVENT_WIFI_DISCONNECTED, on_wifi_event, NULL));
    ESP_ERROR_CHECK(app_events_subscribe(EVENT_MQTT_CONNECTED,    on_mqtt_event, NULL));
    ESP_ERROR_CHECK(app_events_subscribe(EVENT_MQTT_DISCONNECTED, on_mqtt_event, NULL));

    /* 5. 板级 I2C 与 IO 扩展 */
    ESP_ERROR_CHECK(gec_board_i2c_init());

    /* 6. 外设自检：I2C 扫描 + 关键器件存在性 */
    const esp_err_t selftest = gec_board_selftest();
    if (selftest != ESP_OK) {
        ESP_LOGE(TAG, "板级自检未通过 (%s)，仍继续启动任务以便串口诊断",
                 esp_err_to_name(selftest));
    }

    /* 6.5 P0.5 硬件验证试验（会阻塞数秒，完成后才继续联网） */
    p05_run();

    /* 6.6 摄像头（P6-a）
       放在 lcd_init() 之后：它要经 PCA9557 P2 上电，而 PCA9557 挂在 I2C 上
       （已在第 5 步初始化）。**失败不致命** —— 界面照常显示，只是拍照不可用，
       界面上会给出原因。故意用非 ESP_ERROR_CHECK 的写法，避免摄像头成为
       整机启动的单点故障。 */
    const esp_err_t cret = camera_service_init();
    if (cret != ESP_OK) {
        ESP_LOGW(TAG, "摄像头不可用: %s —— %s（其余功能不受影响）",
                 esp_err_to_name(cret), camera_service_status());
    }

    /* 6.7 音频服务（P7）
       ⚠️ 这一处此前是**缺口**：`audio_service_init()` 全工程只出现在三个
       p05_* 试验函数里，而那三个开关（P05_TEST_AUDIO / P05_TEST_C1_I2S_COLLISION /
       P05_TEST_AUDIO_LCD_ORDER）**全是 0**。也就是说 P0.5 之后，正式启动流程里
       音频一直是未初始化状态 —— 喇叭当时响过，但那是试验代码做的。
       P7 的语音对话要真用音频，必须在这里补上。

       与摄像头一样**失败不致命**：界面、传感器、巴法云都不受影响。 */
    const esp_err_t aret = audio_service_init();
    if (aret != ESP_OK) {
        ESP_LOGW(TAG, "音频不可用: %s —— 语音对话将无法工作（其余功能不受影响）",
                 esp_err_to_name(aret));
    } else {
        /* 播放通路常开（含功放使能）。**不等到要说话时才开** ——
           esp_codec_dev 的通道启停不做幂等检查，反复 open/close 会失败，
           这是 P0.5 已经栽过一次的坑（见 audio_service.c 顶部与 architecture.md 7.2）。
           xiaozhi 的 AudioService::Start() 同样是一上来就 EnableOutput(true)。 */
        (void)audio_service_enable_output(true);

#if APP_TEST_MIC_PROBE
        /* 第一层：回环自证 —— 板子自己响/静交替，验**硬件通路**，
           不依赖用户在不在说话。首轮实测对比 ≈15.8 倍 ⇒ 麦克风正常。 */
        ESP_LOGW(TAG, "===== 麦克风回环自证：板子会自己响/静交替，请保持安静 =====");
        vTaskDelay(pdMS_TO_TICKS(1000));
        (void)audio_service_mic_loopback(4000);

        /* 第二层：用**对话真正要走的那条 API** 再跑一遍。
           回环验的是硬件，这里验的是封装本身（rec_start / rec_read /
           24k→16k 重采样 / rec_stop）—— 两者失败的原因完全不同，
           混在一起就分不清是硬件还是代码。 */
        if (audio_service_rec_start() == ESP_OK) {
            const int    n_frames = 50;                 /* 50 × 60ms = 3 秒 */
            int16_t     *pcm = malloc(AUDIO_REC_FRAME_SAMPLES * sizeof(int16_t));
            if (pcm != NULL) {
                const int64_t t0 = esp_timer_get_time();
                size_t  total = 0;
                double  sq    = 0.0;
                int16_t pk    = 0;
                for (int f = 0; f < n_frames; f++) {
                    size_t got = 0;
                    if (audio_service_rec_read(pcm, AUDIO_REC_FRAME_SAMPLES, &got) != ESP_OK) {
                        ESP_LOGW(TAG, "第 %d 帧读取失败，提前结束", f + 1);
                        break;
                    }
                    for (size_t i = 0; i < got; i++) {
                        const double v = (double)pcm[i] / 32768.0;
                        sq += v * v;
                        const int16_t a = (pcm[i] < 0) ? (int16_t)-pcm[i] : pcm[i];
                        if (a > pk) {
                            pk = a;
                        }
                    }
                    total += got;
                }
                const int64_t dt = esp_timer_get_time() - t0;
                ESP_LOGW(TAG, "===== 录音 API 自检 =====");
                ESP_LOGW(TAG, "  %u 样本 / %.2f 秒 = **%.0f Hz**（期望 16000）%s",
                         (unsigned)total, (double)dt / 1e6,
                         (double)total * 1e6 / (double)dt,
                         (total * 1000000.0 / (double)dt < 15000) ? "  <-- 偏低，需查" : "");
                ESP_LOGW(TAG, "  峰值 %d，RMS %.5f", (int)pk, sqrt(sq / (double)total));
                ESP_LOGW(TAG, "========================");
                free(pcm);
            }
            (void)audio_service_rec_stop();
        } else {
            ESP_LOGE(TAG, "rec_start 失败 —— 音频输入通路没起来");
        }
#endif
    }

    /* 6.8 TF 卡存储（P6-c）
       与摄像头/音频同一条原则：**失败不致命**。没插卡、卡是 exFAT、
       接线没接好 —— 都不该让鱼缸停摆，只是相册页会显示"卡不可用"。
       `sd_service_status()` 里已经备好中文原因，界面直接拿去显示。 */
    const esp_err_t sdret = sd_service_init();
    if (sdret != ESP_OK) {
        ESP_LOGW(TAG, "TF 卡不可用: %s（相册功能不可用，其余不受影响）",
                 sd_service_status());
    } else {
        /* P0.5 的最后一格：**写入 → 读回 → 逐字节比对**。
           为什么"挂载成功"不算验证过：DAT0 接触不良可以让卡正常初始化，
           却在数据阶段丢字节 —— 而那种故障平时完全看不出来，
           要等到某张照片存坏了才发现。逐字节比对是唯一能钉死它的判据。 */
        (void)sd_service_selftest();
        /* 再把"读一张真照片并解码"验掉 —— 与上面那条验的不是同一件事，
           失败原因也完全不同（上面是总线/接触，这里是文件读取+解码）。 */
        sd_probe_newest_thumb();
    }

    /* 7. 先启动网络服务，再创建依赖它的任务。
       顺序很重要：mqtt_task 一启动就会调用 bafa_mqtt_pump()，若此时发送队列
       还没建好，就会走「服务未就绪」分支。虽然 pump 已经改为等待超时，
       但从源头消除这个窗口更干净。两步都**不阻塞**——Wi-Fi 由事件驱动重连，
       MQTT 由退避定时器驱动重连。 */
    esp_err_t nret = wifi_sta_start();
    if (nret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 启动失败: %s（设备将保持离线运行）", esp_err_to_name(nret));
    }

    /* 对时。**非阻塞**：内部起一个一次性任务，先等 Wi-Fi 连上再发 SNTP 请求。
       为什么必须有它：系统时钟上电停在 1970，照片文件名就只能退回
       `boot_<开机秒数>` —— 而那个秒数每次重启归零，会让"最近 N 张"
       的排序跨重启乱掉（新照片排到旧照片前面）。对时后文件名变成
       `photo_20261014_140808.jpg`，字典序即时间序。
       失败不致命：退回兜底命名，其余功能不受影响。 */
    net_time_sync_start();

    nret = bafa_mqtt_start();
    if (nret != ESP_OK) {
        ESP_LOGE(TAG, "MQTT 启动失败: %s（设备将保持离线运行）", esp_err_to_name(nret));
    }

    /* 8. 建立任务边界 */
    TaskHandle_t h_ui = NULL, h_mqtt = NULL, h_mon = NULL, h_sensor = NULL;

    BaseType_t ok = xTaskCreatePinnedToCore(ui_task, "ui_task",
                                            APP_TASK_STACK_UI, NULL,
                                            APP_TASK_PRIO_UI, &h_ui,
                                            APP_TASK_CORE_1);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    monitor_register("ui_task", h_ui);

    ok = xTaskCreatePinnedToCore(mqtt_task, "mqtt_task",
                                 APP_TASK_STACK_MQTT, NULL,
                                 APP_TASK_PRIO_MQTT, &h_mqtt,
                                 APP_TASK_CORE_0);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    monitor_register("mqtt_task", h_mqtt);

    /* sensor_task（P1 骨架，当前跑模拟数据）。
       放在 monitor_task 之前创建，这样第一次健康检查就能看到它的栈水位。 */
    ok = xTaskCreatePinnedToCore(sensor_task, "sensor_task",
                                 APP_TASK_STACK_SENSOR, NULL,
                                 APP_TASK_PRIO_SENSOR, &h_sensor,
                                 APP_TASK_CORE_1);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    monitor_register("sensor_task", h_sensor);

    /* 8.4 语音编解码与播放链路（P7）
       放在 xz_client_start() **之前**：客户端一连上就可能收到下行音频，
       那时环形缓冲与 play_task 必须已经就位，否则第一批音频会被静默丢弃。

       失败不致命 —— 界面、传感器、巴法云照常；只是语音对话用不了。 */
    const esp_err_t oret = opus_codec_init();
    if (oret != ESP_OK) {
        ESP_LOGW(TAG, "Opus 初始化失败: %s（语音对话不可用）", esp_err_to_name(oret));
    } else {
        /* 环形缓冲放 PSRAM：96KB，内部 RAM 现在很紧，放不起。
           它只被 CPU 访问（不做 DMA 源），放 PSRAM 没有性能问题。 */
        s_play_ring = heap_caps_malloc(PLAY_RING_SAMPLES * sizeof(int16_t),
                                       MALLOC_CAP_SPIRAM);
        /* 下行包队列：24 个包 × (1000+2) 字节 ≈ 24KB，**放 PSRAM**。
           容量与存放位置的取舍见 opus_pkt_t 上方的「队列容量」一段。 */
        uint8_t *dq_stor = heap_caps_malloc(DL_QUEUE_LEN * sizeof(opus_pkt_t),
                                            MALLOC_CAP_SPIRAM);
        if (dq_stor != NULL) {
            s_dl_q = xQueueCreateStatic(DL_QUEUE_LEN, sizeof(opus_pkt_t),
                                        dq_stor, &s_dl_q_ctrl);
        }
        if (s_play_ring == NULL || s_dl_q == NULL) {
            ESP_LOGW(TAG, "语音缓冲分配失败（PSRAM/内部堆不足？），对话将不可用");
        } else {
            s_play_lock = xSemaphoreCreateMutex();
            if (s_play_lock == NULL) {
                ESP_LOGW(TAG, "播放锁创建失败，下行语音将无声");
            } else {
                ok = xTaskCreatePinnedToCore(play_task, "play_task",
                                             APP_TASK_STACK_PLAY, NULL,
                                             APP_TASK_PRIO_PLAY, &s_play_task_h,
                                             APP_TASK_CORE_0);
                if (ok != pdPASS) {
                    ESP_LOGW(TAG, "play_task 创建失败，下行语音将无声");
                } else {
                    monitor_register("play_task", s_play_task_h);
                }
            }
        }

        /* 上行任务常驻：它自己轮询麦克风按钮，没按键时每 30ms 空转一次。
           做成常驻而不是"按下才创建"，是因为按键说话要求**按下即开始录**，
           而创建任务 + 首次取帧的延迟会实打实地吃掉开头几百毫秒。 */
        TaskHandle_t h_voice = NULL;
        ok = xTaskCreatePinnedToCore(voice_task, "voice_task",
                                     APP_TASK_STACK_VOICE, NULL,
                                     APP_TASK_PRIO_VOICE, &h_voice,
                                     APP_TASK_CORE_0);
        if (ok != pdPASS) {
            ESP_LOGW(TAG, "voice_task 创建失败，上行语音不可用");
        } else {
            monitor_register("voice_task", h_voice);
        }
    }

    /* 8.5 小智语音客户端（P7）
       它**非阻塞**：内部只创建一个任务就返回，连接/激活/重连全在任务里做。
       放在这里（Wi-Fi 已启动之后）而不是更早，是为了让它第一次循环时
       Wi-Fi 大概率已就绪，少打几轮"等待 Wi-Fi"。

       失败不致命 —— 界面、传感器、巴法云都不受影响。 */

    /* 回调必须在 start() **之前**注册，否则会漏掉最早几个状态变化
       （尤其是"需要激活"——那是最该被看到的一条）。 */
    const xz_callbacks_t xz_cbs = {
        .on_state = xz_on_state,
        .on_chat  = xz_on_chat,
        .on_audio = xz_on_audio,
    };
    xz_set_callbacks(&xz_cbs);

    /* MCP 工具的数据源。同样必须在 start() **之前**注册：服务器在 hello 之后
       **立刻**就会发工具请求（P7 收尾时实测到的），注册晚了那一次只能回
       "取不到数据"，而 LLM 只会看到一次失败。 */
    xz_set_tank_status_provider(tank_status_provider);

    /* 拍照工具的数据源。同样要在 start() 之前注册 ——
       工具表在连接建立时就报给服务器了，数据源后注册的话，
       模型第一次调拍照就会拿到"没注册"这条路。 */
    xz_set_camera_capture_provider(camera_capture_provider);

    const esp_err_t xret = xz_client_start();
    if (xret != ESP_OK) {
        ESP_LOGW(TAG, "小智客户端启动失败: %s（其余功能不受影响）",
                 esp_err_to_name(xret));
    } else {
        /* 纳入栈水位监控。这个任务要跑 **TLS 握手**，是全工程栈最吃紧的一个，
           必须能看到它的高水位读数。任务名见 xiaozhi_client.c 的 xTaskCreate。 */
        monitor_register("xz_client", xTaskGetHandle("xz_client"));
        /* 工具任务也要盯 —— 它跑 JPEG 编码 + esp_http_client，是本工程里
           调用链最深的一条（初值照 xiaozhi 的 8192，要靠实测水位来收）。
           ⚠️ 这是第 8 格，加完 s_monitored[] 就满了：再加任务必须先扩容
              APP_MAX_MONITORED_TASKS，否则 monitor_register 会静默丢弃。 */
        monitor_register(XZ_TOOL_TASK_NAME, xTaskGetHandle(XZ_TOOL_TASK_NAME));
    }

    ok = xTaskCreatePinnedToCore(monitor_task, "monitor_task",
                                 APP_TASK_STACK_MONITOR, NULL,
                                 APP_TASK_PRIO_MONITOR, &h_mon,
                                 APP_TASK_CORE_1);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    monitor_register("monitor_task", h_mon);

    ESP_LOGI(TAG, "P0 启动完成：%d 个任务运行中", s_monitored_cnt);
    ESP_LOGI(TAG, "巴法云主题: 状态=%s  告警=%s  AI=%s  设备=%s  命令=%s",
             BAFA_TOPIC_SENSOR, BAFA_TOPIC_ALERT, BAFA_TOPIC_AI,
             BAFA_TOPIC_STATUS, BAFA_SUBSCRIBE_TOPIC);
    log_memory_report();

    /* app_main 返回后由 FreeRTOS 回收其任务，业务全部由上述任务承担 */
}
