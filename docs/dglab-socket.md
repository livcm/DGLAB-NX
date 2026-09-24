# DG-LAB Socket 协议（Wi-Fi + WebSocket）

本文对应 **WebSocket 模式**（已实现）：sysmodule 与手机 DG-LAB App 建立 WebSocket 会话
——Switch 当服务端，App 扫码连入（Switch 不主动外连）；手机负责与设备之间的 BLE，并把
波形数据转发给设备。BLE 模式（sysmodule 直接连接设备）未实现（连接与写入已实机跑通、
通知路径未通，且依赖补丁），见 `docs/ble-poc.md` 与 `docs/ble-re.md`。

本文记录 Switch 侧通过局域网控制手机的 DG-LAB App 所需的协议事实。

## 为什么走 WebSocket 模式而不是 BLE 模式

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

**命名**：V3 / V4 是这套 **Socket 协议**（Wi-Fi + WebSocket）的版本号；`Coyote` 是
**蓝牙协议**的代号（Coyote V2 / V3，见 `docs/dglab-protocol.md`）。两条协议线互相独立，
版本号没有对应关系，不要写成"郊狼 4.0 的 Socket 协议"，也不要把这里的 V3 叫 Coyote V3。

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
这个服务。`NET_STOP`（NRO 上再按一次 `A`，底栏的 `A` 提示会在 start / stop 之间切换）
会把它收掉。

### 睡眠与唤醒

持有 socket 跨过整机睡眠是"睡死"的常见原因（实机反馈：加载 sysmodule 后休眠会卡死，
只能强制重启）。因此 sysmodule 会通过 `psc:m` 注册一个电源管理模块：

| 收到 | 动作 |
| --- | --- |
| `PscPmState_ReadySleep` / `ReadyShutdown` | 停掉服务端（关 socket、结束线程），然后确认 |
| `PscPmState_ReadyAwaken` | 如果睡眠前在运行就重新启动，然后确认 |

实现是保守的：注册用的模块 id 是 `PscPmModuleId_WlanSockets`，若该 id 已被系统占用
（`pscmGetPmModule` 返回失败），就只记一行日志并照常运行。

**注册只尝试 `WlanSockets` 这一个 id，绝不允许再试别的**：拿 `200`/`201` 之类的自定义 id
去调 `pscmGetPmModule` 会**冻住整台主机**（一次卡在开机 logo，一次所有按键无响应，只能
长按电源键）。实机结果是注册被拒绝、返回 `0x0000108A`（模块 8、描述 0x8A；`WlanSockets`
这个 id 由系统自己持有），日志里的 `sleep watch unavailable rc=0x0000108A` 就是正常情况。

**注册被拒这件事仍然成立**：sysmodule 什么都收不到。睡眠这条风险因此分两层来挡——
一层由 NRO 关掉自动休眠，一层是原来的兜底：

1. **自动休眠由 NRO 关掉**：NRO 是 applet，服务端运行期间它用
   `appletSetAutoSleepDisabled()`（`ISelfController`，`applet.h`）把主机的自动休眠关掉，
   服务端停掉时恢复。实现是 `nro/source/platform/auto_sleep.c` 一个模块，只有它碰这个标志：
   进入抑制前先 `appletIsAutoSleepDisabled()` 读一次，**本来就是关的（用户自己的设置或别的
   applet）不去接管、退出时也不恢复**；只有确实是它关的才在停止/退出时调 `false`。调用失败只
   写一行日志，服务端照常运行，界面退回到下面第 4 条的旧警告文案；
2. 服务端不自动启动（已验证：不启动就不会睡死）；
3. **没有客户端连接满 55 秒就自动停服务**；计时从最后一个客户端离开开始。窗口刻意短于
   主机最短的自动休眠时间（60 秒），这样"人走开 → 主机自动休眠"这条路径上服务端一定
   已经收掉了。有人连着（App 会持续发心跳）时不会自动停；
