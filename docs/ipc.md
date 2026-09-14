# IPC 接口

Sysmodule 通过 IPC 向 NRO / Overlay / Game Mod 提供功能。IPC 是本项目的内部公共
API：命令号、结构体和语义都定义在 `common/include/dglab/ipc.h`，客户端与
sysmodule 共用同一个头文件。

## 基本约定

- 服务名：`dglab`（`DGLAB_IPC_SERVICE_NAME`，受 Switch 服务名 8 字符限制）；
- 协议版本：`DGLAB_IPC_PROTOCOL_VERSION`；
- 命令号一旦发布不得重排。新增命令使用新的号码；临时 PoC 命令集中在
  `common/include/dglab/ipc_poc.h` 的 `0x80` 段，不属于稳定契约；
- 每个回复的负载必须能放进 0x100 字节的 IPC 缓冲区（内联数据区约 232 字节）。
  `sysmodule/source/main.c` 用 `_Static_assert` 保证这一点，超长的数据要拆成块
  （日志就是这么做的）。

客户端用 libnx 的 service dispatcher 调用，例如：

```c
Service dglab;
smGetService(&dglab, DGLAB_IPC_SERVICE_NAME);

DglabIpcVersion version;
serviceDispatchOut(&dglab, DGLAB_IPC_CMD_GET_VERSION, version);

DglabNetStatus status;
serviceDispatchOut(&dglab, DGLAB_IPC_CMD_NET_STATUS, status);
```

## 版本

`GET_VERSION` 当前返回 `0.2.0`。

| 版本 | 变化 |
| --- | --- |
| 0.1.0 | `GET_VERSION`、`PING` |
| 0.2.0 | 增加 `NET_*`（Wi-Fi + WebSocket 传输） |

## 命令表

| 命令 | 号 | 入参 | 出参 | 说明 |
| --- | --- | --- | --- | --- |
| `GET_VERSION` | 0 | — | `DglabIpcVersion` | 服务版本 |
| `PING` | 1 | — | `u32` = `DGLAB_IPC_PING_MAGIC` | 确认连到了正确的服务 |
| `NET_START` | 2 | `DglabNetStartRequest` | — | 启动 WebSocket 服务端；`port = 0` 用 `DGLAB_NET_DEFAULT_PORT`。服务端**不会开机自启**，见 `docs/dglab-socket.md` |
| `NET_STOP` | 3 | — | — | 停止服务端并断开所有连接 |
| `NET_STATUS` | 4 | — | `DglabNetStatus` | 状态快照，见下 |
| `NET_QR` | 5 | — | `DglabNetQrChunk` | 二维码内容；没有局域网地址时返回错误 |
| `NET_SEND` | 6 | `DglabNetSendRequest` | — | 向已绑定的 App 发测试指令 |
| `NET_LOG` | 7 | `DglabNetLogRequest` | `DglabNetLogChunk` | 增量读取服务端日志 |

## NET_STATUS

`DglabNetStatus` 是只读快照，字段含义：

| 字段 | 含义 |
| --- | --- |
| `state` | `DglabNetState`：`Idle` / `Listening` / `Paired` / `Stopped` / `Failed` |
| `last_result` | 最近一次失败步骤的 libnx `Result`，0 表示没有失败 |
| `port` | 监听端口 |
| `ip` / `ip_text` | 局域网地址（`struct in_addr` 与点分十进制文本），未联网时为空 |
| `clients` / `paired` | 当前连接数；`paired = 1` 表示 App 已绑定 |
| `sessions` | 启动以来完成 WebSocket 握手的连接总数 |
| `heartbeats_sent` / `messages_in` / `messages_out` | 收发计数 |
| `commands_sent` | 成功发给 App 的指令条数（`channel = 0` 时按通道各计一条） |
| `reports_received` | 收到的 App 强度/反馈报文数 |
| `last_error` | 最近发给客户端的协议错误码（210/400/403/405/500），0 表示没有 |
| `app_strength_a` / `app_strength_b` | App 上报的当前通道强度 |
| `app_limit_a` / `app_limit_b` | App 上报的通道上限 |
| `app_feedback` | 最近按下的 App 反馈按钮；`DGLAB_NET_FEEDBACK_NONE` 表示还没有 |
| `controller_id` | 二维码里携带的控制端 uuid |
| `peer_id` | App 自己的 uuid，未绑定时为空 |

`NET_STATUS` 会顺带刷新局域网地址，所以客户端轮询状态就能等到地址出现（不联网时
地址为空，二维码也就无从生成）。

## NET_SEND

`DglabNetSendRequest`：

| 字段 | 含义 |
| --- | --- |
| `command` | `DglabNetCommand_SetStrength` / `_Clear` / `_TestPulse` |
| `channel` | `1` = A，`2` = B，`0` = 两个通道 |
| `value` | `SetStrength`：0..200；`TestPulse`：波形强度 0..100，0 表示用默认的 10 |

`TestPulse` 发送的是内置的短测试波形（8 个 100ms 元素，固定频率），用来验证
“sysmodule → 服务端 → App”整条链路，不是给实际游戏使用的接口。真正的波形数据
接入按 AGENTS.md 的优先级 6 再做。

## 错误码

失败时返回 `MAKERESULT(Module_Libnx, ...)`，客户端可以用 `R_FAILED()` 判断，
用 `R_DESCRIPTION()` / `R_VALUE()` 取细节：

| 错误 | 场合 |
| --- | --- |
| `LibnxError_NotFound` | 还没有 App 绑定（`NET_SEND`）；没有局域网地址（`NET_QR`） |
| `LibnxError_BadInput` | 命令号、通道号或数值越界；入参长度不足 |
| `LibnxError_IoError` | 监听失败；socket 写入失败 |

启动失败的具体 `Result` 会同时记录在 `DglabNetStatus::last_result` 与日志里。

## NET_LOG

日志是一个环形缓冲区（`DGLAB_NET_LOG_CAPACITY` 字节）。客户端从 `cursor = 0`
开始读，把返回的 `next_cursor` 传给下一次调用：

```c
DglabNetLogRequest request = { .cursor = cursor };
DglabNetLogChunk chunk;
serviceDispatchInOut(&dglab, DGLAB_IPC_CMD_NET_LOG, request, chunk);
cursor = chunk.next_cursor;
```

落后太多时游标会被夹到最旧的可用位置，因此读到的是“还留在环里的最新内容”，不会
报错。每块最多 `DGLAB_NET_LOG_CHUNK_SIZE` 字节，读到 `size == 0` 说明已经追平。

## 临时命令

BLE 直连 PoC 的命令（`DGLAB_IPC_POC_*`）保留在 `common/include/dglab/ipc_poc.h`，
用于 `docs/ble-poc.md` 里描述的实机诊断。BLE 路线已经搁置，这些命令不是稳定契约，
正式设备控制命令会在需要时重新设计。
