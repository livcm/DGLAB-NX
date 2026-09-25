# BLE 固件只读逆向（HOS 22.5.0）

本文记录按根 `AGENTS.md` §15 的要求，对"Switch 后台直连 BLE 外设"这条路线做的
**只读**固件逆向：目标是把 `docs/ble-poc.md` 留下的失败点归类成
**(A) 绑定/结构漂移**（改我们自己代码即可）、**(B) 固件按调用者身份/白名单封死**
（需要 exefs patch 或放弃）、还是 **(C) 蓝牙栈里没有通用 central 实现**（放弃）。

**当前状态看「当前状态（2026-09-25）」一节**：实机已能扫描、连接、读 GATT 表并写入
BF/B0，写入已由实机输出证明到达设备（2026-09-25），**但必须安装一个 exefs 补丁**（固件的
"客户端未激活"闸门），且设备侧一条回包都没有（通知与读应答全哑、B1 未验证）。
下面从「判定（2026-09-21 夜，更正）」起是过程记录：那一轮写的"不需要固件
补丁"**已被 2026-09-24 的实机结果推翻**，但"libnx 的请求形状与固件一致"这条更正仍然成立
——卡点在语义/状态层。第一轮那几张表保留在文末，均已标注取代。

## 素材与工具链（可复现）

素材是本机 Ryujinx 安装的固件（`bis/system/Contents/registered`，234 个 NCA）与
`system/prod.keys`。固件版本用系统版本 title 的 RomFS 直接核对：

    0100000000000809 (Data) → RomFS 里的 file 含 "NintendoSDK Firmware for NX 22.5.0-1.0"

与 11 次实机的环境（HOS 22.5.0 / AMS 1.11.2）一致。工具与命令见
`tools/ble-re/README.md`；其中 `nso2elf.py` 用仓库自己构建的
`sysmodule/DGLAB-NX-Core.nso` / `.elf` 做过往返自测（PASS，段内容逐字节一致）之后才用于
固件模块。密钥、固件、解包产物都只落在 `/tmp`，不入库。

**素材身份核对（2026-09-21 夜，重做）**：`/tmp` 里那份 `bluetooth.elf`（以及
`exefs/bluetooth/`）其实是从 title `010000000000000c`（NPDM Title Name = `bcat`）解出来的，
文件名起错了。真正要分析的是 `010000000000000b` 的 Program NCA
`ca66270be492a16bab1d779645965bc8.nca`，它的 NPDM Title Name 是 **`bluetooth.autog`**，
解出的 NSO 与 `nso-010000000000000b.elf` 逐字节一致（`nso2elf.py` 往返可复现）：

    hactool -k <prod.keys> -t nca -i ncas/ca66270be492a16bab1d779645965bc8.nca | grep "Title Name"
    → Title Name: bluetooth.autog

本文所有地址都来自这份 ELF；`bt` 服务（同名模块内的第二个服务）以客户端身份打开 `btdrv`。
固件的 NCA 头写的 SDK 版本是 `22.2.0.0`，和实机固件版本 `22.5.0-1.0` 不冲突（同一次安装）。

## 判定（2026-09-21 夜，更正）

**不需要固件补丁（这一条不变）；但"libnx 的请求形状过时、要按固件形状重发"是错的。**

第一轮静态分析把 `btdrv` **服务对象的虚表**（`0x159a28`，137 项）当成了"命令表"，于是
"表下标 = 命令号"这个前提从头就错了。由此推出的"注册要 0x40 字节、`TriggerConnection` 要
0x2BE 字节、类型被重新生成过"全部作废。正确的模型是：

- `0x159a28` 是**服务对象的 C++ 虚表**：初始化代码在 `0x1b5c4`–`0x1b5d4` 把
  `0x159a18 + 0x10` 写进对象（`bt` 服务同样处理，写的是 `0x159e70 + 0x10 = 0x159e80`）。
  表里每一项是"某个虚方法的 this 调整 thunk"，不是命令。
- 命令分派在 **`FUN_0001d4b0`**：CMIF 头（magic `SFCI`）由 `0x1d3c8` 校验、取出
  `header.command_id`，再用 `0x11884e` 的字节表 + 分支表跳到**每个命令自己的 case**；
  case 里按 IDL 从请求缓冲解出参数，然后显式调用某个虚表偏移。所以要判断"命令要什么形状"，
  看的是这 259 个 case（命令 `0..0x102`），不是虚表下标。
- 结论：**libnx 的 btdrv 请求形状与固件一致**（下表）。之前"按 0x40 字节发就注册成功"的
  实机结论是误读：那次排到的 `result=0 / client_if=0x02` 事件是 `InitializeBle` 自己注册的
  （同一会话里这条事件本来就会反复出现 4~16 次），显式注册每次都还是
  `result=0x37 / client_if=0xFF`。

### 命令 → 请求形状（btdrv，扫描链与 GATT 这一段）

"载荷"从 `FUN_0001d4b0` 的 case 读出（`param_4` 是指向输入数据的游标，`param_4[1]` 是剩余
字节数，不够就直接返回 `0x1ce0a`）。"调用的虚方法"是虚表偏移 → 包装 thunk → 实际实现。

| 命令 | 固件要求的载荷 | 调用的虚方法 | libnx 对应 | 结论 |
| --- | --- | --- | --- | --- |
| 23 `TriggerConnection` | 8 = 6 字节地址 + u16 | +0xd8 | `btdrvTriggerConnection`（8） | 一致 |
| 40 `GetChannelMap` | 无入参，0x88 出 | +0x160 → `0x1c730` → `0x12900` | `btdrvGetChannelMap` | 一致（会话被关是另一回事） |
| 46 `InitializeBle` | 无入参，0x10 出（事件） | +0x190 → `0x1c790` → `0x12ed0` | `btdrvInitializeBle` | 一致 |
| 53 `SetBleAdvertiseData` | 0xCC | +0x1c8 → `0x1c820` → `0x134b0` | `BtdrvBleAdvertisePacketData`（0xCC） | 一致 |
| 54 | 10 = 6 字节 + u16 + u16 | +0x1d0 → `0x1c860` → `0x13540` | ？ | 待查 |
| 55 `StartBleScan` | 无 | +0x1d8 → `0x1c870` → `0x135f0`：管理器 **+0x28(1)** | `btdrvStartBleScan` | 一致 |
| 56 `StopBleScan` | 无 | +0x1e0 → `0x1c880` → `0x13680`：管理器 **+0x28(0)** | `btdrvStopBleScan` | 一致 |
| 57 添加扫描过滤条件 | 0x3E | +0x1e8 → `0x1c890` → `0x137b0` | `BtdrvBleAdvertiseFilter`（0x3E） | 一致 |
| 58 删除扫描过滤条件 | 0x3E | +0x1f0 → `0x1c8e0` → `0x13840` | 同上 | 一致 |
| 61 `EnableBleScanFilter` | 1 字节 bool | +0x208 → `0x1c950` → `0x139f0` | `bool` | 一致 |
| 62 `RegisterGattClient` | 0x14 | +0x210 → `0x1c960` → `0x13a80`（管理器 +0x70） | `BtdrvGattAttributeUuid`（0x14） | 一致 |
| 63 `UnregisterGattClient` | 1 字节 | +0x218 → `0x1c9a0` → `0x13b10` | `u8` | 一致 |
| 65（GATT 连接） | 0x10 | +0x228 → `0x1c9c0` → `0x13c30` | ？ | 待查 |
| 79 `GetBleManagedEventInfo` | 无入参 | +0x298 → `0x1cc00` | 出参形状待查 | 待查 |
| 258 `GetBleChannelMap` | 0x10 | +0x438 | ？ | 待查 |

第一轮说的"0x40 字节寄存器描述符"其实是 `0x1c8e0`（= 命令 **58** 的包装，复制 0x3E 字节
过滤器结构）和虚表下标 `0x3E` 混在一起了；`SetBleAdvertiseData` 的 0xCC 也不是"落到别的
命令号上"，它就是命令 53 自己的载荷。

### 这更正了什么、没更正什么

- 更正：**形状没有漂移**，"按固件形状重建 btdrv ABI"这份工作量不存在；libnx 的
  `btdrv*` 调用可以直接用。
- 不变：不需要 exefs patch / mitm（没有身份检查的证据；`btm:u` 那条才是 applet 专用）。
- 仍然成立、且要接着查的现象：显式注册返回 `0x37 / client_if=0xFF`；`ConnectGattServer`
  返回 `Bluetooth/0x14F`；扫描看不到结果；cmd 40 会让会话被关。这些只能从**管理器方法**
  （扫描开关 +0x28、事件/结果的分发、cmd 40 的实现）里找原因。

### 事件是怎么交出来的（2026-09-21，第二十八次实机后补）

cmd 79 `GetBleManagedEventInfo` 的完整链路：

    case 0x4f → 管理器 +0x298 → 0x1cc00 → 0x14530 → 0x2b8a0

`0x2b8a0` 就是"取事件"本身：管理器只维护**一个全局事件槽**——
`0x1af890` = 长度、`0x1af898` = `payload[0x400]`、`0x1afc98` = 事件类型（正好在 payload
之后 0x408 处）。取一次就 `memcpy(dst, payload, 长度)`、把类型写进那个 u32 出参、并把长度
清零（消费掉）。

实机（第二十八次）看到的是：扫描期间这个槽每 200ms 就有内容（10 秒 50 次调用全非空、内容
逐字节相同），但**数据落在 payload 的 +0x200**，前 0x200 字节全零，而类型出参读到 0。
也就是说返回布局和 libnx 的 `BtdrvBleEventInfo`（各事件字段都在 +0）对不上：要么类型/偏移
都不对，要么这根本不是扫描结果。下一步用 v5 探针把 +0x200 那一段打全，并在里面搜目标地址。

### 管理器虚表、事件发布点与连接失败点（2026-09-21 夜）

- **管理器虚表在 `0x159400`**：`FUN_0000d200(&DAT_00181018)` 之后的那些 `+0xNN` 调用都落在这
  张表上，方法实现集中在 `0x5600`–`0x6d00`。已核对的对应关系：`+0x20`/`+0x28` = 扫描开关
  （`0x5790`）、`+0x40`/`+0x48` = 过滤器增删（`0x5800`/`0x5820`）、`+0x70` = 注册客户端
  （`0x5880`）、**`+0x88` = GATT 连接（`0x59b0`）**、`+0xb0` = `0x5c40`、
  `+0x128` = `0x6890`。
- **事件发布点**：同一个线程用全局槽 `{长度 @0x1af890, payload[0x400] @0x1af898,
  类型 @0x1afc98}` 发事件，我们找到的 (size, type) 组合与 libnx 的 `BtdrvBleEventType`
  完全对得上：

  | 类型 | 含义 | 载荷大小 |
  | --- | --- | --- |
  | 4 / 5 | Client / ServerConnection | 0x14 |
  | **6** | **ScanResult** | **0x148** |
  | 7 | ScanFilter | 0x08 |
  | 8 | ClientNotify | 0x24c |
  | 12 / 13 | ServerAddAttribute / ServerAttributeOperation | 0x24 / 0x214 |
  | 9 / 10 | ClientCacheSave / ClientCacheLoad | 0x14c |

  于是"载荷超过 0x200 的那些事件"只可能是类型 8 或 13——**不是扫描结果**。这也解释了
  第二十八次实机里 payload 从 +0x200 开始：那是 `ClientNotify` 的 `data[0x200]` 之后的
  内容，前面 0x200 字节在 22.5.0 的实现里就是零。
- **连接失败点**：cmd 65 → 服务虚表 `+0x228` → `0x1c9c0` → `0x13c30` → 管理器 `+0x88`
  即 `0x59b0`。它在调用协议栈**之前**先做两次按 `client_if` 的查表（`0x9a60` / `0x9ba0`，
  遍历管理器里 4 个 0x240 字节的客户端槽：`id@+4`、句柄 `u32@+8`（`-1` = 无）、
  6 字节地址 `@+0xc`，地址由连接成功后的 `0x96c0` 写入）；**任意一次查到就直接返回
  `Bluetooth/0x14F`**。所以 `0x14F` 的含义是"这个 client_if 在管理器里已经有条目"，
  与目标地址无关——这也是为什么三种参数组合（direct/indirect、aruid 0/NRO）返回同一个码。

## 已确证的事实

### 1. 提供服务的是 `bluetooth` 模块，不是独立的 `btdrv` title

| 模块 | Title ID | Program NCA | 依据 |
| --- | --- | --- | --- |
| `bluetooth`（20.0.0+ 改名 `bluetooth.autog`） | `010000000000000B` | `ca66270be492a16bab1d779645965bc8.nca` | switchbrew Title list + 模块自身字符串 |
| `btm` | `010000000000002A` | `03e9f7441faea200b14646c32c019c19.nca` | 同上 |

标题表里的名字行与 Title ID 行是分开的，按行读很容易错位一格（本次先误读成
`0x0C`/`0x2B`）。用模块自身的字符串交叉验证后确认：

- `bluetooth` 模块里 `btdrv` 与 `bt` 两个服务名各被 `adr` 引用一次（`0x1b5dc`、
  `0x1b678`），都出现在同一个初始化函数里、紧跟各自的注册调用；`btdrv` 的注册调用在
  `0x1b610`。
- `btm` 模块里 `btm` / `btm:sys` / `btm:u` / `btm:dbg` 出现在 `0x1aeec`–`0x1b0a8`，
  并且它**以客户端身份**打开 `btdrv`（`0x49248` 的 `adr x1, "btdrv"` +
  `0x49250` 的取服务调用）。
- `audio` 模块（`0x14`）也引用 `btdrv`，与 13.0.0+ 的蓝牙音频功能相符。

### 2. 服务对象与它的虚表

两个服务对象都是 `{vtable_ptr, byte @+8, u32 @+0xc}` 的形状，在初始化时填好：

