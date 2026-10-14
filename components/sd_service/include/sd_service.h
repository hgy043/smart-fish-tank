/**
 * @file sd_service.h
 * @brief TF 卡存储服务（1-bit SD 模式，P6-c）
 *
 * ============================================================================
 * 【本组件做什么】
 *
 * 把「TF 卡」封成一个只管存取的服务：
 *   · 挂载 FATFS 到 /sdcard
 *   · 把一张 JPEG 存成 photo_YYYYMMDD_HHMMSS.jpg
 *   · 列出最近的照片
 *   · 把一张照片**解码**成 LVGL 能直接用的 RGB565 缩略图
 *
 * 不碰界面、不碰摄像头 —— 照片从哪来、显示到哪去，都是 main 的事。
 *
 * ============================================================================
 * 【硬件：1-bit SD 模式，引脚来自主板原理图（2026-10-14）】
 *
 *   CMD = IO48   CLK = IO47   DAT0 = IO21
 *
 * 详见 `board_gec.h` 第六节。要点：
 *   · 这三个脚**不是** S3 的 IO_MUX 固定 SD 脚 —— S3 的 SDMMC 可以走 GPIO 矩阵，
 *     所以任意脚都行（老款 ESP32 不行，别按旧印象判断）。
 *   · 卡座**没有** card-detect 接到 GPIO，所以"卡在不在"只能靠访问去发现。
 *   · 板上 DAT1/DAT2/DAT3 有 51kΩ 上拉到 3V3（1-bit 用不到，但协议要求不能悬空）。
 *
 * ============================================================================
 * 【失败不致命】
 * 没插卡、卡是 exFAT、接线没接好 —— 这些都不该让鱼缸停摆。挂载失败时
 * 本组件只是把原因记进 `sd_service_status()`，调用方照常继续跑，只是相册页
 * 显示"卡不可用"。与 camera_service 同一条原则。
 *
 * ⚠️ **永不自动格式化**。见 sd_service.c 里 `format_if_mount_failed` 那段。
 * ============================================================================
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 约定
 * ========================================================================== */

/** 挂载点。VFS 路径前缀，不是真实目录名。 */
#define SD_MOUNT_POINT      "/sdcard"

/** 照片文件名缓冲上限。"photo_YYYYMMDD_HHMMSS.jpg" 24 字符，
 *  冲突时加 "_%d" 后缀 → 最多 27，留到 32 够用。 */
#define SD_PHOTO_NAME_MAX   32

/** 缩略图：张数与尺寸。
 *  6 张 = 相册页的格子数（见 ui_pages.c 的 ALBUM_COUNT）。
 *  160×120 = 摄像头 QVGA 320×240 按 **1/2 缩放解码**得来 —— 相册格子是
 *  142×104，160×120 缩一点正好放下，而 1/4（80×60）会明显发虚。
 *  ⚠️ 改这里要同步 ui_pages.c 的格子数，否则多出来的图没处放。 */
#define SD_THUMB_MAX        6
#define SD_THUMB_W          160
#define SD_THUMB_H          120
#define SD_THUMB_BYTES      ((size_t)SD_THUMB_W * SD_THUMB_H * sizeof(uint16_t))

/* ==========================================================================
 * 生命周期
 * ========================================================================== */

/**
 * @brief 挂载 TF 卡。**幂等**；失败后再调用会重试一次挂载。
 *
 * @return ESP_OK；失败返回底层错误码 —— 调用方**不必**区分，看
 *         `sd_service_status()` 里已经翻译好的中文原因即可。
 *
 * @note 失败**不致命**：调用方应当只记日志然后继续跑。
 * @note 会阻塞几十到几百毫秒（卡探测 + FAT 挂载）。
 */
esp_err_t sd_service_init(void);

/** @brief 是否已挂载可用。 */
bool sd_service_is_ready(void);

/** @brief 未就绪原因 / 就绪描述，供界面显示。始终返回非 NULL。 */
const char *sd_service_status(void);

/* ==========================================================================
 * 写
 * ========================================================================== */

