# DG-LAB Socket 协议（Wi-Fi + WebSocket）

本文记录 Switch 侧通过局域网控制手机的 DG-LAB App 所需的协议事实。

## 为什么走 Socket 而不是 BLE

主机侧 BLE 在 HOS 22.5.0 上不可用（见 `docs/ble-poc.md`）。DG-LAB App 提供了
Socket 控制能力：手机负责 BLE，Switch 只做 WebSocket 通信。

## 参考来源

- 官方服务端实现：<https://github.com/dungeonlab-open/dglab-websocket-server>
  （`v3-server.ts`、`v4-server.ts`，2026-07 更新）
- 官方 Socket SDK：`dungeonlab-open/dglab-kit`
- 社区 Python 实现（V3 协议细节、指令字符串、二维码格式）：
  <https://github.com/Ljzd-PRO/PyDGLab-WS>

## 连接模型

DG-LAB App 始终是 **WebSocket 客户端**，它需要一个 WebSocket **服务端**：

1. **本地模式（本项目采用）**：Switch 自己当服务端，手机连到 Switch 的局域网地址；
2. **中转模式**：双方都连到第三方中继（`dglab-websocket-server`），用于跨网。

本地模式下不需要任何外部服务器，只要手机与 Switch 在同一局域网。

## 协议版本

官方仓库里有两套：

| 版本 | 默认端口 | 被控方（App）如何指定控制方 |
| --- | --- | --- |
| V3（旧） | `9999` | 连接路径 `ws://host:port/<控制方 clientId>` |
| V4（新） | `9998` | 查询参数 `ws://host:port?tid=<控制方 clientId>` |

两套的消息外壳不同（见下）。由于用户 App 的具体版本待确认，Switch 端服务端应当
同时接受"路径携带 clientId"与"`?tid=` 携带 clientId"两种连接，再按收到的第一条
消息判断版本。

## V3 协议（社区实现完整）

### 消息外壳

```json
{"type":"msg","clientId":"<uuid>","targetId":"<uuid>","message":"..."}
```

- 键名在线上是**驼峰**（`clientId`/`targetId`）；
- `message` 可能是字符串，也可能是错误码的字符串形式（如 `"200"`）；
- 单条消息最大 1950 字节。

`type` 取值：`heartbeat`、`bind`、`msg`、`break`、`error`。

错误码（`RetCode`）：

| 码 | 含义 |
| --- | --- |
| 200 | 成功 |
| 209 | 对方客户端已断开 |
| 210 | 二维码中没有有效的 clientId |
| 211 | 服务端迟迟不下发 App ID |
| 400 | 该 ID 已被其他客户端绑定 |
| 401 | 目标客户端不存在 |
| 402 | 双方不是绑定关系 |
| 403 | 内容不是 JSON 对象 |
| 404 | 收信人不在线 |
| 405 | message 长度超过 1950 |
| 500 | 服务端内部错误 |

### 绑定流程

1. 客户端连上服务端后，服务端下发
   `{"type":"bind","clientId":"<自己的 uuid>","targetId":"","message":"targetId"}`；
2. 控制端把二维码给用户，App 扫码后按 `ws://host:port/<控制方 clientId>` 连接；
3. 服务端把两者配对，向双方各发一条
   `{"type":"bind","clientId":"<对方 uuid>","targetId":"<自己 uuid>","message":"200"}`；
4. 之后才允许互发 `msg`。

服务端会周期性给客户端发 `heartbeat` 消息。

### 控制指令（控制端 → App）

```
strength-<A|B>+<mode>+<value>      mode: 0=减少 1=增加 2=设置为指定值；value 0~200
pulse-<A|B>:["<16位十六进制>", ...] 每个元素 8 字节，最大 86 个
clear-<A|B>                        清空该通道波形队列
```

`pulse` 的每个 8 字节元素 = **4 个波形频率 + 4 个波形强度**，频率范围 10~240、
强度范围 0~100。这与 BLE V3 协议的波形槽格式完全一致，因此本项目
`sysmodule/source/protocol/coyote_v3.c` 的波形编码可以直接复用。

### App → 控制端

```
strength-<A强度>+<B强度>+<A上限>+<B上限>
feedback-<0..9>                     App 上的 10 个反馈按钮
```

### 二维码内容（V3）

```
https://www.dungeon-lab.com/app-download.php#DGLAB-SOCKET#<ws uri>/<clientId>
```

`ws uri` 形如 `ws://192.168.1.161:9999`（末尾不能有 `/`）。

## V4 协议（官方服务端实现）

消息外壳不同：

```json
{"type":"hello","clientId":"<uuid>"}                          ← 服务端 → 客户端
{"type":"message","clientId":"<被控方 clientId>","data":"..."}  ← 控制方 → 服务端
{"type":"message","data":"..."}                                ← 被控方 → 控制方（转发）
{"type":"heartbeat"} / {"type":"ping"} / {"type":"pong","ts":...}
{"type":"error", ...} / {"type":"controller_disconnected", ...}
{"type":"client_disconnected", ...}
```

- 控制方正常连接即可；**被控方（App）带 `?tid=<控制方 clientId>` 连接**；
- 服务端分配 `clientId` 并用 `hello` 下发；
- 控制方发送时用 `message.clientId` 指定目标被控方，真正的指令放在 `data` 里；
- `data` 里的指令内容与 V3 的 `message` 相同（`strength-...` / `pulse-...` 等）。

### 二维码内容（V4）

```
https://dungeon-lab.cn/s/?v=1&action=socket&url=<编码后的 ws://host:port?tid=<clientId>>
```

## 待确认

1. 用户的 App 具体支持哪套（V3/V4）——按上面的设计，服务端可以同时兼容；
2. App 端是否允许手输地址（否则 NRO 需要显示二维码）；
3. 心跳间隔与超时（V4 服务端默认 60 秒级，V3 由服务端配置）——先按 30~60 秒实现。