4. NRO 界面在服务运行时会显示警告：抑制生效时是 `sleep_warning_auto_off`
   （"自动休眠已抑制；手动休眠仍会卡死主机"），抑制不可用或调用失败时是原来的
   `sleep_warning`（"服务端运行时不要休眠"）。两种文案都在 `tests/canvas` 的排版检查里。

**还没有解决的两件事**：

- **手动休眠（电源键）照旧会卡死**。关得掉的是自动休眠；用户按下去的睡眠挡不住——挡它要拿
  sleep lock（`appletRequestToAcquireSleepLock`，同样是 applet 会话的接口），那会让主机在服务端
  运行期间完全无法休眠。所以在服务端运行时手动休眠仍然只能长按电源键恢复；
- **NRO 退出后服务端还在跑的时候**：抑制只覆盖 NRO 存活期，applet 会话结束，标志跟着失效。
  此时服务端仍在跑（手机连着时 55 秒空闲自停也不会触发），主机自动休眠仍会卡死。这是已知
  漏洞，先保留：现在没有游戏侧事件源，NRO 退出后服务端本来也没人用；将来做 Overlay 时一起解决。

**为什么不去改系统设置**：`set:sys` 有直接对口的旋钮——`setsysGetSleepSettings()` /
`setsysSetSleepSettings()` 加 `SetSysHandheldSleepPlan_Never`（libnx 的
`switch/services/set.h`），但它改的是**用户可见、掉电保持的全局系统设置**，sysmodule 有没有
写它的权限也没验证过（`service_access: ["*"]` 只保证能开会话，系统侧还有一层检查），而且同样
挡不住手动休眠。评估后放弃这条路，只在 NRO 侧做抑制。

想要真正修好，需要找到能可靠告知"即将休眠"的机制（`psc` 的模块 id 或其它信号）。
**PSC 这条路现在是关着的**，除非将来有别的可靠机制，不要在 sysmodule 里再对 `psc` 做实验。

同时 `nifm` 改成"用完即走"：查地址时 `nifmInitialize` → `nifmGetCurrentIpAddress` →
`nifmExit`，结果缓存 2 秒（NRO 每帧轮询状态）——长期持有 nifm 会话是"网络占用"的另一个
候选原因，先把它排除掉。

**实机结论（2026-09-18）**：applet 模式（相册进入）与 title override 两种启动方式下抑制都生效，
服务端运行期间主机不再自动休眠——`appletSetAutoSleepDisabled()` 这条路是通的，不是"调用返回成功
但没作用"。主机侧的验证是 `make -C nro`、`tests/canvas`（两套警告文案各渲染一遍）与 `tests/lang`。

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

### 栈上不要放 KB 级缓冲区（血泪教训）

sysmodule 的线程栈很小（主线程来自 NPDM 的 `main_thread_stack_size`，网络线程是
`net_socket.c` 里 `NET_THREAD_STACK_TOTAL` 的静态 `.bss` 栈，见下一节），这条路径上还叠着
libnx 的 IPC/服务调用、newlib 的 `printf` 和 SD 卡写入。**症状**：按 `ZL`/`ZR` 会让整个
sysmodule 死掉——**进程直接消失、所有 IPC
无响应**，客户端拿不到任何 `Result`（所以一开始被当成发送路径的问题查）。原因是
`NET_WAVEFORM` 处理链上 `clear` 与 `pulse` 各有一个 1950 字节的栈上缓冲区
（`char command[DGLAB_SOCKET_MAX_MESSAGE]`）；挪到 `.bss` 后不再复现。同类缓冲区
（`sendError`、`sendHeartbeat`、绑定回复帧、`dglabNetServerOnMessage` 的解析结构）也一并
挪到了 `.bss`，核心代码现在没有接近 2KB 的栈帧（这条链上最大的帧是 `dglabNetServerLog`
的 480 字节 + newlib 的 `printf`）。

规则：