- `btdrv` 的虚表在 **`0x159a28`，连续 137 个指向 `.text` 的指针**；
- `bt` 的虚表在 `0x159e80`，15 个指针。

这是一张 **C++ 虚表**，不是平铺的命令索引表：槽位 0 是 `this` 调整用的 thunk
（`0x1c390`：`add x0, x0, #0xc` 后尾调用），槽位 1（`0x1c3a0`、`0x29a30` 同型）先查
对象 `+0xc` 的引用位、再清 `+0x10` 的标志字节，是析构/引用释放那一对。往下才是各命令
的适配层，例如 `0x1c750: add x0, x0, #8; b 0x12bb0`：把对象交给实现体，实现体再去调
单例的虚方法（如 `0x12bb0` 调单例 `+0x78`）。

### 3. BLE 栈确实在固件里

`bluetooth` 模块 text 段约 1.1 MB，含 `BLE CORE`、`BLE HID`、
`BluetoothBcmHcisuThread` 等字符串，`btdrv` 虚表有 137 个槽位（0–136）。也就是说
**通用 BLE 相关代码存在**，"（C）栈里根本没有实现"这一条目前没有证据支持（但也没被
排除）。

### 4. 旁证：PoC 用的那层封装很久没更新

libnx 的 `btdrv.c` 最近一次改动是 2025-04（`btdrvTriggerConnection` 的超时参数），
2024-06 还改过 GATT server 的参数；但 PoC 实际走的 `btdev.h`（`bt` + `btm:u` 封装）
最后一次改动是 **2020-12-29**。也就是说"用 `btdev` 扫 BLE"这套用法是 HOS 10/11 时代
的东西，从未针对 20.0.0+ 的 `bluetooth.autog` 复核过，与上面那条矛盾证据方向一致。

## 判定（2026-09-21 第一轮，已被上面的更正取代）

> **本节整段已被上文「判定（2026-09-21 夜，更正）」取代。** 它把 `0x159a28`（服务对象
> 虚表）当成了命令表，因此"请求形状漂移 / 类型重新生成过"的判断不成立；原文保留，只作为
> 逆向过程的记录。下面几节的表格同样已被取代。

**是 (A)：libnx 的请求形状过时，改我们自己的调用即可，不需要任何固件补丁。**（第一轮的
错误结论，保留原样）

之前的中间结论曾经指向 (B)（"固件不给后台 sysmodule 通用 BLE central"），那是被错误的
请求形状误导的：libnx 对 `RegisterGattClient`（cmd 62）只发 0x14 字节，而固件适配层要拷
**0x40 字节**参数块，多出来的 0x2C 字节是请求缓冲里的残留，注册因此一直返回
`result=0x37 / client_if=0xFF`。把整块按固件的形状发出去之后（`raw register A`，控制台日志
2026-09-21）：

    raw register A: ClientRegistration result=0x00000000 client_if=0x02 status=0
    raw register A: drained 16 event(s), 0 empty read(s)

**注册成功，拿到真实的 `client_if=0x02`**；同一轮里 `raw register C`（改用指针缓冲）返回
`0xF601` 并把会话关掉（后续 `InitializeBle` 报 `0xE401 = KernelError_InvalidHandle`），
说明这条命令要的是**内联数据**，只是长度必须给够。

各候选的最终判定：

| 候选 | 判定 | 依据 |
| --- | --- | --- |
| (A) 绑定/请求形状漂移 | **是** | 同一条命令、同一台干净主机：libnx 形状 → `result=0x37 / client_if=0xFF`；固件形状 → `result=0 / client_if=0x02` |
| (B) 固件按调用者/白名单封死 | 否 | 没有任何身份检查挡路；形状对了就放行 |
| (C) 栈里没有通用 central | 否 | 管理器会初始化、会发事件、会分配 client_if |

**因此不需要 exefs patch、不需要 mitm**，`tools/ble-re/make_ips.py` 那套补丁工具只当作以后
万一要用的备件留在仓库里。

顺序上的补充（2026-09-21 第二十一次实机）：**显式注册必须在 `InitializeBle` 之后**。放在
之前（同一会话）返回 `0x00029E71`、拿不到接口号；`InitializeBle` 自己会完成注册并给出
`client_if`（实测 `0x02`）。所以运输路径是
`btdrvInitialize → InitializeBle → 取 client_if → ConnectGattServer`，且全部在**同一个
会话**里（管理器把 client_if 绑在那个会话上）。

### 错误码归属（switchbrew 的 module 表，2026-09-21 查核）

| 观测到的 Result | module | description | 含义 |
| --- | --- | --- | --- |
| `0x00029E71` | 113 = `Bluetooth` | 0x14F | 蓝牙模块自己的通用失败（模块里有十几处 `mov w0,#0x9e71; movk w0,#0x2,lsl#16`）：`btdrvConnectGattServer` 与"注册放在 InitializeBle 之前"都拿到它 |
| `0x0002A671` | 113 = `Bluetooth` | 0x153 | 2026-09-25 02:17 新见：`btLeClientWriteCharacteristic` 在**刚发过 GATT 读**（描述符/电量）之后被判失败——无响应写与有响应写都失败，那一包不会发出去。模块里没有这个字面量，可能来自同模块的 `bt` 服务一侧；语义未定（见「当前状态」的第三轮） |
| `0x0000D671` | 113 = `Bluetooth` | 0x6B | 2026-09-25 19:02：`btdrvGetPairedDeviceInfo` 对**没有记录的地址**回它。对照 v26：先 `AddPairedDeviceInfo` 过、记录存在但内容为空时回的是 `rc=0`——所以第三方**能**往那套存储写记录，只是写不出 link key |
| `0x0005568F` | 143 = `Btm` | 0x2AB | btm 模块拒绝（libnx 的 `btmu*` 封装在 sysmodule 里填的 ARUID 无效） |
| `0x0000060A` | 10 = `Sf` | 3 | **服务框架**直接拒了请求：我们按 libnx 形状自造、但填了 NRO 的真实 ARUID 的那条 `btmu StartBleScanForSmartDevice` 就走到了这里 |
| `0x0000F601` | 1 = `Kernel` | 123 = `ConnectionClosed` | 会话被服务端关掉（cmd 40 那次） |
| `0x0000E401` | 1 = `Kernel` | 114 = `InvalidHandle` | 在已死的会话上继续调用 |
| `0x00006359` | 345 = `libnx` | 49 = `LibnxError_Timeout` | PoC 自己的重试耗尽，不是固件错误 |

`Sf`/`Btm` 这两条合起来说明：**`btm:u` 这条路对后台 sysmodule 是不通的**——填无效 ARUID 时
btm 接受请求却什么都不做（所以我们一直看到"扫描成功但没有事件"），填别人（NRO）的 ARUID 时
框架层就直接拒。要拿通用 BLE central 只能走 **btdrv**：它已经接受我们作为客户端（注册拿到
`client_if=0x02`），剩下的只是把各命令的参数形状按固件对齐，以及弄清 `Bluetooth/0x14F`
（最可能是"这个地址在协议栈里还没有记录"，即要用扫描先看到设备）到底卡在哪一步。

### 类型尺寸对照：不是"整体错位"，而是 ABI 重新生成过

> **已被取代**：本节把虚表下标当成命令号，因此"类型被重新生成过"不成立。真正的每命令
> 载荷见上文「命令 → 请求形状」。原文保留。

固件的适配层会明确告诉我们它要拷多少字节（"copy N bytes from the request"），把 libnx 侧的
类型尺寸量出来（`tools/ble-re/abi_sizes.py`，用 devkitA64 编译探头、读符号大小）就能逐条对照：

| libnx 类型 | libnx 大小 | 固件同族命令拷多少 | 结论 |
| --- | --- | --- | --- |
| `BtdrvGattAttributeUuid`（cmd 62 用） | 0x14 | **0x40**（槽 0x3E） | 不一致；按 0x40 发就成功（已实机验证） |
| `BtdrvBleAdvertisePacketData`（cmd 53） | 0xCC | 0xCC（槽 **0x39** = 57） | 尺寸相同但**落在别的命令号上** |
| `SetSysBluetoothDevicesSettings`（cmd 24） | 0x200 | 0x2BE（槽 0x17 = 23） | 不一致，且没有哪个 libnx 类型是 0x2BE |
| `BtdrvGattId` / `BtmBleDataPath` | 0x18 | 0x18（槽 0x42/0x49/0x51 等） | 尺寸一致，语义待逐个确认 |
| `BtdrvBleScanResult` | 0x148 | — | 扫描结果结构 |
| `BtdrvChannelMapList` | 0x88 | 0x88（cmd 40 的 out buffer） | 一致 |

结论：**不能靠"整体挪几个号"把 libnx 的绑定修好**——20.0.0+ 把这一层重新生成成
`bluetooth.autog` 时连类型一起换了（0x40 的注册描述符、0x2BE 的设备记录都不是 libnx 里的任何
类型）。要把它做完，得按模块自己的方式重建这一层 ABI：**逐条命令**从适配层读出"拷多少字节给
哪个方法"，再从实现体读出结构里每个字段的含义（必要时用实机做对照实验），然后在我们这边按
固件形状组请求。方法、脚本（`find_xref.py` / `peek.py` / `nso2elf.py` / `abi_sizes.py` /
`ghidra/`）和判定都已经具备，剩下的是工作量。

`TriggerConnection`（cmd 23）就是这条结论的又一个例子：固件那条要**6 字节地址 + 0x2BE 字节
结构**，libnx 只发 `{addr; u16 timeout}` 共 8 字节，所以它返回的 `Bluetooth/0x1806` 同样不能
当成"设备不存在"的证据。

### 扫描那一段的固件形状（下一步要逐条对齐的对象）

> **已被取代**：表里的"槽位 = 命令"是错的（那是虚表下标）。命令 55/56 其实无参数，
> 53 是 0xCC、57/58 是 0x3E，都与 libnx 一致。原文保留。

按虚表槽位导出（`/tmp/ble-re/adapters.txt` 是原始 dump，命令号 = 槽位下标，已用 62/63 验证过
这个对应关系在这段成立）：

| 槽位 = 命令 | 固件适配层做的事 | libnx 的形状 | 判定 |
| --- | --- | --- | --- |
| 0x35 = 53 | 直通 → `0x12f60` | `SetBleAdvertiseData`，指针缓冲 | 形状一致 |
| 0x36 = 54 | 直通 → `0x132b0` | `SetBleScanParameter`（两个 u16） | 待确认 |
| 0x37 = 55 | 6 字节地址 + 8 字节 + u32 + u32 → `0x13350` | `StartBleScan`，**无参数** | **不一致**：固件这条要参数，实机传垃圾参数 |
| 0x38 = 56 | 12 字节参数块 → `0x13410` | `StopBleScan`，无参数 | 不一致 |
| 0x39 = 57 | 拷 0xCC 字节到栈 → `0x134b0` | `AddBleScanFilterCondition`（`BtdrvBleAdvertiseFilter` 0x3E） | 尺寸对不上：0xCC 是 `BtdrvBleAdvertisePacketData`（libnx 里属 cmd 53） |
| 0x3A = 58 | 6 字节地址 → `0x13540` | `DeleteBleScanFilterCondition`（指针缓冲） | 不一致 |
| 0x3B = 59 / 0x3C = 60 | 直通 → `0x135f0` / `0x13680` | `DeleteBleScanFilter(u8)` / `ClearBleScanFilters()` | 待确认 |
| 0x3D = 61 | 拷 0x40 字节到栈 → `0x137b0` | `EnableBleScanFilter(bool)` | 不一致（和注册那条一样是 0x40 字节结构） |

也就是说"扫描没人给我们跑"这一步目前**还不能下结论**：按 libnx 形状调的 `StartBleScan` 实际
是带着请求缓冲里的残留参数在跑（这正是 0x40 字节注册那条的同类问题），得到的退化
`ScanResult` 也就不能当作"扫描不可用"的证据。要对齐，得照注册那条的做法，从 `0x13350` /
`0x13410` / `0x137b0` 这几个实现体往单例方法的参数里读，把结构字段一个个对出来。

仍然没有解释的一条（与 (A) 并存，属另一个问题）：**`btm:u` 的扫描从不给我们事件**——
完全不碰 btdrv 的会话里，`btdevStartBleScanSmartDevice(0x1812)` 返回 0 但
`events=0 polls=16 devices=0`，general 过滤器也是（`btm` 存的 smart device UUID 是空的、
general 过滤器固定为任天堂 company ID `0x0553`）。下一步要么按同样的方法把 `btm:u` 的请求
形状对齐，要么干脆把扫描也放到 btdrv 这一层做。

## 静态分析的卡点（保留记录）

> **已被取代**：这个卡点（"命令表下标 ↔ libnx 命令号"对不上）是虚表/命令表混淆造成的，
> 已由 `FUN_0001d4b0` 的 259 个 case 解决。原文保留。

## 参数布局对照（2026-09-21，第 1 步）

> **已被取代**：本表同样假设"槽位 = 命令号"。修正后的对照见上文「命令 → 请求形状」。
> 原文保留。

固件侧的形状从 `bluetooth` 模块的适配层读出来（槽位 = 虚表下标，假定等于命令号；下面的
几条与 libnx 的差异反过来支持这个假定），libnx 侧的形状从 `nx/source/services/btdrv.c`
的请求宏读出来。地址都是该模块 text 段的偏移（映射基址 0）。

