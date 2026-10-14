/**
 * @file xiaozhi_mcp.c
 * @brief MCP 工具服务端（P7 阶段 4）—— 让 AI 能读鱼缸
 *
 * ============================================================================
 * 【这是什么的移植】
 *
 * xiaozhi 的 `main/mcp_server.cc` + `mcp_server.h` 是一个用 C++ 写的
 * MCP（Model Context Protocol）服务端：注册若干「工具」，服务器（LLM）通过
 * JSON-RPC 2.0 请求调用它们。本文件把它**逐个语义**翻成 C，并去掉本工程
 * 用不到的部分。对应关系：
 *
 *   mcp_server.cc:535-620  ParseMessage        → xz_mcp_handle()
 *   mcp_server.cc:633-639  ReplyResult         → reply_result()
 *   mcp_server.cc:652-659  ReplyError          → reply_error()
 *   mcp_server.cc:683-740  GetToolsList        → on_tools_list()
 *   mcp_server.cc:769-830  DoToolCall          → on_tools_call()
 *   mcp_server.h:524-548   McpTool::Call       → make_call_result()
 *   mcp_server.cc:117-119  get_device_status   → tool_tank_get_status()
 *   protocol.cc:76-79      SendMcpMessage      → send_payload()
 *
 * ── 报文形状（三层，一层都不能少）─────────────────────────────────────────
 *
 *   ① 服务器 → 设备（WebSocket 文本帧，由 xiaozhi_ws.c 解出 type=mcp）：
 *        {"type":"mcp","payload":{"jsonrpc":"2.0","id":1,"method":"tools/list"}}
 *
 *   ② 设备 → 服务器（本文件产出，外层信封来自 protocol.cc:77）：
 *        {"session_id":"<sid>","type":"mcp","payload":{...JSON-RPC...}}
 *
 *   ③ 工具调用的**结果**不是 JSON-RPC 的裸对象，而是 MCP 约定的信封：
 *        {"content":[{"type":"text","text":"<工具返回的 JSON 字符串>"}],
 *         "isError":false}
 *      注意 text 是**字符串**（里面的 JSON 是转义过的），不是嵌套对象 ——
 *      所以这里必须交给 cJSON 去转义，不能手写拼接。
 *
 * ── 三处刻意偏离参考实现（都有理由，不是省事）──────────────────────────────
 *
 * 1. **不移植 Property/PropertyList 参数机制**（mcp_server.h:97-383）。
 *    当前唯一的工具不带参数，移植过来也无从验证 —— 本工程不接受"写了但从没
 *    跑过"的代码。等真加了带参数的工具，照 `mcp_server.h:769-816` 的参数解析
 *    与必填校验补上。
 *
 * 2. **不移植 tools/list 的分页**（mcp_server.cc:683-740 的 nextCursor）。
 *    那是为了绕开 8000 字节的负载上限，xiaozhi 有十几个工具。本工程 1 个工具，
 *    整份列表约 300 字节，分页分支永远走不到。`nextCursor` 字段在无更多工具时
 *    本来就不发（mcp_server.cc:733-737），所以**线上的报文形状与参考实现一致**。
 *
 * 3. **响应信封用 cJSON 拼，不用字符串相加**。参考实现是 C++，`std::string`
 *    相加很自然；在 C 里手拼一个嵌了转义字符串的嵌套对象，是缓冲区溢出的
 *    标准配方。工具**内部**的值全是数字和固定字面量，那边仍用 snprintf
 *    （与 bafa_mqtt.c:387-399 同一风格）。
 *
 * ── 一个必须知道的运行前提 ────────────────────────────────────────────────
 * 本文件的所有函数都跑在 **esp_websocket_client 自己的任务**上（调用链
 * xz_mcp_handle ← handle_json ← ws_event_handler）。那个任务的栈不归我们管，
 * 所以：
 *   · 工具执行必须**快**（当前是加锁拷贝一个结构体，微秒级）；
 *   · 大缓冲用 static 而不是栈上的局部变量（见 s_result）。
 * xiaozhi 用 Schedule() 把工具执行挪到主线程（mcp_server.cc:818-829），是为了
 * `camera->Capture()` 那种要好几秒的工具。本工程将来若加这类工具，**必须**
 * 照做，否则会堵死 WebSocket 的接收（P7 已经因为在这条任务上解码 Opus
 * 撞过一次整机复位，见 app_main.c 里 s_dl_q 的说明）。
 * ============================================================================
 */
#include "xiaozhi_internal.h"
#include "xiaozhi_config.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"

#include "cJSON.h"

#include "app_types.h"

static const char *TAG = "xz_mcp";

/* ==========================================================================
 * 数据源（由 main 注册）—— 对应 xiaozhi 里 MCP 工具回调捕获的 `board`
 * ========================================================================== */