- 被 IPC 线程、tick 线程、客户端线程共用的代码（`net_server.c`）里，KB 级缓冲区放 `.bss`
  （`static`），注释写明"调用方持有 transport 锁"；
- **每个连接自己的缓冲区**（`netClientThreadMain` 的接收缓冲、`wsConnRecv` 的重组缓冲）
  必须留在那个线程的栈上，不能共享——两个客户端同时在线时共享会互相踩；
- `make -C tests/stack` 用 devkitA64 的 gcc 以 `-fstack-usage` 编译 sysmodule 全部源码，
  除 `tests/stack/Makefile` 的 `ALLOW` 列出的例外（两个每连接接收缓冲 + ble_poc 的三个
  scan 结果结构）外，**任何函数栈帧 ≥ 1KB 就失败**。量单个文件的命令见
  `sysmodule/AGENTS.md` 的"线程与栈"一节；
- 排查顺序：① 比对日志里的构建标识 → ② `dglab-sys.log` 最后停在哪儿 →
  ③ `sdmc:/atmosphere/crash_reports/…_00ff072107210721.log`。**"SD 卡上装的是旧二进制"
  与"代码真有 bug"表现完全一样**，先排除版本再怀疑代码；
- socket 写现在是**整帧一次写 + 非阻塞 + 失败即断开**（见"socket 写路径"一节）：日志里
  - `write failed: … errno 32`（EPIPE）＝ 对端已经断开，属于正常收尾；
  - `write failed: … errno 11/35`（EAGAIN/WOULDBLOCK）＝ 对端来不及收，这条连接会被主动
    断开，不会再有线程卡在 `send()` 里；
  - 波形上传只入队、发送全在 tick 线程：`dglab-sys.log` 停在某条 `tx waveform …` 之后，要看
    的是 tick 线程那条链，而不再是 IPC 线程；
- 服务端"按了几次 `A` 之后就起不来、重启主机才恢复" → 看下面的
  "反复启停后服务端起不来（0x1759 / 0x559 / 0xD401）"。

### socket 写路径：整帧、非阻塞、失败即断开（2026-09-19）

一轮实机暴露的另一种死法：App 已绑定，按下 `ZL`/`ZR` 之后 sysmodule **不再应答**，
`crash_reports/` 与 `atmosphere/fatal_errors/` 都空，`dglab-sys.log` 停在 `waveform tx: …`
（或 tick 线程的 `tx clear-1`）与写返回之间。

**"进程到底死没死"这样判断**：卡住之后用文件管理器打开 `dglab-sys.log`——

- 打不开、提示"资源被占用中" ⇒ **sysmodule 还活着**（那条日志文件还开着），是**挂住**；
- 能打开并读到内容 ⇒ 进程已经没了。

2026-09-19 那次就是前者，于是按"挂住"查，先修掉一个**锁反转**（见下面的规则），又发现真正的
卡点是 `bsd:u` 的 socket 写本身（第 16 条），于是加了一道**卡死自救**：见下面的"阶段标记 +
看门狗"。

规则：

- **波形上传只入队**：`dglabNetServerUploadWaveform()` 只清队列、置 `clear_pending`、入队；
  发送全部由 tick 线程的 `waveformPump()` 做——先发 pending 的 `clear-<ch>`、再发 batch。
  IPC 线程永远不会停在 socket 写里；`Replace` 的"立刻重来"语义只多 ≤100ms 延迟
  （tick 周期 100ms）。
- **一帧两次写（帧头 → payload），互斥靠帧锁**：`wsConnSend()` 在 `WsConn.lock/unlock`
  （传输侧接连接槽的 `write_mutex`）里先写帧头、再写 payload；握手回复也在锁内一次写完。
  互斥来自这把帧锁，不来自"把一帧合并成一次系统调用"——2026-09-19 把帧头和 payload 合并成
  一次 `send()` 之后就卡住了（见下一条），那次改动已经撤回。
