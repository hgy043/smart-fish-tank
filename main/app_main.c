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
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "nvs_flash.h"

#include "gec_board.h"
#include "lcd_display.h"
#include "audio_service.h"
#include "app_config.h"
#include "app_events.h"
#include "app_types.h"
#include "app_secrets.h"
#include "wifi_sta.h"
#include "bafa_mqtt.h"

static const char *TAG = "app_main";

/* ==========================================================================
 * 全局生态状态（唯一实例）
 *
 * 由 ui_task 与 mqtt_task 共享读取。写操作只发生在 sensor_task（P1 接入）；
 * 在此之前的链路状态由本文件的刷新函数维护。
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
    s_eco.ph             = 7.0f;
    s_eco.water_temp_c   = 25.0f;
    s_eco.air_temp_c     = 25.0f;
    s_eco.air_humidity   = 50.0f;
    s_eco.lux            = 300.0f;
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
    s_eco.alert_active   = false;    /* P1 接入阈值判定后改写 */
    xSemaphoreGive(s_eco_mutex);
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

#if P05_TEST_LCD
    /* ======================================================================
     * LCD 项 —— 已改为**移植**粤嵌官方已验证 BSP（06-tcp_client_bafa_mqtt），
     * 不再自行重写、不再做参数扫描。
     *
     * 此前十几轮失败的根本原因是"重写"而非"复用"：自己选 SPI 模式、
     * 自己定 max_transfer_sz/条带尺寸、自己删掉片选步骤、自己造扫描框架，
     * 每一步都引入新变量，失败时无法区分是哪个变量的错。
     *
     * 现在只做一件事：按厂家顺序初始化，然后画彩条，看屏幕。
     * ====================================================================== */
    ESP_LOGI(TAG, "[P05] ========== LCD 点屏（移植版）==========");

    esp_err_t ret = lcd_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[P05] lcd_init 失败: %s", esp_err_to_name(ret));
    } else {
        lcd_dump_pin_levels();

        (void)lcd_backlight(true);
        vTaskDelay(pdMS_TO_TICKS(300));

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
    }
    ESP_LOGI(TAG, "[P05] ========== LCD 点屏结束 ==========");
#endif

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

    const TickType_t period = pdMS_TO_TICKS(APP_UI_REFRESH_PERIOD_MS);
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t   loop = 0;

    for (;;) {
        /*
         * 100Hz 时基 => 1 tick = 10ms。APP_UI_REFRESH_PERIOD_MS = 30ms
         * 不是 10ms 的整数倍，pdMS_TO_TICKS 会取整，因此必须用
         * vTaskDelayUntil 的绝对节拍推进，避免误差累积。
         */
        vTaskDelayUntil(&last_wake, period);

        /* --- 此处为 LVGL 刷新区（P2 接入） --- */

        /* 每 10 秒打一次心跳。用 ESP_LOGD 以便需要时用日志等级过滤，
           避免长期运行时刷屏。 */
        if (++loop % (10000 / APP_UI_REFRESH_PERIOD_MS) == 0) {
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

    /* 7. 先启动网络服务，再创建依赖它的任务。
       顺序很重要：mqtt_task 一启动就会调用 bafa_mqtt_pump()，若此时发送队列
       还没建好，就会走「服务未就绪」分支。虽然 pump 已经改为等待超时，
       但从源头消除这个窗口更干净。两步都**不阻塞**——Wi-Fi 由事件驱动重连，
       MQTT 由退避定时器驱动重连。 */
    esp_err_t nret = wifi_sta_start();
    if (nret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 启动失败: %s（设备将保持离线运行）", esp_err_to_name(nret));
    }

    nret = bafa_mqtt_start();
    if (nret != ESP_OK) {
        ESP_LOGE(TAG, "MQTT 启动失败: %s（设备将保持离线运行）", esp_err_to_name(nret));
    }

    /* 8. 建立任务边界 */
    TaskHandle_t h_ui = NULL, h_mqtt = NULL, h_mon = NULL;

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

    ok = xTaskCreatePinnedToCore(monitor_task, "monitor_task",
                                 APP_TASK_STACK_MONITOR, NULL,
                                 APP_TASK_PRIO_MONITOR, &h_mon,
                                 APP_TASK_CORE_1);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    monitor_register("monitor_task", h_mon);

    ESP_LOGI(TAG, "P0 启动完成：%d 个任务运行中", s_monitored_cnt);
    ESP_LOGI(TAG, "巴法云主题: 上报=%s/up  命令=%s",
             BAFA_TOPIC_SENSOR, BAFA_SUBSCRIBE_TOPIC);
    log_memory_report();

    /* app_main 返回后由 FreeRTOS 回收其任务，业务全部由上述任务承担 */
}
