/**
 * @file camera_service.c
 * @brief 摄像头服务实现（P6-a）
 *
 * 值域/字节序约定与线程约束见 camera_service.h 顶部。
 *
 * ── 与 xiaozhi 参考实现的关系 ──────────────────────────────────────────────
 * 移植自 `xiaozhi-esp32-1.9.4-gec`：
 *   · 板级 `gec_esp32s3_board.cc:394-430` 的 `InitializeCamera()`
 *     （PCA9557 P2 上电 + camera_config_t 各字段）
 *   · `main/boards/common/esp32_camera.cc:87-131` 的 `Capture()`
 *     （取 2 帧求稳 + `__builtin_bswap16`）
 * 有意偏离之处：
 *   · 分辨率 VGA → **QVGA**（理由见 camera_service.h）
 *   · 去掉 C++ 包装类，改为纯 C（本工程其余组件都是 C）
 *   · xiaozhi 的 `Explain()` 拆成两半：**拍照+JPEG 编码留在这里**
 *     （`camera_service_capture_jpeg()`），**上传给云服务的那半挪到了
 *     `xiaozhi_client`**（它才拿着服务器给的 vision 地址）。
 *     拆的理由：`esp_camera.h` / `img_converters.h` 只在本组件内可见，
 *     别处编不了码；而 vision 地址是协议层的东西，本组件不该知道。
 *
 * ⚠️ xiaozhi 的 ESP-IDF 就是 **v5.5.3**（与其 build/project_description.json 一致），
 *    与本工程同版本 —— 这套驱动 API 组合在同版本上跑通过，不是纸面推断。
 *
 * ⚠️ **板上实际是 GC0308，不是 OV2640** —— 见 camera_service.h 顶部：
 *    实测 PID=0x9B、I2C 地址 0x21。此前按"OV2640 类"写的注释都是想当然。
 */
#include "camera_service.h"

#include <stdarg.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "esp_camera.h"         /* 托管组件 espressif/esp32-camera */
#include "img_converters.h"     /* frame2jpg_cb —— JPEG 编码 */
#include "gec_board.h"

static const char *TAG = "camera";

/* ==========================================================================
 * 内部状态
 * ========================================================================== */
static bool             s_inited = false;
static bool             s_ready  = false;   /* init 成功且缓冲已分配 */
static uint16_t        *s_frame  = NULL;    /* 本组件自有的输出缓冲 */
static SemaphoreHandle_t s_lock  = NULL;    /* 串行化整个抓帧过程，见下 */

/* 未就绪原因描述。用静态缓冲是因为它要交给界面显示，不能是栈上的临时量。
   只在 init / capture 失败时写入，不需要保护。 */
static char s_status[96] = "未初始化";

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    va_end(ap);
}

const char *camera_service_status(void)
{
    return s_status;
}

bool camera_service_is_ready(void)
{
    return s_ready;
}

/* ==========================================================================
 * sensor 型号名（仅用于日志 —— 出问题时「认出了哪颗 sensor」是最有用的线索）
 * ========================================================================== */
static const char *sensor_name(uint16_t pid)
{
    switch (pid) {
    case OV2640_PID:  return "OV2640";
    case OV3660_PID:  return "OV3660";
    case OV3640_PID:  return "OV3640";
    case OV5640_PID:  return "OV5640";
    case OV7670_PID:  return "OV7670";
    case OV7725_PID:  return "OV7725";
    case OV9650_PID:  return "OV9650";
    case GC0308_PID:  return "GC0308";
    case GC2145_PID:  return "GC2145";
    case GC032A_PID:  return "GC032A";
    case NT99141_PID: return "NT99141";
    case SC031GS_PID: return "SC031GS";
    default:          return "未知型号";
    }
}

/* ==========================================================================
 * 初始化
 * ========================================================================== */
