/**
 * @file app_config.h
 * @brief 集中配置：周期、阈值、任务参数
 *
 * 禁止在业务代码里散落魔法数字。所有可调参数集中于此。
 *
 * 【重要】CONFIG_FREERTOS_HZ = 100，即 1 tick = 10ms。
 * 因此下列所有周期常量都必须是 10 的整数倍，否则 pdMS_TO_TICKS 会向下取整
 * 到 0 或 10ms，导致任务空转或节拍失真。
 */
#pragma once

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 任务周期
 * ========================================================================== */
#define APP_SAMPLE_PERIOD_MS        1000        /* 传感器采样        */
#define APP_VISION_PERIOD_MS        5000        /* 视觉抓拍          */
#define APP_MQTT_REPORT_PERIOD_MS   10000       /* MQTT 周期上报     */
#define APP_AI_REPORT_PERIOD_MS     1800000     /* 小智 AI 建议 30min */
#define APP_MONITOR_PERIOD_MS       10000       /* 健康检查          */

/* ui_task 期望 30ms 刷新，但 100Hz 时基下最小步进是 10ms。
   使用 30ms 并配合 vTaskDelayUntil 累积，长期平均节拍准确。 */
#define APP_UI_REFRESH_PERIOD_MS    30
#define APP_UI_DATA_REFRESH_MS      200         /* 数据类标签刷新    */

/* ==========================================================================
 * 报警阈值（Agent.md 第 13 节）
 * ========================================================================== */
#define THRESHOLD_PH_MIN            6.5f
#define THRESHOLD_PH_MAX            7.5f
#define THRESHOLD_WATER_TEMP_MIN    22.0f
#define THRESHOLD_WATER_TEMP_MAX    28.0f
#define THRESHOLD_TDS_MIN           100.0f
#define THRESHOLD_TDS_MAX           500.0f
#define THRESHOLD_TURBIDITY_MAX     20.0f
#define THRESHOLD_LUX_MIN           200.0f
#define THRESHOLD_LUX_MAX           1000.0f

/* ==========================================================================
 * 任务优先级 / 栈（Agent.md 第 6 节；未实测前禁止随意调整）
 *
 * ⚠️ 栈大小以**字节**为单位（xTaskCreate 的参数）。
 * 2026-10-12 上板实测的高水位（uxTaskGetStackHighWaterMark 返回字节）：
 *   ui_task      余 6180 / 8192   => 75%
 *   mqtt_task    余 5412 / 8192   => 66%
 *   monitor_task 余  980 / 3072   => 32%   <-- 偏低，已加到 5120
 * 这些读数是**开机 11 秒**时取的，尚非最坏情况（P2 起有 LVGL、
 * P6 起有摄像头）。P8 压力测试时必须重新核对，不得沿用此处结论。
 * ========================================================================== */
#define APP_TASK_PRIO_SENSOR        5
#define APP_TASK_STACK_SENSOR       4096
#define APP_TASK_PRIO_VISION        4
#define APP_TASK_STACK_VISION       8192
#define APP_TASK_PRIO_VOICE         4
#define APP_TASK_STACK_VOICE        8192
#define APP_TASK_PRIO_AUDIO         3
#define APP_TASK_STACK_AUDIO        4096
#define APP_TASK_PRIO_MQTT          3
#define APP_TASK_STACK_MQTT         8192
#define APP_TASK_PRIO_UI            2
#define APP_TASK_STACK_UI           8192
#define APP_TASK_PRIO_AI            2
#define APP_TASK_STACK_AI           8192
#define APP_TASK_PRIO_MONITOR       1
#define APP_TASK_STACK_MONITOR      5120    /* 原 3072，实测仅余 980 字节 */

/* 任务核心绑定：-1 表示不指定 */
#define APP_TASK_CORE_0             0
#define APP_TASK_CORE_1             1
#define APP_TASK_CORE_ANY           (-1)

/* ==========================================================================
 * 缓冲区（放在 PSRAM 的大块内存）
 * ========================================================================== */
#define CAMERA_FB_IN_PSRAM          1
#define LVGL_BUFFER_IN_PSRAM        1

/* ==========================================================================
 * 存储与队列
 * ========================================================================== */
#define APP_SENSOR_QUEUE_LEN        8
#define APP_VISION_QUEUE_LEN        4
#define APP_MQTT_CACHE_LEN          16      /* 断网时本地缓存条数 */

/* ==========================================================================
 * P0.5 硬件验证开关
 *
 * 这些试验的设计目的：**排除法与 A/B 对照**，用一次烧录尽量多地裁决未决项。
 * 每项都会打一条带标记的结论日志，便于从串口直接读答案。
 * ========================================================================== */

/* 【已删除】P05_MODE_LCD_ONLY —— 它唯一的作用是派生 C1 开关。
 * C1 已作废（见下），故该模式开关一并删除，避免留下无人引用的死配置。
 * 若日后确实需要「只跑 LCD、不碰音频」的隔离测试，直接关 P05_TEST_AUDIO 即可。 */

/* 音频重复播放测试（验证"只响一次"已修）。真因与修法见 architecture.md 7.2。 */
#define P05_TEST_AUDIO              1

