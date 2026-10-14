/**
 * @file xiaozhi_vision.c
 * @brief 把照片传给服务器做视觉理解（P7 摄像头工具）
 *
 * 照搬 xiaozhi `main/boards/common/esp32_camera.cc:189-305` 的 `Explain()`：
 * 服务器在 MCP `initialize` 里下发一个 vision 地址，设备把 JPEG 用
 * multipart/form-data POST 上去，服务器跑视觉模型，返回的文本就是工具结果。
 *
 * ── 地址长什么样（2026-10-14 上板实测，不是猜的）────────────────────────────
 *
 *   initialize 的 params 里：
 *     {"vision":{"url":"http://api.xiaozhi.me/vision/explain",
 *                "token":"38fcf287-bf1a-4e8d-93b7-7ea6b02efafb"}}
 *
 *   两点值得注意：
 *     · **token 就是本次会话的 session_id** —— 每个会话都不一样，
 *       所以必须每次 initialize 都重新取，不能缓存。
 *     · 用的是**明文 http**、不是 https —— 上传这一路不需要 TLS（对吃紧的栈是好事）。
 *       但代码里仍挂 crt_bundle（见下），免得服务器哪天改成 https 就哑掉。
 *
 * ── multipart 的形状 ───────────────────────────────────────────────────────
 *   --<boundary>\r\n
 *   Content-Disposition: form-data; name="question"\r\n\r\n
 *   <问题文本>\r\n
 *   --<boundary>\r\n
 *   Content-Disposition: form-data; name="file"; filename="camera.jpg"\r\n
 *   Content-Type: image/jpeg\r\n\r\n
 *   <JPEG 原始字节>\r\n
 *   --<boundary>--\r\n
 *
 * ── 一处刻意偏离参考实现 ───────────────────────────────────────────────────
 * 参考实现发 `Transfer-Encoding: chunked` —— 因为它是**边编码边发**的
 * （编码线程写队列、HTTP 任务从队列取，长度事先不知道）。
 * 本工程是**先把整张 JPEG 编码完**再上传，长度已知，所以改用 **Content-Length**
 * （`esp_http_client_open(h, 全长)`）：少一层分块编码，兼容性也更好。
 * 万一服务器只认分块，回退办法是把 open 的长度改成 -1，并把写入方式保持不动。
 */
#include "xiaozhi_internal.h"
#include "xiaozhi_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "xz_vision";

/* boundary 与参考实现保持一致（esp32_camera.cc:218 的字面量） */
#define VISION_BOUNDARY "----ESP32_CAMERA_BOUNDARY"

static char s_url[XZ_URL_MAX]     = "";
static char s_token[XZ_TOKEN_MAX] = "";

void xz_vision_set(const char *url, const char *token)
{
    snprintf(s_url, sizeof(s_url), "%s", (url != NULL) ? url : "");
    snprintf(s_token, sizeof(s_token), "%s", (token != NULL) ? token : "");
}

bool xz_vision_is_ready(void)
{
    return s_url[0] != '\0';
}

/**
 * @brief 上传一张 JPEG，取回服务器的解释文本。
 *
 * @param out     结果文本落点；失败时写入一句可读的失败说明（给 LLM 看的）
 * @param out_cap out 的容量
 * @return ESP_OK 上传并收到 200；其余为失败（此时 out 里仍有说明）
 */