/**
 * @brief 把一张 JPEG 存进卡里，自动命名。
 *
 * @param[in]  jpeg      完整的 JPEG 字节
 * @param[in]  len       字节数
 * @param[out] out_name  文件名（**只有文件名，不含路径**）；可为 NULL
 * @param[in]  name_cap  out_name 的容量，建议 SD_PHOTO_NAME_MAX
 *
 * @return ESP_OK；ESP_ERR_INVALID_STATE（卡没挂上）；ESP_ERR_NO_MEM（空间不足）；
 *         其余为文件系统错误。
 *
 * @note 命名规则：优先 `photo_YYYYMMDD_HHMMSS.jpg`（时间取自 SNTP 同步后的系统
 *       时钟）。**时钟没同步时退化为 `boot_SECONDS.jpg`** —— 这样即使没联网，
 *       照片也不会因为时间戳全是 1970 而互相覆盖。
 * @note 重名时自动加 `_1`、`_2`… 后缀。
 */
esp_err_t sd_service_save_photo(const uint8_t *jpeg, size_t len,
                                char *out_name, size_t name_cap);

/* ==========================================================================
 * 读
 * ========================================================================== */

/**
 * @brief 列出**最近**的照片，最新的排在最前。
 *
 * @param[out] names     名字数组，每项 SD_PHOTO_NAME_MAX 字节
 * @param[in]  max       最多取几个（SD_THUMB_MAX 即可）
 * @param[out] out_count 实际取到几个
 *
 * @note "最近"是按**文件名倒序**排的 —— 文件名里带时间戳，字典序就是时间序。
 *       这也意味着混进卡里的、名字不匹配 `photo_*` / `boot_*` 的 JPEG 会被忽略，
 *       这是有意的：相册只该显示本机拍的照片。
 */
esp_err_t sd_service_list_photos(char names[][SD_PHOTO_NAME_MAX],
                                 int max, int *out_count);

/**
 * @brief 读一张照片并解码成 **LVGL 可直接用的 RGB565**（1/2 缩放 → 160×120）。
 *
 * @param[in]  name    文件名（不含路径）
 * @param[out] dst     输出缓冲，至少 SD_THUMB_BYTES
 * @param[in]  dst_px  dst 能装多少个 **uint16_t**
 * @param[out] out_w,out_h 实际解出的尺寸；可为 NULL
 *
 * @warning ⚠️ **字节序：`swap_color_bytes` 必须为 0。**
 *          该标志为 0（也就是零初始化时的默认值）时，esp_jpeg 写的是
 *          **低字节在前 = native 小端**，正是 LVGL 要的 —— 与
 *          `camera_service_frame()` 交换之后的一致。
 *          esp_jpeg 自己的 example 里写的是 1，**不能照抄**。
 *          判据：**缩略图颜色不对但界面正常 ⇒ 就是这里**（与摄像头同款判据）。
 *
 * @note 阻塞调用：读文件 + 解码，QVGA 1/2 缩放大致几十毫秒。
 *       **不要在持有 LVGL 锁时调用**。
 */
esp_err_t sd_service_load_thumb(const char *name, uint16_t *dst, size_t dst_px,
                                int *out_w, int *out_h);

/* ==========================================================================
 * 验证
 * ========================================================================== */

/**
 * @brief 【P0.5 硬件验证】写入 → 读回 → **逐字节比对**。
 *
 * 为什么值得单独做一个函数：接线没验证过的时候，"挂载成功"**不等于**
 * "读写可靠"——DAT0 接触不良可以让卡初始化成功却在数据阶段丢字节。
 * 逐字节比对是唯一能把这件事钉死的判据。
 *
 * @return ESP_OK（一致）；ESP_ERR_INVALID_CRC（读回与写入不符）；
 *         ESP_FAIL（底层 I/O 失败）；ESP_ERR_INVALID_STATE（卡没挂上）。
 *
 * @note 会写一个 4KB 的临时文件、校验、然后删掉，**不污染卡上的内容**。
 */
esp_err_t sd_service_selftest(void);

#ifdef __cplusplus
}
#endif