static xz_tank_status_fn s_tank_status = NULL;

void xz_set_tank_status_provider(xz_tank_status_fn fn)
{
    s_tank_status = fn;
}

static xz_camera_capture_fn s_camera_capture = NULL;

void xz_set_camera_capture_provider(xz_camera_capture_fn fn)
{
    s_camera_capture = fn;
}

/* ==========================================================================
 * 工具参数（参考实现 Property/PropertyList 的 C 版，mcp_server.h:97-383）
 *
 * ⚠️ **只实现了 string 一种类型**。参考实现还支持 bool / integer + 默认值 +
 *    整数范围，那是为它十几个工具准备的。本工程用到参数的只有拍照工具的
 *    `question`，所以只写这一种 —— 与工具表同一条纪律：**没有工具用到的
 *    分支不写**，写了也无从验证。将来真需要 int/bool 时照
 *    `mcp_server.h:97-383` 补类型、默认值与范围校验即可。
 * ========================================================================== */

/** 参数**声明**（放在工具表里，const —— 只描述，不存值）。 */
typedef struct {
    const char *name;
    bool        required;
} xz_mcp_prop_t;

/** 参数**取值**（调用时填入）。 */
typedef struct {
    const char *name;                       /* 指向 xz_mcp_prop_t.name */
    char        value[XZ_MCP_QUESTION_MAX];
} xz_mcp_arg_t;

#define XZ_MCP_PROP_MAX 2
typedef struct {
    xz_mcp_arg_t items[XZ_MCP_PROP_MAX];
    int          count;
} xz_mcp_args_t;

static const char *arg_get(const xz_mcp_args_t *args, const char *name)
{
    for (int i = 0; i < args->count; i++) {
        if (strcmp(args->items[i].name, name) == 0) {
            return args->items[i].value;
        }
    }
    return NULL;
}

/* ==========================================================================
 * 工具实现
 * ========================================================================== */

/** 工具结果的落点。
 *  **static 而不是栈上局部变量**：调用方是 WebSocket 任务，栈很紧（见文件头）。
 *  并发安全：只有 ws 任务会走 xz_mcp_handle()，天然串行。 */
static char s_result[XZ_MCP_RESULT_MAX];

/** 异步（慢）工具的结果落点。
 *  必须是**另一块**：慢工具在工具任务上跑着的时候，ws 任务完全可能同时执行
 *  一个同步工具（用户这轮问数据）—— 共用一块就会互相踩。 */
static char s_async_result[XZ_MCP_RESULT_MAX];

/** alert_mask 的位 → JSON 字段名。
 *
 *  字段名与上面数值键**故意同名**：LLM 在 out_of_range 里看到
 *  "water_temperature_c"，能直接在同一个对象里找到对应数值，不用再做映射。
 *
 *  ⚠️ 这里**不写阈值**。阈值在 main/app_config.h（app_types.h 里说明了为什么
 *     不放在 app_core）。把数字抄一份到这里，就会有两个真相来源。 */
static const struct {
    uint32_t    bit;
    const char *field;
} k_alert_fields[] = {
    { ECO_ALERT_PH,        "ph" },
    { ECO_ALERT_WATER_TMP, "water_temperature_c" },
    { ECO_ALERT_AIR_TMP,   "air_temperature_c" },
    { ECO_ALERT_HUMIDITY,  "air_humidity_percent" },
    { ECO_ALERT_LUX,       "illuminance_lux" },
    { ECO_ALERT_TURBIDITY, "turbidity_ntu" },
    { ECO_ALERT_TDS,       "tds_ppm" },
};

/**
 * @brief self.fish_tank.get_status —— 把当前鱼缸快照摊成给 LLM 看的 JSON。
 * @note  无参数。
 * @return false 表示这一轮取不到数据（传感器任务未就绪或取值函数没注册）。
 */
