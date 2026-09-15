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

**路由字段的方向（实机验证，容易写反）**：`clientId` 是**发件人**，`targetId` 是
**收件人**。服务端发给 App 的 `msg` / `heartbeat` 都必须是
`clientId = 控制端 uuid`、`targetId = App 自己的 uuid`。

这一点是用实机扫出来的：最初实现按"clientId = 收件人"发，App 会在日志里显示这条消息
但**完全不执行**（强度不变、设备无输出），而且不会回任何消息；把两个字段对调后立刻就
生效（App 上的强度跟着变、波形也出现在 App 里）。绑定消息 `bind` 恰好一开始就是对的
（`clientId = 控制端`、`targetId = App`），所以当时只有"绑定成功、指令无效"这种表象。

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

### 服务端不会开机自启

`dglabNetSocketStartSleepWatch()` 在 sysmodule 启动时只做两件事：建好会话核心，
并向电源管理注册睡眠通知。**服务端本身要等客户端调用 `NET_START`（NRO 上按 `A`）
才启动**，因为持有 socket 会让整机睡眠出问题（见下一节），而大部分人根本不会用到
这个服务。`NET_STOP`（NRO 上按 `Y`）会把它收掉。

### 睡眠与唤醒

持有 socket 跨过整机睡眠是"睡死"的常见原因（实机反馈：加载 sysmodule 后休眠会卡死，
只能强制重启）。因此 sysmodule 会通过 `psc:m` 注册一个电源管理模块：

| 收到 | 动作 |
| --- | --- |
| `PscPmState_ReadySleep` / `ReadyShutdown` | 停掉服务端（关 socket、结束线程），然后确认 |
| `PscPmState_ReadyAwaken` | 如果睡眠前在运行就重新启动，然后确认 |

实现是保守的：注册用的模块 id 是 `PscPmModuleId_WlanSockets`，若该 id 已被系统占用
（`pscmGetPmModule` 返回失败），就只记一行日志并照常运行。

**实机结果：注册被拒绝，返回 `0x0000108A`**（模块 8、描述 0x8A；`WlanSockets` 这个 id
由系统自己持有）。因此又补了两条兜底：

1. 服务端不自动启动（已验证：不启动就不会睡死）；
2. **没有客户端连接满 55 秒就自动停服务**；计时从最后一个客户端离开开始。窗口刻意短于
   主机最短的自动休眠时间（60 秒），这样"人走开 → 主机自动休眠"这条路径上服务端一定
   已经收掉了。有人连着（App 会持续发心跳）时不会自动停；
3. NRO 界面在服务运行时会显示 `do not sleep the console while the server runs: press Y first`。

**现状是诚实的：只要服务端在跑，这台主机休眠就会卡死，因为没有可用的睡眠通知。** 想要
真正修好，需要找到能可靠告知"即将休眠"的机制（`psc` 的模块 id 或其它信号），在那之前
只能靠上面三条兜底。

**注册只尝试 `WlanSockets` 这一个 id，而且绝不允许再试别的 id。** 实机证据：拿
`200`/`201` 之类的自定义 id 去调 `pscmGetPmModule` 会**冻住整台主机**——一次发生在开机
路径（卡在开机 logo），一次发生在按 `A` 启动服务时（所有按键无响应，只能长按电源键）。
`WlanSockets` 则是安全且"失败很快"的：系统自己持有该 id，调用立刻返回 `0x0000108A`。

结论：**PSC 这条路现在是关着的**，除非将来有别的可靠机制，不要在 sysmodule 里再对
`psc` 做实验。日志里出现的 `sleep watch unavailable rc=0x0000108A` 就是正常情况。

同时 `nifm` 改成"用完即走"：查地址时 `nifmInitialize` → `nifmGetCurrentIpAddress` →
`nifmExit`，结果缓存 2 秒（NRO 每帧轮询状态）。长期持有 nifm 会话是"网络占用"的另一
个候选原因，先把它排除掉。

### 开机路径必须最小（血泪教训）

sysmodule 的开机路径只允许做三件事：

    sm 注册服务  →  初始化互斥锁  →  建会话核心（csrng + 控制器 id + 内存日志环）

**不在开机路径里做的事**（每一条都是实机踩出来的）：

- 不碰文件系统：`mkdir("sdmc:/…")` 会 Data Abort（libnx 只在 applet 里挂 sdmc）；
- 不创建线程：多出来的常驻线程会和开机流程抢时间；
- 不注册 PSC：`pscmGetPmModule` 的额外尝试会让主机停在开机 logo。

socket、服务线程、PSC 注册全部放在 `dglabNetSocketStart()` 里，也就是用户按 `A`
启动服务的时候（`nifm` 连持久会话都不留，见上一节）。空闲自动停服务的检查放在 tick
线程里**只置标志位**，真正的停服务由下一次 IPC 调用执行——因为服务线程不能 join 自己。

### 日志文件（SD 卡）

排查实机问题全靠这三个文件：

