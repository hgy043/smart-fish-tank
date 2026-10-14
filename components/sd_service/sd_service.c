/**
 * @file sd_service.c
 * @brief TF 卡存储服务实现（1-bit SD 模式，P6-c）
 *
 * 引脚与硬件说明见 sd_service.h 顶部；四个"会炸的坑"在下面各处的 ⚠️ 里。
 *
 * ── 与参考实现的关系 ──────────────────────────────────────────────────────
 * 结构照 IDF 自带示例 `examples/storage/sd_card/sdmmc/main/sd_card_example_main.c`：
 * 同一个 `SDMMC_HOST_DEFAULT()` + `SDMMC_SLOT_CONFIG_DEFAULT()` 组合，
 * 只是把三个脚换成本板的、并把 width 显式设成 1（示例里也这么写，但原因不同 ——
 * 见下面坑 ①）。
 */
#include "sd_service.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "jpeg_decoder.h"

#include "gec_board.h"

static const char *TAG = "sd";

/* ==========================================================================
 * 内部状态
 * ========================================================================== */
static bool          s_inited = false;
static bool          s_ready  = false;
static sdmmc_card_t *s_card   = NULL;

/** 未就绪原因描述。与 camera_service 同款：静态缓冲，因为要交给界面显示。 */
static char s_status[128] = "未初始化";

static void set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    va_end(ap);
}

const char *sd_service_status(void)
{
    return s_status;
}

bool sd_service_is_ready(void)
{
    return s_ready;
}

/* ==========================================================================
 * 挂载
 * ========================================================================== */

/**
 * @brief 把底层错误码翻译成"下一步该查什么"。
 *
 * 这是接线还没验证时最省时间的一段。裸的 `0x107` 对排障没有任何帮助，
 * 而"没插卡，或 CMD/CLK 开路"能直接告诉人该看哪里。
 *
 * 出处：`esp_vfs_fat_sdmmc_mount()` → `sdmmc_card_init()` 各阶段
 * （`sdmmc_common.c` / `sdmmc_sd.c` / `sdmmc_transaction.c`）。
 */
static void set_status_from_err(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_TIMEOUT:
        /* CMD8/CMD5 的超时会被吞掉，真正卡住的是 ACMD41/CMD1 —— 没有卡时
           它们收不到响应。而这几根线开路时现象完全一样，所以分不出来。 */
        set_status("没插卡，或 CMD/CLK 开路、接错（这个层次分不出来）");
        break;
    case ESP_ERR_INVALID_CRC:
    case ESP_ERR_INVALID_RESPONSE:
        /* 收到了响应但内容不对 ⇒ 线上有东西在跑，只是不对。
           典型是两根线接反、或上拉/走线有问题。 */
        set_status("响应 CRC/格式错 —— 接线有问题（噪声，或 CMD/CLK 接反）");
        break;
    case ESP_FAIL:
        /* 卡初始化过了（说明 CMD/CLK 都通），但 FAT 读不出来。
           DAT0 接触不良、或卡不是 FAT（exFAT/未格式化）。 */
        set_status("卡能通信但 FAT 挂不上：DAT0 有问题，"
                   "或卡是 exFAT／未格式化（本工程不支持 exFAT）");
        break;
    case ESP_ERR_INVALID_ARG:
        set_status("参数非法（GPIO 号或 width 不对）");
        break;
    case ESP_ERR_NO_MEM:
        set_status("内存不足");
        break;
    default:
        set_status("挂载失败：%s (0x%x)", esp_err_to_name(err), (unsigned)err);
        break;
    }
}