- **不用 `MSG_DONTWAIT`，用阻塞 `send()` + `SO_SNDTIMEO`**：2026-09-19 的探针日志
  （`dglab-probe.log`：6 条 `send begin` 只有 5 条 `send end`，缺的那条是 tick 线程
  `send begin: tick fd 6, 308 left`；此后 `alive:` 每秒继续涨到 66、两个日志文件一直被占用）
  说明：带这个 flag 交给 `bsd:u` 的请求可能**永远不回**，调用者在核里挂着，flag 管不到。
  `send()` 现在只带 `MSG_NOSIGNAL`（HOS 上无信号，等于无语义），超时由连接建立时设的
  `SO_SNDTIMEO`(5s) 兜底；波形 batch 由 tick 线程发、IPC 线程只发小指令，所以慢对端最多拖住
  tick 一轮，不会把 IPC 线程停住。
- **部分写或错误即断开**：`netSocketWrite()` 遇到部分写/错误就 `shutdown()` 该连接，让连接线程
  自己走 detach 流程——不重试。失败信息由 tick 线程打印（下一条）。
- **锁顺序唯一：`transport` → `frame`（`slot->write_mutex`）**。IPC 与 tick 线程按这个
  顺序拿锁；**连接线程只拿 `frame`**，因此它在写失败时**绝不能顺手记日志**——`netLog()` 要去拿
  transport 锁，那就成了 `frame` → `transport`，与上面反向，两条线程各持一把等对方，整个
  sysmodule 就停在那里（NRO 跟着卡死、文件被占用、没有崩溃报告）。失败信息现在由
  `netSocketWrite()` 记进连接槽，tick 线程下一次持锁循环时打印
  （`write failed: … bytes left, errno … fd …`）。

### 诊断 socket 写卡死：探针只能走内存环（2026-09-19 用过一次的方法）

"进程还活着但不再应答"这类问题，最后一次是这样定位的（原始记录见 `docs/history.md` 第 13、
14 条；探针代码在定位结束后已经删除，`dglab-probe.log` 是留在 SD 卡上的文件，可删）：

1. **先判断进程死没死**：卡住后用文件管理器打开 `dglab-sys.log`——"资源被占用"= 进程还活着
   （挂住），能打开 = 进程没了。
2. **探针绝对不要走 SD 日志**：把 `write begin`/`write end` 写成 `dglabNetServerLog()` 之后，
   同样的操作就不再复现（两轮构建只差那几行日志）——SD 写入改变了 `send()` 前后的时序，把
   bug 藏起来。正确的做法是**探针只写内存环（memcpy），由一个独立线程落盘**：那次实现是
   `dglabNetServerProbe()` + 一个不碰 transport 锁、每 25ms 追加
   `sdmc:/switch/DGLAB-NX/logs/dglab-probe.log`、每秒写一条 `alive:` 的线程。这样即使某个线程
   卡在 socket 写里，最后几条探针照样落盘。
3. **怎么读**：成对的 `send begin`/`send end` 是正常的；**只有 begin 没有 end 的那一行**就是
   卡住（或消失）的那次系统调用（写出线程名与字节数）；`alive:` 还在继续 = 进程没死，是挂住。
   最后一次就是这样抓到 `send begin: tick fd 6, 308 left` 没有回音（第 14 条）。

**第二轮：连内存环探针也不能留在构建里**。探针拆掉之后同一个操作又复现，说明热路径里
"每帧一次 memcpy + 一把锁"这种量级的额外工作也足以改变时序。于是换成更轻的一招（同样只写两次
日志文件，别在热路径里做 I/O）：

- **只记阶段**：`netStage()` 往几个 `volatile` 字段里写"最后到达的阶段"（`send begin/end`、
  `log write begin/end`、tick 循环）以及线程号 / fd / 字节数 / errno——纯 store，不加锁、
  不写盘；
- **看门狗**：一个独立线程每 500ms 看一次这个计数器，**超过 3 秒没动**才往
  `sdmc:/switch/DGLAB-NX/logs/dglab-stall.log` 追加一行
  `stall: <阶段> for <n> ms, thread=…, fd=…, bytes=…, errno=…, clients=…, state=…`；
