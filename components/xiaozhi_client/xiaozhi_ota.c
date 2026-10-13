/**
 * @file xiaozhi_ota.c
 * @brief 向小智服务器取配置（WebSocket 地址/token）与设备激活（P7）
 *
 * 逐字照搬 xiaozhi `main/ota.cc` + `main/boards/common/board.cc`。
 * 每个字段/请求头都标了出处，以便日后核对。
 *
 * ── 一句提醒：这里的"怪写法"多半是有意的 ──────────────────────────────────
 * 例如 `minimum_free_heap_size` 是**带引号的字符串**而不是数字 ——
 * 那是 xiaozhi 原文（board.cc:111）如此，服务器可能就按字符串解析。
 * 想"顺手修正"之前先想清楚：这是协议，不是我们自己的代码风格问题。
 */
#include "xiaozhi_internal.h"
#include "xiaozhi_config.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_flash.h"
#include "esp_chip_info.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "xz_ota";

/* 激活信息 —— 由 xz_ota_fetch() 填充，界面通过 xz_activation_code() 取用 */
static char     s_activation_code[16]   = "";
static char     s_activation_challenge[80] = "";
static char     s_activation_message[96] = "";
static uint32_t s_activation_timeout_ms = 0;

/* ==========================================================================
 * 设备标识
 * ========================================================================== */
