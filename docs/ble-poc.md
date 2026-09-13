# BLE Transport PoC

本文说明如何实机验证 Switch 上通过 HOS 蓝牙栈连接 DG-LAB 设备的可行性，以及怎么
把结果反馈回来。

PoC 要回答的是优先级 #5 的问题：

    BLE scan → GATT connect → service discovery → characteristic read/write/notify

## 代码位置

| 内容 | 位置 |
| --- | --- |
| PoC 实现（唯一接触蓝牙的地方） | `sysmodule/source/transport/ble_poc.c`、`sysmodule/include/dglab/transport/ble_poc.h` |
| 临时 IPC 命令与结构 | `common/include/dglab/ipc_poc.h` |
| CMIF 布局规则 | `sysmodule/include/dglab/ipc_cmif.h` |
| NRO 观察与操作界面 | `nro/source/main.c` |

## 为什么用 btdev（以及 btdrv 为什么被放弃）

libnx 提供两套 BLE 接口：

1. `btdrv`：蓝牙驱动的底层封装，btm-sysmodule 使用的就是它。需要自己处理
   事件句柄、`BtdrvBleEventInfo` 的联合体以及 GATT id 的查询顺序。
2. `btdev` / `bt` / `btm:u`：更高层的封装，扫描结果、服务、特征都是类型化结构。

最初担心 `bt`／`btm:u` 是面向 applet 的服务（内部使用
`appletGetAppletResourceUserId()`，后台 sysmodule 里是 0），所以先走 btdrv。
实机结果是：

- btdrv 的 BLE 事件通道在 HOS 22.5.0 上只返回空载荷（详见下文三次实测记录），
  拿不到任何可用的扫描结果；
- 反过来，`btdevInitialize`、`btdevStartBleScanSmartDevice` 等调用在后台 sysmodule 里
  全部返回成功（`rc=0`），说明这套服务可用，ARUID=0 也没有被拒绝。

因此当前实现改为 **btdev**：BLE 所有权仍在 sysmodule，代码也比 btdrv 版本短。

用到的接口：

    btdevInitialize / btdevExit
    btdevAcquireBleScanEvent + btdevStartBleScanSmartDevice / btdevStopBleScanSmartDevice
    btdevGetBleScanResult
    btdevAcquireBleConnectionStateChangedEvent / btdevConnectToGattServer
    btdevDisconnectFromGattServer / btdevGetBleConnectionInfoList
    btdevAcquireBleServiceDiscoveryEvent / btdevGetGattService
    btdevGattServiceGetCharacteristic
    btdevAcquireBleGattOperationEvent / btdevGetGattOperationResult
    btdevEnableGattCharacteristicNotification
    btdevReadGattCharacteristic / btdevGattCharacteristicSetValue / btdevWriteGattCharacteristic

### 扫描过滤器：广播里是 0x1812，不是 0x180C

用手机 BLE 扫描工具（LightBlue）查看 Coyote 3.0 的广播：

    名称          = 47L121000
    Services UUIDs = 1812        ← 广播里的服务 UUID
    （0x180C 是 DG-LAB 服务，连接之后才会出现在 GATT 服务列表里）

所以按 `0x180C` 过滤的扫描永远找不到设备——这也是前面几次"扫描成功但 0 结果"的原因。
现在默认先按 `0x1812` 扫描，失败再退回 `0x180C`。

## 安全性

- PoC 只在按下 `A` 时启动，开机不会接触蓝牙。
- 不写 BF 指令，因此不会修改设备断电保存的软上限或平衡参数。
- 连接后每 100ms 只写一条"强度不变化 + 双通道空闲"的 B0，设备不会有任何输出。
- 只有按 `X` 才会写"双通道绝对置零"，这是把输出降到零的安全方向。

## 安装

仓库根目录的 `make` 会一次生成完整的发布布局：

    release/
    ├── 00FF072107210721/      前端 sysmodule 目录（即 SD 上要放的目录）
    │   ├── exefs.nsp
    │   ├── toolbox.json
    │   └── flags/boot2.flag
    ├── DGLAB-NX.nro
    └── DGLAB-NX-ovl.ovl       （尚未实现，见 overlay/AGENTS.md）