- **读法**：`stall` 行里的阶段就是卡住的那一步——`send begin` = 卡在 socket 写里，
  `log write begin` = 卡在 SD 日志写入里；如果 `dglab-stall.log` 没有出现或内容为空，说明卡住的
  那条链连文件系统都进不去（看门狗自己也被挡住），这本身也是结论。

**这套现在是常驻的**（2026-09-19 实机确认）：第一次实机复现抓到
`stall: send begin for 3282 ms, thread=tick, fd=5, bytes=304`（socket 写被 `bsd:u` 挂住，阻塞
socket + `SO_SNDTIMEO` 都没用），加上"看门狗 `shutdown()` 这个 fd"之后，第二次实机变成**会卡
几秒但会自己恢复**（那次的记录是 `stall: log write end for 3453 ms`，SD 写的一次抖动），App
断开后服务端正常 `stopped`、`heap stop` 与 `heap start` 相等。开销只有：热路径里几个
`volatile` store，加一个每 500ms 醒一次的线程；它只在卡住才写文件，所以不会再掩盖问题。
下一步若要连"卡几秒"也去掉，需要把 socket 写移出 transport 锁（挂住的线程不再挡住 IPC）。

**写已经移出 transport 锁（2026-09-19）**：核心在锁内只把整帧 `memcpy` 进该连接自己的
`slot->tx`（4KB），锁一放开就 flush——`NET_SEND` / `NET_WAVEFORM` 返回前、每次 tick poll 之后、
以及停服时发完 `close` 帧之后。`stop` 会先丢掉里面还积压的波形数据，只把 `close` 帧（几字节）
发出去，所以"停止服务时 App 会跟着断开"这条行为保持不变；网络栈再卡住时，被挂住的只是正在
flush 的那个线程，IPC（NRO）继续工作。

### 反复启停后服务端起不来（0x1759 / 0x559 / 0xD401，血泪教训）

**症状**：在 socket 页连按 `A`，服务端启停若干次之后就再也起不来——NRO 与主机都不卡死，
只是每一次启动都失败，必须重启主机才恢复。日志是这样两行：

    listening on port 9999
    accept thread failed rc=0x00000559

同一个 bug 有两个签名：堆栈版本在**十次左右**之后报 `0x00000559`，把线程栈改成静态
`.bss` 之后**第二次**就报 `0x0000D401`。`listen` 永远成功、只有建线程失败，说明端口与
协议栈都没问题。

**错误码**：

| 码 | 含义 | 出现位置 |
| --- | --- | --- |
| `0x00001759` | `MAKERESULT(Module_Libnx, LibnxError_BadInput)`：`threadClose()` 拒绝回收一个仍挂在它线程链表里的 `Thread`，什么都不释放 | `thread close (<名字>) rc=0x00001759` |
| `0x00000559` | `LibnxError_OutOfMemory`：`threadCreate()` 里 `aligned_alloc(0x1000, 栈 + TLS + reent)` 失败 | 线程栈交给那个 512KB 堆分配时 |
| `0x0000D401` | `KERNELRESULT(InvalidMemoryState)`：`threadCreate()` 映射栈镜像失败（上一轮的镜像还在） | 线程栈是静态 `.bss` 时 |

**根因**：停止路径**先复制 `Thread` 结构、再等它退出、再对副本 `threadClose()`**。libnx 的
`threadClose()` 在 `Thread.tls_array != 0` 时直接返回 `0x1759` 并且什么都不释放——那个字段由
`_EntryWrap` 在线程启动时写入、由 `threadExit` 在退出时清除，而清理只发生在 **live**
结构上；副本里带的是复制那一刻（线程还在跑）的值。于是每一轮启停都留下该线程的栈、栈镜像
映射与句柄：栈从堆里要的版本十轮左右吃光 `INNER_HEAP_SIZE`（512KB，见
`sysmodule/source/main.c`）→ `0x559`；栈改成静态 `.bss` 之后，同一块页的镜像没被解除，下一
次 `threadCreate()` 直接 `0xD401`。同一类"复制再关闭"的写法还在 `netStartClient()` 复用连接
槽和 `ble_poc.c` 的 worker 上（它们通常在 join 之前线程已经自己退出，所以没暴露出来）。