esp_err_t sd_service_init(void)
{
    if (s_ready) {
        return ESP_OK;                  /* 幂等 */
    }
    s_inited = true;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();   /* 槽 1 / 20MHz / 3.3V */
    host.max_freq_khz = GEC_SD_FREQ_KHZ;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();

    /* ★ 坑 ①：width **必须**显式设成 1，绝不能靠默认值。
     *
     *   `SDMMC_SLOT_CONFIG_DEFAULT()` 给的是 width = 0，而 0 的含义是
     *   "用这个槽位的最大宽度"；`soc/esp32s3/sdmmc_periph.c` 把**两个槽都**
     *   声明成 `.width = 8`。于是 sdmmc_host_init_slot() 会去配置 d1..d7 ——
     *   而结构体里那几个字段是全零初始化的 ⇒ **GPIO0..GPIO7**，
     *   它们**恰好全是合法 GPIO**，`GPIO_NUM_CHECK` 会放行。
     *
     *   后果：GPIO0（BOOT 键）、IO3/4/5/6/7/8/9（摄像头的数据脚与时钟）
     *   被 SD 驱动接管。画面会坏得莫名其妙，而且**看不出跟 SD 卡有关系**。 */
    slot.width = 1;

    slot.clk = GEC_SD_PIN_CLK;
    slot.cmd = GEC_SD_PIN_CMD;
    slot.d0  = GEC_SD_PIN_D0;
    /* 板上已有 51kΩ 外部上拉（原理图 R7-R10），这只是兜底。
       IDF 文档说内部上拉"不足以"单独支撑 —— 我们不是单独用，所以无妨。 */
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    const esp_vfs_fat_mount_config_t mnt = {
        /* ★ 坑 ②：**永不自动格式化**。
         *
         *   打开它的话，一旦 DAT0 接触不良（卡能初始化、FAT 读失败），
         *   `s_f_mount()` 会**分区并重新格式化一张完全正常的卡**。
         *   接线还没验证过的时候开这个，等于在用户的卡上放了个雷 ——
         *   照片会无声无息地没掉，而串口上看不出发生过什么。
         *   失败就失败，让用户自己决定要不要格。 */
        .format_if_mount_failed = false,

        .max_files = 4,
        .allocation_unit_size = 0,
        /* 没接 card-detect 脚（原理图确认），所以只能靠 CMD13 去问卡还在不在。
           不开的话，拔卡之后读写会以一种很难懂的方式失败；开了就是干净的
           STA_NOINIT。代价是每次状态检查多一条命令，可忽略。 */
        .disk_status_check_enable = true,
        .use_one_fat = false,
    };

    ESP_LOGI(TAG, "挂载 TF 卡：CMD=IO%d CLK=IO%d D0=IO%d，%d kHz，1-bit",
             GEC_SD_PIN_CMD, GEC_SD_PIN_CLK, GEC_SD_PIN_D0, GEC_SD_FREQ_KHZ);

    const esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot,
                                                  &mnt, &s_card);
    if (ret != ESP_OK) {
        s_ready = false;
        s_card  = NULL;
        set_status_from_err(ret);
        ESP_LOGE(TAG, "[SD] %s（0x%x）", s_status, (unsigned)ret);
        return ret;
    }

    s_ready = true;

    /* 卡信息：容量是"卡对不对"最直接的证据。
       ⚠️ 别在这里报文件系统类型 —— esp_vfs_fat 不把它暴露出来，
       硬猜（比如读 card_command_class）只会写出一个看起来像那么回事、
       实际毫无意义的字符串。要确认 FAT32 就看挂载本身成不成。 */
    const uint64_t total_kb = (uint64_t)s_card->csd.capacity *
                              (uint64_t)s_card->csd.sector_size / 1024;
    set_status("就绪：%.1f GB", (double)total_kb / 1024.0 / 1024.0);
    ESP_LOGI(TAG, "[SD] 挂载成功：容量 %.1f GB，名称 %s，%d-bit 总线 @ %d kHz",
             (double)total_kb / 1024.0 / 1024.0, s_card->cid.name,
             /* ⚠️ 报**我们自己配的**宽度（slot.width），**不要**报
                `s_card->log_bus_width` —— 那个字段只在 sdmmc_mmc.c（MMC 卡）
                和 sdmmc_io.c（SDIO）里被赋值，**SD 存储卡从来不写它**，
                于是永远是 0。照抄它就会打出"0-bit 总线"，
                让人以为宽度配错了 —— 而卡其实跑得好好的。
                首次上板正是这样：日志写 0-bit，紧接着自检却是逐字节一致。 */
             (int)slot.width, s_card->real_freq_khz);
    return ESP_OK;
}

/* ==========================================================================
 * 写
 * ========================================================================== */

/** 生成一个卡上还不存在的文件名。
 *
 * 命名策略：
 *   · 时钟已同步（年份 ≥2020）→ `photo_YYYYMMDD_HHMMSS.jpg`，字典序 = 时间序，
 *     列表排序直接用它，不必读文件时间戳（FAT 时间戳还依赖 RTC，更不可靠）。
 *   · 时钟没同步 → `boot_<开机秒数>.jpg`。**不能都用 1970 那个时间戳** ——
 *     那样每张照片都重名，只能靠后缀区分，而"最近 6 张"就变成随机的了。
 *
 * ⚠️ **两种名字混在一起时，排序是错的**：字典序 `boot_` < `photo_`，
 *    所以一张 `boot_*` 的照片会被当成**最旧**的，哪怕它其实是刚拍的。
 *    正常不会碰到：拍照要联网（AI 那条路），而联网基本意味着对时也成功了。
 *    真碰上（比如网络禁了 UDP 123 而 TCP 443 通）时的表现是：
 *    某张新拍的照片不出现在「最近 6 张」里。**不建议为它加花招**
 *    （比如用假时间戳骗过排序）—— 那会让文件名开始撒谎，代价更大。
 */