| 文件 | 写入方 | 内容 |
| --- | --- | --- |
| `sdmc:/switch/DGLAB-NX/dglab-net.log` | NRO | `NET_LOG` 的增量副本（服务端日志的实际落盘处） |

（曾经还有一个 `dglab-boot.log` 记录 NRO 的启动步骤，用来查"无 sysmodule 时黑屏"。
经实机确认无 sysmodule 时 NRO 可以正常启动，该文件已按要求移除；黑屏那条留到以后完善
前端体验时再处理。）

**sysmodule 自己不写文件。** libnx 只在 applet 里挂载 `sdmc`，sysmodule 需要一个
路径时不会干净地失败：`mkdir("sdmc:/switch")` 会走进 newlib 的 devoptab 兜底路径并解
引用空指针。这不是推测——安装 5.x 那版 sysmodule 后开机 logo 处直接报错，崩溃报告
（`01789399881_00ff072107210721.log`）的调用链是：

    mkdir  <-  netFileLog(net_socket.c:82)  <-  dglabNetServerLog  <-  generateId
           <-  dglabNetServerInit  <-  netCoreEnsureReady  <-  main

因此 sysmodule 侧只保留内存日志环，由 NRO 读出来写盘。真正需要在 sysmodule 里读写
文件时，必须先 `fsInitialize()` + `fsdevMountSdmc()`，并且在 sysmodule 启动早期还要
考虑文件系统可能尚未就绪。

### 连接与错误处理

| 情况 | 行为 |
| --- | --- |
| 请求路径里没有 id（`/`、空） | **仍然配对**，日志写 `client connected without a client id in the target` |
| id 与二维码里的控制器 uuid 不一致 | **仍然配对**，日志写两个 id 并注明 `does not match ... pairing anyway` |
| 该控制器 id 已被绑定 | 回 `400` 并断开 |
| 服务端不在监听状态 | 回 `500` 并断开 |
| 收到超过 1950 字节的消息 | 回 `405` |
| 内容不是 JSON 对象 | 回 `403` |
| 收到 `break` | 解除绑定，回到 `Listening`，等 App 重连 |
| 收到未知 `msg` 指令 | 记日志后忽略，不报错（App 版本比本实现新时不应断线） |

### 测试按键为什么需要同时发强度和波形

### 事件 → 波形：本项目真正的用途

**强度由用户设定，波形值由事件源设定。** 强度相当于音量旋钮（用户在 App 或 NRO 上设一次
就不动），事件源生产的是**波形流**：

    游戏事件 / 手柄传感器 / NRO 参数
              ↓  事件源决定"每个 25ms 槽位的频率与波形强度"
        IPC: 波形上传（每个槽位 = frequency_ms + strength）
              ↓
        Sysmodule：按 4 槽位打包成 16 进制元素 → pulse-<ch>:[…] → App → 郊狼

分工要点：

- **波形数值由事件源产生**（受击给一段高频/高强度、平时给低强度或静音）；
- **强度不参与事件映射**，只在 App 侧对波形整段做幅度缩放（设备输出 = 通道强度 × 波形强度）；
- 协议层已有的编码直接复用：`DglabCoyoteV3WaveformEntry` 的数据形状（frequency_ms +
  strength）就是槽位形状，`dglabCoyoteV3CompressFrequency` + `dglabSocketEncodePulseHex`
  已经把"槽位 → 16 进制元素"做完了，缺的只是"事件源 → 槽位队列 → 分批 pulse"这一段。

打包上限：每个 `pulse` 元素 = 4 个槽位（100ms），单条命令最多 86 个元素（约 8.6 秒）。

两点限制要记住：

1. App 是单向的，不上报强度，所以强度只能"本地记账"（我们设多少就认为是多少）；
2. App 自己的强度上限会静默裁剪我们的请求，这个值我们看不到，需要用户自己在 App 里设好。

郊狼的输出是**两个值相乘**：通道强度（0~200）与波形里的强度（0~100）。只设强度、没有
波形时设备完全没反应；只在设备上有一段默认波形、强度为 0 时同样没反应。

实机第一次测试时，NRO 发的是强度 10/200 + 波形强度 10/100，也就是满量程的 0.5%，
基本不可能感觉到。所以测试按钮现在的行为是：

| 按键 | 发送 |
| --- | --- |
| `ZL` | 测试波形（波形强度固定 100）**并**设置通道强度（滑块值） |
| `X` | 只设置通道强度（滑块值） |
| `B` | `clear-A` + `clear-B` |
| `L`/`R` | 通道强度 0~100，步进 1，**不循环**（0 再减还是 0，100 再加还是 100）；按住会以约 6 帧一步连续调整 |

**滑块用的是设备的原始值，和 App 里显示的强度是同一个数字，不做百分比换算**；官方
文档说明超过 100 只适用于特殊情况，所以测试上限就到 100。界面上的 `buttons` 行显示
`strength N of 100`。

如果按了 `ZL` 仍无输出，要按顺序确认：App 上显示的强度有没有跟着变（说明指令被解析）、
App 的强度上限是不是 0（会把我们的请求夹到 0）、以及设备本身是否连着 App。