**修法**：`threadWaitForExit()`/`threadClose()` 一律作用于 **live** `Thread`
（`net_socket.c` 的 `netJoinThread()` 及其三处调用点、`ble_poc.c` 一处），并且检查、记录
`Result`：失败会写 `thread close (<名字>) rc=0x… (the thread's stack was not released)`。
这条规则写在 `sysmodule/AGENTS.md` 的"线程与栈"一节。

**加固（不是根因修复）**：线程栈仍然用静态 `.bss`（`NET_THREAD_STACK_TOTAL = 0x5000`，
页对齐），建线程不再经过那个 512KB 堆——`ble_poc.c` 的 worker 本来就是这个写法。

**怎么读探针**：每次启停各有一行（下例是实机第一次的数字）：

    heap start: used=42k free=5k arena=47k (delta +0k)
    heap stop: used=42k free=5k arena=47k (delta +0k)

`delta` 是相对上一次*同一相位*的差值，`arena` 是已经向进程要下来的堆总量。反复启停时
`used` 应当基本不动（`heap start` 的 delta 一直是 0）。若同时出现
`thread close (…) rc=0x1759`，说明线程没被回收、下一次启动很可能就失败——这两行要一起读。
另外 `threadCreate()` 失败发生在探针之前，所以"`listening` 之后直接失败、没有 `heap start`
行"本身也是一种签名。

### 日志文件（SD 卡）

排查实机问题全靠这两个文件：

| 文件 | 写入方 | 内容 |
| --- | --- | --- |
| `sdmc:/switch/DGLAB-NX/logs/dglab-net.log` | NRO | `NET_LOG` 的增量副本（服务端日志的实际落盘处） |
| `sdmc:/switch/DGLAB-NX/logs/dglab-sys.log` | sysmodule | 服务端自己的日志副本（NRO 挂掉时仍有记录），**开头一行就是构建标识** |

`dglab-sys.log` 第一行形如 `server start, dglab 645f698-dirty`，是 sysmodule 自己在
`dglabNetSocketStart()` 里写的：`git describe --always --dirty` 的结果，构建时由 Makefile
注入（`sysmodule/include/dglab/build.h`）。装的是哪一版一看就知道——这一条是 2026-09-16
那次"旧二进制看起来像 bug"的教训（见上一节）。

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

### 补流：为什么需要队列

实机确认 App **播完就停**（不是循环），所以一段 `pulse` 播完设备就静音——连续波形必须
持续供给。实现（`net_server.c` 的 waveform 段）：

| 参数 | 值 | 理由 |
| --- | --- | --- |
| 每通道队列 | 128 槽位（3.2s） | 事件源短时突发不会丢素材；溢出丢最旧的并记日志 |
| 每批发送 | 最多 32 槽位（8 元素，800ms） | 远低于 86 元素上限，且能让新事件较快插进来 |
| 提前量 | 200ms | App 队列里始终留一点余量，避免断音 |

两种上传模式：`Replace`（事件：先 `clear` 再立刻播放这一段）与 `Append`（流：排队并按
上面的节奏补）。`NET_SEND` 的 `Clear` 会清空队列，等于"停止播放"。

**这一段还没有实机验证**：NRO 的 `ZL`/`ZR` 现在就走这条路径（一次上传 48 槽位 ≈ 1.2 秒，
Replace 模式 + 设置该通道强度），所以按一下 `ZL` 或 `ZR` 就能验证"分批补流"是否连续。

两点限制要记住：