esp_err_t camera_service_init(void)
{
    if (s_inited) {
        return s_ready ? ESP_OK : ESP_FAIL;
    }

    /* ---- 前置：I2C 总线必须已存在 ----
       SCCB 复用这条总线（`pin_sccb_sda = -1` + `sccb_i2c_port = 端口号`），
       驱动走的是 `i2c_master_get_bus_handle(port)` —— 总线不存在就直接失败。 */
    if (gec_board_get_i2c_bus() == NULL) {
        set_status("I2C 总线未初始化");
        ESP_LOGE(TAG, "%s —— 请先调用 gec_board_i2c_init()", s_status);
        s_inited = true;        /* 记为「已尝试过」，避免反复重试刷屏 */
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- 1. 互斥锁 ----
       原先只有一个调用者（ui_task 的手动拍照），所以整个组件没有锁。
       现在多了 AI 拍照这条路（跑在 xiaozhi 的工具任务上），两个任务会同时
       碰**同一个** `fb_count=1` 的驱动帧缓冲和同一块 s_frame —— 必须串行化。 */
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        set_status("内部锁创建失败");
        ESP_LOGE(TAG, "%s", s_status);
        s_inited = true;
        return ESP_ERR_NO_MEM;
    }

    /* ---- 2. 摄像头电源：PCA9557 P2，**低有效**，置 0 上电 ----
       上电前 xiaozhi 的做法也是先 SetOutputState(2, 0)，
       它的 PCA9557 初值里 P2=1（断电），所以这一步是必需的。 */
    const esp_err_t pret = gec_pca9557_set_output(GEC_PCA9557_IO_DVP_PWDN,
                                                  GEC_PCA9557_LEVEL_DVP_POWER_ON);
    if (pret != ESP_OK) {
        set_status("摄像头上电失败: %s", esp_err_to_name(pret));
        ESP_LOGE(TAG, "%s", s_status);
        s_inited = true;
        return pret;
    }
    ESP_LOGI(TAG, "PCA9557 P2 置 %d —— 摄像头已上电",
             (int)GEC_PCA9557_LEVEL_DVP_POWER_ON);
    vTaskDelay(pdMS_TO_TICKS(CAMERA_SERVICE_POWER_SETTLE_MS));

    /* ---- 3. 驱动配置 ----
       引脚**全部**取自 gec_board.h，不在这里写魔数（pinmap.md 是引脚真源）。 */
    camera_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.ledc_channel = LEDC_CHANNEL_2;      /* S3 的 XCLK 由 LCD_CAM 产生，
    cfg.ledc_timer   = LEDC_TIMER_2;          这两项不参与，填个合法值即可 */
    cfg.pin_pwdn     = -1;                  /* 断电/复位都由 PCA9557 管，无独立 GPIO */
    cfg.pin_reset    = -1;
    cfg.pin_xclk     = GEC_CAM_PIN_XCLK;
    cfg.pin_sccb_sda = -1;                  /* -1 ⇒ 复用已存在的总线，见下 */
    cfg.pin_sccb_scl = -1;                  /*     sda==-1 时该值不被使用 */
    cfg.pin_d0       = GEC_CAM_PIN_D0;
    cfg.pin_d1       = GEC_CAM_PIN_D1;
    cfg.pin_d2       = GEC_CAM_PIN_D2;
    cfg.pin_d3       = GEC_CAM_PIN_D3;
    cfg.pin_d4       = GEC_CAM_PIN_D4;
    cfg.pin_d5       = GEC_CAM_PIN_D5;
    cfg.pin_d6       = GEC_CAM_PIN_D6;
    cfg.pin_d7       = GEC_CAM_PIN_D7;
    cfg.pin_vsync    = GEC_CAM_PIN_VSYNC;
    cfg.pin_href     = GEC_CAM_PIN_HREF;
    cfg.pin_pclk     = GEC_CAM_PIN_PCLK;
    cfg.xclk_freq_hz = GEC_CAM_XCLK_FREQ_HZ;

    cfg.pixel_format = PIXFORMAT_RGB565;    /* 直接出 RGB565，省掉 JPEG 解码 */
    cfg.frame_size   = FRAMESIZE_QVGA;
    cfg.jpeg_quality = 12;                  /* 仅 JPEG 格式时有效，留着与参考一致 */
    cfg.fb_count     = 1;                   /* 单缓冲 ⇒ fb_get/fb_return 必须成对 */
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

    /* SCCB 端口号：驱动拿它去 i2c_master_get_bus_handle() 找**同一条**总线，
       然后只 add_device 挂上自己那个从设备，**不会重建总线**。
       见 esp32-camera 的 esp_camera.c:191-197 → sccb-ng.c:147-160。 */
    cfg.sccb_i2c_port = GEC_I2C_PORT_NUM;

    const esp_err_t iret = esp_camera_init(&cfg);
    if (iret != ESP_OK) {
        set_status("esp_camera_init 失败: %s", esp_err_to_name(iret));
        ESP_LOGE(TAG, "%s", s_status);
        ESP_LOGE(TAG, "  排查顺序：① P2 是否真的被拉低（PCA9557 写失败会打日志）"
                      "② SCCB 能否探到 sensor（示波器看 IO1/IO2）"
                      "③ 排线是否插紧");
        s_inited = true;
        return iret;
    }

    /* 认出型号 —— 失败时这条日志最有价值 */
    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        ESP_LOGI(TAG, "sensor: %s (PID=0x%04X)，%dx%d RGB565",
                 sensor_name(s->id.PID), (unsigned)s->id.PID,
                 CAMERA_SERVICE_H_RES, CAMERA_SERVICE_V_RES);
    } else {
        ESP_LOGW(TAG, "esp_camera_sensor_get() 返回 NULL，无法确认型号");
    }

    /* ---- 4. 镜像/翻转（**无条件下发**，不做"值为 0 就跳过"）----
       理由：这颗 GC0308 的寄存器默认值不是"不镜像"。
       xiaozhi 对 GC0308 有一句特判 `set_hmirror(s, 0)`，注释写"写1镜像 写0不镜像"
       —— 说明必须显式关掉才能得到不镜像的画面。跳过下发就会继承出厂默认。 */
    if (s != NULL) {
        (void)s->set_hmirror(s, CAMERA_SERVICE_HMIRROR);
        (void)s->set_vflip(s, CAMERA_SERVICE_VFLIP);
        ESP_LOGI(TAG, "镜像=%d 翻转=%d（初始值，实测后按实际画面调整）",
                 CAMERA_SERVICE_HMIRROR, CAMERA_SERVICE_VFLIP);
    }

    /* ---- 5. 输出缓冲 ----
       与驱动帧缓冲独立，理由见头文件：避免「LVGL 正在读、摄像头正在写」。
       放 PSRAM —— 它**不直接做 SPI DMA 源**（交给 LVGL 后会经 panel_send
       从内部 RAM 的条带缓冲发出），故不受 architecture.md 7.4 那条约束。 */
    const size_t bytes = (size_t)CAMERA_SERVICE_H_RES * CAMERA_SERVICE_V_RES
                         * sizeof(uint16_t);
    s_frame = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_frame == NULL) {
        set_status("输出缓冲分配失败（需 %u 字节 PSRAM）", (unsigned)bytes);
        ESP_LOGE(TAG, "%s", s_status);
        s_inited = true;
        return ESP_ERR_NO_MEM;
    }

    s_ready  = true;
    s_inited = true;
    set_status("就绪 (%dx%d)", CAMERA_SERVICE_H_RES, CAMERA_SERVICE_V_RES);
    ESP_LOGI(TAG, "摄像头就绪，输出缓冲 %u 字节（PSRAM）", (unsigned)bytes);
    return ESP_OK;
}

