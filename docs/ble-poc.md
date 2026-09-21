# BLE Transport PoC

本文记录 **BLE 模式**（sysmodule 直接连接 DG-LAB 设备）的实机可行性验证，环境
HOS 22.5.0 / AMS 1.11.2 / Switch 1。**结论：这条路走不通，模式已搁置**（见根
`AGENTS.md` §15）；现行传输模式是 WebSocket，见 `docs/dglab-socket.md`。

按根 `AGENTS.md` §15 的前置条件，对这套结论的只读固件逆向下文记为
`docs/ble-re.md`：模块归属与固件侧 IPC 形状已经查清，但"固件命令表 ↔ libnx 命令号"
的对应关系尚未定论，因此**本页的结论暂时维持不变**。

11 次实机记录的原文归档在 `docs/history.md` 的「ble-poc：11 次实机记录」。

## 代码位置

| 内容 | 位置 |
| --- | --- |
| PoC 实现（唯一接触蓝牙的地方） | `sysmodule/source/transport/ble_poc.c`、`sysmodule/include/dglab/transport/ble_poc.h` |
| 临时 IPC 命令与结构 | `common/include/dglab/ipc_poc.h` |
| CMIF 布局规则 | `sysmodule/include/dglab/ipc_cmif.h` |
| NRO 观察与操作界面 | `nro/source/ble_poc_view.c` |

## 为什么用 btdev（以及 btdrv 为什么被放弃）

libnx 提供两套 BLE 接口：`btdrv` 是蓝牙驱动的底层封装（btm-sysmodule 用的就是它），
`btdev` / `bt` / `btm:u` 是更高层的类型化封装。实机结果是：

- btdrv 的 BLE 事件通道在 HOS 22.5.0 上只返回空载荷，拿不到任何可用的扫描结果；
- `btdevInitialize`、`btdevStartBleScanSmartDevice` 等调用在后台 sysmodule 里全部返回
  `rc=0`，说明这套服务可用，ARUID=0 也没有被拒绝。

因此实现改为 **btdev**：BLE 所有权仍在 sysmodule，代码也比 btdrv 版本短。用到的接口
见 `sysmodule/source/transport/ble_poc.c`。

### 扫描过滤器：广播里的服务 UUID 从 0x1812 变成了 0x180C

**2026-09-21 更新（用户实测）**：设备固件更新之后，用手机 BLE 扫描工具看到的广播服务
UUID 是 **`0x180C`**，不是 `0x1812`：

    名称            = 47L121000
    地址            = EA:A8:AC:22:2C:18   ← 与 config/dglab-ble-address.txt 一致
    Services UUIDs  = 180C

早期固件的广播里是 `0x1812`（HID 服务），当时 `0x180C` 只在连接后的 GATT 服务列表里；
固件更新之后反过来——**广播里就是协议服务 UUID 0x180C**。所以按 `0x1812` 过滤的扫描在
新固件上等于把设备全部滤掉（第三十四次实机前一直如此）。

现在 `POC_UUID16_ADVERTISED_SERVICE` 是 `0x180C`，旧的 `0x1812` 保留为
`POC_UUID16_LEGACY_ADVERTISED_SERVICE` 供对照；驱动级探针分三种模式跑：
不加过滤器、`0x180C` + 启用过滤器、`0x180C` + 关闭过滤器。

### 新固件的广播与 GATT 表（2026-09-21 手机实测）

    广播：
      名称             = 47L121000
      地址             = EA:A8:AC:22:2C:18
      Manufacturer ID  = 0x000A          ← 厂商自定义数据（AD 类型 0xFF）里的公司号
      Services UUIDs   = 180C

    连接后的 GATT 表：
      0x180C 服务      → 0x150A（写）、0x150B（通知）      ← DG-LAB 协议服务
      0x180A 设备信息  → 0x1501、0x1502、0x2A25、0x1500、0x2A59

按 SIG 官方分配表核对（`bluetooth-SIG/public` 的 assigned_numbers）：

| UUID | 归属 |
| --- | --- |
| `0x1812` | SIG 分配的 **Human Interface Device** 服务（旧固件广播里借用的号） |
| `0x2A25` | SIG 分配的 **Serial Number String** 特性 |
| `0x000A` | SIG 公司标识 **Qualcomm Technologies International (QTIL)**（原 CSR）——BLE 模组厂，不是 DG-LAB |
| `0x180C` / `0x150A` / `0x150B` / `0x1500` / `0x1501` / `0x1502` / `0x2A59` | **不在任何 SIG 分配表里**（服务表 `0x180A` 直接跳到 `0x180D`，特性表 `0x2A55` 跳到 `0x2A5A`，`0x150x` 整段没有），是 DG-LAB 按基础 UUID 拼的私有号 |

也就是说设备用的就是 `docs/dglab-protocol.md` 记的那套私有号，`0x180A`/`0x2A25` 只是借用了
标准号的"位置"。驱动级探针因此多了一种过滤器：**厂商自定义数据 + 公司号 `0x000A`**
（AD 类型 `0xFF`，两个字节掩码 `FF FF`）。

## 安全性

- PoC 只在按下 `A` 时启动，开机不会接触蓝牙。
- 不写 BF 指令，因此不会修改设备断电保存的软上限或平衡参数。
- 连接后每 100ms 只写一条"强度不变化 + 双通道空闲"的 B0，设备不会有任何输出。
- 只有按 `X` 才会写"双通道绝对置零"，这是把输出降到零的安全方向。

## 安装

BLE 模式已搁置，安装步骤见 `README.md`（`make` 或 `make -C sysmodule package`）；PoC
只要 sysmodule 装好、NRO 能启动即可。

## 操作

| 按键 | 动作 |
| --- | --- |
| `A` | 开始 PoC（扫描 0x1812 → 连接 → 发现 → 订阅 → 周期写入） |
| `X` | 写入"双通道绝对置零"的 B0（序列号 1，预期设备回 B1） |
| `B` | 读取电量特征（0x180A / 0x1500） |
| `R` | 重启会话（断开并重新扫描） |
| `L`（左肩键） | 开关 100ms 的 B0 保活写入 |
| `Y` | 断开连接 |
| `ZL` | 重新扫描（默认顺序：0x1812 → 0x180C） |
| `ZR` | 只用协议服务 UUID `0x180C` 扫描（对照） |
| `↑`（十字键上） | 只用广播里的 UUID `0x1812` 扫描 |
| `↓`（十字键下） | 用 btm 的 general 过滤器（厂商数据）扫描（对照） |
| **`←`（十字键左）** | 手动运行一次 btdrv 驱动级探针（不再自动跑，理由见下） |
| `→`（十字键右） | 运行一次 btdrv 身份探针（读本机名称/MAC/信道图 + GATT 注册序列，见下） |
| `StickL`（按下左摇杆） | 对照扫描：用常见厂商 ID（Apple/Microsoft/Samsung 轮换）扫描，验证扫描机制本身 |
| `StickR`（按下右摇杆） | 同 `→`：身份探针 |
| `-` | 停止 PoC（清理并退出） |
| `+` | 退出 NRO |

方向键指的是**十字键**，不是左肩键也不是摇杆：左肩键 `L` 只开关自动写入，按下左摇杆
`StickL` 是对照扫描（第一轮实机就有人按错过，见 `docs/history.md`）。

进这个页面时用的那个键（菜单里是 `A`）在页面第一帧仍然算"刚按下"，曾经因此直接起了一次
会话、连带把身份探针跑掉；页面现在会吞掉第一帧的按键，所以"进去直接按 `←`"就是干净的
第一个会话。

动作键在 PoC 未运行时（界面显示 `state: idle`）会自动先启动一次运行，
所以空闲界面直接按 `Up`／`Down` 也能工作。

## 命令号是否漂移：`Right` 身份探针

