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

1. ~~App 版本~~：已确认是 **V3**，因此先只实现 V3（路径携带 clientId、驼峰外壳、
   数字通道号）；
2. ~~能否手输地址~~：**不能，必须扫码**，所以 NRO 必须显示二维码 —— 需要在本项目里
   实现一个 QR 编码器（devkitPro 里没有可用的 QR 库）；
3. ~~心跳间隔与超时~~：按 30 秒实现（见下），但心跳报文的**内容**仍未与真实 App
   验证过。

---

## 本项目的实现

### 代码位置

| 文件 | 作用 |
| --- | --- |
| `sysmodule/source/net/ws.c` | 平台无关的服务端 WebSocket（握手、帧、掩码） |
| `sysmodule/source/net/dglab_socket.c` | 平台无关的 Socket 协议：消息外壳、指令构造、上报解析、二维码 |
| `sysmodule/source/net/net_server.c` | 平台无关的服务端会话：绑定、心跳、转发、命令、状态与日志环 |
| `sysmodule/source/transport/net_socket.c` | Switch 侧传输：监听 socket、每连接一个线程、定时线程、互斥锁 |
| `common/include/dglab/ipc.h` | 对外的 `NET_*` IPC 命令与结构体，见 `docs/ipc.md` |

`sysmodule/source/net` 全部是平台无关代码，主机测试在 `tests/net` 里跑。
NRO 侧的界面与二维码渲染见 `docs/nro-ui.md`：界面通过上面的 IPC 命令读出地址与
二维码内容，自己编码并绘制二维码。

### 拓扑：Switch 同时是服务端和控制端

参考实现里控制端也是一条 WebSocket 连接，但那对手机 App 不可见：App 能观察到的
只是“服务端给了它一个绑定、并转发指令”。因此本项目不额外建立回环连接，而是由
sysmodule 内部直接扮演控制端：

- 控制器 uuid 在服务端启动时生成，二维码里携带的就是它；
- App 按 `/ <controllerId>` 连接后，服务端回
  `{"type":"bind","clientId":"<controllerId>","targetId":"<appId>","message":"200"}`；
- App 的 uuid 由服务端分配（`targetId` 是 App 得知自己 id 的唯一途径）；
- 指令由 sysmodule 直接写进 App 的那条连接，App 上报的强度/反馈直接进入 sysmodule。

`controller_id` 在 stop/start 之间保持不变，所以二维码不用重新扫。

### 连接与错误处理

| 情况 | 行为 |
| --- | --- |
| 请求路径里没有 id（`/`、空） | 回 `210` 并断开 |
| id 与二维码里的控制器 uuid 不一致 | 回 `210` 并断开，日志记录对方提供的 id |
| 该控制器 id 已被绑定 | 回 `400` 并断开 |
| 服务端不在监听状态 | 回 `500` 并断开 |
| 收到超过 1950 字节的消息 | 回 `405` |
| 内容不是 JSON 对象 | 回 `403` |
| 收到 `break` | 解除绑定，回到 `Listening`，等 App 重连 |
| 收到未知 `msg` 指令 | 记日志后忽略，不报错（App 版本比本实现新时不应断线） |

同时最多接受 2 条连接（一条绑定 + 一条重连过渡），日志环 4KB。

### 已知的实现约定（需要实机验证）

1. **指令信封的路由字段**：本文假设 `clientId` 是收件人、`targetId` 是发件人
   （与错误码 404 “收信人不在线”的语义一致）。这一点没有官方实现逐行比对过。
2. **心跳内容**：每 30 秒发
   `{"type":"heartbeat","clientId":"<appId>","targetId":"<controllerId>","message":"200"}`，
   绑定成功时立刻补发一条。App 是否要求控制端先发心跳、心跳的期望内容都未验证。
3. **pulse 的 JSON 转义**：`pulse-A:["0A…"]` 自带双引号，因此信封构造会转义
   `"` 与 `\`（这一条有主机测试覆盖）。如果 App 实际接受的是未转义形式，需要改回。
4. **没有空闲超时**：连接只靠 TCP 断开或 `shutdown()` 结束，App 静默 90 秒只记一条
   日志。实机确认 App 的心跳行为后再决定是否主动断开。
5. **V4 未实现**：`?tid=` 形式在解析里被接受（便于排查），但 V4 的消息外壳没有实现。

### 如何验证

主机侧：

```
make -C tests/net        # 内存级协议测试 + 真实回环 TCP 端到端测试
```

- `test_dglab_socket`：外壳解析/构造（含转义）、指令、上报、二维码；
- `test_net_server`：绑定流程、错误码、指令、心跳、二维码、日志环；
- `test_net_loopback`：真实 TCP + WebSocket 握手 + 掩码帧，模拟手机 App 走完
  “扫码连接 → 绑定 → 上报强度 → 收到指令”。

实机侧（需要用户的手机与 Switch）：

1. Switch 与手机连同一个局域网；
2. NRO 显示地址与二维码，用 DG-LAB App 扫码；
3. 期望 `NET_STATUS.state` 变成 `Paired`、`peer_id` 非空、App 界面显示已连接；
4. 用 `NET_SEND` 的测试按钮看强度/清空是否生效；
5. 观察 `NET_LOG` 里的收发记录，据此修正上面 1、2、3 三条约定。
