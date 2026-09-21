# BLE 固件只读逆向（HOS 22.5.0）

本文记录按根 `AGENTS.md` §15 的要求，对"Switch 后台直连 BLE 外设"这条路线做的
**只读**固件逆向：目标是把 `docs/ble-poc.md` 留下的失败点归类成
**(A) 绑定/结构漂移**（改我们自己代码即可）、**(B) 固件按调用者身份/白名单封死**
（需要 exefs patch 或放弃）、还是 **(C) 蓝牙栈里没有通用 central 实现**（放弃）。

**当前状态：判定尚未完成。** 已经确认了模块归属与 IPC 层的形状，但在"固件命令表索引
↔ libnx 命令号"这一环上出现互相矛盾的证据，仍需一小步才能收口；在此之前不给结论。
本轮不写补丁、不改 `sysmodule/source/transport/ble_poc.c`、不做实机验证。

## 素材与工具链（可复现）

素材是本机 Ryujinx 安装的固件（`bis/system/Contents/registered`，234 个 NCA）与
`system/prod.keys`。固件版本用系统版本 title 的 RomFS 直接核对：

    0100000000000809 (Data) → RomFS 里的 file 含 "NintendoSDK Firmware for NX 22.5.0-1.0"

与 11 次实机的环境（HOS 22.5.0 / AMS 1.11.2）一致。工具与命令见
`tools/ble-re/README.md`；其中 `nso2elf.py` 用仓库自己构建的
`sysmodule/DGLAB-NX-Core.nso` / `.elf` 做过往返自测（PASS，段内容逐字节一致）之后才用于
固件模块。密钥、固件、解包产物都只落在 `/tmp`，不入库。

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

## 判定（2026-09-21，最终）

**是 (A)：libnx 的请求形状过时，改我们自己的调用即可，不需要任何固件补丁。**

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

## 参数布局对照（2026-09-21，第 1 步）

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

## 下一步（最小增量）

**2026-09-21 决定：先停在这里，把成果固化进文档，移植工作以后再做。**（BLE 直连的判定已经
完成——不需要固件补丁；剩下的是移植工作量。）重新开工时按这个顺序：

1. **扫描链先行**（这是后面一切的前置）：把 `0x13350`（槽 0x37，固件要"地址+结构+两个
   u32"）、`0x13410`（槽 0x38）、`0x137b0`（槽 0x3D，0x40 字节结构）三个实现体挖到 BLE 管理
   器的方法，把结构字段对出来；然后按固件形状做"启动扫描 / 读扫描结果"的探针，实机确认能
   扫到 Coyote（顺带验证地址）。`btm:u` 那条路已经排除（见上面的 `Sf`/`Btm` 结论）。
2. **连接**：扫描到设备之后再用管理器的 `client_if` + `btdrvConnectGattServer`；若仍返回
   `Bluetooth/0x14F`，按同一方法核对 cmd 65 的结构，以及是否需要先让协议栈"见过"该地址
   （`TriggerConnection` 那条要 6 字节地址 + 0x2BE 结构，也是待对齐对象）。
3. **服务发现 → 订阅 → B0/B1**：每一条都先读适配层形状、再改我们的请求，再实机。

重开工时需要的入口都在仓库里：`tools/ble-re/`（`nso2elf.py`、`peek.py`、`find_xref.py`、
`abi_sizes.py`、`make_ips.py`、`ghidra/`），本页的「参数布局对照」「扫描那一段的固件形状」
两张表，以及 `docs/ble-poc.md` 里的探针说明。

### 对上游的价值（libnx / switchbrew）

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

`pocRunBtdrvIdentityProbe`（NRO 里按 `Right`，或开机后第一次会话自动跑）现在是"运输形状"
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

- 本文全部来自静态分析，**没有任何实机验证**；
- 没有写任何固件补丁、没有 mitm sysmodule、没有改传输层与 IPC 定义；
- 固件、密钥、解包产物都不入库（见 `.gitignore`）。