`docs/ble-re.md` 卡在"libnx 的 btdrv 命令号在 HOS 22.5.0 上是否仍然成立"：我们所有调用
都返回成功，而**命令号漂移的调用也会返回成功**（打到别的命令上，什么都不做）。要分开
这两种情况，只能问固件要**它伪造不了的答案**——本机适配器自己的名字、MAC、信道图。

按 `Right`（或 `ZL` 之外的任意动作键先起一次会话，再按 `Right`）会依次执行：

1. `btdrvInitialize` + `IsBluetoothEnabled`（cmd 100）+ `btmGetState`（交叉核对电台状态）；
2. `GetAdapterProperty(Address / Name / ClassOfDevice)`（12.0.0+ 的 ABI）；
3. `GetChannelMap`（cmd 40）与 `GetBleChannelMap`（cmd 258，高命令组）；
4. **BLE 未初始化时的空转排水**——这段时间里出现的事件不可能由我们引起；
5. `InitializeBle` → 排水 → `RegisterGattClient` → 排水 → `UnregisterGattClient(0xFF)` →
   排水，把每条事件的前 16 字节和 ClientRegistration/ScanResult 的字段原样打出来。

这只是读和本地注册：不写 BF、不改可见性/广播、不动电台开关、不接触任何 DG-LAB 设备。
判读规则见 `docs/ble-re.md` 的「主机侧验证」。

注意：这个页面**复用主菜单已经持有的那一个 `dglab` IPC 会话**。sysmodule 是
`max_sessions=1`（一次只服务一个会话），页面自己再 `smGetService` 会被拒（`0x615`，
看起来像"sysmodule 没运行"，但 socket 页却正常）。2026-09-21 之前的菜单化版本就是
这么退化的，见 `nro/AGENTS.md`。

## 扫描机制本身是否可用：`StickL` 对照扫描

三次过滤器实验（`Up`/`ZR`/`Down`）都是 0 结果，但 btm 的 general 扫描是**厂商数据过滤
器**，而系统里存的是任天堂自己的 company ID（`0x0553`）——它在一个普通房间里本来就
匹配不到任何东西。所以"扫不到"既可能是"btm 根本不替我们扫描"，也可能是"过滤器没匹配
上"，前几轮无法区分。

`StickL` 用手机和耳机真的会广播的厂商 ID 轮换扫描（Apple `0x004C` → Microsoft `0x0006`
→ Samsung `0x0075`），这是阳性对照：

- **扫到任何设备** → btm 的扫描机制对我们这个进程是可用的，问题在设备/过滤器一侧；
- **仍然 0 结果** → 不构成结论（pattern 字段仍可能把一切排除），但和"事件数为 0"放在
  一起就更支持"扫描根本没有为我们运行"。

NRO 会把收到的 sysmodule 日志同步写到：

    sdmc:/switch/DGLAB-NX/logs/dglab-ble-poc.log

目录 `sdmc:/switch/DGLAB-NX/` 与它下面的 `config/`、`logs/` 在 NRO 启动时自动创建。
打不开时退回到 `sdmc:/dglab-ble-poc.log`；NRO 启动后第二行会显示日志文件的实际状态
（`log: ...` 或 `log: unavailable`）。

NRO 会在连接 sysmodule 之前先输出 `console ready`、日志文件状态、`querying sysmodule...`，
所以如果它卡住，屏幕上的最后一行就是卡住的位置。

## 每一步的预期

1. 打开 NRO：显示 `state: idle`、IPC 版本号，说明 sysmodule 在运行。
2. 按 `A`：日志依次出现

       poc start aruid_low=0x...
       btdevInitialize rc=0x00000000
       btdevAcquireBleScanEvent rc=0x00000000
       btdevStartBleScanSmartDevice(0x1812) rc=0x00000000
       state=scanning

3. 扫描：找到设备时打印

       0x1812 scan found N device(s)
         0x1812 scan 0 addr=XX:XX:XX:XX:XX:XX

   长时间没有任何结果时每 10 次轮询打印一条
   `0x1812 scan poll N rc=0x... total=0`；12 秒后超时会自动退回 `0x180C` 再试一次。
4. 连接：`btdevConnectToGattServer rc=0x...`、`state=connecting`，随后
   `conn 0 handle=... addr=...`，里程碑 `+conn`。
5. 发现：`btdevGetGattService(0x180C) rc=0x... flag=1`、`state=discovering`，
   然后是 `char 0x150A ... prop=0x..` 与 `char 0x150B ... prop=0x..`
   （`0x150A` 应该有 write 位，`0x150B` 应该有 notify 位）。
6. 就绪：`btdevEnableGattCharacteristicNotification rc=0x00000000`、`state=ready`，
   随后 `b0 idle write #1 ...`，`b0 writes` 计数持续增长且 `failed 0`。
7. 通知：转动设备本体的强度拨轮，应该出现 `gatt op size=4 data=B1...` 与
   `B1 sequence=0 A=.. B=..`，`notifications` 计数增长。
8. 按 `X`：日志出现 `b0 zero write, sequence 1, expecting B1`，随后应收到
   `B1 sequence=1 ...`，里程碑 `+b1` 点亮。
9. 按 `B`：应显示 `battery value=..`，里程碑 `+bat` 点亮。

## 扫描没有结果时怎么排查

驱动级探针（`←`，十字键左）现在每个阶段都会给出
`fetches=/empty=/events=/scan_results=` 四个计数，
并且把前 8 条非空事件的 `type=` 和 16 字节原始载荷打出来。判读：

- `empty==fetches`：事件队列一直是空的（rc=0、type=0、全零载荷），问题在**事件通路**，
  与设备无关——先别怀疑广播内容；
- `events>0` 但每条都是 `type=0` 且前 16 字节全零：看探针新打出的 `nonzero=/first=0x..`
  两行——数据从哪个偏移开始就说明返回布局和 libnx 的 `BtdrvBleScanResult` 不一样
  （2026-09-21 第二十七次实机就是这个样子）；
- `events>0` 但 `scan_results=0`：事件通路是活的但内容不对。看 `type=`：如果连
  `ScanFilter`（我们自己的加/清过滤器动作的回执）都没有，说明管理器没在按会话投递；
  有 `ScanFilter` 却没有 `ScanResult` 才是"扫到了但被过滤/设备没在广播"；
- `results>0` 但没有 `coyote 3.0 found`：广播内容与预期 AD 类型不一致，
  日志里的 `ad type=.. data=..` 就是它实际广播的内容，此时可以按 `ZR` 直接连接，
  先把 GATT 与通知部分验证掉。

探针在开扫之前会显式 `ClearBleScanFilters` + `EnableBleScanFilter(false)`，
所以"上一轮留下的过滤器把结果全挡掉"这条不会再来捣乱。

## 里程碑

NRO 顶部的 `milestones` 一行用 `+`/`.` 表示是否达成：

    ble scan found client conn svc char notify b0 b1 bat

- `ble`：拿到 BLE 事件句柄
- `scan`：扫描已启动
- `found`：发现目标设备
- `client`：GATT client 注册成功
- `conn`：连接成功
- `svc`：0x180C 服务解析成功
- `char`：0x150A / 0x150B 特征解析成功
- `notify`：通知订阅已下发
- `b0`：至少写入成功一次
- `b1`：收到 B1 报文
- `bat`：读到电量

## 实机结论

11 轮实测（原文见 `docs/history.md`）的主要发现：

