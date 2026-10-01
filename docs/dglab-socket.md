# DG-LAB Socket 协议

Switch 是 WebSocket 服务端兼控制端，手机 DG-LAB App 扫码连入，再通过手机 BLE 驱动设备。
本地模式只需同一局域网；不主动外连，也不需要中继。Socket V3 已实机输出，V4 暂搁置。
[BLE 直连](ble-poc.md) 是另一条传输路径，依赖补丁且只能开环。

## 参考与版本

已有核对来源：[官方服务端](https://github.com/dungeonlab-open/dglab-websocket-server)
（`v3-server.ts`、`v4-server.ts`，文档记录为 2026-07 更新）、官方 `dungeonlab-open/dglab-kit`、
[PyDGLab-WS](https://github.com/Ljzd-PRO/PyDGLab-WS)。
Socket V3/V4 与蓝牙 Coyote V2/V3 不对应。

| 协议 | 默认端口 | 绑定地址 | 状态 |
| --- | --- | --- | --- |
| V3 | 9999 | `ws://host:port/<控制端 clientId>` | 本项目实现 |
| V4 | 9998 | `ws://host:port?tid=<控制端 clientId>` | 只保留已核对资料，外壳/二维码未实现 |

当前解析接受路径 ID 与 `?tid=`，不代表实现了 V4。

## V3 与 App 交互

外壳是 `type/clientId/targetId/message`，键名为驼峰。
**clientId 是发件人，targetId 是收件人**；控制端→App 的 msg/heartbeat 必须按此方向。
2026-09-15 实机证明写反会出现“绑定成功、App 有日志但不执行”。

连接后分配 App UUID 并发送 bind；Switch 自身的控制端 UUID 放入二维码，两者配对后才发送控制。
本项目合并了参考中独立服务端/控制端的角色，不需要真的创建第二个控制端连接。
绑定后立刻发心跳，之后每 30 秒发送，message 为 "200"；实机 App 接受，但不回应。

| 内容 | 语义 |
| --- | --- |
| `strength-<1或2>+<mode>+<value>` | 数字通道 A=1/B=2；mode 0 减、1 加、2 绝对设置；强度是原始值，非百分比 |
| `pulse-<A或B>:["<16位十六进制>", ...]` | 字母通道；每元素四个频率与四个波形值，复用 Coyote V3 波形编码 |
| `clear-<1或2>` | 清空对应 App 波形队列 |
| App 的 strength / feedback | 强度/上限与反馈按钮上报，解析支持；实测 App 3.0 不发送 |

范围、消息长度和错误码的完整定义见 `sysmodule/source/net/dglab_socket.c` 及对应 header，
主机测试见 `tests/net/`。
V3 二维码内容为：

```text
https://www.dungeon-lab.com/app-download.php#DGLAB-SOCKET#<ws uri>/<clientId>
```

ws uri 尾部不带 `/`。实机 App 不能手输地址，必须扫码。
App 3.0 的日志 `tx` 表示收到服务端消息，`rx` 表示自身发送，不能按通常命名反读。

### 排查连接问题看什么

- `accept from`：TCP 已到 Switch；`websocket from ... target ...`：已进入 WebSocket 握手。
- `app ... bound`：绑定完成；握手失败要区分普通 HTTP 请求和 WebSocket。
- `tx` 后设备无输出：先查路由字段、App/设备连接、通道强度和 App 上限。
- 服务允许缺 ID 或不匹配 ID 的路径继续配对并记录 target，这是实机兼容选择；
  不能未经实机确认直接收紧。重复绑定会拒绝，未知 msg 只记录后忽略。
- 客户端有 Sec-WebSocket-Protocol 时回选第一个 token，没有则不回。
- 同时最多两条 TCP（一个绑定、一个重连过渡），已连接 App 无独立空闲超时，靠 TCP 断开/shutdown。
  停服发送 close 再关闭；实机反馈仅 shutdown 时 App 不会自动显示断开。

## 波形流

通道强度由用户设，事件源生成 25ms 槽位的频率与波形值。App 一段 pulse **播完就停**，
持续玩法必须由 sysmodule 排队补流。Replace 用于独立事件，Append 用于连续流。
队列/批次/提前量查 `net_server.c`，IPC 使用语义见 [IPC 文档](ipc.md#波形与通道强度)。

实机 App 3.0 单向通信，不上报强度/上限，客户端只能记账，App 自身上限会静默裁剪请求。
不能把“没有上报”当成解析错误，也不能把本地请求当成设备真实值。

## 睡眠与唤醒

持有 socket 跨整机睡眠会卡死。PSC 仅尝试 WlanSockets，实机被拒 `0x108A`，
所以不能依赖睡眠通知。试自定义 ID 200/201 曾冻住整机，禁止继续试其它 ID；
没有可靠新机制前不在 sysmodule 做 PSC 实验。

兜底：服务端不自启；无客户端 55 秒后请求停服；NRO 运行时抑制自动休眠，停服/退出时只恢复
自己接管的标志。空闲计时从最后客户端离开开始；tick 只置停止标志，下一次 IPC 执行收尾，
不能由服务线程 join 自己。不能假定无 IPC 时一定已在 55 秒收完。

2026-09-18 实机确认相册 applet 与 title override 的自动休眠抑制有效。
**手动休眠仍会卡死；NRO 退出后抑制失效，服务端仍运行时自动休眠也有风险。**
不改用户全局 SleepSettings：它掉电保持、权限未验，且挡不住手动休眠。
未来需可靠的睡眠前信号；Overlay 开发时需处理 NRO 退出后的场景。

## 平台实现与故障依据

平台传输见 `sysmodule/source/transport/net_socket.c`；共用核心见 `source/net/`。

### 开机路径必须最小

boot2 只注册服务、初始化锁和内存会话核心。socket、线程、文件访问及既有 PSC 尝试延到用户开服。
未挂 SD 就 mkdir 曾在开机 logo Data Abort；现在文件访问先显式挂载，不代表 boot2 早期可写盘。
nifm 查地址后即退出，避免持有会话；缓存减少轮询开销。

### 栈上不要放 KB 级缓冲区

主线程栈曾被两个 1950 字节缓冲叠加调用链压爆，表现为进程消失、IPC 无响应且无 Result。
先核对安装构建标识，再查最后日志和崩溃报告。缓冲与线程规则见
[sysmodule AGENTS](../sysmodule/AGENTS.md#线程与栈)，检查入口 `make -C tests/stack`。

### socket 写路径

锁顺序固定 transport→frame，帧内不反向记日志。transport 锁内仅复制到连接发送队列，
释放后 flush；波形由 tick 补流。实际 send 不持有 transport 锁，避免卡住时阻塞其它 IPC。
不用 MSG_DONTWAIT：2026-09-19 实机请求曾永不返回；采用 SO_SNDTIMEO，部分写/错误断开，
但实机仍发现超时不能完全防住 bsd:u 卡住，因此保留阶段标记与看门狗 shutdown 兜底。
NET_SEND/NET_WAVEFORM 包装仍在调用返回前 flush 连接已有待发内容；
不能宣称所有 IPC 写都已异步化。波形上传本身入队、补流由 tick 做，与此并不矛盾。

### 诊断 socket 写卡死

热路径 SD 日志会改变时序、掩盖故障，内存环探针也曾影响复现。
当前用纯 store 阶段标记，独立看门狗只在卡住时写 `dglab-stall.log` 并关闭相关 fd。
2026-09-19 记录 `send begin` 停 3282ms；加兜底后可自行恢复。
2026-09-27 又记录 SD 日志偶发停顿 3195~3393ms，自行恢复期间部分 IPC 会停顿。
日志文件被占用可作进程仍存活的线索，不能单凭打不开文件判死锁。

### 反复启停后服务端起不来

join 前 Thread 副本保留旧 tls_array，threadClose 回 `0x1759` 且不释放。
每轮泄漏导致后续建线程失败（曾见堆栈 `0x559`、静态栈 `0xD401`）。
用 live Thread 等待/关闭并检查两个 Result；静态栈也不能省略回收。

### 日志文件

`SD:/switch/DGLAB-NX/logs/`：NRO 将 NET_LOG 增量写 `dglab-net.log`，sysmodule 开服后
显式挂 SD 写 `dglab-sys.log`，其启动/开服行含构建标识；看门狗写 `dglab-stall.log`。
BLE 会话另在内存 PoC ring，由 NRO 读出，详见 BLE 验证。

## V4 资料（未实现）

官方已核对外壳：服务端 hello 下发 clientId；控制端 message 用 clientId 指定被控方，
指令放 data；转发给被控方为 message/data；另有 heartbeat、ping/pong（ts）、error、
controller_disconnected 与 client_disconnected。指令内容与 V3 一致，绑定用 `?tid=`。

二维码形状：

```text
https://dungeon-lab.cn/s/?v=1&action=socket&url=<编码后的 ws://host:port?tid=<clientId>>
```

V4 App 向下兼容 V3，待官方 beta 稳定、App 版本确认后再实施；不按第一条消息自动猜版本。

## 验证状态与入口

2026-09-15（DG-LAB App 3.0 + Coyote 3.0）确认扫码、绑定、路由方向、心跳、pulse 转义与
真实输出。后续体感输出已确认；分批补流的连续性、55 秒空闲自停仍需专项实机确认。

主机：`make -C tests/net`。实机先重装后台并核对构建标识，再同网扫码，确认 Paired/peer_id，
从低强度分别测 ZL/ZR、十字键调强度、X 清空、A 停服，检查 App 断开与日志。
休眠风险已知，不把休眠当普通验收步骤。