/* ==========================================================================
 * 抓帧
 * ========================================================================== */

/* ---- JPEG 输出缓冲 ----
   惰性分配（第一次要 JPEG 时才申请）：不拍照给 AI 的场景下不白占 64KB。
   放在 PSRAM —— 它是纯粹的字节缓冲，不做 DMA 源。

   **所有权归本组件**，不交给调用方 free：与 camera_service_frame() 一致，
   指针在下一次 camera_service_capture_jpeg() 之前有效。
   这样避免"谁分配谁释放"的配对错误，也省掉每次拍照的 malloc/free。 */
static uint8_t *s_jpg     = NULL;
static size_t   s_jpg_len = 0;
static bool     s_jpg_ovf = false;      /* 回调里写溢出 → 整次调用判失败 */

/** frame2jpg_cb 的输出回调。
 *  ⚠️ 编码器收尾时会调 put_buf(NULL, 0)（to_jpg.cpp:161-165），那是**结束哨兵**，
 *     不是数据 —— 必须吃掉，否则会当成一次长度 0 的写入。 */
static size_t jpg_write_cb(void *arg, size_t index, const void *data, size_t len)
{
    (void)arg;
    (void)index;

    if (data == NULL || len == 0) {
        return 0;                       /* 结束哨兵 */
    }
    if (s_jpg == NULL || s_jpg_len + len > CAMERA_SERVICE_JPEG_MAX) {
        /* 不静默截断 —— 置标志，让调用方把整次拍照判失败并记日志。
           参考实现的缓冲版 frame2jpg() 正是在这里静默丢弃多余数据
           （to_jpg.cpp:201-204，连警告日志都被注释掉了），
           结果是"上传成功但图是坏的"，从日志上完全看不出来。 */
        s_jpg_ovf = true;
        return 0;
    }
    memcpy(s_jpg + s_jpg_len, data, len);
    s_jpg_len += len;
    return len;
}

