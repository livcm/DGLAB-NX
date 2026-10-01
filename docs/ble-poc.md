# BLE 直连与探针验证

本文负责操作、实机证据和验收；固件依据与补丁见 [ble-re.md](ble-re.md)，
协议语义见 [Coyote V3](dglab-protocol.md)，正式 API 见 [IPC](ipc.md)。

## 当前状态

适用已验证环境：Switch 1、HOS 22.5.0、AMS 1.11.2、郊狼 3.0。
正式入口是 NRO `bluetooth (direct)` 与 BLE_*：先驱动级探针打开 BLE 栈，再启动 btm 会话。
扫描、连接、GATT、BF/B0 流式写入已实机跑通；依赖 exefs 补丁，B1/电量/读应答不可得，按开环设计。

| 日期 | 关键证据 | 能说明什么 |
| --- | --- | --- |
| 2026-09-24 | 补丁＋先开栈后，bt 连接事件 status=0、有效 conn_id，后续读取七个 GATT 服务 | 连接与服务发现成功，不等于通知可用 |
| 2026-09-25 02:17 | 整轮 bt 仅连接/connection_update，managed 队列同样；CCCD 读 rc=0 但值未返回 | 无设备回读，不能靠 rc=0 推断订阅落地 |
| 2026-09-25 15:58 | 反应测试：上限 20、A 请求强度 5、短波形，100 次写全 rc=0，用户感到输出，包字节一致 | 写入确实到设备，超出了“请求被受理”的证据 |
| 2026-09-26 | 正式入口 writes=172 全 rc=0，但 BF 上限 0 且波形全零 | 会话写路径跑通；不能把无输出归因于连接失败 |
| 2026-09-27 | 构建 v0.3.0-104-gb5e28c3-dirty，BF A/B=100，B0 四槽全填，writes=318/notify=0/b1=0，用户确认灯闪 | 正式会话默认波形有效；强度只在 0↔1，未验可感档位 |

2026-09-27 会话跨页存活、前端退出 STOP、60 分钟看门狗已在代码中；
**玩法页驱动 BLE、提示和退出收尾仍待实机确认**。补丁尚未纳入发布产物。

## 安全与前提

- 设备必须无手机绑定信息：关闭 App「设备绑定」或重置设备。有绑定时 Switch 能连，写入却被忽略。
- 一个开机周期只走一条 BLE 路径。合法流程是同一启动序列内“驱动级开栈 → btm 会话”；
  不是任意混用探针。再次会话/另一实验前重启。
- btm 会话不再 InitializeBle/EnableBle/RegisterGattClient，不冒用 applet ARUID，
  不留无法完成的连接。看到 `0x668F` 停手重启；依据见固件研究。
- 反应测试会实际输出，先确认设备和电极，从低强度开始。上限与强度是独立 A/B 对，
  有效波形决定是否在播放；灯闪不能证明有可感幅度。
- BLE 状态只有请求记账，不能显示为真实强度/电量，也不做基于设备当前值的相对增减。

## 安装与操作

1. `make -C sysmodule package`（或根 make），安装 `build/<TITLE_ID>/` 与
   `build/DGLAB-NX/`（包含 lang）。补丁按 [诊断补丁](ble-re.md#诊断补丁) 单独安装。
2. 写完卡用 `diskutil eject /dev/diskN`，确认整盘设备消失再拔，重启，核对构建标识。
3. 在干净开机进入 `bluetooth (direct)`，按 A，等待驱动级探针结束并建立会话。
   地址由扫描自动保存至 `config/dglab-ble-address.txt`，客户端无需硬编码设备地址。
4. ↑↓ 调 A、→← 调 B 强度；Y 日志；B 只返回，会话继续运行；蓝牙页 X 停止，退出 NRO 也停止。
   上限来自高级参数，返回蓝牙页会同步改变的 A/B 上限，不通过重连修改。
5. 进入体感/触屏使用同一会话；退出前保留日志，再停止并退出 NRO。

诊断另选 `BLE PoC console`，用 + 退出；一次启动只选一条路线。
按一次 StickR 运行“驱动级探针 → btm 探针”两段序列，日志出现 `one-key probe: step 1/2`、
`driver-level probe done` 后，观察 connected/GATT/write 与最后 writes/notify/b1。
具体当前按键与探针参数以 `nro/source/ble_poc_view.c` 及控制台提示为准，旧诊断动作已删除。

## 实机判读与排查

| 表现 | 先检查 |
| --- | --- |
| 连接 result=0x1A | 匹配的补丁是否加载、驱动级开栈是否完成 |
| BleConnect 返回 0xE401 | 是否成功 btmInitialize，或会话已关闭 |
| 无输出但写 rc=0 | 设备绑定、对应上限、请求强度、有效波形；不要只查写包数 |
| GetConnectionState total=0 | 用 bt 的连接事件确认；该查询曾把有效连接误判为失败，引发主动断连 reason=0x16 |
| 写在读后回 0x2A671 | 已观测读后立即写被拒；探针读后等待 300ms。忙拒语义仍未证实 |
| notify/b1 恒为 0 | 当前服务层限制，不继续改订阅/配对来“试通” |

默认 ATT MTU 23 足够承载 V3 的 20 字节包，未做 MTU 协商；无自动重连，重试先重启。
NET_* 在 BLE 下并非全部等价：Clear/TestPulse 当前被忽略，波形接收采用最新批次替换，
玩法页 X 不能作为 BLE 停止键；实际停止回蓝牙页按 X 或退出 NRO。
服务发现需要等事件并重试，连接刚建立时查 total=0 不代表设备没有服务。
广播 AD 的扫描结论与 GATT UUID 分开，见 [扫描与 GATT](ble-re.md#扫描与-gatt)。
旧 btdev/btm:u 路线曾返回成功但无扫描事件，已不作为正式连接路径；驱动级直连被槽位/所有权阻挡。

### 日志

会话/探针日志在 sysmodule 内存 PoC ring；蓝牙页由 NRO 抽到
`SD:/switch/DGLAB-NX/logs/dglab-net.log`，PoC console 抽到 `dglab-ble-poc.log`。
Socket 与 BLE 屏上日志各用自己的环，磁盘镜像可合写 net 文件。
必须同次开机、关机前读出，关机或拔卡后 ring 消失。sysmodule 的 socket 文件日志不能替代 BLE ring。

## 验证入口与待验

主机入口：`make -C tests/protocol`、`make -C tests/ipc`、`make -C tests/stack`；
构建/安装流程见 [开发文档](development.md)。测试只说明代码与布局，不证明设备输出或蓝牙共存。

下一轮：在上限 >0 下逐步验证可感档位（已有待验建议 A 20~30），进入体感/触屏确认会话持续输出，
检查菜单/连接提示、蓝牙页 X 停、退出 NRO 停，以及运行时上限修改。
长期兼容验收还包括控制器连接/配对、蓝牙音频、断连重连和退出后的系统稳定性；
不能仅凭 GATT/写入成功宣称完整兼容。