1. App 是单向的，不上报强度，所以强度只能"本地记账"（我们设多少就认为是多少）；
2. App 自己的强度上限会静默裁剪我们的请求，这个值我们看不到，需要用户自己在 App 里设好。

### 测试按键

郊狼的输出是**两个值相乘**：通道强度（0~200）与波形里的强度（0~100）。只设强度、没有
波形时设备完全没反应；只在设备上有一段默认波形、强度为 0 时同样没反应。

实机第一次测试"没感觉"的原因一度被记成量程太小（当时 NRO 发的确实是强度 10/200 +
波形强度 10/100，满量程的 0.5%）。实机定位后确认真正的原因是消息路由字段写反：App
会记下我们发的消息却完全不执行（见"消息外壳"一节），把 `clientId`/`targetId` 对调后
立刻就有输出，与量程无关。测试按钮现在的行为是：

| 按键 | 发送 |
| --- | --- |
| `ZL` | 通道 A：测试波形（波形强度固定 100）**并**设置 A 的强度 |
| `ZR` | 通道 B：同上，走 B |
| `X` | `clear-A` + `clear-B` |
| `↑`/`↓` | 通道 A 强度 0~100，步进 1，**不循环**（0 再减还是 0，100 再加还是 100）；按住约 0.4 秒后才开始连发，之后每 0.1 秒一步 |
| `→`/`←` | 通道 B 强度，同上 |

两个通道各有一个强度，默认都是 0，**改一下就立刻发给 App**，没有单独的"发送"按键；
两个值在界面的 `channel A` / `channel B` 行上各显示一个（`A n/100`）。这两个值是我们设的
通道强度。

**强度用的是设备的原始值，和 App 里显示的强度是同一个数字，不做百分比换算**；官方
文档说明超过 100 只适用于特殊情况，所以测试上限就到 100。

如果按了 `ZL`/`ZR` 仍无输出，要按顺序确认：那一路的强度是不是还是 0（默认就是 0）、
App 上显示的强度有没有跟着变（说明指令被解析）、App 的强度上限是不是 0（会把我们的
请求夹到 0）、以及设备本身是否连着 App。

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

### 实机验证结论（2026-09-15，DG-LAB App 3.0 + Coyote 3.0）

真机上跑通：NRO 显示二维码 → App 扫码绑定 → NRO 用真实强度与波形控制设备，设备有输出。

1. **指令信封的路由字段**：`clientId` 是**发件人**、`targetId` 是**收件人**（写反时 App
   会记录消息却完全不执行，详见"消息外壳"一节）；
2. **心跳**：每 30 秒一条、绑定后立刻补发一条的写法，App 接受并保持会话；`message` 用
   `"200"`，App 不回应心跳；
3. **pulse 的 JSON 转义**正确，App 能播放我们排队的波形；
4. **App 3.0 是单向的**：不向服务端发送任何东西，`NET_STATUS` 里 App 上报的强度/上限一直
   是 0；
5. **测试按键的强度语义**：设备输出 = 通道强度(0~200) × 波形强度(0~100)，`ZL` 送满强度
   波形并设置该通道强度，所以屏幕上的数字就是实际强度，上限 100（原始值）。

仍未解决或未验证：服务端运行时休眠会卡死（兜底见"睡眠与唤醒"，其中 55 秒空闲自停这条
本身还没实机确认）；V4 未实现（`?tid=` 形式在解析里被接受，便于排查）；App 的强度上限
不会同步（是 App 设计，不是本项目的缺陷）。原文见 `docs/history.md`。

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
4. 用测试按键确认：`ZL`/`ZR` 各测一个通道（1.2 秒测试波形 + 该通道强度）、
   `↑`/`↓`/`→`/`←` 调强度并立刻下发、`B` 清空；
5. 观察 `NET_LOG` 里的收发记录。

NRO 会把 `NET_LOG` 同步写到 `sdmc:/switch/DGLAB-NX/logs/dglab-net.log`（打不开时退到
`sdmc:/dglab-net.log`），实机测试后直接把这个文件发回来即可。