### 关于 3.0 App 的日志

3.0 App 的 socket 日志里显示的 `tx` 其实是**它收到的服务端消息**（可以看到我们发的
`bind` 和心跳，时间戳与"绑定后立即补发一条心跳"完全对得上）；`rx` 才是它自己发出去的。
所以"看不到 rx"并不代表 App 没收到指令。

实机还观察到：**3.0 App 不向服务端发送任何消息**——连 WebSocket Ping 都没有
（`dglab-sys.log` 里没有任何 `rx` 行，Ping 计数也一直是 0），因此 NRO 上"App 上报的强度
与上限"一栏会一直是空的，这不是我们的解析问题，而是这一版 App 单向通信。

同时最多接受 2 条连接（一条绑定 + 一条重连过渡），日志环 4KB。

宽容配对是实机反馈的结果：按参考实现用 `210` 拒绝"id 不对"的连接，会让手机 App
一直停在"正在连接"上转圈。现在的做法是先配对、把对方的 target 原样记进日志，
等实机日志确认 App 到底发什么（路径里带 id、不带 id、还是 `?tid=`）之后再收紧。

WebSocket 握手方面还有一条兼容处理：如果客户端带了
`Sec-WebSocket-Protocol`，服务端必须回选其中一个协议，否则客户端必须判定连接失败。
服务端现在回显列表里的第一个 token；如果客户端没带这个头，就不回。

### 排查连接问题看什么

每次握手都会记一行：

| 日志 | 含义 |
| --- | --- |
| `accept from <ip>` | 每接受一条 TCP 连接都会记（握手之前）——用来区分"对端根本没到"和"到了但握手没完成" |
| `websocket from <ip>, target '<target>'` | 收到握手，含对端地址与请求路径 |
| `client connected without a client id in the target` | 路径里没有 id |
| `client id X does not match Y, pairing anyway` | 路径里的 id 与二维码里的不同 |
| `app <uuid> bound (target '<target>')` | 已配对，bind 已发出 |
| `websocket handshake from <ip> failed` | 握手失败（不是 WebSocket 请求等） |
| `app <uuid> disconnected` | 对端断开 |

如果手机停在"正在连接"，先看有没有 `websocket from ...`：没有就是 TCP 根本没到
Switch（网络/端口问题），有就说明握手与配对已经完成，问题在配对之后的消息约定上。

实机记录（2026-09-15）：手机（172.20.10.7）能稳定连上并配对，日志里是
`websocket from ... target '/<controller id>'` → `app <uuid> bound`；电脑浏览器访问
同一端口只会留下 `websocket handshake from 172.20.10.3 failed`（普通 HTTP，符合预期）。
也就是说**协议层与配对流程本身是通的**。

停止服务时会给已配对的连接发一个 WebSocket close 帧再关 socket，让 App 有机会显示
断开（实机反馈：之前直接 shutdown，App 不会自动断开）。

### 实机验证记录（2026-09-15，DG-LAB App 3.0 + Coyote 3.0）

优先级 5 在真机上跑通：NRO 显示二维码 → App 扫码绑定 → NRO 用真实强度与波形控制设备，
设备有输出。以下几条原先是推断，现在有实机结论：

1. **指令信封的路由字段**：`clientId` 是**发件人**、`targetId` 是**收件人**（与最初
   的推断相反，详见"消息外壳"一节）。写成反方向时 App 会记录消息却完全不执行。
2. **心跳**：每 30 秒一条、绑定后立刻补发一条的写法，App 接受并保持会话；`message`
   用 `"200"`。App 不回应心跳。
3. **pulse 的 JSON 转义**：转义形式正确，App 能播放我们排队的波形。
4. **App 是单向的**：3.0 App 不向服务端发送任何东西（没有 `msg`、没有 `break`、连
   WebSocket Ping 都没有）。因此 `NET_STATUS` 里 App 上报的强度/上限一直是 0，
   NRO 上显示为 `no report from the app`。
5. **测试按键的强度语义**：设备输出 = 通道强度(0~200) × 波形强度(0~100)。`ZL` 送
   满强度波形并设置通道强度，所以屏幕上的数字就是实际强度；默认 10 / 上限 100（原始值）。

仍未解决或未验证：

1. **服务端运行时休眠会卡死**（原因见上文"睡眠与唤醒"），只能靠"不自动启动 + 55 秒空闲
   自动停 + 界面提示"兜底；55 秒自动停这条本身还没有实机确认过。
2. **V4 未实现**：`?tid=` 形式在解析里被接受（便于排查），但 V4 的消息外壳没有实现。
3. **App 的强度上限不会同步**：见第 4 条，是 App 设计，不是本项目的缺陷。

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

NRO 会把 `NET_LOG` 同步写到 `sdmc:/switch/DGLAB-NX/dglab-net.log`（打不开时退到
`sdmc:/dglab-net.log`），实机测试后直接把这个文件发回来即可。