static bool tool_tank_get_status(const xz_mcp_args_t *args, char *out, size_t cap)
{
    (void)args;                     /* 这个工具不带参数 */
    tank_ecosystem_t eco;
    if (s_tank_status == NULL || !s_tank_status(&eco)) {
        return false;
    }

    /* 先写数值部分。这些都是数字/布尔，没有需要转义的字符串。 */
    int n = snprintf(out, cap,
        "{\"ph\":%.2f,\"tds_ppm\":%.1f,\"turbidity_ntu\":%.2f,"
        "\"water_temperature_c\":%.2f,\"air_temperature_c\":%.2f,"
        "\"air_humidity_percent\":%.1f,\"illuminance_lux\":%.1f,"
        "\"uptime_seconds\":%" PRIu32 ","
        "\"link\":{\"wifi\":%s,\"mqtt\":%s},"
        "\"out_of_range\":[",
        eco.ph, eco.tds_ppm, eco.turbidity_ntu,
        eco.water_temp_c, eco.air_temp_c,
        eco.air_humidity, eco.lux,
        /* tank_ecosystem_t.timestamp 是 esp_timer 的单调秒数 = 开机至今的秒数。
           对 LLM 来说「设备跑了多久」比「某个单调计数值」有意义得多，故改叫
           uptime_seconds。 */
        eco.timestamp,
        eco.wifi_connected ? "true" : "false",
        eco.mqtt_connected ? "true" : "false");

    if (n < 0 || (size_t)n >= cap) {
        ESP_LOGE(TAG, "状态 JSON 缓冲不足（数值段需要 %d 字节，共 %u）",
                 n, (unsigned)cap);
        return false;
    }
    size_t used = (size_t)n;

    /* 再逐个追加越界项。 */
    bool first = true;
    for (size_t i = 0; i < sizeof(k_alert_fields) / sizeof(k_alert_fields[0]); i++) {
        if ((eco.alert_mask & k_alert_fields[i].bit) == 0) {
            continue;
        }
        const int m = snprintf(out + used, cap - used,
                               "%s\"%s\"", first ? "" : ",", k_alert_fields[i].field);
        if (m < 0 || (size_t)m >= cap - used) {
            ESP_LOGE(TAG, "状态 JSON 缓冲不足（越界项列表）");
            return false;
        }
        used += (size_t)m;
        first = false;
    }

    const int m = snprintf(out + used, cap - used, "]}");
    if (m < 0 || (size_t)m >= cap - used) {
        ESP_LOGE(TAG, "状态 JSON 缓冲不足（结尾）");
        return false;
    }
    return true;
}

/**
 * @brief self.camera.take_photo —— 拍一张、传给服务器做视觉理解、回描述。
 *
 * ⚠️ **这个函数跑在工具任务上，不是 WebSocket 任务**（表里标了 is_async）。
 *    它里面会阻塞好几秒：抓帧最长 4 秒 + JPEG 编码 + 一次 HTTP 上传。
 *    放在 ws 任务上跑会堵死 WebSocket 接收 —— P7 已经因为在这条任务上解码
 *    Opus 撞过一次整机复位，这是同一个坑。
 *
 * @param args 需要 `question`（必填，见 k_camera_props）
 * @return false = 这一轮没拍成（会回一条给 LLM 的失败说明，而不是编一张图）
 */
static bool tool_camera_take_photo(const xz_mcp_args_t *args, char *out, size_t cap)
{
    if (s_camera_capture == NULL) {
        /* main 没注册拍照数据源 —— 这是配置错误，不是运行时故障，
           日志要说清楚，否则会跟"摄像头坏了"混为一谈。 */
        ESP_LOGE(TAG, "拍照工具被调用，但 main 没注册 xz_camera_capture_fn");
        snprintf(out, cap, "{\"success\": false, \"message\": "
                           "\"Camera is not available on this device\"}");
        return false;
    }

    const char *question = arg_get(args, "question");
    if (question == NULL) {
        /* 参数校验在 on_tools_call 里已经做过，走到这里说明表里声明了 required
           却没校验到 —— 属于本文件自身的 bug，不是服务器的错。 */
        ESP_LOGE(TAG, "拍照工具缺 question 参数（参数校验漏了？）");
        snprintf(out, cap, "{\"success\": false, \"message\": \"Missing question\"}");
        return false;
    }

    /* ---- 1. 拍照 + 编码。数据源归 main 实现（它才拿得到 camera_service）---- */
    uint8_t *jpeg = NULL;
    size_t   jpeg_len = 0;
    const esp_err_t cret = s_camera_capture(&jpeg, &jpeg_len);
    if (cret != ESP_OK || jpeg == NULL || jpeg_len == 0) {
        ESP_LOGE(TAG, "拍照失败：%s", esp_err_to_name(cret));
        snprintf(out, cap, "{\"success\": false, \"message\": "
                           "\"Failed to capture photo from the camera\"}");
        return false;
    }

    /* ---- 2. 上传。
       失败时 xz_vision_upload() 已经往 out 里写好了给 LLM 看的说明，
       直接透传即可 —— 让模型据实说"拍到了但看不懂"，
       比我们替它编一句强。 ---- */
    const esp_err_t uret = xz_vision_upload(jpeg, jpeg_len, question, out, cap);
    if (uret != ESP_OK) {
        ESP_LOGW(TAG, "拍照完成但上传/识别失败：%s", esp_err_to_name(uret));
        return false;
    }
    return true;
}

/** 拍照工具的参数声明（参考实现 mcp_server.cc:181-183 的 PropertyList）。 */
static const xz_mcp_prop_t k_camera_props[] = {
    { "question", true },   /* 必填：模型想问照片什么 */
};

