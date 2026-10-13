/**
 * @file camera_service.h
 * @brief 摄像头服务（DVP 并口模组，P6-a）
 *
 * ============================================================================
 * 【板上实际是哪颗 sensor —— 实测，不要按型号名猜】
 *
 * 2026-10-13 首版上板实测，驱动的自动探测结果：
 *     camera: Camera PID=0x9b ... Detected GC0308 camera
 *     camera: Detected camera at address=0x21
 *     camera: sensor: GC0308 (PID=0x009B)
 *
 * 是 **GC0308**（I2C 地址 **0x21**），**不是** OV2640（那会是地址 0x30、
 * PID 0x26）。此前所有文档与注释都按「OV2640 类」描述，属于**没有实测的
 * 想当然** —— 以本段和串口日志为准。
 *
 * GC0308 的相关特性（已在驱动 `sensors/gc0308.c` 中核实）：
 *   · 原生支持 `PIXFORMAT_RGB565`（寄存器 OUTPUT_FMT）
 *   · 支持 `set_colorbar` / `set_hmirror` / `set_vflip` —— 本组件要用到的都在
 *   · 原生分辨率 640×480；**QVGA 是驱动按 2:1 抽样( subsample)得到的**
 *     （串口会打 `gc0308: subsample win:640x480, ratio:0.500000`），
 *     所以可能出现轻微摩尔纹/锯齿。若观感不可接受，见下方分辨率一节。
 * ============================================================================
 * 【本组件只做一件事：把「拍一帧能直接交给 LVGL 的画面」封装成一个函数】
 *
 * 值域约定（重要，见 camera_service.c 里的字节序说明）：
 *   `camera_service_frame()` 返回的缓冲是**标准 native(little-endian) RGB565**，
 *   即 `uint16_t` 的高 5 位是红、中间 6 位是绿、低 5 位是蓝 —— 和 LVGL 内部
 *   表示、以及 `lcd_draw_bitmap()` 的入参完全一致。调用方**不需要**再做任何
 *   字节交换。
 *
 *   链条上共两次字节交换，各管一段，不要搞混：
 *       摄像头 DVP 输出（大端）── ①本组件交换 ──▶ LVGL（native LE）
 *                                                      │
 *                              lcd_display::panel_send() ② ──▶ ST7796 面板
 *
 *   所以「照片花屏但界面正常」= 本组件的第 ① 次没做对；
 *      「照片和界面都花」     = 面板侧（第 ② 次）出问题，与本组件无关。
 *
 * ============================================================================
 * 线程约束：`camera_service_capture()` 会**阻塞**（`esp_camera_fb_get()` 最长
 * 等 4 秒），因此：
 *   · **绝不可**在持有 LVGL 锁时调用 —— 会把 taskLVGL 一起卡住，界面停摆；
 *   · 也**不可**在中断里调用。
 * 推荐做法：界面上的按钮只置一个标志，由 `ui_task` 在**锁外**轮询后调用。
 * ============================================================================
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 可调参数
 * ========================================================================== */

/** 输出分辨率。QVGA(320×240) 而非 xiaozhi 的 VGA(640×480)，理由：
 *   ① 屏幕是 480×320 —— QVGA 可以 **1:1 显示**，不缩放就不糊；
 *   ② 单帧 153 KB vs 614 KB，PSRAM 占用小 4 倍；
 *   ③ 每帧要逐像素做字节交换，76800 次 vs 307200 次。
 *  ⚠️ 改这里必须同步确认 `camera_service.c` 里对 `fb->width/height` 的校验 ——
 *     该项校验就是为了防止「配置改了、缓冲没改」导致越界。
 *
 *  【若 QVGA 观感太糊】GC0308 的 QVGA 是 2:1 抽样得来的，不是原生窗口。
 *  两个备选，按代价从低到高：
 *    · 用 FRAMESIZE_VGA 抓、**居中裁 480×320** 显示 —— 1:1、无缩放开销，
 *      代价是丢掉画面四周；
 *    · 用 FRAMESIZE_VGA 抓、让 LVGL 缩放 —— 画面完整，但本工程
 *      `CONFIG_LV_CACHE_DEF_SIZE=0`（无图像缓存），缩放每帧软件重算，费 CPU。
 *  **先按 QVGA 看实际效果再决定**，不要提前优化。 */
#define CAMERA_SERVICE_H_RES        320
#define CAMERA_SERVICE_V_RES        240

/** 抓帧前丢弃的帧数。DVP 模组上电后前几帧常是残影/未稳定的曝光，
 *  xiaozhi 的 `Capture()` 同样取 2 帧（丢第 1 帧）。
 *
 *  ⚠️ 这个"丢弃"不是可选优化，是**功能正确性**的一部分：
 *      `fb_count=1` + `CAMERA_GRAB_WHEN_EMPTY` 下，上次 `fb_return()` 之后驱动
 *      会在下一个 VSYNC 立刻预采一帧并排队，那一帧会**一直老化到下次拍照**。
 *      不丢弃它，**用户按下快门拿到的会是上一次按快门时的画面**。 */