| 命令 | libnx 的请求形状 | 固件适配层的形状 | 结论 |
| --- | --- | --- | --- |
| 40 `GetChannelMap` | `OutBuf`，`HipcMapAlias｜Out｜FixedSize`，0x88 字节 | 直通 `0x12870` | **属性不一致**：固件这条更像是要指针缓冲；实测调完它就把会话关掉（`0xF601`） |
| 46 `InitializeBle` | `GetEvent`（返回事件句柄） | 直通 `0x12bb0`，内部调单例 `+0x78` | 实机可用（干净启动下 rc=0 且事件照常投递），形状差异不影响功能 |
| 47 `EnableBle` | `NoIO` | 直通 `0x12c50`，**往 `*x1` 写一个 bool** | 形状对不上：固件这条像"读一个 bool 的 getter" |
| 55 `StartBleScan` | `NoIO` | 6 字节地址 + 8 字节 + u32 + u32（`0x13350`） | 明显不一致，但实机返回 0 |
| 56 `StopBleScan` | `NoIO` | 12 字节参数块（`0x13410`） | 明显不一致 |
| 57 `AddBleScanFilterCondition` | `InBufPtrFixed`（指针缓冲） | 先把 0xCC 字节拷进栈再调 `0x134b0` | 一致（0xCC ≈ `BtdrvBleAdvertiseFilter`） |
| 61 `EnableBleScanFilter` | `InBool` | 拷 0x40 字节参数块（`0x137b0`） | 不一致 |
| 62 `RegisterGattClient` | `InUuid`（`size` + 16 字节 UUID，共 0x14，内联） | **拷贝 0x40 字节参数块**（`0x13840`） | **不一致，且与实测吻合**：libnx 只给了 0x14 字节，固件读到 0x40 字节，多出来的 0x2C 字节是请求缓冲里的残留，注册因此以 `result=0x37 / client_if=0xFF` 失败 |
| 63 `UnregisterGattClient` | `InU8` | 直通 `0x138d0` | 可能一致 |
| 79 `GetBleManagedEventInfo` | `OutU32OutBuf`（`HipcPointer｜Out`） | 先拷 0x18 字节块（来自 `x2`）再调 `0x14250` | 存疑（`x2` 可能是那个 0x18 字节的出参块） |
| 100 `IsBluetoothEnabled` | `NoInOutBool` | 直通 `0x14ca0` | 一致（实机拿回真实状态） |
| 258 `GetBleChannelMap` | `OutBuf` | **不在 137 槽的表里** | 属于另一个接口/会话，不能按同一张表推 |

两条可以直接去主机上验的假设：

1. **参数块长度**：把 62 号按固件的 0x40 字节形状发（`{u32 size=0x10; u8 uuid[16]}` 放在
   块首或 0x20 处，其余补零），看 `ClientRegistration` 的 `result`/`client_if` 是否变正常；
2. **缓冲属性**：把 40 号从 `HipcMapAlias` 换成 `HipcPointer｜Out｜FixedSize`，看会话还会不会
   被关掉、能不能拿回 0x88 字节信道图。

探针已经实现在 `pocRunBtdrvIdentityProbe` 里（`pocRawRegisterProbe` /
`pocRawChannelMapProbe`）：每个形状尝试都**自己开一次 btdrv 会话**，因为 40 号那条会让固件
关会话，同一会话里后面所有调用都会变成 `0xF601`。日志里看 `raw register:` 与
`raw cmd40:` 两组行。

## 真要打补丁时的机制（第 3 步，已核对）

只有当上面的参数对齐仍然被拒时才需要补丁。机制来自 Atmosphère 源码
（`stratosphere/loader/source/ldr_patcher.cpp`、`libraries/libstratosphere/source/patcher/patcher_api.cpp`）：

- 放在 SD 的 `atmosphere/exefs_patches/<任意子目录>/<NSO module id 十六进制>.ips`，由 loader
  在加载系统模块时套用，**在签名与段哈希校验之后**，所以不需要重新签名；
- 格式是 IPS32：头 `IPS32`、每条记录 `u32 偏移`（大端）+ `u16 长度`（大端）+ 数据
  （长度 0 表示 RLE）、尾 `EEOF`；
- **偏移 = 0x100 + 目标在 NSO 映射里的偏移**（前 0x100 字节是受保护的 NSO 头；映射基址就是
  text 段基址，即反编译里看到的地址）；
- 补丁字节必须取自**已解压**的镜像（我们用 `nso2elf.py` 产出的 ELF），不能直接读 NSO 文件
  ——那三段是 LZ4 压缩的；
- 文件名里的 module id 可以去掉尾部的 0：`bluetooth` 是
  `c91c6fc8aa4c39222d6ccfe0fff468543105e59b`，`btm` 是
  `5a2aa468f272e49ebf0fab8b379cc5b32c1a7409`。

`tools/ble-re/make_ips.py` 负责生成：每个 `--edit 地址:旧字节:新字节` 都会先跟 ELF 里的
实际字节比对（版本变了就报错，不会把补丁打到错的地方），再按上面的偏移规则写出 IPS32；
`--verify` 能把文件解析回读，`--self-test` 校验偏移换算与往返。补丁只对上面这两个 build id
有效，固件更新即失效；回滚 = 删掉该目录 + 重启。

命令表下标与 libnx 命令号的对应关系出现了互相矛盾的证据：

- 第 `0x3E` 槽位的适配层（`0x1c8e0`）把 `x1` 指向的 0x40 字节参数块整个拷进栈再调用，
  **与 libnx 的 `btdrvRegisterGattClient`（cmd 62）那类"带参数块"的命令形状接近**；
- 第 `0x2E` / `0x2F` 槽位（`0x1c750` / `0x1c760`）分别"吃一个布尔量"和"回一个布尔量"，
  **与 libnx 的 `btdrvInitializeBle`（cmd 46，无参数、返回事件句柄）不一致**，
  反而更像 `EnableTxPowerBoostSetting` / `IsTxPowerBoostSettingEnabled` 这类开关命令；

而这张虚表前面还有框架槽位（析构对），所以"槽位号 = 命令号"本身也不能假定；再加上
20.0.0+ 该模块改名成 `bluetooth.autog`，提示这一层被重新生成过。几件事放在一起，
足以说明"libnx 的命令号在 22.5.0 上是否仍然成立"还没有答案。

这个映射必须确定，否则 PoC 的现象有两种同样成立的解释：

1. **编号漂移**：我们的调用打到了别的命令——固件照常返回成功，但什么也不做，
   扫描自然没有事件、注册自然也看不到真实结果；
2. **编号正确、固件内部拒绝**：注册确实被受理，但栈拒绝了这次注册。

同理，PoC 里那条"注册失败"的事件（`type=0`、`result=0x37`、`client_if=0xFF`）也需要
先确认**managed BLE 事件队列是否按会话隔离**：如果不是，它可能本来就是 `btm` 自己的
事件，而不是我们那次调用的返回。所以现在不能据此断定注册被拒。

## base `btm` 服务：命令形状与 ARUID 门槛（2026-09-22）

sysmodule 早就能打开 base `btm`（身份探针里的 `btmGetState` 拿回真实状态），但它的 BLE
命令一条都没调过。它是继 btdrv 直连与 `btm:u` 之后的第三条路：与 applet 用的是同一套
BLE 接口，却接受 sysmodule 这个调用者，因此不需要动架构。开工前先按本文对 `bluetooth`
做过的方法，把 `btm`（title `010000000000002A`）的命令号与载荷形状逐条核对了一遍。

### 命令分派与 libnx 对照

base `btm` 的分派函数在 `0x1bc50`：`cmp w4,#0x75` → 8 位字节表 `0x5e6b0` + 分支表
`0x1bc74`（`ldrb` + `add ..., lsl #2`）。命令 case 只被这张表引用，Ghidra 不会把它们建成
函数，所以要用 `tools/ble-re/ghidra/DecompileForce.java` 逐个地址反编译（用法见该文件
顶部注释）。

| 命令 | 名字 | 固件要求的入参 | 出参 | libnx | 结论 |
| --- | --- | --- | --- | --- | --- |
| 23 | `AcquireBleScanEvent` | 无 | event + 1 字节 flag | `btmAcquireBleScanEvent` | 一致 |
| 24 | `GetBleScanParameterGeneral` | `u16` parameter_id | 8 字节（公司号 + 6 字节 pattern） | `btmGetBleScanParameterGeneral` | 一致 |
| 25 | `GetBleScanParameterSmartDevice` | `u16` parameter_id | 0x14 字节 UUID | 同名 | 一致 |
| 26 | `StartBleScanForGeneral` | 8 字节参数块 | 无 | 同名 | 一致 |
| 28 | `GetBleScanResultsForGeneral` | 无 | 缓冲区（条目 = 0x148）+ `u8` total | 同名 | 一致（**条目 0x148**） |
| 31 | `StartBleScanForSmartDevice` | 0x14 字节 UUID | 无 | 同名 | 一致 |
| 33 | `GetBleScanResultsForSmartDevice` | 无 | 同上 | 同名 | 一致 |
| 34 | `AcquireBleConnectionEvent` | 无 | event + 1 字节 flag | 同名 | 一致 |
| 35 | `BleConnect` | 6 字节地址（**没有 ARUID 字段**） | 无 | `btmBleConnect` | 一致 |
| 38 | `BleGetConnectionState` | 无 | 缓冲区（条目 = **0xC**）+ `u8` total | 同名 | 一致 |
| 39 | `BleGetGattClientConditionList` | 无 | 固定 0x74 字节 | 同名 | 一致（libnx 未解码内容） |
| 46 | `GetGattServices` | `u32` connection_handle | 缓冲区（条目 = **0x24**）+ `u8` total | 同名 | 一致 |
| 57 | `RegisterAppletResourceUserId` | 0x10（`u32` unk + `u64` ARUID） | 无 | 同名 | 一致 |
| 59 | `SetAppletResourceUserId` | `u64` ARUID | 无 | 同名 | 一致 |

也就是说：libnx 的 **base `btm` 绑定在 22.5.0 上仍然成立**，"命令号漂移"这条解释在
btm 上不成立，探针拿到的任何否定结果都来自固件语义，而不是形状。

### ARUID 门槛：`btm:u` 为什么对 sysmodule 回 `Sf/0x60A`

`btm:u` 的 connect（cmd 18，case `0x27b20`）把请求里的 ARUID 和**调用者自己的 ARUID**
比了一次：

    lVar6 = *请求里的 ARUID
    lVar6 == 0 || lVar6 == 调用者的 ARUID ? 调接口 +0x80 : 返回 0x60A

这就是 2026-09-21 那轮"填真实 applet ARUID 反而被 `Sf/0x60A` 拒"的原因：sysmodule 不是
那个 applet，请求里带别人的 ARUID 会被当场挡下。它同时解释了为什么 base `btm` 的命令里
**根本没有 ARUID 字段**——它用 `RegisterAppletResourceUserId`(57) 把调用者的 ARUID 登记
到 btm 里，之后的请求就按调用者身份处理。

因此"btm 侧还缺前置条件"这条假设现在有了一个可测的具体形式：**先登记 ARUID，再扫描/
连接**。这也是 v18 探针（`StickR`）第一步做的事，见 `docs/ble-poc.md`。

### 实机结果（2026-09-22，第四十五次）：登记被受理，btm 仍然什么都不做

base `btm` 这条路实机跑通到"调用全部被受理"这一步：`GetState state=6`、存的扫描参数
可读、`RegisterAppletResourceUserId(0x89)` 返回 0、`StartBleScanForGeneral` /
`StartBleScanForSmartDevice` / `BleConnect` 全部 `rc=0`。但扫描**一个事件都没有**
（`events=0`、`total=0`），连接也**没有任何状态**（`total=0`），
`GetGattClientConditionList` 是四个空槽。形状与 applet 的 `btm:u` 路径完全一致。

所以"btm 对 sysmodule 做身份限制"这条被排除了：**访问权限不是卡点**。剩下的解释是 btm
内部的 worker（connect case `0x23fa0` 就是"组一条消息丢给 worker"）没有真正下发，而它
最可能缺的就是下面那层栈——驱动级探针在扫描前必须 `btdrvInitializeBle` + `btdrvEnableBle`，
两条 btm 路径都没做这一步。v18 因此加了 B 阶段：先把 btdrv 的 BLE 拉起来，再用同一套
btm 调用重放（见 `docs/ble-poc.md` 的「v18」一节）。

### 危险的组合（2026-09-22 第四十六次实机，btm 崩溃）

按上面的假设做了 B 阶段（同一会话里 `btdrvInitializeBle` + `btdrvEnableBle`，然后用 btm
重放扫描与连接），结果是 **`btm` 模块自己崩了**（`hid` 被连累），时间点在退出 NRO 之后：

- B 阶段之后 btm 的每条 BLE 调用都返回 `0x668F`（A 阶段是 `rc=0`），说明把 btdrv 的 BLE
  抢过来之后 btm 就不再能工作；
- 崩溃 PC `0x475ec` 是 `svc #0x26`，调用点 `FUN_00039150` 是 `svcBreak(0, msg, 4)`——btm
  自己的未处理异常/终止路径；
- 崩溃线程的栈里有设备地址 `EA:A8:AC:22:2C:18`，也就是我们排给 btm 的那条 connect；
- 同一轮还替前台 applet 登记了 ARUID（`RegisterAppletResourceUserId`），它在 applet 退出
  时失效。

结论与规则（写进 `docs/ble-poc.md` 的「危险与已知副作用」，探针已按此收窄）：

1. 不要在同一会话里既让 btm 排队 BLE 工作、又去 `btdrv` 的 `InitializeBle`/`EnableBle`；
2. 不要替 applet 登记 ARUID（那是别人的身份，随时会失效）；
3. 不要让 btm 留下无法完成的 connect（扫描没命中就别连）；
4. 这条线复工前先做静态分析：谁有资格 `InitializeBle`、`0x668F` 的确切含义、btm 的 worker
   在什么条件下走 `0x37d50` 的终止路径。v18 探针现在只保留只读部分（状态、扫描参数、
   两遍短扫描、`GetGattClientConditionList`），并且只在手动按键时运行。

### `0x668F` 与 btm 的崩溃路径（2026-09-22 静态核对）

上面这套规则现在有代码依据了，两条都读通了。

