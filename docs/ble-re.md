# BLE 固件研究（HOS 22.5.0）

## 当前状态

结论截至 2026-09-27：**补丁＋驱动级开栈＋btm 连接可写 BF/B0，设备有输出；
回读归 btm，服务层不向第三方暴露，按开环设计。**
正式入口、实机证据和待验流程见 [BLE 验证](ble-poc.md)；协议与设备绑定见
[Coyote 文档](dglab-protocol.md)。本页保留代码不能说明的固件依据与补丁复核信息。

所有地址限定于下述固件构建，不能套用其它 HOS。服务 API 封闭来自静态分析，
不是对所有未来固件的断言；连接、写入和无回包现象有实机证据，见 BLE 验证。

## 当前总览（2026-09-22，历史入口）

源码注释仍引用这一早期收束节点；当时的“直连搁置”已被 09-24/25 的补丁连接和输出取代。
现行状态见本文开头与 [BLE 验证](ble-poc.md#当前状态)，不要照旧探针方案操作。

## 素材与复核

- 固件版本由系统版本 title `0100000000000809` 的 RomFS 核对为
  `NintendoSDK Firmware for NX 22.5.0-1.0`，实机环境 AMS 1.11.2 / Switch 1。
- Bluetooth Program title 是 `010000000000000b`，NCA
  `ca66270be492a16bab1d779645965bc8.nca`，NPDM 名 `bluetooth.autog`。
  NCA SDK 版本 22.2.0.0 与安装固件 22.5.0-1.0 不冲突。
- 模块 build ID `c91c6fc8aa4c39222d6ccfe0fff468543105e59b`；
  btm build ID 前缀 `5a2aa468f272e49ebf0fab8b379cc5b32c1a7409` 与实机崩溃记录一致。
- 曾把 title `010000000000000c`（bcat）误命名 bluetooth；必须按 NCA/NPDM/module ID
  确认素材，不能信文件名。
- 读取/反编译工具见 [工具 README](../tools/ble-re/README.md)。nso2elf 用自建 NSO/ELF
  往返自测 PASS 后才用于固件；密钥、固件、提取物不入库。

## 判定（2026-09-21 夜，更正）

**请求形状没有整体漂移。** btdrv `0x159a28` 是服务对象虚表，不是命令表；
命令分派为 CMIF SFCI → `FUN_0001d4b0` → 字节表 `0x11884e`/分支表 → 各命令 case。
base btm 分派在 `0x1bc50`，服务虚表 `0x76580` / `0x765f8` 同样不能按下标当命令号。
case 解 IDL 参数后显式调用虚方法，核对 API 要读 case。

已核对 btdrv 扫描/GATT 与 base btm 的 13 条 BLE 命令，其请求/出参形状匹配 libnx。
cmd 62 注册是 0x14 字节，cmd 23 是地址＋u16 共 8 字节，cmd 65 是 16 字节；
扫描开关 55/56 无入参，过滤器 57/58 为 0x3E 字节，广播数据 53 为 0xCC。
cmd 54 已确认是广播参数（地址＋min/max），与客户端认领无关。

早期“0x40 注册成功”“类型整体重生成”“无需补丁”均已撤回：
注册成功事件来自 InitializeBle 的自动注册，显式注册仍失败；补丁需求后来由实机确认。
这不否定请求形状匹配，失败点是状态/所有权，不是靠移动命令号可解决。
尚未逐条核对全部命令在 20.0.0+ 的增删，不能外推为所有 Bluetooth API 都不变。

### btm:u 与 ARUID

btm:u 是 applet 接口：cmd 18 case `0x27b20` 比较请求 ARUID 与调用者 ARUID，
0 或相等才继续，否则 `0x60A`。sysmodule 的 btmu 封装曾扫描 rc=0 但事件/结果恒 0；
填 NRO 的真实 ARUID 被框架拒绝。不能借用他人身份。
base btm 的 BleConnect(35) 请求不带 ARUID，通过 RegisterAppletResourceUserId(57) 登记。

### GetChannelMap

cmd 40 在 HOS 22.5.0 两次独立实机使会话关闭，其后调用均 `0xF601`；
libnx 的 0x88 字节 MapAlias Out FixedSize 与改用指针缓冲均复现。
静态 case 形状匹配不代表运行正常，原因未定，不能据此断言 ABI 漂移或固件缺陷。
[上游草稿](../tools/ble-re/upstream.md) 保留断言边界。

## 一个开机周期只走一条 BLE 路径

2026-09-22 实机混用使 btm 崩溃并连累 hid；2026-09-25 对照中，btm 会话碰 btdrv
开栈/注册的四轮失败，与不碰的两轮成功形成依据。
当前合法顺序是一次启动序列内“驱动级开栈完成 → btm 会话”，不能任意交替实验。

- InitializeBle(46) 即使栈已起仍会注册客户端；EnableBle(47)/DisableBle(48) 改全局栈，
  不是当前客户端私有开关。
- btm 会话不能重复开栈/注册，不能冒用 ARUID，不留下无法完成的 connect。
- **0x668F 表示已有请求在飞**。投递点 `FUN_00033890` 的在飞计数拒绝新工作；
  worker `FUN_00033d70` 收到非预期状态，经 `0x37f40→0x37e30→0x37d50→0x39150`
  走 svcBreak，崩溃 PC `0x475ec` 与报告一致。看到它必须停手重启，不能继续改 BLE 全局状态。
- 每轮完成后重启再连接/实验。Socket 路径只在手机上用 BLE，不受这一限制。

## 扫描与 GATT

驱动级扫描已稳定发现设备。2026-09-21 手机曾报告广播 UUID 由 0x1812 变为 0x180C；
2026-09-25 Switch 定长 AD 实测只有 flags、厂商数据（公司号 0x000A）、本地名 47L121000，
**无服务 UUID**。两次观察来源/时间不同，不能据早期手机显示要求 UUID 过滤命中。
定长 `BtdrvBleAdvertisement` 数组不能当紧凑 TLV 链；扫描事件身份/记录判据查探针与日志。

实机七个 GATT 服务中协议服务 0x180C：写 0x150A handle 19、通知 0x150B handle 16；
另有电量服务 0x180A、DFU 0xFE59。这些 handle 是该轮结果，不是跨设备固定 API。
特性 properties 在固件结构 +0x20（handle 在 +0x18）：150B=0x10、150A=0x04；
libnx properties 读 0 是字段偏移问题，不能据此更换通知目标或写类型。

btm 连接按地址建立，不依赖其扫描先命中。当前按 bt 连接事件 status=0、conn_id 与地址确认；
GetConnectionState 在此配置曾恒 total=0，错误据此重连会主动断掉有效连接。
GATT 发现需等事件后重试，不能连接刚建立就判 total=0 为无服务。

## 连接所有权在服务层是封的

2026-09-25 静态分析收束：第三方无法通过现有服务命令使有效连接归自己的 client_if。

1. btdrv ConnectGattServer(65) 按地址查设备槽位表并回写 client_if，不能靠调用者字段认领。
   `FUN_00008150→0x7b60`，表共十槽、步长 0x332，字段为有效标志、地址、client_if。
   不存在/未分配时 GATT 包装返回 Bluetooth/0x1806。
2. 唯一写表链为内部消息 `0x18/kind=1 → FUN_0002e400 → 0x79e0/0x80b0`；
   起点 `FUN_00008820` 无直接分支、adrp/add 或指针引用，服务层不可达。
3. btm BleConnect(35) 走自己的管理器（虚表 +0x130），不经过 btdrv 62/65。
   客户端跳板表 `0x8ce18` 的 113 项覆盖 66–102、128–155、256–258，不包含 23/62/65，
   这些跳板也无数据表引用。

能连上的 btm 路径私有，通知/读应答按 client_if 投递给 btm；第三方 bt 会话只见连接等广播事件。
写入无需回投递，因而能写却收不到 B1/电量/读值。订阅、CCCD 读、managed/其它队列探测及
拨轮实验均未获得回包；除非新固件或上游资料改变前提，不再投入同类探测，按开环处理。

配对不能绕过这层所有权。默认未绑定设备不响应已受理的 CreateBond，link_key_present=0；
有手机绑定信息时控制写入被设备忽略。项目优先不配对，配对探测关闭；设备侧约束见协议文档。

## 诊断补丁

仅适用于上述 HOS 22.5.0 的 bluetooth.autog build ID。
连接失败原始状态 0x85 经 `FUN_000195a0` 映射为事件 result=0x1A；
`FUN_000cd7f0` / `FUN_000cf6f0` 检查同一控制器层客户端激活标志，btm 的 client_if=4 为 0。
跳过检查后连接成功；**仍需先驱动级开栈**，只跑 btm 不成立。

| ELF 地址 | 原字节（内存顺序） | 新字节 | 作用 |
| --- | --- | --- | --- |
| 0xcd820 | 48000034 | d503201f | cbz 改 NOP |
| 0xcf7d4 | c9020034 | d503201f | cbz 改 NOP |

```sh
python3 tools/ble-re/make_ips.py \
  --elf /tmp/ble-re/nso-010000000000000b.elf \
  --module-id c91c6fc8aa4c39222d6ccfe0fff468543105e59b \
  --edit 0xcd820:48000034:d503201f \
  --edit 0xcf7d4:c9020034:d503201f \
  --out build/exefs_patches/DGLAB-NX-BLE/C91C6FC8AA4C39222D6CCFE0FFF468543105E59B000000000000000000000000.ips
```

脚本校验旧字节，不能对不匹配构建强打。IPS 偏移=0x100＋ELF 地址，来自 Atmosphère
`ldr_patcher.cpp` 的 NSO header 保护约定；输入字节来自解压 ELF，不是压缩 NSO 段。
将 DGLAB-NX-BLE 目录放 `SD:/atmosphere/exefs_patches/` 后弹出整盘、重启；
撤销为删除该目录/文件再重启。补丁尚未纳入发行包。

### 加载判据与风险

早期两处 NOP 试装事件没变，随后三处版加可见标记确认加载：
0x5fdb4 的 `02198052` 改 `e2031f2a`（无上下文状态 0xC8→0），
驱动级请求从 0x300C71→0 后，btm 连接成功。
**标记只是诊断，不是成功建立上下文，也不应纳入正常控制配方。**
两处激活检查与先开栈是最终可用配方；保留标记来源，避免把 rc=0 误读为连接成功。

跳过检查会带未激活槽指针继续走，字段可能为零/旧值，可能导致 Bluetooth 模块出问题。
跑完重启，异常则撤销；不能外推为其它固件或完整蓝牙共存已经安全验证。

## 错误码表

| Result | 已有结论/边界 |
| --- | --- |
| 0x29E71（Bluetooth/0x14F） | 多个路径的通用失败；注册探测涉及有限槽位，不可单凭此码判断原因 |
| 0x2A671（Bluetooth/0x153） | GATT 读后紧接写失败，等待 300ms 后消失；忙拒语义未证实 |
| 0x300C71（Bluetooth/0x1806） | 连接路径可由无上下文/槽位产生，也有长度等其它来源，需结合调用路径 |
| 0xD671（Bluetooth/0x6B） | GetPairedDeviceInfo 对不存在记录的地址；有空记录时可 rc=0，但无 link key |
| 0x5568F（Btm/0x2AB） | btmu/sysmodule 的无效 ARUID 拒绝 |
| 0x60A（Sf/3） | 请求身份不匹配被框架拒绝 |
| 0x668F（Btm/0x33） | 工作请求在飞，停手重启 |
| 0xF601（Kernel/123） | 会话关闭，cmd 40 的后续失败不是每个命令都坏 |
| 0xE401（Kernel/114） | 无效句柄；也可能漏 btmInitialize，先查初始化日志 |
| 0x6359（libnx Timeout） | 探针重试耗尽，不是固件 Result |

静态推断与实机事实必须分别表述；旧 vtable/ABI 误读及逐轮日志从 Git 历史查阅。