对应到 SD 卡：

    release/00FF072107210721/  →  SD:/atmosphere/contents/00FF072107210721/
    release/DGLAB-NX.nro       →  SD:/switch/DGLAB-NX.nro
    release/DGLAB-NX-ovl.ovl   →  SD:/switch/.overlays/DGLAB-NX-ovl.ovl

`boot2.flag` 表示随系统启动加载，需要重启生效。也可以单独构建某个组件：
`make -C sysmodule package`、`make -C nro package`。

## 操作

| 按键 | 动作 |
| --- | --- |
| `A` | 开始 PoC（扫描 0x1812 → 连接 → 发现 → 订阅 → 周期写入） |
| `X` | 写入"双通道绝对置零"的 B0（序列号 1，预期设备回 B1） |
| `B` | 读取电量特征（0x180A / 0x1500） |
| `R` | 重启会话（断开并重新扫描） |
| `L` | 开关 100ms 的 B0 保活写入 |
| `Y` | 断开连接 |
| `ZL` | 重新扫描（默认顺序：0x1812 → 0x180C） |
| `ZR` | 只用协议服务 UUID `0x180C` 扫描（对照） |
| `Up` | 只用广播里的 UUID `0x1812` 扫描 |
| `Down` | 用 btm 的 general 过滤器（厂商数据）扫描（对照） |
| `-` | 停止 PoC（清理并退出） |
| `+` | 退出 NRO |

动作键在 PoC 未运行时（界面显示 `state: idle`）会自动先启动一次运行，
所以空闲界面直接按 `Up`／`Down` 也能工作。

NRO 会把收到的 sysmodule 日志同步写到：

    sdmc:/switch/dglab-ble-poc.log

打不开时退回到 `sdmc:/dglab-ble-poc.log`；NRO 启动后第二行会显示日志文件的实际状态
（`log: ...` 或 `log: unavailable`）。测试结束后把这个文件（或屏幕照片）发回来即可。

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

里程碑停在 `scan` 时，先看心跳行：

1. `events=0`：连 BLE 事件都没收到，问题在事件通路（`btdrvInitializeBle` 拿到的事件
   与 `btdrvGetBleManagedEventInfo` 的配对），与设备无关。
2. `events>0, results=0`：事件通路没问题，但扫描没有产生结果，可能是广播被过滤或设备
   没有在广播。按 `ZL` 关闭过滤器重新扫描试一次。
3. `results>0` 但没有 `coyote 3.0 found`：广播内容与我们预期的 AD 类型不一致，日志里的
   `ad type=.. data=..` 会直接说明它到底广播了什么。此时可以按 `ZR` 直接连接，
   先把 GATT 与通知部分验证掉。

## 第一次实机结果（2026-09-13）

日志（`dglab-ble-poc.log`）显示整场会话 `events=0`，即 btdrv 的 BLE 事件通道**一次都没有
回调**：

- `btdrvInitializeBle`、`btdrvStartBleScan`、`btdrvClearBleScanFilters`、
  `btdrvEnableBleScanFilter` 的返回值全是 0，调用都被接受；
- 但这些调用本该产生的 `ScanResult` / `ScanFilter` 事件一个都没有；
- 因此问题不在设备或广播，而在事件通道本身。

据此本轮改动：

1. 每次启动都显式调用 `btdrvEnableBle()`：`btdrvIsBluetoothEnabled` 只报告适配器状态，
   并不代表 LE host 已经在运行；LE 空闲时启动扫描会返回成功但不产生任何事件。
2. 记录第一次 `eventWait` 失败的返回码，用来区分"正常超时"与"句柄错误"。
3. 若第一个事件队列在 20 秒内没有任何事件，自动切换到 btdrv 暴露的另一个事件队列
   （`btdrvRegisterBleHidEvent` + `btdrvGetLeHidEventInfo`）并重新扫描，日志会写明切换。
4. 修复日志环形缓冲在每次启动时重置写指针的问题：读端使用绝对偏移，重置写指针会让它
   读到上一轮的旧数据，也就是第一次日志里那些被截断/串行的行。