| 轮次 | 发现 |
| --- | --- |
| 1~2 | btdrv 的 BLE 事件通道 `events=0`，调用全部返回成功但没有任何事件；换事件源、显式 `btdrvEnableBle` 都无效 |
| 3 | 句柄有回调但载荷全为 0；空队列也返回 `rc=0`/`type=0`（PoC 曾据此误判） |
| 4 | `bt` + `btm:u`（btdev）在后台 sysmodule 里可用，ARUID=0 未被拒绝，但 15 秒扫不到设备 |
| 5 | 手机扫描显示广播里是 `0x1812`（`0x180C` 只在连接后的 GATT 服务里）；改按 `0x1812` 过滤 |
| 6~7 | btm 的 general 过滤器固定为任天堂 company ID `0x0553`，smart device 过滤器为空（`size=0`）；按地址直连被拒（`0x0005568F`，module `0x8F`、description `0x2AB`） |
| 8 | 三种过滤器下 `events=0`，扫描事件本身就没触发过；general 过滤器匹配不到 DG-LAB |
| 9~10 | 修 PoC 自己的调度与日志截断问题，探针改为每次会话自动执行 |
| 11 | 驱动级探针：调用全被接受，但 `ScanResult` 载荷全 0，客户端注册事件带 `result=0x00000037`、`client_if=0xFF`（注册失败） |

### 第十二次实机（2026-09-21，菜单化之后第一次真正跑起来）

菜单化那次提交（`940bd6a`）之后 PoC 页面一直打不开：`main()` 全程持有 `dglab` 会话，而
页面自己又 `smGetService` 开第二个，被 `max_sessions=1` 拒掉（`0x615`，看起来像没装
sysmodule，但 socket 页正常）。NRO 侧改成复用调用方的会话后页面恢复。这次日志还带来：

- **对照扫描**：Apple `0x004C`、Microsoft `0x0006`、Samsung `0x0075` 三个常见厂商 ID
  各扫一轮，全都是 `btdevStartBleScanGeneral rc=0` 但 `events=0 polls=16 devices=0`。
  也就是说连**扫描事件本身**都没有，不只是"没匹配到设备"；
- **驱动级探针**（每次会话自动跑）：phase 0（无过滤器）拿到 2 条
  `type=0 raw=37000000FF000000` 与 1 条退化 `ScanResult`（`status=0`、地址全 0、
  `entries=0`）；phase 1（过滤 0x1812）拿到 3 条 `type=0 raw=0000000000FF0000`、无扫描结果；
- 两条 ClientRegistration 载荷**不一样**：`result=0x37 / client_if=0xFF / status=0` 与
  `result=0 / client_if=0x00 / status=0xFF`。按 libnx 的结构体定义，后者的 `status=0xFF`
  不是合法的注册状态，说明 22.5.0 的事件布局/语义可能和 libnx 的定义对不上；
- ARUID 是 `0x8D`（NRO 作为 applet 的真实 ARUID，不再是早期轮次的 0）；
- `FAIL scan rc=0x00006359` 是 libnx 自己的 `LibnxError_Timeout`（`module=345`、
  `description=49`），也就是 PoC 的重试耗尽，不是固件返回的错误。

### 第十三次实机（2026-09-21，身份探针）

按 `Right` 的身份探针结果（同一次会话里自动跑的驱动级探针**已经**先调过
`btdrvInitializeBle`）：

    identity: btdrvInitialize rc=0x00000000
    identity: IsBluetoothEnabled rc=0x00000000 value=1
    identity: btmGetState rc=0x00000000 state=6
    identity: address rc=0x00000000 size=6 A4:38:CC:87:FD:2B
    identity: name rc=0x00000000 size=15 'Nintendo Switch'
    identity: class rc=0x00000000 size=3 3C0400
    identity: channel map rc=0x0000F601 00000000000000000000
    identity: ble channel map rc=0x0000F601 0000000000
    identity idle (BLE not initialized): GetBleManagedEventInfo rc=0x0000F601
    identity: InitializeBle rc=0x0000F601

两条结论：

1. **命令号没有漂移**。`GetAdapterProperty(Address/Name)` 拿回了本机真实 MAC
   `A4:38:CC:87:FD:2B` 与 `Nintendo Switch`，`IsBluetoothEnabled=1`、`btmGetState=6`；
   命令号错位的话这些数据不可能出现。`docs/ble-re.md` 里那条"绑定是否漂移"的疑问，
   答案是否。
2. **BLE 侧命令全部返回 `0x0000F601`**，按 libnx 的解码是
   `MAKERESULT(Module_Kernel=1, KernelError_ConnectionClosed=123)` —— 内核的"连接已关闭"。
   注意同一会话里**更早**的驱动级探针调 `btdrvInitializeBle` 返回的是 `0`，也就是
   这条命令本身没问题，是**状态**问题：BLE 管理器把"谁初始化了它"记在会话上，一旦那个
   会话结束（探针结尾的 `btdrvExit()`），后面的 BLE 侧命令就都拿不到那条内部连接。

因此驱动级探针**不再在每次会话开始时自动运行**（`Left` 仍可手动跑），并且会话开始会先等
300 ms 处理用户排队的动作，让新会话的第一次蓝牙操作就是用户要的那个探针。验证方法：重启
主机后直接按 `Right`，看 `InitializeBle` 是否回到 `0`、以及排水到的
ClientRegistration 里 `client_if` 是不是仍然是 `0xFF`。

### 第十四次实机（2026-09-21，只有扫描）

这次日志里**没有任何 btdrv BLE 调用**（驱动级探针已经改成手动），只有 `btm:u` 的扫描：

    0x1812 scan summary: events=0 polls=16 devices=0
    0x180C scan summary: events=0 polls=16 devices=0
    general scan summary: events=0 polls=16 devices=0

也就是说，**在一个完全没有碰过 btdrv BLE 的会话里，btm 的扫描依然一个事件都不产生**。
"我们的调用把 BLE 状态弄脏了"解释不了这一条；扫描路径本身对后台 sysmodule 就是不出事件。
另外这轮用户按的是 `R` 肩键（日志里十几次 `action queued 5`，那是 rescan）而不是十字键
右，所以身份探针又没跑成——于是把身份探针改成了**开机后第一次会话自动执行**。

### 第十五次实机（2026-09-21，干净启动下的身份探针）

这轮 `identity: first session after boot, running the probe before any scan` 说明主机是刚
重启过的（探针在会话一开始、任何扫描之前就跑）：

    identity: btdrvInitialize rc=0x00000000
    identity: IsBluetoothEnabled rc=0x00000000 value=1
    identity: btmGetState rc=0x00000000 state=6
    identity: address rc=0x00000000 size=6 A4:38:CC:87:FD:2B
    identity: name rc=0x00000000 size=15 'Nintendo Switch'
    identity: class rc=0x00000000 size=3 3C0400
    identity: channel map rc=0x0000F601 00000000000000000000
    identity: ble channel map rc=0x0000F601 0000000000
    identity idle (BLE not initialized): GetBleManagedEventInfo rc=0x0000F601
    identity: InitializeBle rc=0x0000F601

**`btdrvGetChannelMap`（cmd 40）就是那一串 `0xF601` 的源头**：它之前每条命令都是 0，
它自己开始返回 `0xF601`（`MAKERESULT(Module_Kernel, KernelError_ConnectionClosed)`），
而同一会话里它之后的每条命令也都是 `0xF601`。也就是说固件在收到这条请求后**把我们的
会话关掉了**，后面的 `GetBleChannelMap` / `GetBleManagedEventInfo` / `InitializeBle`
都只是被殃及。对照第十三轮：驱动级探针里 `btdrvInitializeBle` 返回 `0`（那时还没人调过
cmd 40），所以 cmd 46 本身没问题。

据此改动：身份探针改成**先做 BLE 侧测量、把两条 channel map 放到最后**，并给 START 增加
`DGLAB_POC_START_FLAG_SKIP_PROBES`：用扫描键（`Up`/`ZL`/`ZR`/`Down`/`StickL`）从空闲界面
起会话时不跑任何探针，这样才有一轮**完全不碰 btdrv BLE** 的干净扫描。

### 第十六 / 十七次实机（2026-09-21，干净扫描 + 干净探针）