/* 音频 / LCD **初始化顺序对照试验** —— **默认关闭**。
 * 设计目的：判定「喇叭无声」是否由 LCD 占用 I2S 引脚引起。
 * 2026-10-13 查明真因是 **I2S 的 BCLK/WS 被错写成 IO39/IO40**（正是 LCD 的
 * DC/MOSI），改回厂家代码的 IO14/IO13 后冲突即消失，本试验不再需要。
 * 代码保留，供日后怀疑引脚冲突时直接复用。 */
#define P05_TEST_AUDIO_LCD_ORDER    0

/* 上电即点亮 LCD 并画彩条（验证 SPI 通路、方向、颜色通道） */
#define P05_TEST_LCD                1

/* 方向(8) × 背光极性(2) 枚举试验 —— **已裁决，默认关闭**（2026-10-13）。
 *   方向：彩条能被逐条读出 ⇒ MV|MX|MY 正确（MADCTL 终值 0xE0）
 *   背光：LCD_BL_INVERT 改 1 后屏亮 ⇒ 高电平点亮
 * 默认关闭的原因：它要跑 64 秒并来回翻转背光极性，会干扰「颜色顺序」的裁决。
 * 复现方法：置 1 重新编译。 */
#define P05_TEST_LCD_ORIENT_SWEEP   0

/* 全屏单色探针：依次全屏刷 红→绿→蓝→白→黑（每色 3 秒，串口同步打印）。
 *
 * 为什么需要它：把 rgb_ele_order 由 BGR 改 RGB 后，彩条由「蓝|绿|红|白|黑」
 * 变为「红|蓝|绿|白|黑」—— 红蓝归位，却出现**绿蓝对调**。而 MADCTL bit3
 * 只交换红蓝，**不存在绿蓝互换的开关** ⇒ 现象与理论不符。
 *
 * 此时不能再靠彩条推：彩条的结论依赖「左→右」这一人为读数顺序，报错一位
 * 就会把结论带偏（本项目多次栽在未受控的观测上）。全屏单色去掉了这个隐患。 */
#define P05_TEST_LCD_COLOR_PROBE    1

/* ---- 已裁决完毕、相关代码已删除的试验（结论保留在此，避免重蹈）----
 *
 * ① LCD 参数扫描（SPI 模式 {0,3,1,2} × 时钟 {80,40}MHz）：框架已删。
 *    SPI 模式定案为 **3**（见 lcd_display.c 的 LCD_SPI_MODE），以能出图的
 *    06 BSP 为准。原 `P05_LCD_SWEEP` / `P05_LCD_HOLD_MS` 两个宏**全工程无人
 *    引用**，属指向已删框架的死配置，已按本项目惯例一并清除。
 *
 * ② 背光引脚：**IO42，高电平点亮**。
 *    ⚠️ 此处原记「拉高熄屏、拉低亮屏 ⇒ 低电平点亮」，**该结论已被推翻**。
 *    2026-10-13 把 lcd_display.c 的 LCD_BL_INVERT 由 0 改为 1（改为高电平
 *    点亮）后屏幕随即点亮，与 reference 06 BSP 的 `bsp_display_backlight_on()`
 *    （duty=1023，引脚恒高）一致。早前那次 A/B 的观测方式已不可考，以本次为准。
 *
 * ③ LED 引脚：拉低 IO10 时 LED 亮且屏幕闪 ⇒ IO10 同时关联 LED 与屏，
 *    不得作普通 GPIO 使用（见 docs/pinmap.md 8.2）。
 *
 * ④ 颜色顺序：**RGB（MADCTL BGR 位 = 0）**，与两套厂家参考都相反。
 *    判据：配 BGR 时彩条呈「蓝|绿|红|白|黑」——红蓝互换、黑白正常。
 *    详见 lcd_display.c 内 lcd_init() 的注释。
 *
 * 教训：本项目已**四次**遇到「厂家注释与硬件不符」，本次又添一例 ——
 * **连本项目自己的实测记录本身也会错**（见②）。一律以能复现的对照实验为准，
 * 且每次只改一个变量。 */

/* 【C1 已作废】原「I2S 引脚占用冲突检测」。
 *
 * 2026-10-13 查明：**C1（LCD 与 I2S 争用 IO39/IO40）根本不存在**，是引脚表
 * 把 I2S 的 BCLK/WS 抄成了 LCD 的 DC/MOSI。两套厂家已验证代码一致写
 * **BCLK=IO14、WS=IO13**，与 LCD 的 IO39/IO40/IO41 零重叠。
 * 完整证据与教训见 gec_board.h 第四节。
 *
 * 它被当作「本项目最大架构风险」追了多轮，实际从未存在 —— 这正是把二手
 * 表格当真源、而不回源码核对的代价。试验代码保留但**永久关闭**，作为记录。 */
#define P05_TEST_C1_I2S_COLLISION   0

/* 在 LCD 彩条上叠加进度条，用于长跑观察是否花屏/撕裂 */
#define P05_TEST_LCD_ANIMATION      0

#ifdef __cplusplus
}
#endif