**Btm/0x33（`0x668F`）= "已经有请求在飞"**，不是通用错误。所有 BLE 接口方法（扫描、连接）
最后都走同一个"投递工作给 worker"的函数 `FUN_00033890`：

    uVar1 = FUN_0003b6c0(&DAT_000b4778);        // 取投递锁（带在飞计数）
    if ((uVar1 & 1) == 0) return 0x668f;        // 拿不到 → Btm/0x33
    if ((DAT_000b4758 & 1) == 0) {              // 没有在飞的请求
        DAT_000b4768 = param_2;                 // 记下这条消息
        ...投递到 worker...
    } else {
        uVar2 = 0x668f;                         // 已经有请求在飞 → Btm/0x33
    }

所以第四十六次实机 B 阶段那一串 `0x668F` 的含义是：**btm 的 worker 还在处理 A 阶段那条
connect**（设备不在广播范围/连不上，这一步永远完不成），新的请求全被"忙"挡回来了。

**崩溃是 btm 自己的状态机断言**。btm 的 worker 是一个轮询状态机 `FUN_00033d70`
（状态放在 `DAT_000b475c`，0..6），每一步问 `FUN_00033960` 要下一步；一旦返回值不是它
预期的几个值，就走到终止路径：

    local_1b8 = CONCAT44(local_1b8._4_4_, 0x20a8f);   // 把结果码塞进局部变量
    FUN_00037f40(&local_1b8);                          // 不返回

而 `FUN_00037f40` → `FUN_00037e30` → `FUN_00037d50` → `FUN_00039150(msg)` 正是
`svcBreak(0, msg, 4)`。与崩溃报告对得上：崩溃点 `0x475ec` 是 `svc #0x26`，寄存器
`X[01]` 指向**崩溃线程自己栈上**的局部变量、`X[02]=4`——就是这条把 `&local_1b8` 当消息
传进去的路径（`0x20A8F` = Btm/0x105 那类内部结果码）。

把两件事连起来：**A 阶段留下的在飞 connect + B 阶段把 btdrv 的 BLE 抢走**，让状态机那一步
拿到意料之外的结果 → 断言 → `svcBreak`。它死在"退出 NRO 之后"也顺理成章：状态机是轮询的，
什么时候再轮到自己就什么时候死，而 B 阶段之后又跑过一次（`action queued 4` 那次会话）。

因此新增一条硬规则：**看到 `0x668F` 就说明 btm 还有请求没做完，此时绝不能再去动 BLE
（尤其是 `btdrvInitializeBle`/`EnableBle`），应该停手并重启主机**，而不是继续发命令。

### `InitializeBle` / `EnableBle` 是全局的（2026-09-22 静态核对）

顺着 btdrv 的命令表查下去（分派 `FUN_0001d4b0` 的字节表 `0x11884e` + 分支表 `0x1d4d4`）：

| 命令 | case | 服务对象虚表 | 实现 | 做什么 |
| --- | --- | --- | --- | --- |
| 46 `InitializeBle` | `0x219b0` | `+0x190` → `0x1c790` | `0x12ed0` | 栈没起来就起来并注册一个客户端；已起来就**再注册一个客户端**（所以实机先拿到 `client_if=0x02`、后拿到 `0x03`） |
| 47 `EnableBle` | `0x21b10` | `+0x198` → `0x1c7a0` | `0x12ff0` | **全局**打开 BLE：必要时启动 BLE 线程、置全局标志，然后调管理器 |
| 48 `DisableBle` | `0x21c40` | `+0x1a0` → `0x1c7b0` | `0x13150` | 与 47 对称：全局关掉 |

也就是说 `EnableBle` **不是"给我的客户端开 BLE"，而是把整个模块的 BLE 栈打开/启动线程**。
这解释了第四十六次实机为什么是 B 阶段触发的：btm 正卡在 A 阶段那条永远完不成的 connect 上，
我们却在下面把 BLE 栈整体打开（还顺带启动了 BLE 线程），btm 的状态机下一步就拿到了意料之外
的结果 → `svcBreak`。

由此得到两条可执行的规则：

1. **一个开机周期里只走一条 BLE 路径**：要么 btm（任何 btm 请求），要么 btdrv 直连（`←` 探针
   会 `InitializeBle` + `EnableBle`）。两者混用就是在别人干活时改全局状态。
2. **跑过任何会碰 BLE 的探针之后先重启**再继续；看到 `0x668F` 立即停手。

### `0x1806` 的完整来源链（2026-09-22 修正：原结论是对的）

上一版这里写过一句"状态 `0x68` 映射成 `0x1806` 与代码不符"，**那句话是错的**：漏看了
归一化那一步。现在整条链路都读通了：

    cmd 65 → 服务虚表 +0x228 → 0x1c9c0 → 0x13c30 → 管理器 +0x88 = 0x59b0
    0x59b0: 先按 client_if 查两次表（命中任一 → 0x29E71 = Bluetooth/0x14F），
            否则调 FUN_00017f00(client_if, addr)
    FUN_00017f00: 把 {client_if, addr(6), 1} 打包成消息 → FUN_00046030 → FUN_00017e80(原始状态)
    FUN_00017e80: FUN_000195a0(原始状态) 归一化成 0..0x3F，再查表 DAT_0011863c[index]

两张表都读出来了：

- 归一化 `FUN_000195a0` 里 **`case 0x68:`（还有 `0x6f`、`200`、`0xc9`）`return 0x32;`**；
- 结果表 `DAT_0011863c[0x32] = 0x00300C71` = **Bluetooth/0x1806**（`[0x37] = 0x29E71` 正是
  `0x14F`，与"查表命中"那条对上）。

所以实机看到的 `Bluetooth/0x1806` 就是**原始状态 `0x68` 的映射**。顺带说明：同一个码在
模块里还有别的产出点（GATT 操作包装 `FUN_00018e80` / `FUN_00019010` 在载荷超过 600 字节时
直接返回 `0x300C71`，若干 HID 分支也一样），所以判读时要看调用路径，不能只看码。

剩下要查的是**原始状态 `0x68` 是谁产出的**。线索：同一模块里 `FUN_00045ac0` 一类的函数在
参数为空指针时也返回 `0x68`，说明这一族的 `0x68` 更像"参数不对"；而 cmd 65 的请求形状
（`{u8 client_if; addr[6]; u8 is_direct; u64 aruid}`，16 字节）与 libnx 完全一致，
`FUN_00017f00` 还把 `is_direct` 固定成 1。下一步要查的是：**地址类型问题**——设备
`EA:A8:AC:22:2C:18` 是 random static（`addr_type=1`），而这条连接 API 只传 6 字节地址、
没有类型字段，栈可能按 public 地址处理而报"参数不对"。

### 状态 `0xC8` 才是连接被拒的真正原因（2026-09-22 续查，结论更正）

顺着 `FUN_00017f00` → `FUN_00046030` 往下，链路全部读通了，**"参数不对/地址类型"这条猜测
被否定**：

    FUN_00017f00: 打包 {client_if, addr(6), 1} → FUN_00046030(msg)
    FUN_00046030: FUN_00047dd0(opcode = 0x6AA, msg, 8)      ← 就是老笔记里要找的 0x6AA
    FUN_00047dd0: FUN_00047e10(...) → 把回复载荷（状态）取回来；非 0 就原样返回
    FUN_00017e80(状态): FUN_000195a0(状态) 归一化 → 查表 DAT_0011863c

**BLE 线程侧 opcode `0x6AA` 的处理函数是 `FUN_0005fc50`**，它只有三个出口：

    param_1 == NULL                      → 回复 0x86AA / 状态 0xD1
    FUN_0007c0f0(client_if) == NULL      → 回复 0x86AA / 状态 200 (0xC8)   ← 我们的情况
    找到条目                              → FUN_00075c60(client_if, addr, is_direct, 2) → 状态 0

`0xC8` 正好在归一化函数的同一组里（`case 0x68: case 0x6f: case 200: case 0xc9: return 0x32;`），
`0x32` → `Bluetooth/0x1806`。所以实机那个 `0x1806` 的准确含义是：

> **`client_if` 在这张连接上下文表里没有条目**——不是策略门禁，也不是地址类型问题。

两张表要分清楚：

| 表 | 位置 | 条目 | 谁写 |
| --- | --- | --- | --- |
| 管理器**客户端槽**（4 个） | 管理器 `+4 / +0x244 / +0x484 / +0x6c4`，步长 `0x240` | 首字节 = `client_if` | 注册流程里的 `FUN_00009460(client_if)`（`0x5880` 调用） |
| **连接上下文表**（5 个） | `PTR_DAT_0015e228` `+0x50 / +0x2c8 / +0x540 / +0x7b8 / +0xa30`，步长 `0x278` | `+8` = 占用标志，`+9` = `client_if`（`0x2c8` 等为 `+0`/`+1`） | BLE 事件分发 `FUN_000788b0` 里的 `0x1F17 → FUN_00078e60`（分配槽位，`client_if` 由 `FUN_000c2fb0(客户端对象)` 给出）；`0x1F1A → FUN_00078ac0` 清空全部；`0x1F18 → FUN_0007c0f0` + 拆解 |

`0x59b0`（cmd 65 的服务端）先在**第一张表**里按 `client_if` 查两次，命中就回
`0x29E71`(0x14F)；查不到才把请求发到 BLE 线程，交给 `0x6AA`，由**第二张表**决定成败。

另外，`0x6AA` 这条路径**没有任何调用者身份检查**：同一份分发里 0x6A8 / 0x6A9 / 0x6AC /
0x6AD 都有 `DAT_002c7df0._2_2_ != (uVar5 & 0xffff) → 回复 0xCD` 这类比对，而 0x6AA 只检查
"BLE 是否已启动"（`DAT_002c7df0` 首字节）。所以"非任天堂客户端不能连"这个假设进一步被削弱：
**能不能连取决于那 5 个连接上下文槽里有没有属于你这个 `client_if` 的条目**。

### 缺的那一步找到了：`0x6A8` = cmd 62 `RegisterGattClient`

创建连接上下文条目的是 **opcode `0x6A8`**（事件 `0x1F17` → `FUN_00078e60` → 分配槽位、
把 `client_if` 写进 `+9`）。它的发送方是 `FUN_00045f80`：

    FUN_00045f80(param_1): 需要 param_1+0x18（回调）、发送 FUN_00047e10(0x6A8, param_1, 0x20, &out, 4)

而 `FUN_00045f80` 正是 **cmd 62 `RegisterGattClient`** 走的最后一步：

    cmd 62 → case 0x22f50 → 服务虚表 +0x210 → 0x1c960 → 0x13a80 → 管理器 +0x70 = 0x5880
    0x5880 → FUN_00017010 → FUN_00045f80(0x6A8) → 分配 client_if 并建上下文
    0x5880 随后把 client_if 通过事件队列报出来（载荷里的第 5 个字节，也就是 `data[4]`）

也就是说，**cmd 62 不只是"注册一个 GATT 客户端"，它同时建立连接上下文**；之后 cmd 65 的
`0x6AA` 才能查到条目。于是正确的顺序是：

    InitializeBle(46，成为 BLE 属主并把栈拉起来)
      → RegisterGattClient(62，分配 client_if + 建上下文)
      → ConnectGattServer(65，用 62 报出来的那个 client_if)

而之前所有实机尝试都缺了中间那一步，或者用错了 client_if：

- `←` 驱动级探针：只 `InitializeBle`，从不注册 → 用的是 `InitializeBle` 自己那个客户端
  （`client_if=0x02`），它没有连接上下文 → `0x6AA` 回 `0xC8` → `Bluetooth/0x1806`；
- 身份探针：注册过，但连接时用的还是从事件里抓的 `client_if`，而且管理器那边已经留下
  连接记录 → 两个查表命中 → `0x29E71`(`0x14F`)。

所以**"栈拒绝非任天堂客户端连接"这条假设目前没有证据支持**：拒绝是两个纯本地前置条件
（"这个 client_if 没有连接上下文"、"这个 client_if 已有连接记录"）造成的。

探针已按这个顺序更新（`←` 驱动级探针在扫描后先 `RegisterGattClient`、取新报出的
`client_if`、再连接，然后才跑原来的对照矩阵）。

### 客户端的槽位是有限的：`0x14F` = "4 个槽全占满"（2026-09-22 第四十七次实机）

按上面的顺序跑了一轮（设备在广播、`target_seen=1`、rssi=-45），结果注册这一步就被挡了：

    btdrv probe: RegisterGattClient(0x180C) rc=0x00029E71      ← Bluetooth/0x14F
    btdrv probe: register event raw=0000000002000000 client_if=2   （队列在重放旧载荷）
    btdrv probe: no new client_if, falling back to 0x02
    btdrv probe: ConnectGattServer(client_if=0x02, registered) rc=0x00300C71   ← 还是 0x1806

`0x29E71` 的出处也确认了：`FUN_00005880`（cmd 62 的服务端）第一步就是

    iVar1 = FUN_0000a900();          // 数管理器那 4 个客户端槽（+4 / +0x244 / +0x484 / +0x6c4，
                                     // 首字节 != 0xFF 即为占用）
    if (3 < iVar1) return 0x29E71;   // 4 个都占了 → Bluetooth/0x14F

也就是说：**这台机器上 4 个客户端槽已经满了**（系统自己的 BLE 使用者——btm 等——在开机时
就占掉了若干，而每一次 `InitializeBle` 又会给自己分配一个：第四十四次实机里同一个开机周期
内先后拿到 `client_if=0x02` 和 `0x03` 就是证据）。槽位满了 → 注册被拒 → 我们没有带
连接上下文的客户端 → 连接必然回 `0xC8` → `0x1806`。

因此探针改成**先腾位再注册**：用 `UnregisterGattClient` 释放本会话从 `InitializeBle`
拿到的那个接口（只动这一个，绝不碰别人的），随后 `RegisterGattClient`，再拿新报出的
`client_if` 去连接。

### 第四十八次实机：`0x14F` 的真正含义 + btm 才是槽位的主人

先腾位那一步也失败了，而且失败得很有信息量：

    btdrv probe: UnregisterGattClient(0x02) rc=0x00029E71     ← 也是 0x14F
    btdrv probe: RegisterGattClient(0x180C) rc=0x00029E71