static esp_err_t make_unique_path(char *path, size_t path_cap,
                                  char *name_out, size_t name_cap)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    /* 先把各字段夹进本来的位宽，再用 %0Nu 格式化。
     *
     * 【为什么要多这一道】不加的话 GCC 会按 int 的极值去算 snprintf 的最大输出
     * （`%04d` 最坏 11 个字符），进而判定文件名缓冲"可能被截断"，
     * 而本组件开了 `-Werror=format-truncation` ⇒ **直接挡住构建**。
     * 取模之后每个字段的最大宽度可以静态推算出来，警告随之消失。
     * 顺便也把"时间字段本身就已经越界"这种情况兜住了。 */
    const unsigned year = (unsigned)(tm.tm_year + 1900) % 10000u;
    const unsigned mon  = (unsigned)(tm.tm_mon + 1)      % 100u;
    const unsigned day  = (unsigned)tm.tm_mday           % 100u;
    const unsigned hh   = (unsigned)tm.tm_hour           % 100u;
    const unsigned mm   = (unsigned)tm.tm_min            % 100u;
    const unsigned ss   = (unsigned)tm.tm_sec            % 100u;

    /* "photo_" + 4+2+2 + "_" + 2+2+2 = 21 字符，+NUL = 22。
       给 24 是为了让后面那句 snprintf(name, ..., "%s_%d.jpg") 的
       最大输出（23 + 1 + 2 + 4 + 1 = 31）仍装得进 SD_PHOTO_NAME_MAX(32)。 */
    char base[24];
    if (tm.tm_year + 1900 >= 2020) {
        snprintf(base, sizeof(base), "photo_%04u%02u%02u_%02u%02u%02u",
                 year, mon, day, hh, mm, ss);
    } else {
        snprintf(base, sizeof(base), "boot_%08u",
                 (unsigned)(esp_timer_get_time() / 1000000) % 100000000u);
    }

    for (int attempt = 0; attempt < 100; attempt++) {
        char name[SD_PHOTO_NAME_MAX];
        if (attempt == 0) {
            snprintf(name, sizeof(name), "%s.jpg", base);
        } else {
            snprintf(name, sizeof(name), "%s_%d.jpg", base, attempt);
        }

        const int n = snprintf(path, path_cap, "%s/%s", SD_MOUNT_POINT, name);
        if (n < 0 || (size_t)n >= path_cap) {
            return ESP_ERR_INVALID_SIZE;
        }

        struct stat st;
        if (stat(path, &st) != 0) {         /* 不存在 ⇒ 可用 */
            if (name_out != NULL && name_cap > 0) {
                snprintf(name_out, name_cap, "%s", name);
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_STATE;           /* 同名太多，不正常 */
}

esp_err_t sd_service_save_photo(const uint8_t *jpeg, size_t len,
                                char *out_name, size_t name_cap)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (jpeg == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[96];
    const esp_err_t pret = make_unique_path(path, sizeof(path), out_name, name_cap);
    if (pret != ESP_OK) {
        ESP_LOGE(TAG, "取不到可用文件名");
        return pret;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "打不开 %s 写入", path);
        return ESP_FAIL;
    }

    const size_t written = fwrite(jpeg, 1, len, f);
    /* fclose 的返回值也要看 —— FATFS 的缓冲可能到这一刻才真正落盘，
       只查 fwrite 会把"写了一半"当成成功（bafa_mqtt 那边踩过同款）。 */
    const int cret = fclose(f);

    if (written != len || cret != 0) {
        ESP_LOGE(TAG, "写 %s 失败：fwrite %u/%u，fclose=%d",
                 path, (unsigned)written, (unsigned)len, cret);
        (void)remove(path);                 /* 别在卡上留半张图 */
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[SD] 已存 %s（%u 字节）", path, (unsigned)len);
    return ESP_OK;
}

/* ==========================================================================
 * 读
 * ========================================================================== */

/** 只认本机拍的照片。混进卡里的其它 JPEG（图标、壁纸…）不该出现在相册里。 */
static bool is_our_photo(const char *name)
{
    if (strncmp(name, "photo_", 6) != 0 && strncmp(name, "boot_", 5) != 0) {
        return false;
    }
    const size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".jpg") == 0;
}

/** 把一个名字插进**降序**数组，保持降序，超过 max 就丢掉最旧的。 */
static void insert_desc(char names[][SD_PHOTO_NAME_MAX], int *count, int max,
                        const char *name)
{
    int pos = *count;
    while (pos > 0 && strcmp(names[pos - 1], name) < 0) {
        pos--;                              /* 前面的更旧 ⇒ 新名字要插到它前面 */
    }
    if (pos >= max) {
        return;                             /* 比现有全都旧，留不下 */
    }

    const int last = (*count < max) ? *count : (max - 1);
    for (int i = last; i > pos; i--) {
        memcpy(names[i], names[i - 1], SD_PHOTO_NAME_MAX);
    }
    snprintf(names[pos], SD_PHOTO_NAME_MAX, "%s", name);

    if (*count < max) {
        (*count)++;
    }
}

esp_err_t sd_service_list_photos(char names[][SD_PHOTO_NAME_MAX],
                                 int max, int *out_count)
{
    if (out_count == NULL || names == NULL || max <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    DIR *dir = opendir(SD_MOUNT_POINT);
    if (dir == NULL) {
        ESP_LOGE(TAG, "打不开 %s", SD_MOUNT_POINT);
        return ESP_FAIL;
    }

    int count = 0;
    struct dirent *ent;
    int scanned = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (!is_our_photo(ent->d_name)) {
            continue;
        }
        scanned++;
        /* 逐个插进"最近 max 个"的候选表 —— 不需要先把整卡读进内存，
           卡上放了几百张也只占 max 条。 */
        insert_desc(names, &count, max, ent->d_name);
    }
    closedir(dir);

    *out_count = count;
    ESP_LOGI(TAG, "[SD] 扫到 %d 张照片，取最近 %d 张", scanned, count);
    return ESP_OK;
}

/* 读文件的暂存缓冲。惰性分配、跨次复用 —— 一次只解一张图，不必每张都 malloc。
   放 PSRAM：它可能装下一整张几 MB 的 JPEG，而内部堆是本工程最紧的资源。 */
static uint8_t *s_jpg_buf = NULL;
static size_t   s_jpg_cap = 0;

/** 单张 JPEG 的大小上限。QVGA 实拍约 7~15KB；用户从电脑拷进来的可能大得多。
 *  256KB 足够放下 1/8 缩放的任何常见照片，再大就只能放弃了。 */
#define SD_JPEG_MAX     (256 * 1024)

static esp_err_t read_file(const char *name, uint8_t **out, size_t *out_len)
{
    char path[96];
    const int n = snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, name);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        /* 别静默返回 —— 相册页只会显示"无法显示"，看不出是"文件不存在"
           还是"解码失败"。这条日志是两者之间唯一的分界。 */
        ESP_LOGE(TAG, "stat(%s) 失败 —— 文件不在，或 VFS 不支持 stat", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size <= 0 || st.st_size > SD_JPEG_MAX) {
        ESP_LOGW(TAG, "%s 大小 %ld 超出范围", name, (long)st.st_size);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t need = (size_t)st.st_size;
    if (need > s_jpg_cap) {
        if (s_jpg_buf != NULL) {
            heap_caps_free(s_jpg_buf);
            s_jpg_buf = NULL;
            s_jpg_cap = 0;
        }
        s_jpg_buf = (uint8_t *)heap_caps_malloc(need,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_jpg_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        s_jpg_cap = need;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "fopen(%s) 失败", path);
        return ESP_FAIL;
    }
    const size_t got = fread(s_jpg_buf, 1, need, f);
    fclose(f);
    if (got != need) {
        ESP_LOGE(TAG, "读 %s 不完整：%u/%u", name, (unsigned)got, (unsigned)need);
        return ESP_FAIL;
    }

    *out = s_jpg_buf;
    *out_len = need;
    return ESP_OK;
}

/** 选一个能把整张图装进 dst 的缩放比。
 *
 * 【⚠️ 一个踩过的坑：`esp_jpeg_get_image_info()` 不把 out_scale 算进 width/height】
 *
 * 它的实现是（`jpeg_decoder.c` 的 SOF0 分支）：
 *     img->height = ldb_word(seg + 1);      ← **原始值**，没有除以 scale_div
 *     img->width  = ldb_word(seg + 3);      ← 同上
 *     const uint8_t scale_div = jpeg_get_div_by_scale(cfg->out_scale);
 *     img->output_len = (h / scale_div) * (w / scale_div) * ...;  ← 只有它用了缩放
 *
 * 而**同一族的 `esp_jpeg_decode()` 是按缩放后的尺寸写的**
 * （`img->width = JDEC.width / scale_div`）。两个函数语义不一致，头文件又没写，
 * 于是很容易照 decode 的行为去理解 get_image_info —— 拿 320 去跟 160 比，
 * 永远不成立，最后把"解析出来的图太大"当成结论。
 * **实测代价**：卡里两张 320×240 的照片在相册页全显示不出来，
 * 而电脑上都能正常打开；串口上那句"缩到 1/8 仍超过 160x120"是**假话**。
 *
 * 所以这里**只用 SCALE_0 取一次原始尺寸**，缩放比由我们自己算 ——
 * 不依赖那个会误导人的语义。
 *
 * 【返回值要能区分"解析失败"与"尺寸不合适"】
 * 上一版把所有情况归到同一句话里，把真正的失败原因盖住了。
 * 日志说错话比没有日志更费时间，所以这里分开报。 */
static esp_err_t pick_scale(const uint8_t *jpeg, size_t len,
                            esp_jpeg_image_scale_t *out)
{
    esp_jpeg_image_cfg_t probe = {
        .indata      = (uint8_t *)jpeg,
        .indata_size = len,
        .out_format  = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale   = JPEG_IMAGE_SCALE_0,      /* 1:1 ⇒ 拿到的就是原始尺寸 */
    };
    esp_jpeg_image_output_t raw;
    const esp_err_t pret = esp_jpeg_get_image_info(&probe, &raw);
    if (pret != ESP_OK) {
        ESP_LOGE(TAG, "解析 JPEG 头**失败**：%s（文件 %u 字节）",
                 esp_err_to_name(pret), (unsigned)len);
        return pret;
    }
    ESP_LOGI(TAG, "JPEG %u 字节，原始尺寸 %ux%u",
             (unsigned)len, raw.width, raw.height);

    if (raw.width <= SD_THUMB_W && raw.height <= SD_THUMB_H) {
        *out = JPEG_IMAGE_SCALE_0;      /* 原图就够小，不缩 */
        return ESP_OK;
    }
    if (raw.width / 2 <= SD_THUMB_W && raw.height / 2 <= SD_THUMB_H) {
        *out = JPEG_IMAGE_SCALE_1_2;
        return ESP_OK;
    }
    if (raw.width / 4 <= SD_THUMB_W && raw.height / 4 <= SD_THUMB_H) {
        *out = JPEG_IMAGE_SCALE_1_4;
        return ESP_OK;
    }
    if (raw.width / 8 <= SD_THUMB_W && raw.height / 8 <= SD_THUMB_H) {
        *out = JPEG_IMAGE_SCALE_1_8;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "原始 %ux%u 缩到 1/8 仍超过 %dx%d —— 这张图确实太大",
             raw.width, raw.height, SD_THUMB_W, SD_THUMB_H);
    return ESP_ERR_INVALID_SIZE;
}

esp_err_t sd_service_load_thumb(const char *name, uint16_t *dst, size_t dst_px,
                                int *out_w, int *out_h)
{
    if (out_w != NULL) { *out_w = 0; }
    if (out_h != NULL) { *out_h = 0; }

    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (name == NULL || dst == NULL || dst_px < (size_t)SD_THUMB_W * SD_THUMB_H) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *jpeg = NULL;
    size_t   len  = 0;
    const esp_err_t rret = read_file(name, &jpeg, &len);
    if (rret != ESP_OK) {
        return rret;
    }

    esp_jpeg_image_scale_t scale = JPEG_IMAGE_SCALE_0;
    const esp_err_t sret = pick_scale(jpeg, len, &scale);
    if (sret != ESP_OK) {
        /* pick_scale 里已经把"解析失败"与"图太大"分开说清楚了，这里不重复。
           ⚠️ 判据是**返回值**，不是 scale —— SCALE_0 现在是个合法结果
              （原图本来就够小、不用缩），不能再拿它当失败标志。 */
        ESP_LOGE(TAG, "%s 选不出缩放比，跳过", name);
        return sret;
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata      = jpeg,
        .indata_size = len,
        .outbuf      = (uint8_t *)dst,
        .outbuf_size = (uint32_t)(dst_px * sizeof(uint16_t)),
        .out_format  = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale   = scale,

        /* ★ 坑 ③：**必须是 0**。
         *
         *   它是 0 时，esp_jpeg 写的是**低字节在前 = native 小端**，
         *   正是 LVGL 要的 —— 与 `camera_service_frame()` 字节交换之后一致。
         *   esp_jpeg 自己的 example（decode_image.c:51）里写的是 1，
         *   **不能照抄**：那会红蓝互换、且绿色通道（6 位、跨字节边界）被打散，
         *   表现为"颜色怪怪的但形状对"。
         *
         *   判据：**缩略图颜色不对但界面正常 ⇒ 就是这里**（与摄像头同款判据）。 */
        .flags = { .swap_color_bytes = 0 },

        /* advanced.working_buffer 留 NULL：让 esp_jpeg 自己按它编译期定的
           JPEG_WORK_BUF_SIZE 分配（约 3.1KB，走 PSRAM）。**不自己估这个尺寸** ——
           它随 JD_FASTDECODE 变，估小了会写越界，估大了白占。 */
    };

    esp_jpeg_image_output_t out;
    const esp_err_t dret = esp_jpeg_decode(&cfg, &out);
    if (dret != ESP_OK) {
        ESP_LOGE(TAG, "解码 %s 失败：%s", name, esp_err_to_name(dret));
        return dret;
    }

    if (out_w != NULL) { *out_w = out.width; }
    if (out_h != NULL) { *out_h = out.height; }
    return ESP_OK;
}

/* ==========================================================================
 * 验证：写入 → 读回 → 逐字节比对
 * ========================================================================== */
#define SD_TEST_PATH    SD_MOUNT_POINT "/selftest.bin"
#define SD_TEST_BYTES   4096

esp_err_t sd_service_selftest(void)
{
    if (!s_ready) {
        ESP_LOGE(TAG, "[SD] 未挂载，无法自检");
        return ESP_ERR_INVALID_STATE;
    }

    /* 用固定种子的伪随机，而不是纯递增 —— 递增序列在"整块错位"这类故障下
       仍可能碰巧对上，随机数据不会。 */
    static uint8_t wr[SD_TEST_BYTES];
    uint32_t seed = 0x5A5A1234u;
    for (size_t i = 0; i < sizeof(wr); i++) {
        seed = seed * 1103515245u + 12345u;
        wr[i] = (uint8_t)(seed >> 16);
    }

    FILE *f = fopen(SD_TEST_PATH, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "[SD] 自检：打不开 %s", SD_TEST_PATH);
        return ESP_FAIL;
    }
    const size_t wn = fwrite(wr, 1, sizeof(wr), f);
    const int    wc = fclose(f);
    if (wn != sizeof(wr) || wc != 0) {
        ESP_LOGE(TAG, "[SD] 自检：写入失败 %u/%u", (unsigned)wn, (unsigned)sizeof(wr));
        (void)remove(SD_TEST_PATH);
        return ESP_FAIL;
    }

    static uint8_t rd[SD_TEST_BYTES];
    f = fopen(SD_TEST_PATH, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "[SD] 自检：写进去了却打不开读");
        return ESP_FAIL;
    }
    const size_t rn = fread(rd, 1, sizeof(rd), f);
    fclose(f);

    esp_err_t ret = ESP_OK;
    if (rn != sizeof(rd)) {
        ESP_LOGE(TAG, "[SD] 自检：读回长度不对 %u/%u",
                 (unsigned)rn, (unsigned)sizeof(rd));
        ret = ESP_FAIL;
    } else {
        for (size_t i = 0; i < sizeof(wr); i++) {
            if (wr[i] != rd[i]) {
                ESP_LOGE(TAG, "[SD] 自检：**第 %u 字节不符**（写 0x%02X 读 0x%02X）"
                              " —— 卡能挂载不等于读写可靠，多半是 DAT0 接触不良",
                         (unsigned)i, wr[i], rd[i]);
                ret = ESP_ERR_INVALID_CRC;
                break;
            }
        }
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "[SD] 自检通过：%u 字节写入 → 读回 → **逐字节一致**",
                 (unsigned)sizeof(wr));
    }

    (void)remove(SD_TEST_PATH);             /* 不留垃圾在用户卡上 */
    return ret;
}