esp_err_t xz_vision_upload(const uint8_t *jpeg, size_t jpeg_len,
                           const char *question, char *out, size_t out_cap)
{
    if (jpeg == NULL || jpeg_len == 0 || out == NULL || out_cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    if (!xz_vision_is_ready()) {
        /* 服务器没给 vision 地址 —— 不是网络问题，是这次会话不支持看图。
           照实告诉 LLM，让它别硬猜（这正是"报错要可归因"的用法）。 */
        snprintf(out, out_cap,
                 "{\"success\": false, \"message\": \"Vision service is not "
                 "available in this session\"}");
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- 拼出除 JPEG 本身之外的三段。用 snprintf 拼，长度都是算出来的 ----
       ⚠️ head 的尺寸**必须由 XZ_MCP_QUESTION_MAX 推出来**，不要写死数字：
          固定部分是 80 字节（`--` + boundary + 各段 CRLF 与字段名），
          加上最长 question。曾经写死 320 —— 那装不下允许的最长 question
          （80 + 255 = 335），于是一个完全合法的请求被判成 "Question too long"，
          表现是"拍照工具时灵时不灵"，而参数校验明明放行了。 */
#define VISION_MULTIPART_FIXED 80
    char head[VISION_MULTIPART_FIXED + XZ_MCP_QUESTION_MAX];
    char mid[256];      /* file 字段的头，内容是固定的 */
    char tail[128];

    const int n_head = snprintf(head, sizeof(head),
        "--" VISION_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"question\"\r\n"
        "\r\n"
        "%s\r\n", question);
    const int n_mid = snprintf(mid, sizeof(mid),
        "--" VISION_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n"
        "\r\n");
    const int n_tail = snprintf(tail, sizeof(tail),
        "\r\n--" VISION_BOUNDARY "--\r\n");

    if (n_head < 0 || n_head >= (int)sizeof(head) ||
        n_mid  < 0 || n_mid  >= (int)sizeof(mid)  ||
        n_tail < 0 || n_tail >= (int)sizeof(tail)) {
        ESP_LOGE(TAG, "multipart 头/尾缓冲不足（问题文本过长？）");
        snprintf(out, out_cap,
                 "{\"success\": false, \"message\": \"Question too long\"}");
        return ESP_ERR_INVALID_SIZE;
    }

    const int total = n_head + n_mid + (int)jpeg_len + n_tail;

    /* ---- 建客户端 ---- */
    const esp_http_client_config_t cfg = {
        .url        = s_url,
        .method     = HTTP_METHOD_POST,
        /* 服务器要跑视觉模型，比普通接口慢得多 —— 超时给宽。
           （本项目其余的 HTTP 用 XZ_HTTP_TIMEOUT_MS=10s，那个是 OTA 用的） */
        .timeout_ms = XZ_VISION_TIMEOUT_MS,
        /* 当前地址是 http://，这一项用不上；挂着是为了服务器哪天改成
           https:// 时不至于静默失败。 */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h == NULL) {
        snprintf(out, out_cap,
                 "{\"success\": false, \"message\": \"Failed to create HTTP client\"}");
        return ESP_FAIL;
    }

    /* 身份与鉴权头，与参考实现一致（esp32_camera.cc:222-229） */
    esp_http_client_set_header(h, "Device-Id", xz_device_id());
    esp_http_client_set_header(h, "Client-Id", xz_client_id());
    if (s_token[0] != '\0') {
        char auth[XZ_TOKEN_MAX + 16];
        snprintf(auth, sizeof(auth), "Bearer %s", s_token);
        esp_http_client_set_header(h, "Authorization", auth);
    }
    esp_http_client_set_header(h, "Content-Type",
                               "multipart/form-data; boundary=" VISION_BOUNDARY);

    esp_err_t ret = ESP_FAIL;

    /* open 里给全长 ⇒ 走 Content-Length（见文件头的偏离说明） */
    if (esp_http_client_open(h, total) != ESP_OK) {
        ESP_LOGE(TAG, "连接 vision 地址失败：%s", s_url);
        snprintf(out, out_cap,
                 "{\"success\": false, \"message\": \"Failed to connect to the "
                 "vision service\"}");
        esp_http_client_cleanup(h);
        return ret;
    }

    /* ---- 分四段写请求体。每段都要校验写全了 ---- */
    struct { const void *p; int n; const char *name; } parts[] = {
        { head,    n_head,           "question 字段" },
        { mid,     n_mid,            "file 字段头"  },
        { jpeg,    (int)jpeg_len,    "JPEG 数据"    },
        { tail,    n_tail,           "结尾"         },
    };

    bool write_ok = true;
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        const int wr = esp_http_client_write(h, (const char *)parts[i].p, parts[i].n);
        if (wr != parts[i].n) {
            ESP_LOGE(TAG, "写入不完整：%s（期望 %d，实际 %d）",
                     parts[i].name, parts[i].n, wr);
            write_ok = false;
            break;
        }
    }
    if (!write_ok) {
        snprintf(out, out_cap,
                 "{\"success\": false, \"message\": \"Failed to upload photo\"}");
        esp_http_client_cleanup(h);
        return ESP_FAIL;
    }

    /* ---- 收响应 ----
       ⚠️ 与 xiaozhi_ota.c 同一条纪律：必须手动 fetch_headers + read，
          不能用 perform()（它会把响应体读掉丢弃，出错时就看不到原因了）。 */
    const int64_t clen = esp_http_client_fetch_headers(h);
    const int status = esp_http_client_get_status_code(h);

    const int cap = (clen > 0 && clen < (int)out_cap - 1) ? (int)clen + 1
                                                          : (int)out_cap;
    int total_read = 0;
    while (total_read < cap - 1) {
        const int n = esp_http_client_read(h, out + total_read, cap - 1 - total_read);
        if (n <= 0) {
            break;
        }
        total_read += n;
    }
    out[total_read] = '\0';

    esp_http_client_cleanup(h);

    ESP_LOGI(TAG, "上传 %u 字节 JPEG（问题：%s）→ HTTP %d，响应 %d 字节",
             (unsigned)jpeg_len, question, status, total_read);

    if (status != 200) {
        /* ⚠️ 把响应体一起打出来 —— 否则"上传失败"完全无法归因。
           这条教训是从 xiaozhi_ota.c:192-198 学来的（那里 400 的响应体
           曾被 perform() 吞掉，查了很久）。 */
        ESP_LOGE(TAG, "vision 上传失败，HTTP %d，响应体：%s",
                 status, (total_read > 0) ? out : "(空)");
        snprintf(out, out_cap,
                 "{\"success\": false, \"message\": \"Vision service returned HTTP %d\"}",
                 status);
        return ESP_FAIL;
    }

    if (total_read == 0) {
        ESP_LOGW(TAG, "vision 服务返回 200 但响应体为空");
        snprintf(out, out_cap,
                 "{\"success\": false, \"message\": \"Vision service returned an "
                 "empty response\"}");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "vision 响应：%s", out);
    return ESP_OK;
}