注销路径 `FUN_00005940`（管理器 `+0x78`）的第一件事是**在客户端表里找这个 client_if**：

    uVar2 = FUN_00009650(manager, client_if);     // 比对 4 个槽的首字节
    if ((uVar2 & 1) == 0) return 0x29E71;         // 找不到 → Bluetooth/0x14F

`FUN_00009650` 只做首字节比对（`+4` / `+0x244` / `+0x484` / `+0x6c4`），而 `2` 不在其中。
**也就是说我们一直读到的"`client_if=0x02`"根本不是客户端表的接口号**：那个 8 字节事件载荷
（`00 00 00 00 02 00 00 00`）不是注册回复，把它按 libnx 的 `{result; client_if; status}`
解出来的"client_if"是误读。注册 `0x14F` 则确实来自 `FUN_00005880` 的
`if (3 < FUN_0000a900(manager)) return 0x29E71`——4 个槽全被占。

那 4 个槽是谁的？**btm 的。** btm 模块里有一份自己的 btdrv 客户端封装，直接调
`+0x210`（= btdrv 的 `RegisterGattClient`）：

    FUN_00048cb0(param_1): FUN_00049210(&client); uVar1 = (**(*client + 0x210))(client, param_1);

而且 btm 自己的 IPC 分发里 **case 0x3f 就是"通过 btm 注册 GATT 客户端"**（`+0x210`）。
所以正常开机后 btdrv 的 4 个客户端槽由 btm 持有（btm 那边 `GetGattClientConditionList`
也是 4 个槽，正好对应），第三方进程再调 `RegisterGattClient` 必然拿 `0x14F`。

**于是 btdrv 直连这条路的结论明确了**：在没有空闲客户端槽的情况下，第三方进程既拿不到
自己的 `client_if`，也无法创建连接上下文（`0x6A8` 只能由注册触发），连接必然停在
`0xC8` → `Bluetooth/0x1806`。这**不是策略门禁**，而是**容量/所有权**：BLE 客户端被
btm 占满了。

反过来，btm 那条路能通过 `FUN_0007c0f0`（它的连接被受理，`rc=0`），正说明 **btm 持有的
客户端是有连接上下文的**——要连设备，就得让 btm 替我们连。下一步静态目标因此换成：
btm 的 worker 在受理连接之后为什么停住（状态机 `FUN_00033d70` 在等什么），以及 btm 的
`case 0x3f`（在固件里存在、libnx 没有对应封装）能否给调用者分配一个可用的客户端。

### 第四十九次实机：注册成功，但注册事件不在我们读的队列里

这一轮重启后单独跑 `←`，注册**成功了**：

    btdrv probe: UnregisterGattClient(0x02) rc=0x00029E71   ← 2 仍然不在表里
    btdrv probe: RegisterGattClient(0x180C) rc=0x00000000   ← 成功
    btdrv probe: register event raw=37000000FF000000 client_if=255   （×8，全是重放）
    btdrv probe: no new client_if, falling back to 0x02

所以上一轮的 `0x14F` 确实是"同一个开机周期里前面的会话把槽位占满"，**重启后单会话就能注册**。
但注册报出来的接口号还是没拿到：队列里那 8 个载荷是 `37 00 00 00 FF …`，不是注册事件。

