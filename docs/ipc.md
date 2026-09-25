# IPC 接口

Sysmodule 通过 IPC 向 NRO / Overlay / Game Mod 提供功能。IPC 是本项目的内部公共
API：命令号、结构体和语义都定义在 `common/include/dglab/ipc.h`，客户端与
sysmodule 共用同一个头文件。

## 基本约定

- 服务名：`dglab`（`DGLAB_IPC_SERVICE_NAME`，受 Switch 服务名 8 字符限制）；
- 协议版本：`DGLAB_IPC_PROTOCOL_VERSION`（按 `0xMMmmpp` 打包，`GET_VERSION`
  直接返回它的三个字节）；
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

`GET_VERSION` 返回的号来自 `common/include/dglab/ipc.h` 的
`DGLAB_IPC_PROTOCOL_VERSION`（打包值 `0x000203u`），当前是 `0.2.3`。它是 **IPC 接口
版本**，`GET_VERSION` 是它唯一的出口（不进任何配置文件）。发行版本号是另一回事：
它属于 NRO 的 NACP 与 About 页，也写进 sysmodule 安装目录的 `toolbox.json` 的
`version` 字段，单一来源是仓库根 `VERSION`（见 `nro/AGENTS.md` 的"元信息与版本"、
`sysmodule/AGENTS.md` 的"构建与发布"）。接口发生不兼容变更时递增次版本号。

| 版本 | 变化 |
| --- | --- |
| 0.1.0 | `GET_VERSION`、`PING` |
| 0.2.0 | 增加 `NET_*`（Wi-Fi + WebSocket 传输） |
| 0.2.1 | 增加 `BLE_*`（sysmodule 直连设备；强度与波形仍走 `NET_SEND` / `NET_WAVEFORM`） |
| 0.2.2 | 增加 `BLE_LIMIT`（会话运行时改上限） |
| 0.2.3 | `BLE_START` / `BLE_LIMIT` 的上限从一个数（两通道共用）改成 **A/B 两个**（BF 本来就是两个） |

## 命令表

| 命令 | 号 | 入参 | 出参 | 说明 |
| --- | --- | --- | --- | --- |
| `GET_VERSION` | 0 | — | `DglabIpcVersion` | IPC 接口版本，见上文“版本” |
| `PING` | 1 | — | `u32` = `DGLAB_IPC_PING_MAGIC` | 确认连到了正确的服务 |
| `NET_START` | 2 | `DglabNetStartRequest` | — | 启动 WebSocket 服务端；`port = 0` 用 `DGLAB_NET_DEFAULT_PORT`。服务端**不会开机自启**，见 `docs/dglab-socket.md` |
| `NET_STOP` | 3 | — | — | 停止服务端并断开所有连接 |
| `NET_STATUS` | 4 | — | `DglabNetStatus` | 状态快照，见下 |
| `NET_QR` | 5 | — | `DglabNetQrChunk` | 二维码内容；没有局域网地址时返回错误 |
| `NET_SEND` | 6 | `DglabNetSendRequest` | — | 向已绑定的 App 发测试指令 |
| `NET_LOG` | 7 | `DglabNetLogRequest` | `DglabNetLogChunk` | 增量读取服务端日志 |
| `NET_WAVEFORM` | 8 | `DglabNetWaveformRequest` | — | 上传（或替换）一批波形槽位，服务端按节奏补流给 App |
| `BLE_START` | 9 | `DglabBleStartRequest` | — | 启动 **BLE 会话**：sysmodule 自己连设备并驱动它（见下） |
| `BLE_STOP` | 10 | — | — | 停止 BLE 会话（收尾会先写 BF=0 再把两通道归零） |
| `BLE_STATUS` | 11 | — | `DglabBleStatus` | BLE 会话状态快照（**开环**：强度是"我们请求过多少"） |
| `BLE_LIMIT` | 12 | `DglabBleLimitRequest` | — | 会话运行中改两个通道强度上限（BF）；没有会话时被拒——那一路由 `BLE_START` 带 |

## BLE_*（sysmodule 直连设备）

`BLE_START` 的入参是 `DglabBleStartRequest`：`limit_a` / `limit_b`（两个**通道强度上限**
0~200，设备侧强制、可以逐通道不同——不主动要求的话就是 0，那一通道无法输出）+
`address[6]`（要驱动的设备；由客户端从
`SD:/switch/DGLAB-NX/config/dglab-ble-address.txt` 读，那个文件由 sysmodule 的扫描自动
写入，见 `docs/ble-poc.md`）。

**上限不是强度，是两组变量。**上限是设备强制的天花板（BF 指令的"通道强度软上限"），强度是
客户端拨的那个数（`NET_SEND` 的 `SetStrength` 等）。两者**各有一对 A/B**：`limit_a` /
`limit_b` 是设备侧不允许越过的值，`strength_a` / `strength_b`（状态里那两个）是我们请求过
的值。会话把请求的强度按对应通道的上限夹紧，而不是拿一个数管两个通道。

**强度与波形沿用 Socket 模式那两个命令**（`NET_SEND` 的 `SetStrength` / `IncreaseStrength`
/ `DecreaseStrength`，以及 `NET_WAVEFORM`），BLE 会话激活时它们被路由到本地协议层而不是
转发给 App——所以玩法的代码不需要为 BLE 改一行。

`BLE_LIMIT` 是**会话运行中**改那两个上限的办法（0.2.2 加的，0.2.3 起带两个数）。上限在
`BLE_START` 里带一次；补上 `BLE_LIMIT` 是为了让"改上限"不必把会话停掉重连（重连会掉链路，
而且这个固件一个开机周期只走得了一条 BLE 路径）。上限一变，之后请求的强度按新值夹紧。

