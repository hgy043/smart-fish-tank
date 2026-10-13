# 巴法云 MQTT 协议约定

> 配置值来源：用户已验证可运行工程 `06-tcp_client_bafa_mqtt`
> （`main/mqtt_demo.h`、`main/wifi_config.c`），已迁入本项目
> `components/net_service/include/app_secrets.h`。

## 1. 连接参数

| 项 | 值 | 说明 |
|---|---|---|
| 服务器 | `bemfa.com` | |
| 端口 | `9501` | 明文 TCP。另：9503=TLS、9504=WebSocket(路径 `/wss`) |
| 客户端 ID | 用户私钥（UID） | 巴法云的鉴权方式：**用 UID 当 client_id** |
| 用户名 / 密码 | 留空 | 不需要 |
| 协议版本 | MQTT 3.1.1 | |
| QoS | 0 | **严禁 QoS 2**，巴法云不支持，会被强制下线甚至封号 |
| Keepalive | 60s | |

## 2. 主题设计

巴法云主题规则（关键，决定消息是否推送）：

| 发布到 | 行为 |
|---|---|
| `<topic>` | 正常推送，所有订阅者收到 |
| `<topic>/set` | 推送给所有订阅者，**发布者自己收不到**（防自循环） |
| `<topic>/up` | **只更新云端数据，不推送**（省流量，适合高频上报） |

本项目主题：

| 用途 | 主题 | 后缀 | 说明 |
|---|---|---|---|
| 上报生态状态 | `fishsensor` | `/up` | 10s 周期，高频，用 `/up` 省流量与功耗 |
| 上报告警 | `fishalert` | `/set` | 异常即时推送 |
| 上报 AI 建议 | `fishai` | `/set` | 小智养护建议 |
| 设备在线状态 | `fishstatus` | `/set` | 上线通告 / 心跳 |
| 订阅控制命令 | `fishcmd` | — | 手机下行，`esp_mqtt_client_subscribe` |

> ⚠️ **这些主题名必须先在巴法云控制台创建**，否则发布无效。

## 3. 上报数据格式

### 3.1 生态状态（`fishsensor/up`）

字段与 `Agent.md` 第 9 节协议一致：

```json
{
  "ts": 1710000000,
  "water": { "ph": 7.10, "tds": 320.0, "turbidity": 12.50, "temp": 25.30 },
  "env":   { "air_temp": 26.10, "hum": 58.0, "lux": 450.0 },
  "vision":{ "fish_count": 3, "muddy": false },
  "status":{ "wifi": true, "mqtt": true, "alert": false }
}
```

由 `bafa_mqtt_post_status()` 生成；`BAFA_TX_PAYLOAD_MAX`=512 字节，
超长会被 `snprintf` 截断并返回 `ESP_ERR_INVALID_SIZE`（不静默发送残缺 JSON）。

### 3.2 告警（`fishalert/set`）

```json
{ "alert": "水温 31.2℃ 超过上限 28℃" }
```

### 3.3 AI 建议（`fishai/set`）

```json
{ "ai": "水质正常，建议 3 天后换水 1/4。" }
```

## 4. 下行命令（`fishcmd`）

当前已实现的命令解析（`handle_command()`）：

| 命令 | 行为 |
|---|---|
| `on` | 记录日志（P4/P2 阶段挂接灯光/继电器） |
| `off` | 记录日志 |

其它内容按未知命令记录。**解析结果应经事件总线投递**，不要在 MQTT
事件回调里直接操作外设——回调运行在 MQTT 客户端任务上下文。

## 5. 断线重连策略（重要设计决策）

**已显式关闭 ESP-IDF 内建自动重连**（`network.disable_auto_reconnect = true`），
改由本模块的 `esp_timer` 指数退避定时器接管：

```
首次 1s → 2s → 4s → 8s → 16s → 上限 30s 封顶
连接成功即重置回 1s
```

理由：内建 `reconnect_timeout_ms` 只在**初始连接**阶段有效，断线后的重连
间隔不可控（实测会漂移到数分钟），无法满足验收指标「MQTT 断线后 30s 内重连」。

Wi-Fi 侧同理（`components/net_service/wifi_sta.c`），退避参数
`APP_WIFI_BACKOFF_MIN_MS`=1s / `APP_WIFI_BACKOFF_MAX_MS`=30s。

## 6. 线程边界

**所有 `esp_mqtt_client_publish()` 只能出现在 `mqtt_task` 内**，经
`bafa_mqtt_pump()` 从发送队列取消息后发布。其它任务要上报数据，调用：

```c
bafa_mqtt_post_status(&snapshot);   // 生态状态
bafa_mqtt_post_alert("...");        // 告警
bafa_mqtt_post_ai("...");           // AI 建议
```

队列深度 `BAFA_TX_QUEUE_LEN`=8。**队列满时丢弃并告警，不阻塞调用者**——
调用者可能是高优先级 sensor_task，阻塞它会造成整机节拍失真。

## 7. 待办（P3 阶段补齐）

- [ ] **断网本地缓存**：当前未连接时直接丢弃消息。P3 应改为写 N 条
      （`APP_MQTT_CACHE_LEN`=16）环形缓存，联网后补发。
- [ ] **下行命令完整解析**：接入灯光/继电器控制与阈值下发。
- [ ] **设备状态心跳**：周期性发布 `fishstatus`。
- [ ] 上报周期改为可配置，并按数据变化率自适应（变化大时加密上报）。