const char *xz_device_id(void)
{
    static char s_mac[18];
    static bool s_done = false;

    if (!s_done) {
        uint8_t m[6];
        /* ⚠️ 必须用 **Wi-Fi STA** 的 MAC。
           esp_read_mac() 支持多种类型，拿错类型会得到不同的地址，
           服务器就会把它当成一台新设备、要求重新激活。
           出处：xiaozhi `system_info.cc:34-44` 传的是 ESP_MAC_WIFI_STA。 */
        esp_read_mac(m, ESP_MAC_WIFI_STA);
        snprintf(s_mac, sizeof(s_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
        s_done = true;
    }
    return s_mac;
}

const char *xz_client_id(void)
{
    static char s_uuid[37];
    static bool s_done = false;

    if (s_done) {
        return s_uuid;
    }

    nvs_handle_t h;
    /* namespace/键名与 xiaozhi 一致（board/uuid）—— 这样来回刷固件不会变成新设备 */
    esp_err_t ret = nvs_open("board", NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s —— 无法持久化 UUID", esp_err_to_name(ret));
        s_uuid[0] = '\0';
        s_done = true;
        return s_uuid;
    }

    size_t len = sizeof(s_uuid);
    if (nvs_get_str(h, "uuid", s_uuid, &len) == ESP_OK && s_uuid[0] != '\0') {
        s_done = true;
        nvs_close(h);
        ESP_LOGI(TAG, "设备 UUID（NVS 已有）: %s", s_uuid);
        return s_uuid;
    }

    /* 首次：生成标准 UUIDv4 并落盘（board.cc:24-45 同款） */
    uint8_t r[16];
    esp_fill_random(r, sizeof(r));
    r[6] = (uint8_t)((r[6] & 0x0F) | 0x40);   /* 版本 4 */
    r[8] = (uint8_t)((r[8] & 0x3F) | 0x80);   /* 变体 1 */
    snprintf(s_uuid, sizeof(s_uuid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
             r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);

    if (nvs_set_str(h, "uuid", s_uuid) == ESP_OK) {
        (void)nvs_commit(h);
        ESP_LOGI(TAG, "首次生成设备 UUID: %s（存于 NVS board/uuid）", s_uuid);
    } else {
        ESP_LOGW(TAG, "UUID 写入 NVS 失败 —— 下次开机会重新生成，服务器会当成新设备");
    }
    nvs_close(h);

    s_done = true;
    return s_uuid;
}

/* ==========================================================================
 * 请求体（board.cc:69-161 的等价物）
 * ========================================================================== */
static cJSON *build_body(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddNumberToObject(root, "version", 2);
    cJSON_AddStringToObject(root, "language", XZ_LANGUAGE);

    uint32_t flash_size = 0;
    (void)esp_flash_get_size(NULL, &flash_size);
    cJSON_AddNumberToObject(root, "flash_size", (double)flash_size);

    /* ⚠️ **字符串**，不是数字 —— 见文件头的提醒（board.cc:111）。 */
    char heap_str[16];
    snprintf(heap_str, sizeof(heap_str), "%u",
             (unsigned)esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(root, "minimum_free_heap_size", heap_str);

    cJSON_AddStringToObject(root, "mac_address", xz_device_id());
    cJSON_AddStringToObject(root, "uuid", xz_client_id());
    cJSON_AddStringToObject(root, "chip_model_name", CONFIG_IDF_TARGET);

    /* chip_info（board.cc:118-122） */
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON *ci = cJSON_CreateObject();
    cJSON_AddNumberToObject(ci, "model",    chip.model);
    cJSON_AddNumberToObject(ci, "cores",    chip.cores);
    cJSON_AddNumberToObject(ci, "revision", chip.revision);
    cJSON_AddNumberToObject(ci, "features", (double)chip.features);
    cJSON_AddItemToObject(root, "chip_info", ci);

    /* application（board.cc:125-134） */
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *ap = cJSON_CreateObject();
    cJSON_AddStringToObject(ap, "name",         app->project_name);
    cJSON_AddStringToObject(ap, "version",      app->version);
    cJSON_AddStringToObject(ap, "idf_version",  app->idf_ver);
    cJSON_AddStringToObject(ap, "compile_time", app->date);
    cJSON_AddItemToObject(root, "application", ap);

    /* partition_table（board.cc:137-152）—— 服务器用它核对 OTA 布局。
       本工程不做服务器侧固件升级，但照发，避免服务器按缺失字段拒绝请求。 */
    cJSON *pt = cJSON_CreateArray();
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                                     ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (p == NULL) {
            continue;
        }
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "label",   p->label);
        cJSON_AddNumberToObject(e, "type",    p->type);
        cJSON_AddNumberToObject(e, "subtype", p->subtype);
        cJSON_AddNumberToObject(e, "address", (double)p->address);
        cJSON_AddNumberToObject(e, "size",    (double)p->size);
        cJSON_AddItemToArray(pt, e);
    }
    esp_partition_iterator_release(it);
    cJSON_AddItemToObject(root, "partition_table", pt);

    /* board（wifi_board.cc:139-154）—— 服务器据此判断设备型号 */
    cJSON *bd = cJSON_CreateObject();
    cJSON_AddStringToObject(bd, "type", XZ_BOARD_NAME);
    cJSON_AddStringToObject(bd, "name", XZ_BOARD_NAME);
    cJSON_AddStringToObject(bd, "mac",  xz_device_id());
    cJSON_AddItemToObject(root, "board", bd);

    return root;
}

/* ==========================================================================
 * HTTP 客户端（ota.cc:163-187 的等价物）
 *
 * ⚠️ 这里**必须**用「open → write → fetch_headers → read」的手动流程，
 *    不能用 `esp_http_client_perform()`：perform 会把响应体整个读掉并丢弃，
 *    之后再 read 只能拿到 0 字节 —— 服务器返回的错误说明就再也看不到了。
 *    （第一版就是栽在这上面：400 的响应体打出来是空的，无法定位原因。）
 * ========================================================================== */
static esp_err_t setup_http(esp_http_client_handle_t *out, const char *url)
{
    const esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = XZ_HTTP_TIMEOUT_MS,
        /* 服务器是 HTTPS。用 IDF 的证书包校验（本工程已开
           CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y）—— 不跳过校验。 */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h == NULL) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(h, "Content-Type", "application/json");
    /* Activation-Version：1 = 无 eFuse 序列号，2 = 有。
       本板未烧序列号，恒为 1。烧了序列号的板子还需要 Serial-Number 头与
       HMAC 激活体（见 xz_ota_activate()）。 */
    esp_http_client_set_header(h, "Activation-Version", "1");
    esp_http_client_set_header(h, "Device-Id",    xz_device_id());
    esp_http_client_set_header(h, "Client-Id",    xz_client_id());
    esp_http_client_set_header(h, "Accept-Language", XZ_LANGUAGE);

    char ua[64];
    snprintf(ua, sizeof(ua), "%s/%s", XZ_BOARD_NAME,
             esp_app_get_description()->version);
    esp_http_client_set_header(h, "User-Agent", ua);

    *out = h;
    return ESP_OK;
}

/**
 * @brief 发一次 POST 并把响应体读成字符串。
 *
 * @param[out] code_out HTTP 状态码
 * @param[out] resp_out 响应体（堆上，调用方 free）；读取失败时为 NULL
 */
static esp_err_t do_post(const char *url, const char *body,
                         int *code_out, char **resp_out)
{
    *code_out = 0;
    *resp_out = NULL;

    esp_http_client_handle_t h = NULL;
    esp_err_t ret = setup_http(&h, url);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 客户端创建失败");
        return ret;
    }

    const int body_len = (int)strlen(body);

    /* open 里已经带了 content length，随后 write 请求体 */
    ret = esp_http_client_open(h, body_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "open 失败: %s（URL=%s）", esp_err_to_name(ret), url);
        esp_http_client_cleanup(h);
        return ret;
    }

    if (esp_http_client_write(h, body, body_len) != body_len) {
        ESP_LOGE(TAG, "请求体写入不完整（期望 %d 字节）", body_len);
        esp_http_client_cleanup(h);
        return ESP_FAIL;
    }

    /* 读走响应头。返回值是 body 长度（-1 = 分块/未知） */
    const int64_t content_len = esp_http_client_fetch_headers(h);
    *code_out = esp_http_client_get_status_code(h);

    const int cap = (content_len > 0 && content_len < 16384)
                    ? (int)content_len + 1 : 8192;
    char *buf = malloc((size_t)cap);
    if (buf == NULL) {
        esp_http_client_cleanup(h);
        return ESP_ERR_NO_MEM;
    }

    int total = 0;
    while (total < cap - 1) {
        const int n = esp_http_client_read(h, buf + total, cap - 1 - total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    buf[total] = '\0';
    *resp_out = buf;

    esp_http_client_cleanup(h);
    return ESP_OK;
}

/* ==========================================================================
 * 解析响应（ota.cc:249-352 的等价物）
 * ========================================================================== */
static void parse_activation(const cJSON *root)
{
    const cJSON *act = cJSON_GetObjectItem(root, "activation");
    if (!cJSON_IsObject(act)) {
        return;
    }

    const cJSON *j;
    j = cJSON_GetObjectItem(act, "code");
    if (cJSON_IsString(j)) {
        snprintf(s_activation_code, sizeof(s_activation_code), "%s", j->valuestring);
    }
    j = cJSON_GetObjectItem(act, "challenge");
    if (cJSON_IsString(j)) {
        snprintf(s_activation_challenge, sizeof(s_activation_challenge),
                 "%s", j->valuestring);
    }
    j = cJSON_GetObjectItem(act, "message");
    if (cJSON_IsString(j)) {
        snprintf(s_activation_message, sizeof(s_activation_message),
                 "%s", j->valuestring);
    }
    j = cJSON_GetObjectItem(act, "timeout_ms");
    if (cJSON_IsNumber(j)) {
        s_activation_timeout_ms = (uint32_t)j->valuedouble;
    }

    ESP_LOGW(TAG, "服务器要求激活 —— 激活码 **%s**（%s）",
             s_activation_code, s_activation_message);
    ESP_LOGW(TAG, "请在小智 App 里输入上面的 6 位码完成绑定；"
                  "本机每 %d 秒会自动重试一次。", XZ_ACTIVATE_WAIT_MS / 1000);
}

static esp_err_t parse_ws_cfg(const cJSON *root, xz_ws_cfg_t *out)
{
    const cJSON *ws = cJSON_GetObjectItem(root, "websocket");
    if (!cJSON_IsObject(ws)) {
        ESP_LOGE(TAG, "响应里没有 websocket 对象 —— 服务器可能改了协议");
        return ESP_ERR_INVALID_RESPONSE;
    }

    out->url[0] = '\0';
    out->token[0] = '\0';
    out->version = 1;   /* 缺省 1（websocket_protocol.h:27），即裸 Opus 无帧头 */

    const cJSON *j;
    j = cJSON_GetObjectItem(ws, "url");
    if (cJSON_IsString(j)) {
        snprintf(out->url, sizeof(out->url), "%s", j->valuestring);
    } else {
        ESP_LOGE(TAG, "websocket.url 缺失");
        return ESP_ERR_INVALID_RESPONSE;
    }
    j = cJSON_GetObjectItem(ws, "token");
    if (cJSON_IsString(j)) {
        snprintf(out->token, sizeof(out->token), "%s", j->valuestring);
    }
    j = cJSON_GetObjectItem(ws, "version");
    if (cJSON_IsNumber(j)) {
        out->version = j->valueint;
    }

    ESP_LOGI(TAG, "取到 WebSocket 配置：%s（协议版本 %d，token %s）",
             out->url, out->version,
             (out->token[0] != '\0') ? "有" : "无（首次连接不带 Authorization）");
    return ESP_OK;
}

/** 把 url/token/version 落盘。
 *
 *  ⚠️ 开机时**仍然每次都调 OTA**（与 xiaozhi 一致）—— 因为服务器可能改地址、
 *     换 token、或要求重新激活。这里落盘只是留个记录便于排查，
 *     以及将来要做"离线先用上次地址"时的起点。 */
static void save_ws_cfg(const xz_ws_cfg_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open("websocket", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    (void)nvs_set_str(h, "url", cfg->url);
    (void)nvs_set_str(h, "token", cfg->token);
    (void)nvs_set_i32(h, "version", cfg->version);
    (void)nvs_commit(h);
    nvs_close(h);
}

/* ==========================================================================
 * 对外
 * ========================================================================== */
const char *xz_activation_code(void)
{
    return (s_activation_code[0] != '\0') ? s_activation_code : NULL;
}

esp_err_t xz_ota_fetch(xz_ws_cfg_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_activation_code[0] = '\0';
    s_activation_challenge[0] = '\0';
    s_activation_message[0] = '\0';
    s_activation_timeout_ms = 0;

    cJSON *body = build_body();
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (body_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "POST %s（Device-Id=%s，Client-Id=%s）",
             XZ_OTA_URL, xz_device_id(), xz_client_id());

    int code = 0;
    char *resp = NULL;
    const esp_err_t ret = do_post(XZ_OTA_URL, body_str, &code, &resp);
    free(body_str);

    if (ret != ESP_OK) {
        return ret;
    }
    if (code != 200) {
        ESP_LOGE(TAG, "服务器返回 HTTP %d，响应体: %s",
                 code, (resp != NULL && resp[0] != '\0') ? resp : "(空)");
        free(resp);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGD(TAG, "OTA 响应: %s", resp);

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (root == NULL) {
        ESP_LOGE(TAG, "响应不是合法 JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }

    parse_activation(root);

    esp_err_t out_ret = ESP_OK;
    if (s_activation_code[0] != '\0' || s_activation_challenge[0] != '\0') {
        /* 未激活：设备必须先完成绑定，服务器才会给可用的 websocket 配置。
           出处：application.cc:243-247 —— 有 activation 就卡在 OTA 循环里。 */
        out_ret = ESP_ERR_NOT_FINISHED;
    } else {
        out_ret = parse_ws_cfg(root, out);
        if (out_ret == ESP_OK) {
            save_ws_cfg(out);
        }
    }

    cJSON_Delete(root);
    return out_ret;
}

esp_err_t xz_ota_activate(void)
{
    if (s_activation_challenge[0] == '\0' && s_activation_code[0] == '\0') {
        return ESP_ERR_INVALID_STATE;   /* 没有待激活的信息 */
    }

    /* URL：OTA 地址后面接 activate（ota.cc:727-733） */
    char url[192];
    const size_t base = strlen(XZ_OTA_URL);
    if (base == 0 || base + 16 >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(url, sizeof(url), "%s%s", XZ_OTA_URL,
             (XZ_OTA_URL[base - 1] == '/') ? "activate" : "/activate");

    /* ⚠️ 请求体：本板**没有烧 eFuse 序列号**，按 ota.cc:660-663 的等价分支
       直接发空对象 `{}`。有序列号的板子才需要 hmac-sha256(challenge, HMAC_KEY0)
       那套（那时还要在头里带 Serial-Number，且 Activation-Version=2）。 */
    int code = 0;
    char *resp = NULL;
    const esp_err_t ret = do_post(url, "{}", &code, &resp);

    ESP_LOGI(TAG, "激活请求 -> HTTP %d %s", code,
             (resp != NULL) ? resp : "");
    free(resp);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 202 = 已受理、等用户在 App 里确认；200 = 成功（ota.cc:748-759） */
    if (code == 200) {
        ESP_LOGI(TAG, "✅ 激活成功");
        s_activation_code[0] = '\0';
        s_activation_challenge[0] = '\0';
        return ESP_OK;
    }
    if (code == 202) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_FAIL;
}

esp_err_t xz_ota_refresh(void)
{
    xz_ws_cfg_t cfg;
    const esp_err_t ret = xz_ota_fetch(&cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "配置已刷新");
    }
    return ret;
}