顺带记录的错误码：按 `R` 时 `btdrvConnectGattServer` 返回 `0x00300C71`
（module `0x71`、description `0x1806`）。那次调用使用的是 `client_if=0` 和全零地址，
因为当时还没有注册 GATT client、也没有扫描到设备，属于预期内的拒绝，不能据此判断
连接本身是否可行。

## 第二次实机结果（同一天，开启 BLE 与换事件源之后）

- `btdrvEnableBle rc=0x00000000`：显式开启 BLE 成功，但事件依然为 0；
- `btdrvInitializeBle` 与 `btdrvRegisterBleHidEvent` 都返回 0，两个事件句柄都拿到了；
- 两个事件源上的 `eventWait` 只返回 `0x0000EA01`，即内核 `Timeout`
  （module 1、description `0x75`）——说明句柄有效，只是**从未被触发**；
- 结论：问题不在设备、广播或扫描参数，而是"没有任何 BLE 事件被投递到本 sysmodule"。

因此第三次迭代改为**直接轮询事件队列**（`btdrvGetBleManagedEventInfo` /
`btdrvGetLeHidEventInfo`），不再依赖事件句柄：日志会打印
`poll managed rc=0x... type=...` 与 `poll lehid rc=0x... type=...`。
这样可以把"队列里没有数据"与"有数据但事件没通知"区分开；一旦轮询拿到事件，
PoC 会自动切换到轮询模式继续跑完后续流程。

## 第三次实机结果（HOS 22.5.0 / AMS 1.11.2 / Switch 1）

轮询实验给出了两个结论，其中一个暴露了 PoC 自己的 bug：

1. **空队列也返回 `rc=0` 且 `type=0`**。PoC 把这种结果当成真实的
   `ClientRegistration` 事件，于是被虚假的 `status=1 client_if=3` 推进到连接阶段，
   对着 `00:00:00:00:00:00` 发起连接。这是 PoC 的错，已改为**轮询只写日志、
   绝不驱动状态机**。
2. 事件句柄确实有回调，但**载荷全是空的**：整份日志里唯一的 `ScanResult` 是
   `status=0 addr=00:00:00:00:00:00 rssi=0 entries=0`，从来没有出现过真实地址或广播数据。

也就是说，在这台 HOS 22.5.0 上，btdrv 的 BLE 事件通道"会响但没内容"：调用全部成功，
拿不到任何可用的扫描结果。可能的原因（都需要证据，不能靠猜）：新版本 HOS 改变了 BLE
事件的路由或载荷布局；或者 btdrv 的 managed 状态只对 btm-sysmodule 有效。

### 新增诊断：btdev 探针（`Up` 键）

`Up` 会运行一次 btdev 探针，用 libnx 已经封装好的 `bt` + `btm:u` 路径重新走一遍扫描：

```
btdevInitialize
btdevAcquireBleScanEvent
btdevStartBleScanSmartDevice(0x180C)
btdevGetBleScanResult        (每 500ms，共 15 秒)
btdevStopBleScanSmartDevice
btdevExit
```

每一步的返回码和扫描到的地址都会写进日志。判断方式：

- 如果拿到真实地址 → 说明 bt/btm:u 在后台 sysmodule 里可用，后续传输实现改用 btdev
  （代码还会更短），BLE 所有权仍然留在 sysmodule；
- 如果 `btdevInitialize` 或扫描调用返回错误 → 说明后台进程用不了这套服务，
  那时需要在"让 applet 承担 BLE"与"继续挖 btdrv"之间做架构决策。

## 第四次实机结果（btdev 探针）

```
btdevInitialize rc=0x00000000                 ← bt + btm:u 在后台 sysmodule 里可用
btdevAcquireBleScanEvent rc=0x00000000
btdevStartBleScanSmartDevice(0x180C) rc=0x00000000
btdevGetBleScanResult rc=0x00000000 count=0   ← 15 秒内一个设备都没有
btdevStopBleScanSmartDevice rc=0x00000000
btdev probe: done, 0 results
```