把 `FUN_00005880` 成功后那段反汇编读出来，注册事件的确切载荷是：

    58e0: x8 = 0x0000010000000000      → str x8, [sp,#8]      ；载荷 8 字节
    58fc: strb w8, [sp,#12]            ；byte4 = client_if
    5900: bl 0x2ba40(&payload, 8, 4, 0x20)                    ；投递（type 索引 2）

也就是 **`00 00 00 00 <client_if> 01 00 00`——第 6 个字节固定是 `1`，是这条事件的标志位**。
实机队列里从没出现过这种载荷，说明**注册事件不投到我们轮询的那个 managed 队列**（很可能
走 `bt` 服务的通道，那条通道是 applet 侧 `btdev` 用的）。这也解释了为什么之前用"载荷变了"
当判据会误判。

因此探针改成两手准备：

1. 用标志位 `data[5] == 1` 识别注册事件（如果它某天出现在这个队列里）；
2. 拿不到就**用连接探测把接口号试出来**：对候选 `client_if = 0..3` 各发一次
   `ConnectGattServer`，`0x14F` = 不在客户端表、`0x1806` = 没有连接上下文、
   `0` = 这个接口被栈接受了——那就是我们的接口。

### 第五十次实机：四个候选全是 `0x14F`，于是加了两条对照

这一轮注册仍然成功（`rc=0`），但四个候选接口**全部**回 `0x29E71`：

    btdrv probe: probe connect client_if=0x00 rc=0x00029E71
    btdrv probe: probe connect client_if=0x01 rc=0x00029E71
    btdrv probe: probe connect client_if=0x02 rc=0x00029E71
    btdrv probe: probe connect client_if=0x03 rc=0x00029E71

这说明"`0x14F` 只表示接口在表里"这个读法还不够：`0x29E71` 有两个来源——
（a）`0x59b0` 两次查表命中后直接返回（`FUN_00009a60` 查"该客户端有**挂起的**连接请求"、
`FUN_00009ba0` 查"该客户端已有**活动**连接"）；
（b）`FUN_00017f00` 把消息层的状态映射过来——归一化函数里 `case 0x65/0x71/0x72 → 0x37`，
而 `0x37` 正好是 `0x29E71`，其中 `0x72` 就是发送器 `FUN_00047e10` 的"**没有空闲任务槽**"。

所以四个候选都一样，很可能是 (b)：消息层忙/满，而不是接口问题。探针因此加了两条对照：

1. **`client_if = 0xFF` 的对照**：它永远不是合法接口。若它也回 `0x14F`，说明 `0x14F` 与接口
   无关（是消息层的事）；若它回 `0x1806` 而 `0..3` 回 `0x14F`，才说明 `0x14F` 是按接口判的。
2. **打开 `bt` 服务的用户侧事件通道**：libnx 的注释写明 `btGetLeEventInfo` 与
   `btdrvGetLeHidEventInfo`"用不同的状态"——也就是**两条事件通道**，我们之前只轮询了 btdrv
   那条。注册事件（`00 00 00 00 <client_if> 01 00 00`）很可能投在 `bt` 那条。探针现在会在
   注册前后与连接前后都 drain 一次 `bt` 通道，任何带标志位 `byte5 == 1` 的载荷就直接给出
   我们的 `client_if`。

### 第五十一次实机：`bt` 通道能开，但 `0x14F` 与接口无关

结果很干脆：

    btdrv probe: btInitialize rc=0x00000000          ← sysmodule 能开 bt 服务
    btdrv probe: btRegisterBleEvent rc=0x00000000
    btdrv probe before register: bt event raw=37000000FF000000   （×16）
    btdrv probe after register: bt event raw=37000000FF000000    （×20+，没有 01 标志位）
    btdrv probe: control connect client_if=0xFF rc=0x00029E71    ← 非法接口也是 0x14F！
    btdrv probe: probe connect client_if=0x00 rc=0x00029E71
    btdrv probe: probe connect client_if=0x01 rc=0x00029E71
    btdrv probe: probe connect client_if=0x02 rc=0x00029E71
    btdrv probe: probe connect client_if=0x03 rc=0x00029E71

两条结论：

1. **`bt` 服务对 sysmodule 开放**（两条事件通道都能开、都能读），而且 `bt` 通道给出的载荷与
   btdrv 通道**完全一样**（都是 `37 00 00 00 FF …`），注册事件没有出现在任何一条通道上。
   那个固定载荷更像"某次操作失败的通报"，不是注册回复。
2. **`0x14F` 与接口无关**：`client_if=0xFF`（永远不合法）也返回同一个码，说明它是
   `FUN_00017f00` 那条**消息层状态映射**的结果，对应状态是 `0x65/0x71/0x72` 之一，其中
   `0x72` = **"BLE 线程没有空闲任务槽"**。也就是说：**连接请求根本没能排进 BLE 线程**。

探针因此再加一条"冷启动对照"：在动任何 BLE 之前先发一次 `client_if=0xFF` 的连接。若它已经
返回 `0x14F`，说明**消息层在我们动手之前就已经是满的**（系统自身占用），这条路的失败与
探针行为无关；反之则说明是我们前面的调用把槽位吃掉的。

### 第五十二次实机：消息层是**我们自己**弄满的

冷启动对照给出了明确答案：

    btdrv probe: cold control btdrvInitialize rc=0x00000000
    btdrv probe: cold control connect client_if=0xFF rc=0x00300C71   ← 0x1806（请求进了线程）
    ...
    btdrv probe: control connect client_if=0xFF rc=0x00029E71        ← 0x14F（消息层自己拒了）

也就是说开机后**第一个**请求能正常进 BLE 线程（线程按 `0xC8` 正确拒绝非法接口），而在我们这
一轮做了 `InitializeBle` / `EnableBle` / 注册 / drain 之后，**任何**请求都变成消息层的
"没有空闲任务槽"。**`0x14F` 不是系统占用造成的，是我们自己的调用序列吃掉的。**

这条把结论从"资源被系统占满，所以不可能"改写成"**顺序/清理问题，可能可修**"。为了定位是
哪一步吃掉槽位，探针现在在关键步骤之间各插一次 `0xFF` 对照连接：

    after InitializeBle / after EnableBle / after bt open / after RegisterGattClient

哪一步之后从 `0x1806` 变成 `0x14F`，就是那一步占住了消息层；下一轮按这个结果调整顺序
（例如把注册挪到最后、或注册后不再做多余调用）再试。

### 第五十三次实机：占住消息层的正是 `EnableBle`

四段对照的结果非常干净：

    after InitializeBle:      control connect client_if=0xFF rc=0x00300C71   ← 0x1806
    after EnableBle:          control connect client_if=0xFF rc=0x00029E71   ← 变 0x14F
    after bt open:            control connect client_if=0xFF rc=0x00029E71
    after RegisterGattClient: control connect client_if=0xFF rc=0x00029E71

所以是 **`btdrvEnableBle`（cmd 47）** 让消息层不再接受连接请求。回看它的实现
`FUN_00012ff0`：必要时启动 BLE 线程、`FUN_0000d1c0(PTR_DAT_0015d750, 1)` 置一个全局标志、
再调管理器的 `vtable[0]`——也就是"把整个 BLE 栈打开"。打开之后，连接请求就排不进去了
（状态 `0x72` = 没有空闲任务槽）。

探针因此把顺序改成：**`InitializeBle` → `RegisterGattClient` → 先连接 → 再 `EnableBle` →
扫描**（扫描需要 `EnableBle`，连接显然不能排在它后面）。下一轮就看这个顺序下
`RegisterGattClient` 之后的连接是不是能进线程（不再是 `0x14F`）。

### 第五十四次实机：两个约束互斥

按新顺序跑（注册在 `EnableBle` 之前）：

    RegisterGattClient(0x180C) rc=0x00029E71          ← 注册失败（这一轮没先 EnableBle）
    after RegisterGattClient: control connect 0xFF rc=0x00300C71
    probe connect 0x00/0x01/0x02/0x03 rc=0x00300C71   ← 干净的"无上下文"拒绝
    early ConnectGattServer(client_if=0x02) rc=0x00300C71
    btdrvEnableBle rc=0x00000000
    after EnableBle: control connect 0xFF rc=0x00029E71   ← 又变成消息层拒绝

两轮合起来就是：

| 状态 | 注册（建上下文） | 连接请求能否进线程 | 扫描 |
| --- | --- | --- | --- |
| 未 `EnableBle` | ❌ `0x14F` | ✅（干净地回 `0x1806`） | — |
| 已 `EnableBle` | ✅ `rc=0` | ❌ `0x14F`（消息层"无空闲任务槽"） | ✅ |

也就是说：**用现有的调用组合，我们只能拿到"上下文"或"能排队的连接"，不能同时拿到**。
`EnableBle` 既是注册成功的前提，又是连接排队失败的原因。

下一步换成 btm 这条路（它是唯一持有连接上下文的客户端，连接里 `rc=0` 被受理），并且用
本轮新打开成功的 `bt` 用户侧事件通道去观察 btm 的连接到底发生了什么——btm 探针现在会在连接
前后 drain `bt` 通道，并在没有扫描命中时也连一次配置地址（明确记录"跑完要重启"，因为 btm
会把这条请求一直挂在工作队列里）。

### 第五十五次实机：`bt` 通道抓到了连接事件

自动跑起来之后，连接前后各 drain 一次 `bt` 通道，载荷**变了**：

    btm probe before connect: bt event raw=0000000001000000      （×16，连接前）
    btm probe (configured): BleConnect(EA:A8:AC:22:2C:18) rc=0x00000000
    btm probe (configured): AcquireBleConnectionEvent rc=0x00000000
    btm probe (configured): no connection / GetConnectionState total=0
    btm probe after connect: bt event raw=1A00000002040000        （×16，连接后）

按 libnx `btdrv.h` 的 `client_connection` 布局解这条载荷：

    {u32 result; u8 status; u8 client_if; u8 pad[2]; u32 conn_id; BtdrvAddress address; u16 reason}
    1A 00 00 00 | 02 | 04 | 00 00 | FF FF FF FF | EA A8 AC 22 2C 18
    result=0x1A   status=2 (Disconnected)   client_if=4   conn_id=无(0xFFFFFFFF)   addr=设备

（第 46 次实机在 btdrv 侧看到的 `1A00000002040000FFFFFFFFEAA8AC22...` 就是同一条。）

所以：**btm 确实把连接请求下发到了栈，栈也针对这台设备产生了连接事件，结果是"断开/未建立"
（status=2、conn_id 无效）**。这比之前"受理后什么都没发生"进了一大步——现在有明确的失败
事件可读。下一步要查的是 `result=0x1A` 与 `reason` 字段的含义（`reason` 在地址后面两个字节，
探针已把整条事件按结构体解出来打印），以及为什么连接没有建立：设备侧？地址类型？还是栈
在连接前需要别的登记。

探针现在把 `bt` 事件按 `client_connection` 结构解码后打印
（`result/status/client_if/conn_id/addr/reason`），下次日志可以直接读字段而不是手工拆字节。

### 第五十六次实机：失败事件的字段解全了

    btm probe after connect: bt event as connection:
      result=0x0000001A  status=2(Disconnected)  client_if=4
      conn_id=0xFFFFFFFF  addr=EA:A8:AC:22:2C:18  reason=0x0000

连接**前**队列里是另一条旧记录（`00 00 00 00 01 00 00 00` → 解出来 result=0/status=1/
全零地址），连接**后**稳定换成这一条。所以：

- btm 受理了连接（`rc=0`），栈**确实针对这台设备发起了连接尝试**；
- 结果是 `status=2`（未建立/断开），**没有 `conn_id`、`reason=0`**——这不是"HCI 断开原因"，
  而是"连接没有成立"的合成事件；
- `result=0x1A` 是这次尝试的状态码，具体含义待查（同族的 `0x68/0x72` 是消息层状态）。

## 当前总览（2026-09-22 收束，已被「当前状态（2026-09-25）」取代）

这张表的最后一行（btm 连接"未建立"）在 2026-09-24 被推翻：装上补丁并先打开 BLE 栈之后
连接就成立了。表里"扫描（btm / btm:u）受理但零结果"这一条**至今仍然成立**。

| 环节 | 状态 | 依据 |
| --- | --- | --- |
| 扫描（btdrv 驱动级） | ✅ 稳定扫到设备（地址 / rssi / AD 内容） | 第 36 次起多轮 |
| 扫描（btm / btm:u） | ❌ 受理但零事件零结果 | 第 45、55 次 |
| btdrv 直连建客户端 | ❌ 4 个客户端槽被 btm 占满；`EnableBle` 前注册被拒（`0x14F`），`EnableBle` 后连接排不进消息层（同样 `0x14F`） | 第 48、53、54 次 |
| btdrv 直连连接 | ❌ 没有连接上下文 → `0xC8` → `Bluetooth/0x1806` | 第 49~54 次 |
| btm 连接 | ⚠️ 受理（`rc=0`），栈发起连接尝试，结果是"未建立"（`status=2`，无 conn_id/reason） | 第 55、56 次 |
| 设备侧 | ✅ 手机一点就连上 | 用户实测 |

**btdrv 直连这条路可以定性为不可行**：客户端槽与 BLE 消息层都被系统自身的 BLE 使用者
（主要是 btm）占住，第三方进程拿不到"有连接上下文的客户端"，也就永远过不了 `0xC8`。

**btm 那条路是目前唯一走到"栈真的发起连接"的**，剩下唯一的未知是：这次尝试为什么没有成立
（`result=0x1A`、无 reason）。下一步的两个候选：

1. **配对/自动连接前置**：`btm` 的 `StartBleScanForPaired`（libnx 里对应
   `btdevEnableBleAutoConnection`）——如果栈要求设备先进入"已配对/已知"列表才会建立连接，
   这就是缺的那一步；
2. **诊断用 IPS 补丁**：把栈里那段"合成未建立事件"的检查临时改掉，确认它是不是唯一门禁
   （只作诊断，不是方案）。

### 第五十七次实机：配对/自动连接前置无效

    btm probe: StartBleScanForPaired(company=0x000A) rc=0x00000000
    btm probe: StopBleScanForPaired rc=0x00000000
    btm probe: connection state after paired scan rc=0x00000000 total=0   ← 没有自动连接
    btm probe (configured): BleConnect(EA:A8:AC:22:2C:18) rc=0x00000000
    btm probe after connect: result=0x0000001A status=2 client_if=4
                             conn_id=0xFFFFFFFF addr=EA:A8:AC:22:2C:18 reason=0x0000

与上一轮**逐字段相同**。到这一步，能试的"任天堂式前置"（ARUID 登记、配对/自动连接、扫描、
两次注册顺序、控制连接定位）都试过了，结论不再变化。

### 当前状态（2026-09-25）：连接 + GATT + 写入已通，通知路径待解

**可用配方（一个按键，两段会话）**

1. **补丁**：exefs IPS，模块 `bluetooth.autog`（build ID `c91c6fc8aa4c…`），
   **2 处编辑**——`0xcd820`、`0xcf7d4` 两条"控制器层客户端未激活"的 `cbz` 改成 `NOP`；
2. 按一次 `StickR`（或空闲屏 `B`）：NRO 自动先起**驱动级探针会话**（它把 BLE 栈拉起来），
   该会话结束后自动起**btm 探针会话**（连接 → GATT 表 → 传输层）。

**已验证**

| 环节 | 状态 |
| --- | --- |
| 扫描 | ✅ btdrv 驱动级扫描稳定拿到设备（地址/rssi/AD） |
| 连接 | ✅ btm 路径受理并建立连接（`GetConnectionState` handle=4、`bt` 事件 `status=0`） |
| GATT 表 | ✅ 7 个服务，`0x180C`（写 `0x150A` handle 19，通知 `0x150B` handle 16）、`0x180A`、`0xFE59` |
| 通知订阅 | ⚠️ `RegisterNotification rc=0`，但**至今没收到任何事件**：02:17 那轮把整条事件 dump、CCCD 回读、managed 对照都跑了，`bt` 通道与 btdrv 的 managed 队列里都只有连接级记录（见下面的「第三轮」） |
| 写入 | ✅ **已被硬件证明**：`BF`（7 字节）与 `B0`（20 字节）写入 `rc=0`，2026-09-25 15:58 的反应测试让设备真的输出了（用户感觉到，日志里的波形字节与发出的完全一致，见「第四轮」） |
| B1 回包 | ❓ 未观察到 |
| 广播内容 | ✅ 定长 AD 数组：flags + 厂商数据（公司号 `0x000A`）+ 本地名 `47L121000`；**没有服务 UUID** |

**这轮学到的边界（都写进了代码注释）**

- **btm 探针绝不能碰 btdrv**：同一次开机里第二次 `InitializeBle`/`EnableBle`、或让本进程
  `RegisterGattClient`（btm 之后会用我们创建的上下文、`client_if=3`），都会让连接建立不起来。
  连续 4 轮失败 vs 2 轮成功的对照见 `docs/history.md` 第 28 节；
- **驱动级直连仍然不可行**：4 个客户端槽被系统占满，`RegisterGattClient` 稳定回 `0x14F`，
  没有连接上下文就永远是 `Bluetooth/0x1806`。补丁的两处 NOP 管的是"激活标志"，不是"有没有槽位"；
- 连接判定**必须用事件**（`bt` 通道的 `client_connection`，`status=0` + `conn_id` + 地址），
  `btmBleGetConnectionState` 在这套配置下会恒为 `total=0`，据此判断会把已建立的连接当成失败
  （然后反复重连、把它断掉——日志里的 `reason=0x0016` = 本地主动断开就是这么来的）。

**下一步（留给下一次对话）**

1. **订阅做 A/B**：v21 起默认只留 `RegisterNotification`（不再手工写 CCCD），先这样跑一轮；
   若仍全哑，再翻过来（只手工写 CCCD、不 `RegisterNotification`）跑一轮——两种方式在 v19/v20
   是同时开着的，不能排除手工写与栈内部订阅状态不一致；
2. 若两种都静默：读我们还没碰过的两条队列（`btdrvGetEventInfo`、`btdrvGetLeHidEventInfo`），
   看回包是不是被路由到那边；读它们仍要守住"不 `InitializeBle`/`EnableBle`/`RegisterGattClient`"
   这条线；
3. 通知通路确认可用后，再驱动一次**真正的强度变化**去看 B1（v21 已把强度包整包打进日志，
   包括 `seq / A mode,value / B mode,value`）；
4. 传输层稳定后，把它接到 NRO 的玩法（体感/触屏）上，替换现在不成功的 btdev 路径；
5. 发布口径（用户已定）：**BLE 直连仅在安装该 exefs 补丁时可用**，补丁需要纳入发布产物。

**第三轮（2026-09-25 02:17）的结果**：上一轮留下的三条判据（`ev#N` 整条 dump、
`ReadDescriptor` CCCD、`managed#N`）这一轮全部跑出来了（日志 322 行，见 `docs/ble-poc.md`
的「第四十九次实机」）：

1. **`bt` 事件通道整轮只有两条连接级记录**：`result=0 conn=4`（尾全零）与同一头、
   `+0x08=12` / `+0x0C=1000` 的 `connection_update`。写 28 包 B0、读电量、读 CCCD 之后
   **没有第三条记录**；
2. `ReadDescriptor(CCCD 0x2902 id=0) rc=0x00000000`，**值没有回来**；
3. **managed 队列只有 1 条记录**，与第 1 条里那份 `connection_update` 逐字节相同。

所以上一轮列的两个候选解释各砍掉一半：通知既不落 `bt` 的这个状态，也不落 btdrv 的 managed
队列。剩下最可能的解释就是**订阅没有真的落到 CCCD 上**（`RegisterNotification` 只回"受理"），
但要拿到回读值才算坐实。

这一轮顺带定论了两件事：`properties` 的真实位置在 **+0x20**（`0x150B`=`0x10` notify、
`0x150A`=`0x04` write without response、`0x180A` 那五个是 `0x02`/`0x12`）——libnx 读出
`0x00` 只是结构偏移不一致，"写类型/订阅目标弄错"因此被排除；以及新错误码
`0x0002A671` = `Bluetooth/0x153`（见「错误码归属」），它只出现在"刚发过 GATT 读之后"的两次
写上（无响应写与有响应写都失败，那一包没发出去），像忙拒，未证实。

**第四轮（2026-09-25 15:58）的结果：出方向通了，入方向全哑。** v19 把两次 `0x153` 干掉
（读后停 300ms，BF 不再被丢），v20 加了一次**反应测试**：软上限 20 + A 通道一段短波形 +
请求强度 5。实机**有输出**（用户感觉到），日志里的 B0 字节与发出去的波形完全一致
（`B0 0000 00 00 64646464 001E3C1E …`），整轮 100 次写全 `rc=0`。所以**写入确实到达了
设备**——这条从现在起不靠 `rc=0` 推断，有硬件证据。

而回包仍然一条没有：`notify=0 / b1=0`，`bt` 通道与 btdrv 的 managed 队列里只有连接级记录，
电量与 CCCD 的读应答也没回来。问题因此被压成二选一：**设备不回**（与"官方 App 能收到 B1"
矛盾，而且 ATT 读请求按规范必须应答），或者**回包送不到我们能读的客户端/队列**。完整记录见
`docs/ble-poc.md` 的「第五十到第五十二次实机」。

**顺带定论的一件事**：广播里**没有服务 UUID**。设备记录的 AD 是定长数组
（`BtdrvBleAdvertisement`，不是紧凑链）：`0x01` flags、`0xFF` 厂商数据（公司号 `0x000A`）
和 `0x09` 本地名 `47L121000`。所以按 UUID 过滤的 smart-device 扫描不可能找到它，
"手机实测 0x180C"那种说法在这台设备/这个固件上不成立。

补丁加上"先打开 BLE 栈"这两步之后，实机达成了最初的目标（日志见 `docs/ble-poc.md`）：

    btm probe (configured): connected handle=4 addr=EA:A8:AC:22:2C:18
    btm probe (configured): GetGattServices attempt 2 rc=0x00000000 total=7
      service[2] uuid=0x180C handle=14 end=19   ← DG-LAB 协议服务
        char[0] uuid=0x150B handle=16            ← 通知
        char[1] uuid=0x150A handle=19            ← 写
      service[3] uuid=0x180A                     ← 电量
      service[6] uuid=0xFE59                     ← DFU
    btm probe (configured): BleDisconnect rc=0x00000000

**可用配方**（两步，缺一不可；2026-09-25 起是一个按键的序列，"先 `←` 再 `StickR`"的
两步操作已经退休）：

1. **安装 exefs 补丁**（见下一节）：跳过"控制器层客户端未激活"的检查，否则连接必然
   停在 `result=0x1A`（原始状态 `0x85`）。
2. **先让 BLE 栈起来**：只有驱动级探针会 `InitializeBle` + `EnableBle`，所以按一次
   `StickR`（一键序列的第一段就是它）或先按 `←` 都行。**这一步不能挪进 btm 会话里**：
   在 btm 会话里再 `InitializeBle`/`EnableBle`、或让本进程 `RegisterGattClient`，连接就
   建立不起来（4 轮失败 vs 2 轮成功的对照见 `docs/history.md` 第 28 节）。

**正式结论**：BLE 直连要"安装补丁 + 先打开 BLE 栈"才连得上；不装补丁时连不上。项目按此
口径对外描述（`README.md`、根 `AGENTS.md` §15）。写入侧（BF/B0）已由**实机输出**证明到达
设备（2026-09-25 15:58）；**回包侧全哑**，所以还不能算"能控制设备"——没有 B1 就不知道
设备当前的实际强度，也无法做协议要求的"确认后再改强度"。

已知遗留：

- 已定：属性字节在固件结构里位于 **+0x20**（+0x18 是 handle）。libnx 的
  `BtmGattCharacteristic.properties` 读出 `0x00` 只是偏移不一致——要用这个字段就按 +0x20
  取，不要相信 libnx 的字段；
- 连接是"按地址直连"，设备没有先被 btm 扫描到（`btm` 的扫描仍然不出结果），
  目前看这不影响连接；
- **回包一条都没回来**：写入已由实机输出证明到达设备（「第四轮」），但 `bt` 事件通道与
  btdrv 的 managed 队列里只有连接级记录，B1 与读应答全无；
- `btLeClientWriteCharacteristic` 在**刚发过 GATT 读**之后会回 `Bluetooth/0x153`
  （`0x0002A671`，无响应写与有响应写都失败，那一包不会发出去）：读之后要留间隔，否则会
  静默丢包（02:17 那轮就这样丢了 BF）；
- 补丁还没纳入发布产物；`btm` 探针每次跑完都要重启，一个开机周期只走一条 BLE 路径。

### 连接归谁：管理器的"设备槽位表"（2026-09-25 静态分析）

btdrv 的 `ConnectGattServer`（cmd 65）**不用调用者传进来的 `client_if`，而是按地址去查一张表
再写回来**：

    FUN_000088d0 / FUN_00008930 / FUN_000089a0 / FUN_00008a10 / FUN_00008a80 / FUN_00008cf0
      └─ local_client_if = 0xFF
         FUN_00008150(manager, &local_client_if, addr)   ← 按地址查表，成功时回写 client_if
           └─ FUN_00007b60(manager, addr)                ← 真正的表查找
         查不到 / 槽位的 client_if 是 0xFF → 包装一律返回 0x300C71（`Bluetooth/0x1806`）

表在管理器对象里，槽位从 `manager + 6` 开始、步长 `0x332`、共 **10** 个：

| 槽位偏移 | 含义 |
| --- | --- |
| `+0` | 有效标志（`0x01`） |
| `+1..+6` | 设备地址 |
| `+7` | `client_if`（`0xFF` = 未分配） |

写这张表只有一条路，而且两个函数**只有一个调用者**：

    FUN_0002e400(payload)                     ← 内部消息 case 0x18、payload 里 kind == 1
      ├─ FUN_000079e0(manager, addr)          占一个新槽位（10 个全满 → 0x2ef271）
      └─ FUN_000080b0(manager, addr, if)      给已有槽位分配 client_if

补丁那两处（`0xcd820` 在 `FUN_000cd7f0`、`0xcf7d4` 在 `FUN_000cf6f0`）正好是这条路的两道
"槽位已激活 / 设备已配对"闸门——`FUN_000cf6f0` 拿地址去比对主机自己存的那几条设备记录
（状态字节 `0x02`）。

**推论**：连接的"所有权"就是槽位里的 `client_if`。我们借 btm 的连接时，槽位里写的是 btm 的
`client_if=4`，于是这条连接的**事件（通知、读应答）按 client_if 投递，归 btm**；我们的 `bt`
会话只拿得到广播型事件（连接建立、连接参数更新）——这正是四条队列里始终只有连接级记录、
而 B1 一条不来的原因，也解释了"写畅通、读不回"。写入不需要回投递，所以不受影响。

下一步：找出内部消息 `0x18 / kind == 1` 的发送者（哪条服务命令能触发它），这样我们就能在
自己的会话里给设备地址分配到 `client_if=2`，让连接归我们。

**配对支线（2026-09-25 晚）**：用户 iOS 实测说明设备**支持 bonding 但不强制**（App 的
「设备绑定」开关才触发真配对，见 `docs/dglab-protocol.md`）。主机侧这条路的形状也查清了：
配对请求走 **通用事件队列**（`btdrvGetEventInfo`）的 `BtdrvEventType_SspRequest`(3) 或
`PairingPinCodeRequest`(2)，并且可以用 `btdrvRespondToSspRequest(addr, variant, accept,
passkey)` 自己应答——主机上没人会替我们答。v26 因此试了「驱动级会话里 `CreateBond` + 自己
应答」，并在 btm 会话的连接还活着时再试一次（见 `docs/ble-poc.md`）。判据是
`GetPairedDeviceInfo` 回来的 `link_key_present`：非 0 才说明真的绑上了；绑上之后再看
`ConnectGattServer(client_if=自己的)` 是否还回 `0x1806`。

**用户口径（2026-09-25）**：**优先做成"不需要配对"的连接**；只有在那条路实在走不通时，
才走"触发配对请求但立刻自动取消"的方案（即 `POC_BTM_BOND_ACCEPT = 0`，不要把设备绑到主机上
——App 里写明绑定后只有被绑定的主机能连，绑定信息只能在 App 或设备重置里清除）。

**实测结果（2026-09-25 18:43，v26）：在"设备绑定"关闭的状态下，设备不理配对请求**。

    btdrv probe: AddPairedDeviceInfo(EA:A8:...:18) rc=0x00000000
    btdrv probe: GetPairedDeviceInfo rc=0x00000000 name=""      ← 记录没写进去
    btdrv probe: CreateBond(type=0) rc=0x00000000 (accept=0)
    btdrv probe: paired readback rc=0x00000000 link_key_present=0 name=""   ← 没有绑定
    btdrv probe: ConnectGattServer(client_if=0x02, after paired write) rc=0x00300C71

即：`CreateBond` 被受理，但**整轮没有任何配对事件**（没有 `SspRequest` / `PairingPinCodeRequest`），
设备也没有被绑上（`link_key_present=0`，所以不会影响手机 App）。`AddPairedDeviceInfo` 写的记录
也留不下（读回来是空的），说明那套存储要靠真配对产生的 link key。btm 会话里那次尝试因为
传输窗口末尾的 peek 已经 `btdrvExit` 而拿到 `Kernel/InvalidHandle`，没真正执行。

**注意这轮的前提**：用户当时**没有开 App 里的「设备绑定」**。按 iOS 上的行为，设备只在那个模式
下才发起配对，所以这一轮**只证明"默认状态下它不配对"，没有证明配对支线不可用**。要判定这条支线，
必须先让设备进入可绑定状态（由 App 控制），再在**连接还活着**的时候试——iOS 也是在连着的时候
才弹配对框。v28 因此把配对探测改成：驱动级会话一次，**以及 btm 会话窗口结束后、连接还没断时
再由 peek 跑一次**（修掉 v26 那次 `InvalidHandle` 的顺序问题）；仍然是"收到请求就取消"
（`POC_BTM_BOND_ACCEPT = 0`），不会真的绑定。

**配对支线的收尾（2026-09-25 晚，用户对照实验）**：绑定由**设备侧**强制，而且会影响我们的
连接质量——用户做了"设备里有/没有绑定信息"的对照：

| 设备状态 | 第三方主机（Switch）直连 | 能否发控制信号 | 拨轮会不会回传 |
| --- | --- | --- | --- |
| 有绑定信息 | ✅ 能连上 | ❌ 写入被设备忽略 | ❌ |
| 无绑定信息 | ✅ 能连上 | ✅ （体感输出已验证） | ❌ |

结论：**实机控制实验必须在"设备无绑定信息"状态下做**；v26/v28 那两轮如果设备正绑着手机，
它们的"输出/传输"部分不能当作有效数据（`notify=0 / b1=0` 的结论仍然成立，因为命令行与
连接行为都没变）。另外，"设备处于可绑定状态却没有配对信息"这个组合不存在——可绑定状态就是
靠配对建立的。配对这条路因此对我们关闭（`POC_BTM_BOND_PROBE = 0`，代码留着备用）。

顺带得到一个新观测点：连接尝试失败后，btdrv 的 managed 队列里出现了一条**带设备地址**的记录
（重复出现）：

    03 00 00 00 EA A8 AC 22 2C 18 00 00 00 00 00 00 00 00
    └ 0x3 ───┘ └── 目标设备地址 ──┘

它不匹配 libnx `BtdrvBleEventInfo` 里的任何成员（`client_connection` 的地址在 +0xC），
所以固件这条记录的布局还没有对上；它紧接着连接尝试出现，像是"针对这个地址的连接尝试/结果"
的记录，是第一个能观测"栈对我们的连接请求做了什么"的东西，留给后面查。

### 早期收束结论（2026-09-22，已被上面的状态取代）

1. **扫描可用**：btdrv 驱动级扫描稳定拿到设备（地址、rssi、AD 内容）。
2. **btdrv 直连不可行**：4 个 BLE 客户端槽由系统自身的 BLE 使用者（btm）持有；第三方
   `RegisterGattClient` 在 `EnableBle` 之前被拒（`0x14F`），而 `EnableBle` 之后连接请求
   又排不进 BLE 消息层（同样映射到 `0x14F`）——**"有上下文"和"能排队"二者不可兼得**。
   没有上下文，连接必然停在 `0xC8` → `Bluetooth/0x1806`。
3. **btm 唯一走到"栈真的发起连接"**：`btmBleConnect` 受理（`rc=0`），随后 `bt` 用户侧
   事件通道给出针对本设备的连接事件，但结果是"未建立"（`status=2`、`conn_id` 无效、
   `reason=0`、`result=0x1A`），且没有任何可读的失败原因。
4. 设备侧正常（手机一点就连上）；因此限制在 Switch 侧。

当时判断"要保持搁置"，推进只剩两条路：

- **诊断用 IPS 补丁**：临时改掉栈里"合成未建立事件"的那段检查，确认它是不是唯一门禁；
  这只回答"能不能"，不构成可发布方案；
- 等新线索（固件更新、上游 libnx/switchbrew 出现 BLE central 的资料）再评估。

### 诊断补丁：btm 连接的"客户端未激活"闸门（2026-09-22）

顺着 `result=0x1A` 一路查到了链路的最后一跳：

    FUN_000170c0 case 3: local_2b8 = FUN_000195a0(*param_2)     ← 事件里的 result = 归一化后的原始状态
    归一化表: case 0x85 → 0x1a                                   ← 所以原始状态是 0x85
    0x85 只有一处来源: FUN_00079270（0x1F00 连接处理器）的"间接"分支
        cVar2 = FUN_000c3b30(client_if, addr, 0, mode);  // == 0
        uVar6 = 0x85;  FUN_0007da40(ctx, 0x85, addr, 0xffff, 2, 0);
    FUN_000c3b30: mode==2 时调 FUN_000cf6f0(client_if, 1, addr, 1)
    FUN_000cf6f0: 要求 (client_if-1) < 10 且 控制器层客户端槽的"已激活"标志 != 0
    FUN_000cd7f0: 同一个标志的第一道检查（更早，返回槽指针）

两个检查读的是同一张表：基址 `PTR_DAT_0015e478`，条目步长 `0x50`，标志在 `+0x2399`
（即 `槽基址 + 0x49`）。btm 的连接用的是 `client_if=4`，那个标志是 0，于是：
`FUN_000cd7f0` 返回 0 / `FUN_000cf6f0` 返回 0 → `0x85` → 事件里的 `result=0x1A`。

**诊断补丁**就是把这两条 `cbz`（标志为 0 就跳去返回 0）改成 `nop`：

| 地址 | 原指令 | 原字节 | 新字节 | 所在函数 |
| --- | --- | --- | --- | --- |
| `0xcd820` | `cbz w8, 0xcd828` | `48 00 00 34` | `d5 03 20 1f` | `FUN_000cd7f0` |
| `0xcf7d4` | `cbz w9, 0xcf82c` | `c9 02 00 34` | `d5 03 20 1f` | `FUN_000cf6f0` |

生成（模块 build ID 来自正确的 NCA `ca66270be492a16bab1d779645965bc8.nca`，
NPDM 名 `bluetooth.autog`）：

    python3 tools/ble-re/make_ips.py \
      --elf /tmp/ble-re/nso-010000000000000b.elf \
      --module-id c91c6fc8aa4c39222d6ccfe0fff468543105e59b \
      --edit 0xcd820:48000034:d503201f \
      --edit 0xcf7d4:c9020034:d503201f \
      --out build/exefs_patches/DGLAB-NX-BLE/C91C6FC8AA4C39222D6CCFE0FFF468543105E59B000000000000000000000000.ips

安装：整个 `DGLAB-NX-BLE/` 拷到 `SD:/atmosphere/exefs_patches/`，重启。
撤销：删掉那个目录/文件后重启。

**判读与风险**：

- 如果补丁生效，`btm probe (configured): BleConnect(...)` 之后的行为会变：
  `bt` 事件的 `result/status` 不再固定是 `0x1A / 2`，或者出现 `conn_id` 有效的条目——
  那就说明"控制器层客户端未激活"**就是**唯一门禁，接下来要查的是"谁能合法激活它"。
- 如果事件仍然一样，说明还有别的闸门（或本轮的 client_if 依然不对）。
- **风险**：闸门被跳过后，连接会带着一个"未激活"的槽指针继续走（`x0 = x8`），
  那一槽的字段可能是零/旧值，最坏情况是 `bluetooth` 模块自己出问题。这是**诊断**，
  不是方案；跑完请重启，出问题就把补丁删掉。

#### 第一次试打（2 处 NOP）：结果完全没变 → 先验证补丁有没有生效

补丁按上面的两处 NOP 装到 SD（`atmosphere/exefs_patches/DGLAB-NX-BLE/`）并重启后，btm 探针的
事件**逐字段不变**（`result=0x1A status=2 client_if=4 conn_id=0xFFFFFFFF addr=设备`）。
先把"补丁根本没被加载"这种可能排除掉：

- **build ID 校验**：用同一份固件解出的 `btm` NSO 的 module id 是
  `5a2aa468f272e49ebf0fab8b379cc5b32c1a7409…`，与崩溃报告里 btm 的 `Module Id` **逐字节一致**，
  说明我们解出的固件与实机是同一构建；bluetooth 模块的 `c91c6fc8aa4c39222d6ccfe0fff468543105e59b`
  同理可信。
- **偏移约定**：Atmosphère `ldr_patcher.cpp` 里
  `NsoPatchesProtectedSize = NsoPatchesProtectedOffset = sizeof(NsoHeader) = 0x100`，
  即 IPS 偏移 = `0x100 + 目标地址`——与 `make_ips.py` 的约定一致。
- 文件名大小写不敏感（`psl::IsIpsFileForModule` 用 `isxdigit`，a-f/A-F 都收），所以
  大写文件名没问题。

于是加一条**可见的标记补丁**来回答"补丁到底有没有生效"：`0x6AA` 处理端 `FUN_0005fc50` 里
"没有连接上下文"时的回复状态是 `200 (0xC8)`（`0x5fdb4: mov w2, #0xc8`），把它改成 0：

| 地址 | 原指令 | 原字节 | 新字节 | 作用 |
| --- | --- | --- | --- | --- |
| `0x5fdb4` | `mov w2, #0xc8` | `02 19 80 52` | `e2 03 1f 2a` | "无上下文"改回 `0`（标记） |

（注意 `--edit` 的旧/新字节是**内存顺序**：`0x5fdb4` 要写 `02198052`，不是指令字的
`52801902`。）

标记的判读很直接：装了这版补丁后，`←` 驱动级探针里那些"没有上下文"的连接
（`probe connect client_if=0x0X`）应当从 `rc=0x00300C71` 变成 **`rc=0x00000000`**。

- **变了** → 补丁机制正常，那么上一轮闸门 NOP 也生效了 ⇒ 说明"未建立"来自**另一条**
  上报 `0x85` 的路径（`FUN_000795d0`，它挂在 BLE 事件处理对象的槽位 3 上，由事件驱动），
  下一步就查那一条；
- **没变** → 补丁根本没被加载，问题在交付路径（文件名/放置位置/Atmosphère 版本），
  与固件分析无关。

#### 第二次试打（3 处编辑，含标记）：**开关跳过后连接成功**

`←` 驱动级探针的连接全部从 `0x00300C71` 变成 **`rc=0x00000000`** ⇒ 补丁确实被加载、
两处闸门 NOP 也生效。随后的 btm 探针（同一开机周期、后一个会话）结果：

    btm probe (configured): BleConnect(EA:A8:AC:22:2C:18) rc=0x00000000
    btm probe (configured): connection state event #1
    btm probe (configured): connected handle=4 addr=EA:A8:AC:22:2C:18    ← 连上了
    btm probe (configured): GetGattServices rc=0x00000000 total=0        ← 查得太早
    btm probe (configured): BleDisconnect rc=0x00000000
    btm probe after connect: result=0 status=0(Connected) client_if=4 conn_id=0x00000004 addr=设备

也就是说：**"控制器层客户端未激活"那道闸门就是当时唯一的门禁**；跳过它之后 Switch 真的与
设备建立了连接（`GetConnectionState` 给出 handle=4 + 设备地址，用户侧事件 `status=0`、
`conn_id=4`）。

**但还差第二个前置**：同一版补丁、同一个开机周期里，如果 **只**跑 btm 探针（不先跑 `←`），
连接又变回 `result=0x1A`（原始状态 `0x85`）。两次运行的差别是 `←` 驱动级探针会
`btdrvInitializeBle` + `btdrvEnableBle`——**把 BLE 栈打开**。所以目前的经验是：

    先跑 ←（打开 BLE 栈） → 再跑 StickR/B（btm 连接）

探针已把这条提示写进日志（连接失败时提示先跑 `←`），GATT 读取也改成"等
`btmAcquireBleServiceDiscoveryEvent` + 最多 8 次重试"，因为连接刚建立时服务发现还没跑完。

## 下一步（2026-09-22 更新，结论已被「当前状态（2026-09-25）」取代）

这一段是 2026-09-22 收束时写的，当时连接还没通。**现状看文中的「当前状态
（2026-09-25）」**：连接在 2026-09-24 就跑通了（补丁 + 先打开 BLE 栈），传输层在
2026-09-25 接上。这里保留的价值是它列的复工顺序——先静态、再单变量实机、动 BLE 归属的
实验单独开机周期——这几条一直有效。

形状问题已经排除，**扫描也做通了**，现在只剩"发起连接"这一件事。三条路各自的状态
（细节与日志见 `docs/ble-poc.md`）：

- **sysmodule → btdrv**：`StartBleScan` + 厂商数据过滤器（AD `0xFF`、公司号 `0x000A`）
  能稳定拿到设备记录（地址、`status=2`、`addr_type=1`、rssi≈-40、AD 内容可解）；
  `ConnectGattServer`(65) / `TriggerConnection`(23) 在本地检查通过后返回
  `Bluetooth/0x1806`。注意：这个码**不是**连接专用的拒绝码，它在本模块里是通用的
  "参数/状态不对"，具体产出点还没定位（见上文「`0x1806` 不是"连接被拒"的专用码」）。
- **NRO(applet) → btm:u**：`btdevConnectToGattServer` 返回 `0`（受理，不像 sysmodule 那样
  被 `Sf`/`Btm` 拒），但只触发一次连接状态事件、`GetConnectionState` 恒为 `total=0`；
  它的 smart-device 与 general 扫描都不报结果（连控制台自己存储的参数也一样），
  **扫描事件从不触发**。
- **sysmodule → base `btm`（新，2026-09-22）**：命令形状已逐条核对、与 libnx 一致（见
  上文表格），`btm:u` 的 ARUID 门槛也已定位，所以探针的第一步是先
  `RegisterAppletResourceUserId` 登记 NRO 的 ARUID，再扫描、连接、读 GATT 表。
  这一轮实机数据还没有，`StickR` 探针（v18）就是为它准备的。
- 设备侧没问题：手机一点就连上；广播里带 flags、厂商数据（公司号 `0x000A`，其后 4 个零
  字节）和本地名 `47L121000`。**广播里到底有没有服务 UUID `0x180C` 已经定论（2026-09-25）：
  没有**——AD 是定长数组，只有那三条，所以按 UUID 过滤的 smart-device 扫描看不到设备，
  只有厂商数据过滤的 general 扫描能看到。

因此复工的路线按优先级是：

1. **先做静态（现在的第一步）**：查清 btm 与 btdrv 的 BLE 归属规则——谁有资格
   `InitializeBle`/`EnableBle`、`0x668F` 的确切含义、btm 的 worker 在什么条件下走到
   `0x37d50` 的终止路径（`svcBreak(0,msg,4)`），以及 `RegisterAppletResourceUserId` 登记
   的身份活多久。没有这一层，任何"btm + btdrv 混用"的实机实验都可能再次把 btm 弄崩。
2. **再设计安全的实机实验**：只读探针（`StickR`）确认扫描面；任何要动 BLE 归属的实验必须
   单独会话、单独开机周期，并且一次只动一个变量。
3. **btm 的其余前置条件**：若扫描通了而连接不通，再查 auto-connection
   （`btm:u` 的 `StartBleScanForPaired`）、设备是否必须先登记进 btm 的列表
   （`btmAddDeviceInfo` / 配对流程），以及 `btmBleGetGattClientConditionList` 里到底有什么。
4. **栈的准入（收尾用）**：若 base `btm` 这条路也走不通，且 `0x1806` 的产出点能定位到
   "非任天堂客户端"的检查上，就按"架构上不可行"收尾：扫描可用、请求形状正确、设备可连、
  连接被栈拒。诊断用的 IPS 补丁只用来确认"某个检查是不是唯一门禁"，不是方案。

重开工需要的入口都在仓库里：`tools/ble-re/`（`nso2elf.py`、`peek.py`、`find_xref.py`、
`abi_sizes.py`、`make_ips.py`、`ghidra/`）、本页上面的「命令 → 请求形状」表、
「base `btm` 服务」一节、`docs/ble-poc.md` 的探针说明（`←` 驱动级扫描、空闲屏 `Y`
applet 侧连接探针、`StickR` base-btm 探针）。用到的地址与命令 case 都可用
`analyzeHeadless ... -postScript DecompileAt.java <地址>` 复现（命令 case 地址由
`0x11884e` 的字节表 + `0x1d4d4` 的分支表算出）；只被跳转表引用、没有函数入口的 case
用 `tools/ble-re/ghidra/DecompileForce.java`。

### 对上游的价值（libnx / switchbrew）

> **下面那 4 条已经重写过**：第 1 条（cmd 62 变成 0x40 字节）和第 4 条（ABI 与
> `btdrv_types.h` 不一致）已被上面的更正推翻，不能按原样提交；第 2 条（cmd 40 关会话）
> 与第 3 条（`btm:u` 是 applet 专用）仍然成立。重写后的草稿是
> `tools/ble-re/upstream.md`（3 条，顶部写明哪两条作废），提交动作暂缓。

这次的结果对 libnx 是**新信息**：`btdrv.h` 的版本注记只到 12.x，`btmu.c` 最后一次改动是
2020-12-29，整个仓库没有 20.0.0+（`bluetooth` → `bluetooth.autog`）的记录。已经有实机证据、
可以直接提上去的有四条（草稿见 `tools/ble-re/upstream.md`）：

1. **cmd 62 的载荷变成 0x40 字节**：libnx 的 0x14 字节让注册稳定失败（`result=0x37 /
   client_if=0xFF`），换成 0x40 字节内联块后拿到 `result=0 / client_if=0x02`；改成指针缓冲
   反而返回 `F601`。
2. **cmd 40（`GetChannelMap`）会让固件关掉调用者的会话**：之后同会话所有调用都是
   `F601 = KernelError_ConnectionClosed`，两次独立实机复现。
3. **`btm:u` 是 applet 专用**：sysmodule 里用 libnx 的封装（无效 ARUID）会"扫描成功但没有
   事件"；填真实 applet ARUID 时服务框架直接拒（`Sf/0x60A`）。
4. **20.0.0+ 的 btdrv ABI 与 `btdrv_types.h` 不再一致**：给出一张"固件要拷多少字节 vs libnx
   类型尺寸"的对照表，说明这不是挪命令号能修的，并附上"从适配层反推形状"的方法。

按 libnx 的惯例，1/2/3 适合提 issue（附实机日志片段与最小复现），4 适合提文档注记；等结构
字段逐个对出来再考虑 PR。提交动作留给你决定（也可以用同一份草稿贴到 switchbrew 的
`BTM_services` 页）。

## 主机侧流程（当前）

`pocRunBtdrvIdentityProbe`（NRO 里按 `→`（十字键右），或开机后第一次会话自动跑）现在是"运输形状"
的流程，不再是形状对照：本机身份读取 → **固定形状的 GATT client 注册**（`client_if`）→
（配了地址时）**同会话内连接** → `InitializeBle`。只做只读与本地注册，不写 BF、不改
可见性/广播、不动电台开关、不接触设备。

### 过程记录（阶段性结论，已被最终判定取代）

**命令号没有漂移，判定 (A) 被否掉**。身份探针在实机上拿回了
`GetAdapterProperty(Address)` = `A4:38:CC:87:FD:2B`、`Name` = `Nintendo Switch`、
`IsBluetoothEnabled` = 1、`btmGetState` = 6（`module` 与命令号错位都拿不到这些数据）。

但同一份日志里，BLE 侧命令（`GetChannelMap` 40、`GetBleChannelMap` 258、
`GetBleManagedEventInfo` 79、`InitializeBle` 46）全部返回 `0x0000F601` =
`MAKERESULT(Module_Kernel, KernelError_ConnectionClosed)`，而**同一会话更早**的驱动级
探针里 `InitializeBle` 返回的是 `0`。

干净启动下的那一轮（`docs/ble-poc.md` 第十五次实机）把这条因果链钉住了：**`btdrvGetChannelMap`
（cmd 40）自己就是那一串 `0xF601` 的源头**。它之前每条命令都返回 0，它开始返回
`ConnectionClosed`，而同一会话里它**之后**的每条命令也都是同一个错误 —— 固件在收到
这条请求后直接把我们的会话关掉了，`GetBleChannelMap` / `GetBleManagedEventInfo` /
`InitializeBle` 都只是被殃及。也就是说：

- libnx 对 **cmd 40** 的绑定在 22.5.0 上是坏的（命令号或请求形状对不上，严重到固件关会话）
  这是 (A) 类问题，改我们自己的调用即可；
- `InitializeBle`（46）本身没问题（第十三轮里在 cmd 40 之前调过，返回 0）；
- 之前那条"注册失败 `client_if=0xFF`"和"`InitializeBle` 返回 0xF601"都是**被 cmd 40
  殃及**测出来的，必须在干净状态下重测。

对照扫描那部分：三个常见厂商 ID 全部 `btdevStartBleScanGeneral rc=0` 但
`events=0 polls=16 devices=0`，`btdevStartBleScanSmartDevice` 也一直 `total=0`。也就是说
**连扫描事件本身都不来**，与"过滤器没匹配上"相比更支持"btm 根本没有替这个进程跑扫描"
（pattern 字段语义未知，所以仍不是严格证明）。

随后一轮（`docs/ble-poc.md` 第十四次实机）里驱动级探针已经改成手动，整个会话**没有碰过
btdrv 的 BLE**，三种过滤器的扫描仍然全是 `events=0 polls=16 devices=0`。所以"扫描不出
事件"不是被我们自己的 btdrv 调用污染的，`btm` 的扫描路径对后台 sysmodule 本身就不产生
事件——当时把它当成"最接近 (B) 的一条实证"，后来证明与注册失败无关：注册换对形状就成功了，
扫描这条仍然是独立未解的问题。

为了不再依赖按键，身份探针改成**开机后第一次会话自动执行**（第一次会话里若用户主动要求
别的动作则跳过，留到下一次安静会话）。

同一份日志还暴露出**两条不同的 ClientRegistration 载荷**：
`result=0x37 / client_if=0xFF / status=0` 与 `result=0 / client_if=0x00 / status=0xFF`。
按 libnx 的 `BtdrvBleEventInfo::client_registration`（`u32 result; u8 client_if; u8 status`，
status 只有 0/1）后者不合法，这正是"事件布局/语义在 22.5.0 上与 libnx 不符"的方向，
与命令号是否漂移的疑问指向同一个地方。

身份探针（`Right` / `StickR`）随后跑通了，结论见上：命令号没漂移，卡点是 BLE 会话状态。

## 边界

- 本文的固件结论来自静态分析；文中引用的实机日志出自 `docs/ble-poc.md` 的实机记录；
- 静态结论本身**没有**在实机上逐条复验（更正后的形状表来自固件代码，尚未用它做过新实验）；
- 没有写任何固件补丁、没有 mitm sysmodule、没有改传输层与 IPC 定义；
- 固件、密钥、解包产物都不入库（见 `.gitignore`）。