/**
 * @brief 工具表。对应参考实现的 `AddTool(name, desc, props, callback)`。
 *
 * ⚠️ **只放能被验证的工具**。每加一个工具，就是给 LLM 多一个可以"说错"的出口：
 *    数据源没接上却有工具暴露给模型，模型会照着空值一本正经地编。
 *    之所以**仍然没有**视觉字段（fish_count / water_muddy）：
 *    `tank_ecosystem_t` 里那几个字段**全工程没有任何地方写入**（P6-a 的识别结果
 *    只上了屏，没回写 s_eco），报给 LLM 等于让它笃定地说"缸里没有鱼"。
 *    （拍照工具是另一回事：它拍的是**实时画面**，不依赖那几个死字段。）
 */
typedef struct {
    const char       *name;
    const char       *desc;
    const xz_mcp_prop_t *props;       /* 参数声明；无参数时为 NULL */
    int               prop_count;
    /** true = 交给工具任务执行（会阻塞好几秒的工具必须置它）。
     *  false = 就在 WebSocket 任务上就地执行，必须微秒级返回。 */
    bool              is_async;
    /** 工具实现。结果写进 out（成功时返回 true）。 */
    bool            (*run)(const xz_mcp_args_t *args, char *out, size_t cap);
} xz_mcp_tool_t;

static const xz_mcp_tool_t k_tools[] = {
    {
        .name       = "self.fish_tank.get_status",
        /* 描述照参考实现的写法（mcp_server.cc:111-115）：先说是什么，再列
           "Use this tool for"，让模型知道**什么时候**该调它。
         *
         * ⚠️ 那段 IMPORTANT 是**上板实测逼出来的**，不是预防性写的：
         *    2026-10-14 第一次上板，模型只在第一轮调了工具，之后两轮都直接
         *    复用 70 秒前的旧值作答 —— 用户问「现在有没有不正常」，它答
         *    "都在正常范围内"，而同一时刻浊度其实已经越界。
         *    它报的是**过期的真数字**，不是编的，从回答上根本看不出来。
         *    根因：JSON 里的 uptime_seconds 对模型没有参照点，它无从判断数据多旧。
         *    参考实现就是用描述来约束调用时机的（`set_volume` 里那句
         *    "you must call `self.get_device_status` tool first"），照做。 */
        .desc = "Get the current water quality and environment readings of the fish tank, "
                "including pH, TDS, turbidity, water temperature, air temperature, air humidity "
                "and illuminance.\n"
                "IMPORTANT: the readings change continuously, so values returned by a previous "
                "call are already out of date. Whenever the user asks about the tank's current "
                "state, call this tool again -- never answer from the values of an earlier call.\n"
                "Use this tool for:\n"
                "1. Answering questions about the current condition of the tank "
                "(e.g. what is the water temperature now?)\n"
                "2. Checking whether any reading is out of the safe range before giving care advice.\n"
                "Return:\n"
                "  A JSON object with the latest readings plus an `out_of_range` array listing "
                "the fields that are currently outside their safe range (empty when all normal).",
        .props      = NULL,
        .prop_count = 0,
        .is_async   = false,
        .run        = tool_tank_get_status,
    },
    {
        .name = "self.camera.take_photo",
        /* 描述照参考实现（mcp_server.cc:175-183）。那句 "always take a fresh
           photo" 与上面 get_status 的 IMPORTANT 是同一个教训：不说清楚，
           模型会拿上一张照片的描述当这一张答。 */
        .desc = "Take a photo with the device camera and have it analysed, returning a "
                "description of what is visible.\n"
                "Use this tool after the user asks you to look at something or asks what "
                "you can see.\n"
                "IMPORTANT: this tool captures a new photo every time it is called. When the "
                "user asks about what is in front of the camera now, call this tool again -- "
                "never answer from the description returned by an earlier call.\n"
                "Args:\n"
                "  `question`: The question that you want to ask about the photo.\n"
                "Return:\n"
                "  A JSON object that describes the photo in answer to the question.",
        .props      = k_camera_props,
        .prop_count = (int)(sizeof(k_camera_props) / sizeof(k_camera_props[0])),
        .is_async   = true,     /* 拍照要好几秒 —— 必须挪出 WebSocket 任务 */
        .run        = tool_camera_take_photo,
    },
};

#define TOOL_COUNT (sizeof(k_tools) / sizeof(k_tools[0]))

/* ==========================================================================
 * 回包
 * ========================================================================== */