这是重要的一步：**`bt`／`btm:u` 这套服务在后台 sysmodule 里完全可用**（这是我们自己
的 ARUID=0 也没有被拒绝），所以 BLE 所有权可以继续留在 sysmodule，不需要把 BLE 搬到
applet。libnx 的 btdev 封装能直接用，后续传输实现的代码还会更短。

但没有扫到任何设备。两条独立路径（btdrv 与 btm:u）都是"调用成功、结果为空"，所以
问题更可能在**广播侧**而不是主机侧：

1. 测试时设备是否已开机、处于未被占用的状态（没有连着手机 App）；
2. 设备的广播包里到底有没有 0x180C 服务 UUID —— smart device 扫描就是按它过滤的，
   如果设备只广播名称，这个过滤器永远匹配不到。

因此新增 `Down`（general 扫描）作为对照：

- 如果 general 扫描能列出设备，而 smart device 扫描不能 → 主机扫描没问题，
  是广播内容/过滤器不匹配，需要改用名称匹配或直接连地址；
- 如果两者都是 0 → 说明当时周围没有可发现的 BLE 设备，先用手机上的
  nRF Connect 之类的工具确认 Coyote 在广播什么（名称、服务 UUID、厂商数据）。

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

## 第五次实机（改用 btdev 与 0x1812 过滤器）

手机扫描给出的广播内容（名称 `47L121000`、Services UUIDs `1812`）解释了前面所有
"扫描成功但 0 结果"：**广播里没有 0x180C**，它只存在于连接之后的 GATT 服务列表。

本轮改动：

1. 传输层整体改用 btdev（`bt` + `btm:u`），删除 btdrv 的事件/状态机代码；
2. 扫描默认按 `0x1812` 过滤，12 秒没结果就自动退回 `0x180C` 再试一次；
3. 连接、服务发现、特征查询、订阅通知、读写全部走 btdev 的类型化接口；
4. 动作键改为扫描变体（见上表），便于在同一次实机里对比不同过滤器。

## 这次要确认的开放问题

以下都是实现时无法从 libnx 头文件确定、只能靠实机日志回答的问题：

1. Sysmodule 能否获取 BLE 事件（`btdrvInitializeBle` 的返回值）。
2. 扫描结果里 `47L121000` 到底广播了哪些 AD 数据（日志会打印原始字节）。
3. `btdrvConnectGattServer` 需要什么样的 AppletResourceUserId；NRO 的 ARUID 与 0
   哪个能被接受（按 `R` 可以重试 0）。
4. 服务发现结果是否通过 `ClientCacheSave` 事件回传（日志会列出每个属性）。
5. GATT 属性里 16-bit UUID 的存储字节序（日志同时打印 `uuid_size` 与解析出的
   `uuid16`，两种字节序都被接受）。
6. 读取特征的结果以哪种事件返回（PoC 会记录收到的任何事件）。
7. 订阅 0x150B 之后设备是否立即产生事件，还是必须等到强度变化。
8. 100ms 周期的 B0 写入在实机上能否长时间稳定（`b0 writes` / `failed`）。

## 已知限制

- 这些 IPC 命令是临时调试接口，真实传输层设计完成后会删除。
- PoC 还没有接协议会话层：写的是固定报文，不是 `DglabCoyoteV3Session` 的输出。
- sysmodule 当前一次只服务一个 IPC 会话。
- 没有做 MTU 协商：V3 报文 20 字节，默认 ATT MTU 23 已经够用。
- 没有实现自动重连；断开后需要重新按 `A`。

## 验证方式

主机侧（不需要 Switch）：

    make -C tests/protocol     # 协议层 363 项检查
    make -C tests/ipc          # CMIF 布局 45 项检查

组件构建：

    make -C sysmodule          # 生成 exefs.nsp 与安装目录
    make -C nro                # 生成 DGLAB-NX.nro

`tests/ipc` 使用 libnx 自己的 `switch/sf/cmif.h` 编码请求，再交给 sysmodule 实际使用
的解析辅助函数，用来确认"客户端怎么发"与"服务端怎么读"一致。