**会话自己会播一段波形。**`BLE_START` 之后两个通道就开始播"会话默认波形"（100ms、波形强度
100，和 Socket 页的测试键同一形状），所以**强度、上限都是 0 时设备上的灯也会闪**——那是
"输出开着"的样子（手机 App 的实测，见 `docs/ble-re.md`）。不播波形时设备收到的是全零波形
数据，按官方文档"某通道只要有一个值超出有效范围就放弃该通道全部 4 组数据"，设备等于什么
都没在播：灯不闪，强度加上去也没东西可放大。

三条已知限制（都来自实机结论，见 `docs/ble-re.md`）：

1. **开环**：这版固件不给第三方进程回读（B1、电量、读值都到不了我们），所以
   `DglabBleStatus` 里的 `strength_a` / `strength_b` 是**我们请求过的值**，不是设备实际状态；
   UI 不应把它当成真实强度显示；
2. **需要 exefs 补丁**，且要按 `docs/ble-poc.md` 的顺序启动（先驱动级探针把 BLE 栈打开，
   再起 BLE 会话）。会话内部还要自己开一次 btm 服务（`btmInitialize()`，和 btm 探针一样）——
   漏掉它时 `btmBleConnect` 会直接回 `0xE401`（`Kernel/114`，见 `docs/ble-re.md` 错误码表）；
3. **一个开机周期只走一条 BLE 路径**：会话跑完，这一周期里就不要再走第二条 BLE 路径
   （想再连一次、或再跑探针，先重启主机）。这条规矩管的是 BLE 自己——BT 栈与 btm 被我们
   动过，同一个周期里混着用会崩整机（`docs/ble-re.md` 的「`0x668F` 与 btm 的崩溃路径」）。
   **Socket 模式不受它影响**：那条路是手机连设备、Switch 只跑 WebSocket，会话停掉
   （`BLE_STOP`）后 `NET_SEND` / `NET_WAVEFORM` 立刻回到 Socket 转发，不用重启。

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

`command` 的完整取值：

| 取值 | 含义 |
| --- | --- |
| `DglabNetCommand_SetStrength` | 把通道强度设为 `value`（0..200，原始值） |
| `DglabNetCommand_IncreaseStrength` | 通道强度相对增加 `value` |
| `DglabNetCommand_DecreaseStrength` | 通道强度相对减少 `value` |
| `DglabNetCommand_Clear` | 清空该通道波形队列（`value` 忽略） |
| `DglabNetCommand_TestPulse` | 送一段内置测试波形，`value` = 波形强度 0..100 |

相对增减是给**用户级控制**用的：手柄的 +/-、overlay 的加减按钮不必自己维护当前强度
（App 是单向的，它不会把强度回报给我们），说一句"加 5"就够了。

**强度是用户设定、波形由事件源设定**：事件源（游戏 Mod / 手柄传感器）生产的是波形流
（25ms 槽位的频率与波形强度），见 `docs/dglab-socket.md` 里"事件 → 波形"一节。

`TestPulse` 发送的是内置的短测试波形（8 个 100ms 元素，固定频率），用来验证
“sysmodule → 服务端 → App”整条链路，不是给实际游戏使用的接口。真正的波形数据
接入走下面的 `NET_WAVEFORM`。

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

## NET_WAVEFORM（事件源上传波形）

App 收到一段 `pulse` 后**播完就停**，所以连续波形必须由我们持续供给。事件源（游戏
Mod、手柄传感器）用这个命令把波形槽位交给 sysmodule，由 sysmodule 负责排队和补流：

| 字段 | 含义 |
| --- | --- |
| `channel` | `1` = A、`2` = B、`0` = 两个通道 |
| `mode` | `DglabNetWaveform_Append`：接在当前波形之后；`DglabNetWaveform_Replace`：先 `clear` 再立刻播放这一段 |
| `slot_count` | 1~`DGLAB_NET_WAVEFORM_MAX_SLOTS`（48），每槽位 25ms，一次最多 1.2 秒 |
| `slots[i].frequency_ms` | 10~1000，App 侧频率（服务端压缩成设备值） |
| `slots[i].strength` | 0~100 波形强度，与用户设定的通道强度相乘 |

两种模式对应两种用法：

- **Replace**：一次性事件（受击、结算音效）——每次都是一个独立"动作"，先清空再播放；
- **Append**：连续流（手柄传感器）——事件源按 100~200ms 一批持续上传，服务端保持 App
  队列不空（提前约 200ms 补下一批，每批最多 8 个元素 = 800ms）。

槽位请按 **4 的倍数** 上传（4 个槽位 = 一个协议元素）；不足 4 个的余量会留在队列里等
下一次上传补齐。队列每通道 128 个槽位（3.2 秒），溢出时丢弃最旧的并记一行日志。

`NET_SEND` 的 `Clear` 会同时清空这两个队列，所以"停止"按钮能把流停干净。

## 临时命令

BLE 直连 PoC 的命令（`DGLAB_IPC_POC_*`）保留在 `common/include/dglab/ipc_poc.h`，
用于 `docs/ble-poc.md` 里描述的两个探针（驱动级 / base `btm`）与它们的日志回读
（`START` / `STOP` / `STATUS` / `LOG` / `ACTION`）。它们不是稳定契约，正式设备控制命令
（连接、设强度、发波形）会在 BLE 传输定稿时重新设计；2026-09-25 那次清理已经把
btdev 路线、身份探针与扫描过滤器这些诊断动作从这套临时命令里删掉了。