两次分别做"干净扫描"和"干净探针"，各重启一次主机。

**干净扫描**（扫描键起会话，START 带 `SKIP_PROBES`，整个会话没有一次 btdrv 调用）：

    probes: skipped for this session (START flag)
    btdevStartBleScanSmartDevice(0x1812) rc=0x00000000   ← 三次都是 rc=0
    0x1812 scan summary: events=0 polls=16 devices=0

扫描 API 接受请求，但一个事件都不产生——**这与我们自己的 btdrv 调用无关**。

**干净探针**（开机后第一次会话自动跑）：

    identity: first session after boot, running the probe before any scan
    identity: btdrvInitialize rc=0x00000000
    identity: btdrvInitializeBle 成功（其后开始收到事件）
    identity after RegisterGattClient: ClientRegistration result=0x00000037 client_if=0xFF status=0
    identity after UnregisterGattClient: （同样的载荷连续 16 条）
    identity after UnregisterGattClient: drained 16 event(s), 0 empty read(s)
    identity: ble channel map rc=0x0000F601 0000000000
    identity: channel map rc=0x0000F601 00000000000000000000

即：**干净状态下 GATT client 注册依然失败（`client_if=0xFF`）**；而且这次探针跑完之后
`btdevStartBleScanSmartDevice` 变成 `rc=0x0005168F`（前一次会话里是 0）——说明我们初始化
BLE 之后 btm 的扫描 API 会被拒，会话结束后又恢复。

两份日志的开头都有几行被环形缓冲覆盖后拼接出来的碎片（例如 `=0 raw=37000000...`），
是 NRO 侧读日志的已知取舍，不影响上面的字段。

### 第十八次起的改动（2026-09-21，参数形状探针）

身份探针里多了两组"按固件形状重发"的实验（不需要新按键，跟着自动探针一起跑）：

    raw register A / B / C: ...     # cmd 62，按固件适配层要求的 0x40 字节参数块发
    raw cmd40: pointer buffer ...   # cmd 40，改用指针缓冲而不是 libnx 的 MapAlias

每个实验都自己开一次 btdrv 会话（cmd 40 那条会让固件关会话，同一会话里后面的调用都会被
带成 `0xF601`）。判读见 `docs/ble-re.md` 的「参数布局对照」：如果 `raw register` 之后的
ClientRegistration 变成 `result=0 / client_if!=0xFF`，就说明**不需要补丁**，只是 libnx 的
请求形状过时了。

### 第十九次实机（2026-09-21，形状对齐成功）

> **本节结论已作废（2026-09-21 夜）**：日志本身没错，但"按固件形状（0x40 字节）注册成功"
> 是误读——`client_if=0x02` 那条事件来自 `InitializeBle` 自己的注册（同一会话会反复投递
> 4~16 次），显式注册每次都还是 `result=0x37 / client_if=0xFF`。固件侧更正见
> `docs/ble-re.md` 的「判定（2026-09-21 夜，更正）」：libnx 的请求形状本来就是对的。

    raw register A: ClientRegistration result=0x00000000 client_if=0x02 status=0
    raw register A: drained 16 event(s), 0 empty read(s)
    raw register: raw register B (inline, uuid@0x20) rc=0x00029E71
    raw register: raw register C (pointer buffer, uuid@0x0) rc=0x0000F601
    identity: InitializeBle rc=0x0000E401

**按固件形状（0x40 字节内联块）发 cmd 62，GATT client 注册成功，`client_if=0x02`。**
（B 那条"UUID 挪到 +0x20"返回 `0x29E71`，大概是"已经注册过"；C 那条指针缓冲返回
`0xF601` 并把会话关掉，所以后面的 `InitializeBle` 报 `0xE401 = KernelError_InvalidHandle`
——三条实验各自开了独立会话，正好把责任分清楚了。）

据此探针改成"运输形状"的流程，不再跑形状对照：

    identity: fixed RegisterGattClient rc=... client_if=0x..
    identity: connect attempt to XX:XX:... (client_if=0x..)     # 仅当配了地址
    identity: ConnectGattServer rc=...
    identity after ConnectGattServer: event type=4 ...
    identity: InitializeBle rc=...

连接用的地址来自 `sdmc:/switch/DGLAB-NX/config/dglab-ble-address.txt`（和以前一样，界面上的
`target:` 行能看到有没有读到）；没配地址就只做注册、跳过连接。连接必须在**注册所在的同一个
会话**里做，因为管理器把 client_if 绑在那个会话上。

### 第二十次实机（2026-09-21，探针输出被日志环吃掉）

这一轮日志里只有 5 次正常会话（配了地址 → `direct connect ... scan skipped` →
`btdevConnectToGattServer rc=0x0005568F` ×3 → 超时），**身份探针一行都没有**。原因是日志
环太小又被清空：探针排水时一次写几千字节（16 条事件 × 2 行），把 4KB 的环冲掉，而每次
`poc start` 又会 `memset` 整个环，于是 NRO 还没轮询到的探针输出直接没了。

修掉的三处：

- 日志环 4KB → 16KB（一次完整探针装得下）；
- 每次会话开始**不再清空环**（只推进 `log_valid_from`，读者仍然只拿到本次会话之后的行）；
- 排水最多记 4 条事件（队列会把同一条 ClientRegistration 反复交回来，16 条纯属刷屏）。

顺带一个提醒：`btdevConnectToGattServer` 用的还是 `btm:u` 那条路，和我们在 btdrv 层修好的
注册不是同一条；它这次仍然被拒（`0x5568F`）。

> **地址那条提醒已撤回（2026-09-21 夜）**：当时写的"`EA:A8:AC:22:2C:18` 是随机静态地址，
> 可能已经变了"不成立——这台 Coyote 的地址是**稳定**的（用户确认为固定地址，不是会轮换的
> 随机地址）。地址的最高两位是 `11` 只说明它的**类型**是 random static，不代表它每次开机都换。
> 所以连接被拒不能归因于地址过期，扫描先行的意义是"让协议栈见过这个设备"，不是"重新找一个
> 新地址"。

### 第二十一次实机（2026-09-21，顺序搞清楚了）

日志环修好之后探针输出终于完整：

    identity: fixed RegisterGattClient rc=0x00029E71 client_if=0xFF   ← 在 InitializeBle 之前
    identity: InitializeBle rc=0x00000000
    identity after InitializeBle: ClientRegistration result=0x00000000 client_if=0x02 status=0
    identity: EnableBle rc=0x00000000
    btdevConnectToGattServer rc=0x0005568F   ×3（btm:u 那条路，仍然被拒）

**顺序很重要**：显式注册放在 `InitializeBle` **之前**时返回 `0x29E71`、拿不到接口号；而
`InitializeBle` 自己会把管理器带起来，并由固件完成注册、给出 **`client_if=0x02`**。所以运输
路径的正确顺序是：

    btdrvInitialize → InitializeBle → （从 ClientRegistration 事件取 client_if）→ 连接

探针据此改成：`InitializeBle` + 排水 → 显式注册（仅作诊断、记录 rc）→ 用拿到的 `client_if`
在同一会话里 `ConnectGattServer` → 排水 6 秒看 `ClientConnection`（新增了这条事件的解码，
会打出 status / conn_id / 地址 / reason）。

### 第二十二次实机（2026-09-21，接口号来源修正）

    identity: InitializeBle rc=0x00000000
    identity after InitializeBle: ClientRegistration result=0x00000000 client_if=0x02 status=0  ×4
    identity: EnableBle rc=0x00000000
    fixed register: dispatch rc=0x00000000          ← 显式注册的 IPC 返回成功
    fixed register: result=0x00000037 client_if=0xFF status=0   ← 但它的事件说注册失败
    identity: explicit RegisterGattClient rc=0x00000000 client_if=0xFF
    identity: no client_if yet, connect skipped     ← 探针于是跳过了连接