#define CAMERA_SERVICE_WARMUP_FRAMES 2

/** 上电后等待模组稳定的时间。
 *  xiaozhi 没有显式等待（它上电后立刻 init），这里加一点保险 ——
 *  代价只有一次开机延迟，而模组供电未稳时 SCCB 探测失败是很难查的故障。 */
#define CAMERA_SERVICE_POWER_SETTLE_MS 100

/** 镜像 / 翻转。**由实测决定，不要凭空写。**
 *  确定方法：拍一张有明确左右/上下特征的画面（例如拍一段文字），
 *  对着屏幕核对。若文字左右反了 ⇒ 置 HMIRROR=1；上下颠倒 ⇒ VFLIP=1。
 *
 *  ⚠️ xiaozhi 在它的构造函数里对 GC0308 有一句特判：`if (PID == GC0308_PID)
 *     set_hmirror(s, 0);`，注释写"写1镜像 写0不镜像" —— 说明这颗 sensor 的
 *     寄存器默认值不是"不镜像"，需要显式关掉。故本组件**总是**显式下发这两个
 *     设置（而不是"值为 0 就跳过"），以避开出厂默认的不确定性。
 *
 *  当前值 0/0 是**初始值，不是实测结论** —— 第一次看到画面后按实际情况改。 */
#define CAMERA_SERVICE_HMIRROR      0
#define CAMERA_SERVICE_VFLIP        0

/* ==========================================================================
 * API
 * ========================================================================== */

/**
 * @brief 初始化摄像头：PCA9557 P2 上电 → esp_camera_init() → 分配输出缓冲。
 *
 * @note  **幂等**，且失败后可重试（但注意 `esp_camera_init()` 若已成功过一次，
 *        驱动不允许重复初始化 —— 本函数用 s_inited 挡住）。
 * @note  必须先 `gec_board_i2c_init()` —— SCCB 复用那条总线，总线不存在则
 *        驱动探测不到 sensor。
 * @note  失败**不致命**：调用方应记录原因并继续跑，界面照常显示，
 *        只是拍照不可用（本组件提供 `camera_service_status()` 供界面显示原因）。
 */
esp_err_t camera_service_init(void);

/** @brief 是否已就绪（可拍照）。 */
bool camera_service_is_ready(void);

/** @brief 未就绪原因 / 就绪描述，供界面显示。始终返回非 NULL。 */
const char *camera_service_status(void);

/**
 * @brief 抓一帧并转成标准 native RGB565。
 *
 * @return ESP_OK；ESP_ERR_INVALID_STATE（未初始化）；
 *         ESP_ERR_TIMEOUT（取帧超时）；ESP_ERR_INVALID_SIZE（帧格式/尺寸与预期不符）。
 *
 * @warning **阻塞调用**，最长约 4 秒。见文件头的线程约束。
 */
esp_err_t camera_service_capture(void);

/**
 * @brief 取最近一次成功抓取的画面。
 *
 * @param width,height 输出尺寸，可为 NULL。
 * @return 标准 native RGB565 缓冲指针（尺寸 = width×height，行连续）；
 *         未就绪或从未成功抓取过则返回 NULL。
 *
 * @warning 指针指向**本组件自有**的缓冲，会被下一次 `camera_service_capture()`
 *         覆盖。调用方若需长期持有（例如交给 LVGL 显示），**必须自行拷一份**。
 */
const uint16_t *camera_service_frame(int *width, int *height);

/**
 * @brief 【诊断】让 sensor 输出测试彩条，而不是真实画面。
 *
 * @param on true = 出彩条，false = 恢复正常取景。
 *
 * @note  存在的理由：判断「颜色通道对不对」时，彩条比真实场景硬得多 ——
 *        8 条**纯饱和色**不依赖拍摄场景、不依赖自动白平衡、不依赖光照。
 *        这与本项目 LCD 侧那张自标注色卡是同一个思路：
 *        **先让被测对象自身可判读，再谈结论**。
 *
 *        判据：应出现 8 条纯色竖条。若出现"暗蓝/暗绿"这类不纯的颜色，
 *        **是字节序问题**（绿字段 6 位且跨字节边界，交换后会把它打散，
 *        而红/蓝各 5 位对称、几乎抵消）—— 而不是画面内容问题。
 *
 * @note  改完记得关掉，否则拍到的永远是彩条。
 */
esp_err_t camera_service_set_colorbar(bool on);

#ifdef __cplusplus
}
#endif