/**
 * @brief 抓一帧的内部实现。**调用方必须已持有 s_lock。**
 *
 * @param want_jpeg true = 顺带编码成 JPEG 存进 s_jpg/s_jpg_len
 */
static esp_err_t capture_locked(bool want_jpeg)
{
    if (!s_ready || s_frame == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- 取帧 ----
       丢前 N 帧：DVP 模组上电/长时间不取帧后，队列里那一帧往往是残影或
       曝光未稳的画面。xiaozhi 的 Capture() 同样取 2 帧。
       ⚠️ fb_return 必须与 fb_get 成对 —— fb_count=1 时漏掉一次，
          之后再也取不到帧（表现为"只成功拍一次"）。 */
    camera_fb_t *fb = NULL;
    for (int i = 0; i < CAMERA_SERVICE_WARMUP_FRAMES; i++) {
        if (fb != NULL) {
            esp_camera_fb_return(fb);
        }
        fb = esp_camera_fb_get();       /* 阻塞，最长 4s */
        if (fb == NULL) {
            set_status("取帧超时");
            ESP_LOGE(TAG, "esp_camera_fb_get 超时（第 %d 帧）", i + 1);
            return ESP_ERR_TIMEOUT;
        }
    }

    /* ---- 校验帧参数 ----
       这一项是**防越界**用的，不是多余的谨慎：下面的字节交换按固定
       CAMERA_*_RES 循环，一旦驱动给的帧尺寸与配置不一致（改配置漏改宏、
       或 sensor 不支持 QVGA 而驱动回退到别的尺寸），就会写越界。 */
    const size_t want_px = (size_t)CAMERA_SERVICE_H_RES * CAMERA_SERVICE_V_RES;
    if (fb->format != PIXFORMAT_RGB565 ||
        fb->width  != CAMERA_SERVICE_H_RES ||
        fb->height != CAMERA_SERVICE_V_RES ||
        fb->len    <  want_px * sizeof(uint16_t)) {
        set_status("帧参数不符：%ux%u fmt=%d len=%u",
                   (unsigned)fb->width, (unsigned)fb->height,
                   (int)fb->format, (unsigned)fb->len);
        ESP_LOGE(TAG, "%s（期望 %dx%d RGB565，%u 字节）", s_status,
                 CAMERA_SERVICE_H_RES, CAMERA_SERVICE_V_RES,
                 (unsigned)(want_px * sizeof(uint16_t)));
        esp_camera_fb_return(fb);
        return ESP_ERR_INVALID_SIZE;
    }

    /* ---- JPEG 编码（先做，失败就早退，省掉下面的字节交换）----
       ⚠️ 编码的是**驱动给的原始 fb**，不是下面交换过的 s_frame。
          编码器默认 `rgb565_big_endian = true`（to_jpg.cpp:34），正是驱动输出的
          字节序 —— 原样喂进去即可。喂 s_frame 才需要 jpgSetRgb565BE(false)，
          而那是**全局可变状态、非线程安全**（to_jpg.cpp:247-255），能不用就不用。
       ⚠️ 用 **_cb 流式版**，理由见 jpg_write_cb()。 */
    if (want_jpeg) {
        if (s_jpg == NULL) {
            s_jpg = (uint8_t *)heap_caps_malloc(CAMERA_SERVICE_JPEG_MAX,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (s_jpg == NULL) {
                set_status("JPEG 缓冲分配失败（需 %u 字节 PSRAM）",
                           (unsigned)CAMERA_SERVICE_JPEG_MAX);
                ESP_LOGE(TAG, "%s", s_status);
                esp_camera_fb_return(fb);
                return ESP_ERR_NO_MEM;
            }
            ESP_LOGI(TAG, "JPEG 输出缓冲就绪：%u 字节（PSRAM）",
                     (unsigned)CAMERA_SERVICE_JPEG_MAX);
        }
        s_jpg_len = 0;
        s_jpg_ovf = false;

        if (!frame2jpg_cb(fb, CAMERA_SERVICE_JPEG_QUALITY, jpg_write_cb, NULL)
            || s_jpg_ovf) {
            if (s_jpg_ovf) {
                set_status("JPEG 超出缓冲上限 %u 字节",
                           (unsigned)CAMERA_SERVICE_JPEG_MAX);
            } else {
                set_status("JPEG 编码失败");
            }
            ESP_LOGE(TAG, "%s（已写 %u 字节）", s_status, (unsigned)s_jpg_len);
            /* 溢出时已写进去的半截图不能留在缓冲里冒充成品 */
            s_jpg_len = 0;
            esp_camera_fb_return(fb);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "JPEG 编码完成：%u 字节（原始 %u 字节）",
                 (unsigned)s_jpg_len, (unsigned)fb->len);
    }

    /* ---- 字节交换（链条上的第 ① 次）----
     * 驱动交出的 RGB565 是**大端**（高字节在前）：
     *   · cam_hal.c 初始化时 `cam_obj->swap_data = 0`
     *   · ll_cam.c 把该值直接写进 `LCD_CAM.cam_ctrl.cam_byte_order`
     *   · conversions 里 JPEG 编码器也默认按大端解读（`rgb565_big_endian = true`）
     * ESP32-S3 是小端 CPU，直接当 uint16_t 读会得到字节颠倒的值 ——
     * **不换这一次，照片就是花屏，而 UI 完全正常**（因为 UI 不走这条路）。
     * 这是本项目最可能出现的一处错误，判据写在这里备查。 */
    const uint16_t *src = (const uint16_t *)fb->buf;
    for (size_t i = 0; i < want_px; i++) {
        s_frame[i] = __builtin_bswap16(src[i]);
    }

    esp_camera_fb_return(fb);

    s_ready = true;
    set_status("就绪 (%dx%d)", CAMERA_SERVICE_H_RES, CAMERA_SERVICE_V_RES);
    return ESP_OK;
}

/** 取锁。**带超时，不是 portMAX_DELAY** —— 理由见头文件的线程约束。 */
static esp_err_t lock_camera(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(CAMERA_SERVICE_LOCK_TIMEOUT_MS))
        != pdTRUE) {
        set_status("摄像头忙（另有拍摄在进行）");
        ESP_LOGW(TAG, "%s —— 本次请求放弃", s_status);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t camera_service_capture(void)
{
    const esp_err_t lret = lock_camera();
    if (lret != ESP_OK) {
        return lret;
    }
    const esp_err_t ret = capture_locked(false);
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t camera_service_capture_jpeg(uint8_t **out, size_t *out_len)
{
    if (out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out     = NULL;
    *out_len = 0;

    const esp_err_t lret = lock_camera();
    if (lret != ESP_OK) {
        return lret;
    }
    const esp_err_t ret = capture_locked(true);
    xSemaphoreGive(s_lock);
    if (ret != ESP_OK) {
        return ret;
    }

    *out     = s_jpg;
    *out_len = s_jpg_len;
    return ESP_OK;
}

/* ==========================================================================
 * 取结果
 * ========================================================================== */
const uint16_t *camera_service_frame(int *width, int *height)
{
    if (!s_ready || s_frame == NULL) {
        return NULL;
    }
    if (width  != NULL) { *width  = CAMERA_SERVICE_H_RES; }
    if (height != NULL) { *height = CAMERA_SERVICE_V_RES; }
    return s_frame;
}

/* ==========================================================================
 * 诊断：测试彩条
 * ========================================================================== */
esp_err_t camera_service_set_colorbar(bool on)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL || s->set_colorbar == NULL) {
        ESP_LOGE(TAG, "当前 sensor 不支持 set_colorbar");
        return ESP_ERR_NOT_SUPPORTED;
    }

    const esp_err_t ret = s->set_colorbar(s, on ? 1 : 0);
    ESP_LOGW(TAG, "测试彩条 -> %s（%s）", on ? "开" : "关",
             esp_err_to_name(ret));
    if (on) {
        ESP_LOGW(TAG, "  判据：应出现 8 条纯饱和色竖条");
        ESP_LOGW(TAG, "  若呈「暗蓝/暗绿」这类不纯的颜色 ⇒ **字节序问题**，"
                      "不是画面内容问题（绿字段 6 位跨字节边界，交换后会打散）");
    }
    return ret;
}