**显式注册这条命令在 22.5.0 上就是不能用**：即使放在 `InitializeBle` 之后，IPC 返回 0，
事件仍带着 `result=0x37 / client_if=0xFF`。有效的接口号来自**管理器自己**在 `InitializeBle`
里的注册（`client_if=0x02`）。改法：排水时把第一个成功注册的 `client_if` 记下来
（`pocDrainBleEvents(..., &client_if)`），显式注册从探针里删掉，连接直接用管理器的接口号。

两个界面上的现象也解释一下：

- **`found` 是 `+` 并不代表扫到了设备**：配了目标地址时 PoC 会跳过扫描、并把 `found` 标成
  已完成（"有地址就当找到"）。这有误导性，已经改成只有真扫到设备才点亮 `found`。
- **不到 5 秒就 FAILED**：那是**第二次起**的会话——配了地址就直接走 `btm:u` 的
  `btdevConnectToGattServer`，它被拒（`0x5568F`）是立即返回的，不是超时；探针只在**开机后
  第一次会话**自动跑一次，所以后面的会话看起来"秒挂"。那条 `btm:u` 路径不是我们正在修的
  btdrv 路径。

### 第二十三次实机（2026-09-21，第一次真正发起连接）

    identity: client_if=0x02
    identity: connect attempt to EA:A8:AC:22:2C:18 (client_if=0x02)
    identity: ConnectGattServer rc=0x00029E71
    identity after ConnectGattServer: ClientRegistration result=0x00000037 client_if=0xFF  ×4

接口号取对了（`client_if=0x02`，来自管理器），但**连接被 btdrv 模块自己拒了**：
`0x00029E71` = `MAKERESULT(module 0x71, description 0x14F)`——在 `bluetooth` 模块里有十几处
`mov w0, #0x9e71; movk w0, #0x2, lsl #16`，是个通用的失败返回。紧接着管理器又报
`ClientRegistration result=0x37 / client_if=0xFF`，也就是连接这条路上它还试了一次注册并失败。

下一轮要同时排除两件事：

1. ~~**地址是否还有效**~~：**这一条已作废**（见第二十次实机的更正说明）——地址是稳定的，
   `EA:A8:AC:22:2C:18` 就是这台设备的地址，不用重扫。
2. **扫描结果里地址是不是落在别的偏移**：请求形状已经没有疑问（两者一致，见
   `docs/ble-re.md` 的更正），但事件载荷布局仍然可疑。排水会把 ScanResult 的前 32 字节整段
   打出来，并在整块里搜索配置的地址、命中时打印偏移；按 `←` 跑一次驱动级扫描就能看到。

探针里还加了两个廉价参数变体：`is_direct=false`（后台自动连接）与 `aruid=0`，各自排水 3 秒，
把返回码都记下来。

### 第二十四次起：btm:u 也带上真实 ARUID（2026-09-21）

查 `btm:u` 的形状时发现一件很可能解释"扫描永远 0 事件、连接被拒"的事：libnx 的 `btmu*`
封装在构造请求时填的是 **`appletGetAppletResourceUserId()`**，而我们在 **sysmodule** 里调用，
那个值是没有意义的（不是 applet）；同一批请求还带 `.in_send_pid`，也就是服务端能看到
调用者身份。

所以探针里新增 `pocRunBtmuAruidProbe`：用 libnx 的**载荷形状**（`nx/source/services/btmu.c`
里读出来的）但把 **NRO 报上来的真实 ARUID** 填进去，重发三条命令并轮询：

    cmd 8  StartBleScanForSmartDevice {uuid, pad, aruid}     （in_send_pid）
    cmd 10 GetBleScanResultsForSmartDevice（MapAlias 出缓冲 + total）
    cmd 18 BleConnect {addr, pad, aruid}                      （in_send_pid）
    cmd 20 BleGetConnectionState（HipcPointer 出缓冲 + total）

日志里看 `btmu:` 开头的行：出现 `btmu: scan[i] addr=..` 就说明 **btm 认这个 ARUID、扫描真的
跑起来了**（顺便也就拿到了设备当前地址）；`btmu: BleConnect rc=0` 加 `connection state` 就说明
`btm:u` 这条路能用。0x148 字节的扫描结果结构放在 `.bss`，没有占用 worker 的栈预算。

### 第二十五次实机（2026-09-21，三条路都试过一遍）

    identity: ConnectGattServer rc=0x00029E71                       （direct, aruid=NRO）
    identity: ConnectGattServer(indirect, aruid=NRO) rc=0x00029E71
    identity: ConnectGattServer(direct, aruid=0) rc=0x00029E71
    btmu: StartBleScanForSmartDevice(0x1812, aruid=0x89) rc=0x0000060A

按 switchbrew 的 module 表解码：`0x29E71` = **Bluetooth**(113)/0x14F，`0x60A` =
**Sf**(10)/3（服务框架直接拒请求）。结论：

- **`btm:u` 对后台 sysmodule 不通**：填无效 ARUID 时 btm 收下请求却什么都不做（所以一直
  "扫描成功但 0 事件"）；填 NRO 的真实 ARUID 时框架层就拒（`Sf`）。要拿通用 central 只能走
  btdrv——它已经把我们当合法客户端（注册成功、`client_if=0x02`）。
- btdrv 的 `ConnectGattServer` 三种参数组合都返回 `Bluetooth/0x14F`，同一个通用失败在模块
  里有十几处，最可能是"这个地址在协议栈里还没有记录"——也就是**得先用扫描看到设备**。

探针里另加了一条 `btdrvTriggerConnection`（libnx cmd 23，专门对已知地址发起连接）作为对照。
下一步的重点转到**把 btdrv 的扫描路线做通**（`SetBleScanParameter` / 过滤器 /
`StartBleScan` 的适配层形状逐条对齐），这样既能拿到设备的当前地址，也给连接准备"设备记录"。

### 第二十七次实机（2026-09-21，`←` 的驱动级探针真正跑起来了）

    action: btdrv scan probe
    probes: skipped for this session (START flag)
    btdrv probe: v3 (counts empty/events, connects to what it scans)
    btdrv probe: btdrvInitialize rc=0x00000000
    btdrv probe: btdrvInitializeBle rc=0x00000000
    btdrv probe: adapter enabled=1
    btdrv probe: btdrvEnableBle rc=0x00000000
    btdrv probe after InitializeBle: ClientRegistration result=0x00000037 client_if=0xFF status=0 ×4
    btdrv probe: client_if=0xFF
    btdrv probe: ClearBleScanFilters rc=0x00000000
    btdrv probe: EnableBleScanFilter(false) rc=0x00000000
    btdrv probe: SetBleScanParameter(0x0060, 0x0030) rc=0x00000000
    btdrv probe: btdrvStartBleScan (phase 0) rc=0x00000000
    btdrv probe: event #1 type=0 raw=0000000000000000 0000000000000000    （#2..#3 相同）
    btdrv probe: phase 0 done fetches=50 empty=0 events=50 scan_results=0
    ...（phase 1 加了 0x1812 过滤器 + EnableBleScanFilter(true)，结果一样）
    btdrv probe: done, 0 scan result(s) in total
    btdrv probe: no scanned address to connect to (have_address=0 client_if=0xFF)

同一轮里第二次按 `←` 的会话 `client_if=0x02`（注册事件 `result=0`），其余完全相同。

三个新事实：

1. **队列不是空的**：10 秒里 50 次调用每次都拿回一条"事件"，没有一次空读。之前记的
   `events=0` 是旧探针只看前 16 字节造成的——这些事件 `type=0`、前 16 字节全零，但 0x400
   字节答案的**后面**有非零内容（新计数用的是整块，所以 `empty=0`）。