/** 把构造好的 payload 套上外层信封发出去（protocol.cc:76-79）。 */
static void send_payload(cJSON *payload)
{
    if (payload == NULL) {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        cJSON_Delete(payload);
        return;
    }
    cJSON_AddStringToObject(root, "session_id", xz_ws_session_id());
    cJSON_AddStringToObject(root, "type", "mcp");
    cJSON_AddItemToObject(root, "payload", payload);   /* 移交所有权 */

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        return;
    }

    /* 这里是在**事件回调内部**发文本。安全性已核对过：
       esp_websocket_client 分发 WEBSOCKET_EVENT_DATA 时不持有锁
       （esp_websocket_client.c:1108），且它的锁是递归锁；
       现网已有的 send_hello() 也是在事件回调里发的。 */
    const esp_err_t ret = xz_ws_send_text(text);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MCP 回复发送失败：%s", esp_err_to_name(ret));
    }
    cJSON_free(text);
}

/** 成功响应。**取得 result 的所有权。** */
static void reply_result(int id, cJSON *result)
{
    cJSON *pl = cJSON_CreateObject();
    if (pl == NULL) {
        cJSON_Delete(result);
        return;
    }
    cJSON_AddStringToObject(pl, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(pl, "id", id);
    cJSON_AddItemToObject(pl, "result", result);
    send_payload(pl);
}

/** 错误响应（mcp_server.cc:652-659）。
 *
 *  ⚠️ 这里**只有 `message`，没有 `code`** —— 与 JSON-RPC 2.0 规范不符
 *     （规范里 code 是必填），也与 `docs/mcp-protocol.md` 里写的
 *     `{"code": -32601, "message": ...}` 不符。
 *     但参考实现就是这么发的（`ReplyError` 只拼 message），而那份文档开头自己写着
 *     "AI 辅助生成, ……请参照代码确认细节"。**两个参考产物打架时以代码为准**，
 *     因为它才是真正跟服务器跑了很久的那一个。若上板后发现服务器拒收错误包，
 *     这里补一个 code 即可（改一行）。
 *
 *     实际触发概率极低：服务器只会调用它从 tools/list 里学到过的工具。 */
static void reply_error(int id, const char *message)
{
    cJSON *pl = cJSON_CreateObject();
    if (pl == NULL) {
        return;
    }
    cJSON_AddStringToObject(pl, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(pl, "id", id);
    cJSON *err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(pl, "error", err);
    send_payload(pl);
}

/** 工具调用的结果信封（mcp_server.h:524-548 的 `McpTool::Call`）。 */
static cJSON *make_call_result(const char *text)
{
    cJSON *result  = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *item    = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    /* text 的值本身是一段 JSON 字符串 —— 交给 cJSON 转义 */
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(result, "content", content);

    /* ⚠️ isError 保持 **恒为 false**，与参考实现一致（mcp_server.h:541）。
       工具级的失败（传感器没数据、将来拍照失败）写在 text 里那条 JSON 的
       `success` 字段上，**不用** isError。改了会让服务器对两种失败采取不同
       处理，而服务器那边的预期是按参考实现来的。 */
    cJSON_AddBoolToObject(result, "isError", false);
    return result;
}

/* ==========================================================================
 * 慢工具：队列 + 执行任务
 *
 * 【为什么非要有它】
 * 拍照要好几秒（抓帧最长 4 秒 + JPEG 编码 + 一次 HTTP 上传）。而 MCP 请求是在
 * **esp_websocket_client 的事件回调**里进来的 —— 在那条任务上跑几秒，WebSocket
 * 就什么都收不了（下行音频、后续消息、连接维护全堵住）。
 * P7 已经因为在这条任务上解码 Opus 撞过一次整机复位，这是同一个坑。
 *
 * 【做法】照本工程下行音频那条路（app_main.c 的 s_dl_q）：
 *   回调里只**入队**（微秒级返回），真正的执行交给一条专用任务。
 *   xiaozhi 是把它 Schedule 到主事件循环任务上（application.cc:610-613，
 *   `2048 * 4` = 8192 字节栈）—— 二者同构，区别只是本工程没有"主循环任务"。
 * ========================================================================== */

typedef struct {
    int           id;           /* JSON-RPC 的 id，回包时要原样带回去 */
    int           tool_idx;     /* k_tools 下标 */
    xz_mcp_args_t args;         /* 值已校验并填好 */
} xz_tool_req_t;

static QueueHandle_t s_tool_q       = NULL;
static StaticQueue_t s_tool_q_ctrl;
static uint8_t      *s_tool_q_store = NULL;     /* PSRAM */
static TaskHandle_t  s_tool_task    = NULL;

/** 慢工具的执行任务。 */
static void xz_tool_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "工具任务启动（栈 %u 字节，优先级 %d，核 %d）",
             (unsigned)XZ_TOOL_TASK_STACK, XZ_TOOL_TASK_PRIO, XZ_TOOL_TASK_CORE);

    for (;;) {
        xz_tool_req_t req;
        if (xQueueReceive(s_tool_q, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (req.tool_idx < 0 || req.tool_idx >= (int)TOOL_COUNT) {
            continue;
        }
        const xz_mcp_tool_t *t = &k_tools[req.tool_idx];

        const bool ok = (t->run != NULL) &&
                        t->run(&req.args, s_async_result, sizeof(s_async_result));

        ESP_LOGI(TAG, "← 工具 %s：%s", t->name,
                 ok ? s_async_result : "执行失败（已回 success:false）");

        /* 回包在**本任务**上发 —— esp_websocket_client_send_text 内部有锁，
           跨任务调用安全（现网 voice_task 一直在这么做）。
           ⚠️ 若这条工具跑到一半会话断了/重连了，这条回包会落到**新会话**上、
           id 对不上而无人认领。服务器忽略即可，本工程不做额外处理 ——
           要处理就得把 session_id 一起存进请求再比对，不值当。 */
        reply_result(req.id, make_call_result(
            ok ? s_async_result
               : "{\"success\": false, \"message\": \"Tool execution failed\"}"));
    }
}

esp_err_t xz_mcp_start_worker(void)
{
    if (s_tool_task != NULL) {
        return ESP_OK;                      /* 幂等 */
    }

    /* 队列存储放 PSRAM：一条请求约 540 字节 × 2 条，内部堆省下来干别的。 */
    const size_t store = sizeof(xz_tool_req_t) * XZ_MCP_QUEUE_LEN;
    s_tool_q_store = (uint8_t *)heap_caps_malloc(store,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_tool_q_store == NULL) {
        ESP_LOGE(TAG, "工具队列存储分配失败（需 %u 字节 PSRAM）", (unsigned)store);
        return ESP_ERR_NO_MEM;
    }

    s_tool_q = xQueueCreateStatic(XZ_MCP_QUEUE_LEN, sizeof(xz_tool_req_t),
                                  s_tool_q_store, &s_tool_q_ctrl);
    if (s_tool_q == NULL) {
        heap_caps_free(s_tool_q_store);
        s_tool_q_store = NULL;
        ESP_LOGE(TAG, "工具队列创建失败");
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t ok = xTaskCreatePinnedToCore(xz_tool_task, XZ_TOOL_TASK_NAME,
                                                  XZ_TOOL_TASK_STACK, NULL,
                                                  XZ_TOOL_TASK_PRIO, &s_tool_task,
                                                  XZ_TOOL_TASK_CORE);
    if (ok != pdPASS) {
        vQueueDelete(s_tool_q);
        s_tool_q = NULL;
        heap_caps_free(s_tool_q_store);
        s_tool_q_store = NULL;
        ESP_LOGE(TAG, "工具任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ==========================================================================
 * 方法分派
 * ========================================================================== */

/** initialize（mcp_server.cc:572-584）。
 *
 *  参考实现还会在这里解析 `params.capabilities.vision.{url,token}` 用来配摄像头的
 *  Explain URL（mcp_server.cc:499-516）。本工程**目前**用不到（没有拍照工具），
 *  但**要把它打出来** —— 服务器到底给不给 vision 地址、给的是什么，是"能不能让
 *  AI 看图"这个功能的前提，看不到就等于在猜。 */
static void on_initialize(int id, const cJSON *params)
{
    const esp_app_desc_t *desc = esp_app_get_description();

    /* ---- 诊断：服务器给了哪些能力 ---- */
    const cJSON *caps_in = cJSON_GetObjectItem(params, "capabilities");
    if (cJSON_IsObject(caps_in)) {
        char *dump = cJSON_PrintUnformatted(caps_in);
        ESP_LOGI(TAG, "服务器能力: %s", (dump != NULL) ? dump : "(序列化失败)");
        cJSON_free(dump);

        const cJSON *vision = cJSON_GetObjectItem(caps_in, "vision");
        if (cJSON_IsObject(vision)) {
            const cJSON *url   = cJSON_GetObjectItem(vision, "url");
            const cJSON *token = cJSON_GetObjectItem(vision, "token");
            ESP_LOGI(TAG, "服务器提供 vision: url=%s, token=%s",
                     cJSON_IsString(url) ? url->valuestring : "(无)",
                     (cJSON_IsString(token) && token->valuestring[0] != '\0')
                         ? "有" : "(无)");
            /* 存下来给拍照工具用。**每个会话都要重存** —— 实测 token 就是
               session_id，每次连接都换，缓存上一轮的必然 401。 */
            xz_vision_set(cJSON_IsString(url) ? url->valuestring : NULL,
                          cJSON_IsString(token) ? token->valuestring : NULL);
        } else {
            /* 这一刻没给，就把上一轮的清掉 —— 免得拿旧 token 去撞。 */
            xz_vision_set(NULL, NULL);
            ESP_LOGW(TAG, "服务器**没有**提供 vision 上传地址"
                          "（拍照给 AI 看这条路走不通）");
        }
    } else {
        ESP_LOGW(TAG, "initialize 没带 capabilities");
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");

    cJSON *caps = cJSON_CreateObject();
    cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
    cJSON_AddItemToObject(result, "capabilities", caps);

    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", XZ_BOARD_NAME);
    cJSON_AddStringToObject(info, "version",
                            (desc != NULL) ? desc->version : "unknown");
    cJSON_AddItemToObject(result, "serverInfo", info);

    reply_result(id, result);
}

/** 单个工具的 MCP 描述（mcp_server.h:470-508）。 */
static cJSON *tool_to_json(const xz_mcp_tool_t *t)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", t->name);
    cJSON_AddStringToObject(o, "description", t->desc);

    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");

    cJSON *props = cJSON_CreateObject();
    cJSON *req   = NULL;
    for (int i = 0; i < t->prop_count; i++) {
        cJSON *p = cJSON_CreateObject();
        /* 目前只有 string 一种类型（见文件里参数机制那段说明） */
        cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddItemToObject(props, t->props[i].name, p);

        if (t->props[i].required) {
            if (req == NULL) {
                req = cJSON_CreateArray();
            }
            cJSON_AddItemToArray(req, cJSON_CreateString(t->props[i].name));
        }
    }
    cJSON_AddItemToObject(schema, "properties", props);
    /* 空对象与"没有必填项"时都**不发** required —— 与参考实现一致
       （mcp_server.h:483-489 只在 required 非空时才加这个字段）。 */
    if (req != NULL) {
        cJSON_AddItemToObject(schema, "required", req);
    }

    cJSON_AddItemToObject(o, "inputSchema", schema);
    return o;
}

/** tools/list（mcp_server.cc:683-740，去掉分页）。 */
static void on_tools_list(int id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *arr    = cJSON_CreateArray();
    for (size_t i = 0; i < TOOL_COUNT; i++) {
        cJSON_AddItemToArray(arr, tool_to_json(&k_tools[i]));
    }
    cJSON_AddItemToObject(result, "tools", arr);
    reply_result(id, result);
}

/** 参数值落点。**static** 理由同 s_result：ws 任务栈很紧，
 *  且只有 ws 任务会走 on_tools_call()，天然串行。
 *  异步工具在投递进队列前会把值**拷进队列项**，所以这块被覆盖不影响在跑的工具。 */
static xz_mcp_args_t s_args;

/**
 * @brief 按工具的声明校验并填好参数（= 参考实现 `DoToolCall` 的参数解析段，
 *        mcp_server.cc:784-816）。
 *
 * @param[out] err 失败原因（写进 JSON-RPC 的 error.message）
 * @return false = 参数不合法，调用方应回错误包
 */
static bool bind_args(const xz_mcp_tool_t *t, const cJSON *arguments,
                      char *err, size_t errcap)
{
    s_args.count = 0;

    for (int i = 0; i < t->prop_count; i++) {
        const xz_mcp_prop_t *p = &t->props[i];

        const cJSON *v = cJSON_IsObject(arguments)
                         ? cJSON_GetObjectItem(arguments, p->name) : NULL;

        if (!cJSON_IsString(v)) {
            if (p->required) {
                /* 参考实现在这里是 "Missing valid argument: X"，照抄措辞 */
                snprintf(err, errcap, "Missing valid argument: %s", p->name);
                return false;
            }
            continue;               /* 可选参数没给就跳过 */
        }

        if (strlen(v->valuestring) >= XZ_MCP_QUESTION_MAX) {
            snprintf(err, errcap, "Argument too long: %s", p->name);
            return false;
        }

        xz_mcp_arg_t *a = &s_args.items[s_args.count++];
        a->name = p->name;
        snprintf(a->value, sizeof(a->value), "%s", v->valuestring);
    }
    return true;
}

/** tools/call（mcp_server.cc:769-830）。 */
static void on_tools_call(int id, const cJSON *params)
{
    if (!cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "tools/call：缺少 params");
        reply_error(id, "Missing params");
        return;
    }

    const cJSON *name = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(name)) {
        ESP_LOGE(TAG, "tools/call：缺少 name");
        reply_error(id, "Missing name");
        return;
    }

    /* 即使工具没有参数也要校验它是不是对象：参考实现这里就报
       "Invalid arguments"（mcp_server.cc:609-613）。不校验的话，一个格式错的
       请求会被当成"无参数工具"静默跑掉，问题被吞掉。 */
    const cJSON *arguments = cJSON_GetObjectItem(params, "arguments");
    if (arguments != NULL && !cJSON_IsObject(arguments)) {
        ESP_LOGE(TAG, "tools/call：arguments 不是对象");
        reply_error(id, "Invalid arguments");
        return;
    }

    int tool_idx = -1;
    for (size_t i = 0; i < TOOL_COUNT; i++) {
        if (strcmp(k_tools[i].name, name->valuestring) == 0) {
            tool_idx = (int)i;
            break;
        }
    }
    if (tool_idx < 0) {
        ESP_LOGE(TAG, "tools/call：未知工具 %s", name->valuestring);
        char buf[96];
        snprintf(buf, sizeof(buf), "Unknown tool: %s", name->valuestring);
        reply_error(id, buf);
        return;
    }
    const xz_mcp_tool_t *t = &k_tools[tool_idx];

    /* ---- 参数校验（同步/异步都要做，且在入队**之前**）---- */
    char errbuf[96];
    if (!bind_args(t, arguments, errbuf, sizeof(errbuf))) {
        ESP_LOGE(TAG, "tools/call：%s（工具 %s）", errbuf, t->name);
        reply_error(id, errbuf);
        return;
    }

    /* ---- 慢工具：入队后立即返回，**本次不回包** ---- */
    if (t->is_async) {
        if (s_tool_q == NULL) {
            ESP_LOGE(TAG, "工具任务未启动（xz_mcp_start_worker 没被调用？）");
            reply_error(id, "Tool worker not started");
            return;
        }
        xz_tool_req_t req;
        req.id       = id;
        req.tool_idx = tool_idx;
        req.args     = s_args;              /* 值**拷贝**进队列项 */

        ESP_LOGI(TAG, "→ 工具 %s 交给工具任务执行（id=%d）", t->name, id);
        if (xQueueSend(s_tool_q, &req, 0) != pdTRUE) {
            /* 队列满 = 上一条还在跑。回一条"忙"比无限排队好：
               排队会让模型的"现在"变成几十秒前。 */
            ESP_LOGW(TAG, "工具队列已满，拒绝本次 %s", t->name);
            reply_result(id, make_call_result(
                "{\"success\": false, \"message\": \"The device is busy with "
                "another tool call, please try again in a moment\"}"));
        }
        return;
    }

    /* ---- 同步工具：就地执行（必须微秒级）---- */
    const bool ok = (t->run != NULL) &&
                    t->run(&s_args, s_result, sizeof(s_result));

    /* 这一行是验收判据里「串口出现 MCP 工具调用」的证据，也是唯一能看到
       **LLM 到底拿到了什么数据**的地方 —— 界面上只有它说的那句话。 */
    ESP_LOGI(TAG, "← 工具 %s：%s", t->name,
             ok ? s_result : "取不到数据（已回 success:false）");

    reply_result(id, make_call_result(
        ok ? s_result
           : "{\"success\": false, \"message\": \"Sensor data not available yet\"}"));
}

/**
 * @brief 处理一条 MCP payload（= 参考实现的 `McpServer::ParseMessage`）。
 *
 * 校验顺序刻意与 mcp_server.cc:535-569 逐条一致 —— 包括「校验不过就**静默丢弃**
 * 而不是回错误包」。服务器发来的畸形请求，回一条错误包只会换来更多来回。
 */
void xz_mcp_handle(const cJSON *payload)
{
    const cJSON *version = cJSON_GetObjectItem(payload, "jsonrpc");
    if (!cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "MCP：jsonrpc 版本不对（%s）",
                 cJSON_IsString(version) ? version->valuestring : "缺失");
        return;
    }

    const cJSON *method = cJSON_GetObjectItem(payload, "method");
    if (!cJSON_IsString(method)) {
        ESP_LOGE(TAG, "MCP：缺少 method");
        return;
    }
    const char *m = method->valuestring;

    /* 通知类无需回复（mcp_server.cc:552-554）。前缀匹配，与参考一致。 */
    if (strncmp(m, "notifications", strlen("notifications")) == 0) {
        ESP_LOGD(TAG, "MCP：%s（通知，不回）", m);
        return;
    }

    const cJSON *params = cJSON_GetObjectItem(payload, "params");
    if (params != NULL && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "MCP：%s 的 params 不是对象", m);
        return;
    }

    /* id 必须是数字（mcp_server.cc:565-568）。服务器发字符串 id 的话，
       我们这条请求会没人应答 —— 但那是参考实现就有的行为，不在这里"改进"。 */
    const cJSON *id = cJSON_GetObjectItem(payload, "id");
    if (!cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "MCP：%s 的 id 不是数字", m);
        return;
    }
    const int id_int = id->valueint;

    ESP_LOGI(TAG, "← MCP %s (id=%d)", m, id_int);

    if (strcmp(m, "initialize") == 0) {
        on_initialize(id_int, params);
    } else if (strcmp(m, "tools/list") == 0) {
        on_tools_list(id_int);
    } else if (strcmp(m, "tools/call") == 0) {
        on_tools_call(id_int, params);
    } else {
        ESP_LOGE(TAG, "MCP：未实现的方法 %s", m);
        char buf[96];
        snprintf(buf, sizeof(buf), "Method not implemented: %s", m);
        reply_error(id_int, buf);
    }
}