2. 所以 cmd 79 的返回**不是** libnx 的 `BtdrvBleScanResult` 布局：真要是扫描结果，
   地址会在前 8 字节里就出现。要么事件类型/布局和 libnx 不同，要么结果被写在缓冲的另一段。
3. `client_if` 会被"同一轮开机里先跑过身份探针"影响（那之后注册事件变成
   `result=0x37 / client_if=0xFF`），干净会话里是 `0x02`。**驱动级探针要在开机后的第一个
   会话里按 `←`**（那时身份探针还没跑）。

下一次的探针（v4）会打印整块缓冲里**第一个非零字节的偏移**和那附近的 32 字节，先把这 50
条事件到底是什么弄清楚，再决定是解码布局不对还是扫描本身没启动。

### 第二十八次实机（2026-09-21，v4：数据落在 +0x200）

    btdrv probe: v4 (dumps where each event's data starts, connects to what it scans)
    btdrv probe: client_if=0x02                      ← 这一轮开机后先跑的是身份探针
    btdrv probe: event #1 type=0 nonzero=70 first=0x200
    btdrv probe:   200 BC804E74 7AFE0000 00000000 00000000
    btdrv probe:   210 00000000 00000000 00000000 00000000
    btdrv probe:   220 00000000 00002404 18B85CCB 4653D995
    btdrv probe: phase 0 done fetches=50 empty=0 events=50 scan_results=0
    btdrv probe: phase 1 done fetches=50 empty=0 events=50 scan_results=0
    btdrv probe: no scanned address to connect to (have_address=0 client_if=0x02)

要点：

1. 70 个非零字节**全在 +0x200 之后**（前 0x200 字节是零），事件 #1/#2/#3 逐字节相同，
   两个阶段也一样——是同一个事件被反复交回来，不是 50 个不同设备。
2. `type` 出参是 0。固件侧确认事件槽是 `{长度 @0x1af890, payload[0x400] @0x1af898,
   类型 @0x1afc98}`，取事件时把 payload 整块拷出来、类型单独写进 u32 出参（见
   `docs/ble-re.md` 的「事件是怎么交出来的」）。类型真的是 0、数据真的从 +0x200 开始，
   所以 libnx 的 `BtdrvBleEventInfo` 布局（字段都在 +0）对不上这次返回。
3. 同一轮开机里第一次按 `←` 时 `client_if=0xFF`、第二次是 `0x02`，和上一轮一致：**第一个
   会话按 `←` 才能拿到干净的管理器状态**。

v5 探针把 +0x200 起的 0x60 字节整段打出来、标出事件是否与上一条完全相同，并在整块里搜
配置的目标地址——这三样加起来就能判断那 0x200 之后的内容是不是扫描结果。

### 第二十九次实机（2026-09-21，v5：那 50 条事件不是扫描结果）

    btdrv probe: v5 (dumps 0x60 bytes at the data start, spots repeats)
    btdrv probe: client_if=0xFF                      ← 这一轮又是身份探针先跑
    btdrv probe: event #1 type=0 nonzero=70 first=0x200 repeat=0
    btdrv probe:   200 BC804E74 7AFE0000 00000000 00000000
    btdrv probe:   220 00000000 00002404 18B85CCB 4653D995
    btdrv probe:   230 E6CFA55D 9ECD25A6 EB000000 00000400
    btdrv probe: event #2 ... repeat=1 / #3 ... repeat=1
    btdrv probe: phase 0 done fetches=50 empty=0 events=50 scan_results=0
    btdrv probe: phase 1 done fetches=50 empty=0 events=50 scan_results=0

结论：

1. 那 70 个非零字节里有一个 BLE 地址样子的 6 字节（`BC:80:4E:74:7A:FE`）和一个 v4 UUID
   （`240418B8-5CCB-4653-D995-E6CFA55D9ECD`），但没有目标地址，`repeat=1` 说明 100 次调用
   拿到的是**同一条**事件。
2. 固件侧确认扫描结果事件的载荷是 0x148、类型是 6；而这条事件的载荷超过 0x200，只能是
   类型 8（`ClientNotify`，0x24c）或 13（`ServerAttributeOperation`，0x214）。两个阶段里
   连我们自己发过滤器、开扫描都没有产生新事件——**扫描没有产出任何 ScanResult**。
3. `type` 出参恒为 0：libnx 从响应 `+0x10` 读那个 u32，而固件只在响应带够出参块时才写它，
   所以这一轮之后不再依赖类型来判断，改用**载荷大小 + 内容**识别事件。

连接那边另有收获（见 `docs/ble-re.md`）：`ConnectGattServer` 的 `Bluetooth/0x14F` 是在
**调用协议栈之前**由两次 `client_if` 查表失败产生的，跟地址无关。所以 v6 探针在扫描结束后
**无论有没有扫到设备都会用配置地址试连一次**，用来验证"干净会话里 client_if 能不能过这个
检查"。

### 第三十次实机（2026-09-21，v6：`client_if` 又被上一次会话弄脏）

    identity: first session after boot, running the probe before any scan   ← 第一个会话被身份探针占掉
    ...
    action queued 11
    action: btdrv scan probe
    btdrv probe: v6 (dumps 0x60 bytes at the data start, connects even without a scan hit)
    btdrv probe after InitializeBle: ClientRegistration result=0x00000037 client_if=0xFF ×4
    btdrv probe: client_if=0xFF
    ...（两个阶段的 100 条事件与上一轮逐字节相同）
    btdrv probe: no scan result, falling back to the configured address
    btdrv probe: no address to connect to (have_address=1 client_if=0xFF)

`←` 这次是在**第二个**会话里按的（第一个会话被身份探针自动跑掉了），而身份探针那几次失败连接
已经把管理器里的客户端注销掉（`result=0x37 / client_if=0xFF`），所以 connect 那一步还是没能
执行。事实上有两轮日志都出现过同样规律：**管理器可以被重新注册，但要靠新的 btdrv 会话**。

v7 探针因此自己解决：`InitializeBle` 之后如果 `client_if` 还是 `0xFF`，就关掉会话、
`btdrvExit()`、等 500ms、重开一次（最多 3 次），拿到干净的 `client_if` 再继续；扫描结束后
无论有没有扫到设备都会用配置地址试连。

### 第三十一次实机（2026-09-21，v7：第一个会话干净，且第一次拿到 ScanResult）

    poc start aruid_low=0x00000089
    action queued 11                                    ← 进页面没有再自起会话（NRO 已修）
    action: btdrv scan probe
    btdrv probe: v7 (retries InitializeBle for a clean client_if, always tries to connect)
    btdrv probe after InitializeBle: ClientRegistration result=0x00000000 client_if=0x02 status=0 ×4
    btdrv probe: client_if=0x02 (attempt 0)              ← 一次就拿到，没走重试
    btdrv probe: event #3 type=7 nonzero=48 first=0x004
    btdrv probe:   000 00000000 02000000 00000000 00000000
    btdrv probe: scan result status=0 addr=00:00:00:00:00:00 entries=0 rssi=0   ← 第一次收到
    btdrv probe: phase 0 done fetches=48 empty=0 events=48 scan_results=1
    btdrv probe: phase 1 done fetches=50 empty=0 events=50 scan_results=0
    btdrv probe: connect attempt to 00:00:00:00:00:00 (client_if=0x02)
    btdrv probe: ConnectGattServer rc=0x00300C71          ← Bluetooth/0x1806，不再是 0x14F

三条结论：

1. **`client_if=0x02` 是干净的**，而且 `ConnectGattServer` 这次返回 `Bluetooth/0x1806`
   而不是 `0x14F` —— 与固件侧"`0x14F` 来自那两次 client_if 查表"完全吻合。也就是说
   `0x14F` 确实只是状态问题，剩下的是"地址/设备"这一层。
2. **扫描会产出 ScanResult 事件了**，但这一条是全零的（`addr=00:00:00:00:00:00 entries=0`），
   更像"扫描开始/结束"的标记而不是设备记录。探针把它当成目标地址，才有了那次连到
   `00:00:00:00:00:00` 的尝试。
3. `type` 出参确实不可靠（同一条 ClientRegistration 载荷这次被标成 type=7），所以识别事件
   只能靠载荷内容。

v8 探针：忽略全零地址的 ScanResult（回落到配置地址再连），并且**每条内容与上一条不同的事件
都会打印载荷**（不再只看前 3 条），这样真正的设备记录一出现就能看到。

### 第三十二次实机（2026-09-21，v8：事件种类、以及连到配置地址的尝试）

    btdrv probe: v8 (dumps every new payload, ignores zero scan addresses)
    btdrv probe: client_if=0x02 (attempt 0)
    btdrv probe: event #1 type=0 len=.. 000 37000000 FF000000 ..   ← ClientRegistration 0x37/0xFF
    btdrv probe: event #3 type=7 len=.. 000 00000000 02000000 ..   ← ClientRegistration 0/0x02
    btdrv probe: event #4 type=7 len=.. 000 00000000 04000000 ..   ← 另一个客户端 0x04
    btdrv probe: event #5 type=6 len=.. 200 BC804E74 7AFE0000 ..   ← 数据在 +0x200 的那条
    btdrv probe: scan result status=0 addr=00:00:00:00:00:00 entries=0 rssi=0
    btdrv probe: event #8/#9 000 00000000 03010000 / 03000000      ← 0x03 注册/注销
    btdrv probe: connect attempt to EA:A8:AC:22:2C:18 (client_if=0x02)
    btdrv probe: ConnectGattServer rc=0x00300C71

要点：

1. 事件槽里现在能看到**多个客户端的注册/注销**（`client_if` 0x02/0x03/0x04），说明这个槽是
   全系统共享的，我们读到的不全是自己的事件。
2. 那条"数据在 +0x200"的事件这次被标成 `type=6`（上一轮是 0），再次说明 `type` 出参不可信；
   它只出现一次、内容与别的都不同，仍然带一个地址样子的六字节和一个 v4 UUID。
3. **连到配置地址的尝试这次真的发生了**，结果 `0x00300C71`（`Bluetooth/0x1806`）——与
   `TriggerConnection` 同一个码。也就是说 `client_if` 这一层已经过了，卡在"设备/协议栈"这一层。

v9 探针换了个更硬的办法来识别事件：**读之前把缓冲区预填成 `0xAA`**。固件拷贝时不看我们给的
长度（用管理器里的 size），所以"最后一个不是 `0xAA` 的字节"就是这次事件的实际长度，而长度按
发布点表可以直接对应到类型（0x08/0x0c/0x14/0x148/0x214/0x24c…）。日志里现在有
`len=0x...`，全空读也会表现为整块 `0xAA`。

### 第三十三次实机（2026-09-21，v9：长度量不出来，但确认了记录形状）

    btdrv probe: v9 (pre-fills the buffer to measure each event's size)
    btdrv probe: event #1 type=0 len=0x400 filled=1024 first=0x000 repeat=0
    btdrv probe:   000 37000000 FF000000 ...
    btdrv probe: event #5 type=6 len=0x400 filled=1024 first=0x000 repeat=0
    btdrv probe:   000 00000000 00000000 ...
    btdrv probe: connect attempt to EA:A8:AC:22:2C:18 (client_if=0x02)
    btdrv probe: ConnectGattServer rc=0x00300C71

`len=0x400 filled=1024` 的意思是**整块 0x400 都被写掉了**（`0xAA` 一个不剩），所以拷贝长度
不可能从缓冲区里量出来——出缓冲要么被框架整块清零、要么管理器就是按整块写的。长度法作废。

同一次运行里仍然只有一条"设备形状"的记录（`BC:80:4E:74:7A:FE` + 一个 v4 UUID），
**始终没有出现配置的目标地址**。

v10 探针把 dump 改成固定看两个区域：`000`–`01F`（ClientRegistration 落在这里）与
`200`–`21F`（到目前为止所有"像设备"的数据都落在这里），每个内容不同的事件都打这两段，
这样一次运行就能把所有不同事件的样子看全。

### 第三十四次实机（2026-09-21，v10 + 手机确认广播 UUID 变成 0x180C）

    btdrv probe: v10 (dumps the head and the +0x200 region of every new event)
    btdrv probe: event #1 type=0 nonzero=49 first=0x000 repeat=0
    btdrv probe:   000 37000000 FF000000 00000000 00000000
    btdrv probe:   200 BC804E74 7AFE0000 00000000 00000000
    btdrv probe: event #5 type=6 nonzero=47 first=0x200 repeat=0
    btdrv probe:   000 00000000 00000000 00000000 00000000
    btdrv probe:   200 BC804E74 7AFE0000 00000000 00000000
    ...
    btdrv probe: ConnectGattServer rc=0x00300C71

两个结论：

1. **`+0x200` 那段是常量**：每条事件都在同样的位置带 `BC:80:4E:74:7A:FE`，所以它不是事件
   载荷，而是管理器缓冲区里的一段固定内容（多半是它自己记录的某个设备/适配器记录）。
   payload 里**从来没有**目标地址 `EA:A8:AC:22:2C:18`。
2. 手机扫描确认：设备**看得到**、地址就是配置文件里那个、**广播的服务 UUID 是 `0x180C`**。
   而这一轮 phase 1 用的过滤器还是旧的 `0x1812` —— 等于把设备全滤掉；phase 0 的
   "不加过滤器"也没给出任何设备记录，说明这套固件上"过滤器关闭"并不等于"上报全部"。

v11 探针改成三种模式各跑一段：不加过滤器 / `0x180C` + 启用过滤器 / `0x180C` + 关闭过滤器。
这样能一次确定过滤器语义，并且用**新固件真正的广播 UUID**去找设备。

### 第三十五次改动（2026-09-21，v12：四种过滤模式）

手机实测（见上文「新固件的广播与 GATT 表」）确认新固件的广播里除了服务 UUID `0x180C`，
还带了 **Manufacturer ID `0x000A`**（Qualcomm/QTIL，模组厂）。所以驱动级探针改成四段：

1. 不加过滤器（对照）；
2. `0x180C` 服务过滤（AD 类型 `0x03`）+ 启用过滤器；
3. 厂商自定义数据过滤（AD 类型 `0xFF`，公司号 `0x000A`）+ 启用过滤器；
4. `0x180C` 服务过滤 + **关闭**过滤器（验证"关闭"到底是不上报还是上报全部）。

每段 10 秒、逐段清空过滤器，日志里 `repeat=0` 的事件才会打出载荷；目标地址仍会在整块
0x400 里全文搜索。

### 第三十六次实机（2026-09-21，v12：设备终于出现在事件里）

    btdrv probe: phase 0 done ... scan_results=1     ← 只有那条全零标记
    btdrv probe: AddBleScanFilterCondition(0x180C, type 0x03) rc=0x00000000
    btdrv probe: phase 1 done fetches=50 empty=0 events=50 scan_results=0
    btdrv probe: AddBleScanFilterCondition(company 0x000A, type 0xFF) rc=0x00000000
    btdrv probe: event #1 type=0 nonzero=100 first=0x004 repeat=0
    btdrv probe:   000 00000000 020001EA A8AC222C 18020106
    btdrv probe: target address at offset 0x007
    btdrv probe: phase 2 done fetches=50 empty=0 events=50 scan_results=0
    btdrv probe: phase 3 done fetches=50 empty=0 events=50 scan_results=0
    btdrv probe: ConnectGattServer rc=0x00300C71

这条日志把扫描这一段彻底讲清楚了：

1. **设备确实能被扫到**——但只在**厂商自定义数据（AD 类型 `0xFF`，公司号 `0x000A`）**
   被当作过滤器时。按 `0x180C` 服务过滤、以及不加过滤器都不出现设备记录（说明这套固件上
   "启用过滤器"是必要条件，而且手机看到的 `0x180C` 很可能来自扫描响应而不是广播包本身）。
2. 载荷**就是 libnx 的 `BtdrvBleScanResult` 布局**，逐字节对得上：

       +0x00 u32 result   = 0
       +0x04 u8  status   = 2            ← "发现新设备"
       +0x05 u8  device_type = 0
       +0x06 u8  ble_addr_type = 1       ← 随机静态地址
       +0x07 u8[6] address  = EA:A8:AC:22:2C:18   ← 目标设备
       +0x0D ...            = 02 01 06（Flags AD 结构）

   所以布局没问题，**唯一坏掉的是 `type` 出参**（恒为 0），而我们之前正是靠它判断
   `scan_results`，于是把设备记录全跳过了。
3. 连接仍然回 `Bluetooth/0x1806`——但这一次的连接是在**四段扫描全部结束之后**才发的。

v13 探针两处改动：扫描结果改成**按载荷判断**（地址非零即为设备），并且**在设备刚被扫到的
那一刻**立刻发一次 `TriggerConnection` + `ConnectGattServer`（再排水 3 秒），测的就是
"协议栈刚见过它"这个组合。

### 第三十七次实机（2026-09-21，v13：扫描通了，连接还是不通）

    btdrv probe: phase 0 done fetches=50 empty=0 events=50 scan_results=0
    btdrv probe: phase 1 done fetches=50 empty=0 events=50 scan_results=0
    btdrv probe: device EA:A8:AC:22:2C:18 status=2 type=0 addr_type=1 entries=3 rssi=-42
    btdrv probe: TriggerConnection(EA:A8:AC:22:2C:18) rc=0x00300C71
    btdrv probe: ConnectGattServer (right after the scan result) rc=0x00300C71
    ...
    btdrv probe: device 61:6A:4C:DE:6C:3E status=2 type=0 addr_type=1 entries=3 rssi=-43
    btdrv probe: phase 3 done fetches=50 empty=0 events=50 scan_results=50
    btdrv probe: done, 99 scan result(s) in total
    btdrv probe: ConnectGattServer rc=0x00300C71        ← 四段都跑完之后再连，同样

结论：

1. **扫描完全通了**：phase 2/3 各 50 条设备记录，目标设备 `status=2`（发现新设备）、
   `addr_type=1`（随机静态）、`entries=3`、`rssi=-42`；同场还有另一台设备
   `61:6A:4C:DE:6C:3E`。之前 `scan_results=0` 纯粹是探针用 `type` 判断造成的。
2. **连接仍然被拒**，而且"刚扫到就立刻连"和"扫完再连"都是同一个码。查固件侧：
   `0x00300C71` 由消息层的状态 `0x68`（"某个必需对象为空"）映射而来，也就是说连接请求在
   **发往蓝牙栈的消息层**就被挡下了，而不是协议栈拒绝了对端。

v14 探针把连接做成"矩阵"：在**扫描前**和**扫描后**各试四种变体（`ConnectGattServer`
direct / background、`TriggerConnection` timeout 0 / 0x1000），每次排水 1.5 秒。这样一次
运行就能看出到底是哪种调用形态、哪种时序能过。

### 第三十八次实机（2026-09-22，v14：扫描前后差一个错误码）

    扫描前（还没有任何 scan result）：
      ConnectGattServer(direct)      rc=0x00029E71   ← Bluetooth/0x14F（本地 client_if 检查没过）
      ConnectGattServer(background)  rc=0x00029E71
      TriggerConnection(timeout 0)   rc=0x00300C71
      TriggerConnection(timeout 0x1000) rc=0x00300C71
      （两条 ConnectGattServer 之后都出现 ClientRegistration 0x37/0xFF，也就是管理器把客户端注销了）

    扫描后（目标设备以 status=2、rssi≈-40 反复出现）：
      ConnectGattServer(direct)      rc=0x00300C71
      ConnectGattServer(background)  rc=0x00300C71
      TriggerConnection(timeout 0)   rc=0x00300C71
      TriggerConnection(timeout 0x1000) rc=0x00300C71

所以分界线很清楚：**扫到设备之后，本地那层检查就过了**（0x14F 消失），四种形态全部走到
"发给蓝牙栈"这一步，然后被栈自己的状态 `0x68` 挡回来（映射成 `Bluetooth/0x1806`）。
也就是说，问题不再是我们的调用形状，也不是 client_if/地址记录，而是**栈拒绝发起这条 LE
连接**。

下一步两条线：一是用手机在控制台探针运行期间连一次设备（证明那一刻设备是可连接的），
二是继续在固件里找 opcode `0x6AA` 的处理端，看它返回 `0x68` 的条件是什么。

### 当前状态：暂停（2026-09-21）

BLE 直连的判定已经完成——**不需要固件补丁**；固件侧的第二次核对更正了"libnx 的 btdrv
请求形状过时"这个理由（形状其实一致，见 `docs/ble-re.md` 的「判定（2026-09-21 夜，更正）」）。
剩下的是语义/状态问题：扫描结果走哪条路径、显式注册为什么回 `0x37`、`ConnectGattServer`
为什么回 `Bluetooth/0x14F`。重开顺序写在 `docs/ble-re.md` 的「下一步（更正后）」；
`tools/ble-re/upstream.md` 的草稿等整条研究做完再整理（第 1、4 条已作废）。

本页的探针保留原样，下次开工时可以直接接着用：`A` 起会话（首次会话自动跑身份探针，
含 btdrv 注册/连接尝试与 btm:u ARUID 对照）、`←` 跑驱动级扫描探针、`StickL` 跑常见厂商
ID 的对照扫描。

### 结论（截至 HOS 22.5.0 / AMS 1.11.2）

已经排除的可能：扫描过滤器 UUID、扫描参数（interval/window）、事件源选择（managed 与
LE HID 两个队列）、轮询与事件两种读取方式、按地址直连、以及权限/调用顺序。

剩下的解释（2026-09-21 更正后）：**请求形状不是原因**（固件的每个命令 case 都读过，与
libnx 一致），所以问题在事件载荷布局/语义或服务端状态上——例如 `btm:u` 只对 applet 开放
（`Sf/0x60A`，已排除），btdrv 的事件载荷是否与 `btdrv_types.h` 一致还没验证，
`ConnectGattServer` 的 `Bluetooth/0x14F` 也没定位到具体分支。

**因此这条路的现状是"暂停"，不再是"走不通"**：不需要固件补丁，也不排除能走通，
只是还差"事件/结果路径 + 注册/连接被拒的确切原因"这一步（顺序见
`docs/ble-re.md` 的「下一步（更正后）」）。

## 已知限制

- 这些 IPC 命令是临时调试接口，真实传输层设计完成后会删除。
- PoC 还没有接协议会话层：写的是固定报文，不是 `DglabCoyoteV3Session` 的输出。
- sysmodule 当前一次只服务一个 IPC 会话。
- 没有做 MTU 协商：V3 报文 20 字节，默认 ATT MTU 23 已经够用。
- 没有实现自动重连；断开后需要重新按 `A`。

## 验证方式

主机侧（不需要 Switch）：

    make -C tests/protocol     # 协议层 363 项检查
    make -C tests/ipc          # CMIF 布局 51 项检查

组件构建：

    make -C sysmodule package  # 生成 exefs.nsp 与安装目录
    make -C nro                # 生成 DGLAB-NX.nro

`tests/ipc` 使用 libnx 自己的 `switch/sf/cmif.h` 编码请求，再交给 sysmodule 实际使用
的解析辅助函数，用来确认"客户端怎么发"与"服务端怎么读"一致。
