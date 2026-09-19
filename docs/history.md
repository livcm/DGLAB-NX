# 文档历史归档

2026-09-17 文档压缩时从主文档移出的原文，按「文档 → 小节」存放，**未做改写**。
主文档只保留结论与一句指针；要查某个说法的来龙去脉时读这里。

## 索引（本轮删除 / 合并清单）

| 原位置 | 性质 | 原行数 | 去向 |
| --- | --- | --- | --- |
| `docs/ble-poc.md` 第一~十一次实机记录 | 迭代过程 | 约 250 | 本文「ble-poc：11 次实机记录」 |
| `docs/nro-ui.md`「将来若要安装依赖」 | 低价值背景（当前不执行） | 6 | 本文「nro-ui：将来若要安装依赖」 |
| `docs/nro-ui.md` deko3d 2026-09-15 复核表与触发条件 | 迭代过程 | 约 35 | 本文「nro-ui：deko3d 复核（2026-09-15）」 |
| `docs/nro-ui.md` 二维码编码器抓到的 4 个 bug | 迭代过程 | 4 | 本文「nro-ui：二维码编码器的 bug 清单」 |
| `docs/nro-ui.md`「面板：按内容量高度」的事故叙述与 `panel.h` 收裁剪的写法 | 已失效（面板已删除） | 约 12 | 本文「nro-ui：面板方案的事故与旧 API」 |
| `docs/nro-ui.md` 第二次/第六次/第七次修正的叙述 | 迭代过程 | 约 130 | 本文「nro-ui：第二、六、七次修正的原文」 |
| `docs/dglab-socket.md`「睡眠与唤醒」的实机经过 | 迭代过程 | 约 18 | 本文「dglab-socket：睡眠与唤醒的实机经过」 |
| `docs/dglab-socket.md`「栈上不要放 KB 级缓冲区」的两次报告与修复经过 | 迭代过程 | 约 45 | 本文「dglab-socket：栈缓冲事故的经过」 |
| `docs/dglab-socket.md`「实机验证记录（2026-09-15）」 | 迭代过程 | 约 25 | 本文「dglab-socket：2026-09-15 实机验证记录」 |
| `docs/joycon-input.md` 连接判定被推翻的两轮（09-16、09-17） | 迭代过程 | 约 15 | 本文「joycon-input：连接判定的两轮推翻」（另见「nro-ui：第七次修正的原文」） |
| `docs/docs-audit.md` 全文（570 行） | 报告明细 | 570 | 本文「docs-audit：2026-09-17 审计报告原文」 |

---

## ble-poc：11 次实机记录

以下原文来自 `docs/ble-poc.md`（2026-09-13 ~ 09-17 ，HOS 22.5.0 + AMS 1.11.2）。
结论保留在主文档的「实机结论」与「结论」两节。

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

## 第五次实机（改用 btdev 与 0x1812 过滤器）

手机扫描给出的广播内容（名称 `47L121000`、Services UUIDs `1812`）解释了前面所有
"扫描成功但 0 结果"：**广播里没有 0x180C**，它只存在于连接之后的 GATT 服务列表。

本轮改动：

1. 传输层整体改用 btdev（`bt` + `btm:u`），删除 btdrv 的事件/状态机代码；
2. 扫描默认按 `0x1812` 过滤，12 秒没结果就自动退回 `0x180C` 再试一次；
3. 连接、服务发现、特征查询、订阅通知、读写全部走 btdev 的类型化接口；
4. 动作键改为扫描变体（见主文档的操作表），便于在同一次实机里对比不同过滤器。

## 第六次实机结果与"直接连接"通道

改按 0x1812 过滤后依然是 0 结果，说明问题不只是过滤器 UUID。这份日志还暴露了两个
PoC 自己的问题：

1. 会话以 `state=failed` 结束后，NRO 仍然认为"有会话在运行"，于是 Up/Down 的动作
   全部被忽略（日志里的 `action 9 ignored: no run active`）——**general 扫描其实一次
   都没真正跑过**。现在 Idle／Failed／Stopped 一律视为"没有运行中的会话"，
   按动作键会先自动启动新会话。
2. 默认扫描顺序改成 `0x1812 → 0x180C → general`，不再需要手动按 Down 才有对照数据。

同时新增两项诊断/绕行手段：

### btm 存了哪些过滤参数

每次会话开始时会打印 btm 自身保存的扫描过滤参数：

```
stored scan param 0xFFFF rc=0x... company=0x.... pattern=............
stored scan param 0x0001 rc=0x... company=0x.... pattern=............
stored smart device UUID rc=0x... size=0x.. bytes=........
```

btm 没有提供"设置过滤参数"的接口，只有"读取"，所以这些值可能就是扫描实际使用的
过滤器。如果它们与本项目的 UUID 不一致，就能解释"开始扫描成功但永远 0 结果"。

### 直接连接（跳过扫描）

把设备的蓝牙地址写进：

    sdmc:/switch/DGLAB-NX/config/dglab-ble-address.txt

内容格式：

    11:22:33:44:55:66

NRO 启动时会读取该文件并显示在界面上（`target: ...`）。存在时，按 `A`/`Up` 会
**跳过扫描，直接连接该地址**，随后照常做服务发现、特征、订阅通知与读写。
这样即使主机的扫描过滤器完全不可用，也能把 GATT 之后的部分验证掉。

注意：这里要的是设备的**真实 MAC 地址**。Android 版 LightBlue 能直接看到；
iOS 显示的是系统为该 App 生成的随机标识（形如 UUID），那个不能用于连接。

## 第七次实机结果（btm 的过滤器与直接连接）

```
stored scan param 0xFFFF rc=0 company=0x0553 pattern=000100000
stored scan param 0x0001 rc=0 company=0x0553 pattern=ADDE00EFB
stored smart device UUID rc=0 size=0x0 bytes=8F71DD81
direct connect to EA:A8:AC:22:2C:18, scan skipped
btdevConnectToGattServer rc=0x0005568F   (每次都是同一个错误)
```

三个结论：

1. **btm 的 general 过滤器是 Nintendo 自己的 company ID `0x0553`**，所以这条扫描路径只能
   匹配任天堂自家设备，永远匹配不到 DG-LAB。
2. **btm 保存的 smart device UUID 是空的（`size=0`）**，也就是说系统没有为"智能设备
   扫描"配置过滤器；传入我们自己的 UUID 也不会改变它实际使用的过滤器。
3. **按地址直接连接被服务拒绝**：`btdevConnectToGattServer` 返回 `0x0005568F`
   （module `0x8F`、description `0x2AB`），三次重试都是同一个错误，说明是服务直接拒绝
   而不是超时。另外这个地址 `EA:A8:AC:22:2C:18` 的最高两位是 `11`，属于 BLE 的
   **随机静态地址**，而 `btmu` 的连接接口只接受地址、不接受地址类型，也没有"设备未被
   发现过"的处理路径。

合起来看：**btm／bt 的扫描与连接接口是围绕任天堂自家设备生态设计的，不是通用 BLE
外设接口。** 这解释了为什么"扫描开始成功、连接被拒、结果永远为空"。

## 第八次实机结果（扫描事件也是 0）

三种过滤器、多轮独立扫描，每次都是：

```
<filter> scan summary: events=0 polls=24 devices=0
```

`events=0` 是关键：连"扫描事件本身"都没有触发过，说明 btm 根本没有替我们运行扫描
（否则至少会有扫描开始/结束事件）。同时 general 过滤器用的是 Nintendo 自己的
company ID `0x0553`，即使扫描真的运行，也匹配不到 DG-LAB。

注：Joy-Con 属于经典蓝牙 HID 设备，配对时走的是 BR/EDR 而不是 BLE 广播，所以
"用 Joy-Con 验证 BLE 扫描通路"这个测试本身并不成立，不能作为 BLE 扫描可用的证据。

### 最后一个驱动级尝试：`Left` 键

btm 在扫描前会设置 BLE 的 scan interval / window；如果缺省值是 0，那么"扫描"实际上
从未真正开启——这与我们的观测一致。`Left` 键会运行一次 btdrv 驱动级探针：

1. `btdrvInitialize` + `btdrvInitializeBle` + `btdrvEnableBle`；
2. 显式设置 `btdrvSetBleScanParameter`（phase 0：0x0060/0x0030；phase 1：0x0030/0x0030）；
3. phase 0 不带过滤器扫描 10 秒，phase 1 用 `btdrvAddBleScanFilterCondition` 过滤
   0x1812 后再扫描 10 秒；
4. 期间不依赖事件句柄，直接轮询 `btdrvGetBleManagedEventInfo`，并把前三次读取的
   `type` 与原始前 8 字节、以及所有 `ScanResult` 的地址/条目数打印出来。

日志里的判读：

- 出现 `btdrv probe: scan result ... addr=XX:...` → 驱动级扫描可用，可以基于它重做传输层；
- 只有 `fetch type=0 raw=0000...` → 事件队列是空的，配合 `btdrvConnectGattServer` 的
  拒绝（module `0x71`），基本可以确定这一代 HOS 不给后台进程通用 BLE 能力。

## 第九次实机结果：探针没跑起来（PoC 的调度问题）

日志里只有 `action queued 11`，没有任何 `btdrv probe:` 输出。原因是：动作只在会话循环的
顶部被取出，而用户按键时通常正在扫描中；扫描达到重试上限（3 次）后会话直接失败退出，
排队中的动作被一起丢弃。

两处修复：

1. **扫描过程中也会处理动作**（在轮询循环里取动作），并且用户操作会重置重试计数，
   不再出现"按键被忽略/会话提前结束"；
2. btdrv 探针改为**延迟执行**：动作只做标记，由会话循环在扫描之外执行，避免在扫描
   轮询内部重入。

另外修了一个日志保存问题：NRO 之前把行截断到 71 字符才写文件，而 `pattern=`、
`bytes=` 这些字段正好在行尾，所以你看到的是 `pattern=0` 这种断尾。现在**屏幕仍显示
短行，但写入 SD 的日志保留完整行**（最长 192 字符）。

## 第十次实测：探针改为自动执行

这一轮日志只有一次会话、没有 `action queued 11`（`Left` 没被按下），但日志截断修复
生效了：`pattern=000100000100`、`pattern=ADDE00EFBE00` 都能完整看到。

为了让这一步不再依赖按键时机，**每次会话开始都会自动运行一次 btdrv 驱动级探针**
（`Left` 仍然可以手动重复）。同时把单次扫描超时从 12 秒降到 8 秒，避免一次会话过长。

于是按 `A` 的日志顺序是：

    poc start ... / btdevInitialize / stored scan param ...
    btdrv probe: ...（约 20 秒，phase 0 无过滤器，phase 1 过滤 0x1812）
    <filter> scan ...（0x1812 → 0x180C → general）

## 第十一次实测：驱动级探针的结果（结论）

```
btdrv probe: btdrvInitialize rc=0x00000000
btdrv probe: btdrvInitializeBle rc=0x00000000
btdrv probe: adapter enabled=1
btdrv probe: btdrvEnableBle rc=0x00000000
btdrv probe: SetBleScanParameter(0x0060, 0x0030) rc=0x00000000
btdrv probe: btdrvStartBleScan (phase 0) rc=0x00000000
btdrv probe: fetch type=0 raw=37000000FF000000          ← 客户端注册事件带错误结果
btdrv probe: fetch type=6 raw=0000000000000000          ← ScanResult，载荷全 0
btdrv probe: scan result status=0 addr=00:00:00:00:00:00 entries=0 rssi=0
btdrv probe: phase 0 done fetches=46 scan_results=1
btdrv probe: SetBleScanParameter(0x0030, 0x0030) rc=0x00000000
btdrv probe: AddBleScanFilterCondition(0x1812) rc=0x00000000
btdrv probe: EnableBleScanFilter(true) rc=0x00000000
btdrv probe: btdrvStartBleScan (phase 1) rc=0x00000000
btdrv probe: phase 1 done fetches=50 scan_results=0
btdrv probe: done, 1 scan result(s) in total
```

解读：

- 所有 btdrv 调用都被接受（`rc=0`），说明**权限和调用顺序没有问题**；
- 事件队列**确实有回调**，但 `ScanResult` 的载荷全为 0，从来没有真实地址或广播数据；
- `type=0` 的客户端注册事件带着 `result=0x00000037`、`client_if=0xFF`，也就是
  **注册本身是失败的**（`0xFF` 是无效接口号）。

### 已经排除的可能

扫描过滤器 UUID、扫描参数（interval/window）、事件源选择（managed 与 LE HID 两个
队列）、轮询与事件两种读取方式、按地址直连、以及权限/调用顺序。

## 这次要确认的开放问题（当时，已被上面的结论回答）

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

---

## nro-ui：将来若要安装依赖

原文来自 `docs/nro-ui.md`（记录，未执行）：

    dkp-pacman -S switch-sdl2 switch-sdl2_ttf switch-sdl2_image switch-mesa \
        switch-glfw switch-glm switch-freetype

（需要联网与写入 `/opt/devkitpro` 的授权；本条只是记录，当前不执行。）

## nro-ui：deko3d 复核（2026-09-15）

原文来自 `docs/nro-ui.md`。分层建议已经落地（`nro/source/ui/canvas.c` 是绘制层，
`nro/source/platform/framebuffer.c` 是后端），所以真要迁的时候成本比上面估的还低。
复核的结论是**当时不值得做**：

| 想要的收益 | deko3d 能给吗 | 现有方案够不够 |
| --- | --- | --- |
| 省 CPU | 能（GPU 合成） | 已解决：界面改成按需重绘，状态不变就不 Begin/End |
| 双缓冲 / vsync | 能 | libnx 的 `framebufferCreate` 已是 2 缓冲 + `framebufferEnd` 提交 |
| dock 1080p 清晰度 | 能 | 未定：`framebufferCreate` 能否直接开 1920×1080 还没实测（见"实机待确认"第 4 条）；若可行，缺的只是布局缩放，与渲染后端无关 |
| 动画 / 实时曲线 | 能（着色器、批绘） | 目前没有需求：静态文字 + 一个二维码 |
| 文字渲染 | **不能**：deko3d 是 GPU API，字体光栅化仍要自己解决 | 现成的 16×16 位图字体够用 |

当时列出的触发条件（满足任一条再启动）：

1. Joy-Con 传感器要在屏幕上显示**实时**波形/曲线；
2. dock 1080p 的清晰度或缩放成为实际痛点，而"canvas 按分辨率缩放"解决不了；
3. 需要成套动画/过渡，或要把位图字体换成矢量字体的观感。

**2026-09-17 更新：决定做，走路线 A。** 上面那张"收益表"的结论不改，仍作为记录：这次
做的是**为后续 GPU 绘制与动画铺路**，不是拿它解决现有的清晰度或性能问题。原触发条件
不再当作"要不要做"的门槛，只作为"先做哪些能力"的顺序参考。

## nro-ui：二维码编码器的 bug 清单

原文来自 `docs/nro-ui.md`：

> 工作过程中被这些测试抓到的真实 bug：格式信息两块的位置写反（等价于转置）、
> 版本 1 被当成有校正图案（越界写内存）、暗模块被格式信息保留区覆盖、以及信封构造
> 没有转义 `pulse` 命令里的引号。

## nro-ui：面板方案的事故与旧 API

原文来自 `docs/nro-ui.md`。2026-09-16 实机截图暴露的问题：面板高度、标签列宽都是
**按 16px 位图字体写死的常数**，换成 24px 系统字体后行高从 16 变成 34，于是
「服务端运行时不要休眠」的字形下沿压在服务端面板的下框线上、日志行的尾部画到二维码
那栏、体感页的说明文字掉出面板、关于页最后一行的 `Language` 压在下框线上，标题也因为
没有用统一的居中算式而偏低。

当时的收裁剪写法（面板已删除，`dglab/ui/panel.h` 不再存在）：

> `dglab/ui/panel.h` 是唯一的画框入口：先画底 + 框 + 标题，再用 `dglabPanelClip` 把裁剪
> 区收到框内侧。所以即使某一屏量错了，文字也只会被框线切断，不会画到框外

当时用 `tests/canvas` 的 `testNothingLeavesItsPanel` 守住这几点：用**实机字体的度量**
（24px 字身、34px 行距、ASCII 半宽、汉字全宽）把五屏**在两种语言下各渲染一遍**，然后检查
①每条框线上没有别的颜色（文字没压到框线）、②框内侧第一圈没有别的颜色（文字没被切）、
③面板之外没有文字（标题栏、面板下方的安全提示与页脚除外）、④关于页标题与体感页标题
在同一行、⑤体感页的标签加最宽的数值仍放得下（英文写 `channel strength` 就会失败）、
⑥面板下方的安全提示能在面板与页脚之间排完。这几条正是上面那些截图里的问题：拿旧代码
跑这套测试会全部失败。（无边框页面里的对应物是 `testEveryPageStaysInItsRegions`。）

## nro-ui：第二、六、七次修正的原文

### 第二次修正（2026-09-16 晚）：按键、图标与页面结构

参考图 1（好友的通知设置）复核后又改了一轮，几个之前量错或没量到的地方：

- **底栏线是白的**：之前把内容行的 `#4D4D4D` 当成了底栏线（还写在 y=620）。全宽扫描 5 张参考图，
  页头线（y=87）与底栏线（**y=647**）都是纯白 `#FFFFFF` 1px；`#4D4D4D` 只出现在行与行之间。
- **首行焦点框被切**：焦点框比行高（82 vs 71），第一行的框上沿在 y≈122，而裁剪区从 y=128 开始，
  上边框被切掉。现在裁剪区从 y=88（页头线下方一像素）开始。
- **按键图标是"实心挖空"**：把参考图脚标的 A 键像素画出来后可以看到，它是**实心圆盘 + 把字母挖掉**
  （字母是背景色的洞），字母墨迹约 15px，对应 22px 字号。之前的"细圆环 + 18px 字"是错的。
  （**2026-09-16 第六次修正**：22px 字母在 26px 的图标框里只剩约 1px 留白，实机截图看得很明显，
  所以字母改成 `DGLAB_TEXT_ICON` 20px 并按字形墨迹居中，四边留白约 6px；图标框尺寸不变，
  见下面的"第六次修正"。）
- **页面段落 ≠ 行说明**：图 1 顶部的"只有当您在线时，才会收到发送给您的通知。"是**白色 24px 段落、无 ◆**；
  行下面的小字才是灰色 18px 说明（缩进 16px）。
- **滑块全部去掉**：连接测试的通道强度、体感页强度、高级参数的档位都改回文字值。

### 第六次修正（2026-09-16）：日志滚动、二维码、菜单、图标与字体生命周期

一轮实机 / 模拟器反馈后的修正，逐条记在这里，免得以后重踩：

- **日志页一次按键滚一行**：`log_offset` 一直是像素单位，而 `logScrollFromDirections()` 每按一次
  只加 `1`，所以"滚一行"实际是滚一个像素。现在一步 = `DGLAB_SCREEN_LOG_PITCH`(37px)，单击永远
  只走一行；连发用它自己的时序（`LOG_SCROLL_HOLD_NS` 300ms 后每 `LOG_SCROLL_REPEAT_NS` 50ms
  一行，约 20 行/秒），不再借用体感强度的 `TEST_STRENGTH_*` 时序，也不再共用它的 hold 状态变量。
  打开日志页仍然停在最新一行（`offset = logMaxOffset()`），所以往上翻时最后一行可能只露出一截
  ——"停在最新"和"对齐行网格"只能选一个。
- **二维码只在服务端运行时显示**：`NET_QR` 只要有局域网地址就返回 payload（地址本身有用，
  docs/ipc.md 的契约不变），但界面以前拿到就画码，于是出现"服务端还没启动就显示二维码"。
  现在 `screen.c` 的 `qrCode()` 还要求 `status_ok && (Listening || Paired)`，否则显示
  `qr_not_running` 的说明。`tests/canvas` 用"这一屏唯一的纯黑像素"数守住它：未运行时左栏黑色
  模块数必须是 0，运行时 > 1000。
- **按键图标的字母缩小并居中**：字母以前用 24px body 画进 26px 圆盘/方框，而且按 line box
  居中——line box 带着基线以下的 descender 空间，实测字母离边框只有 1px（实机截图就是这个
  观感）。现在字母用新增的 `DGLAB_TEXT_ICON`(20px)，并按**字形墨迹盒**
  (`dglabTextInkTop` / `dglabTextInkHeight`) 在框内居中，四边留白约 6px。图标几何没变，
  `dglabHintDraw` / `dglabPageHints` 因此要显式传两个字体（图标一个、动作文字一个）；
  `tests/canvas` 有两条回归：留白不足、以及"图标没有用传进来的图标字体"都会失败。
- **主菜单不循环**：`dglabMenuMove()` 从"回绕"改成"夹紧"到 `0..count-1`，到底再按不会跳回顶端。
- **体感页的灰色小字删掉**：那条 note 讲的是"通道强度是音量、在 Socket 服务端里设置"，而这一页
  自己有通道强度行、也能直接改，属于过期说明。`motion_safety` 这个 key 从 `strings.h` /
  `strings.c` / `lang/*.json` 三处一起删掉（`tests/lang` 保证三方一致）。
- **字体生命周期**：进 BLE PoC 控制台前会 `dglabFontClose()`（`plExit()` 把共享字体的映射释放
  掉），回来后**必须**重新 `dglabFontOpen()`。以前只重建 framebuffer，字形源里的
  `stbtt_fontinfo` 仍指向已释放的映射：缓存里已有的字形还能画，新字符全是空，整屏文字看起来
  "丢了"，只有重启 NRO 才能恢复。这一步现在统一在 `main.c` 的 `appDisplaySuspend()` /
  `appDisplayReopen()` 里（BLE PoC 与底座切换共用）：重建 framebuffer → 重新 ApplyLanguage
  （重开共享字体、按当前 scale 重新光栅化全部字号）→ `g_display_generation++`；每一屏都把
  generation 计入"是否要重绘"，所以重建之后一定重画一帧。

### 第七次修正（2026-09-16）：体感页那两行的名字与判定

实机截图暴露的是**命名**问题：体感页原本把输入侧的两行叫 `通道 A` / `通道 B`，而它们显示的
其实是"左 Joy-Con / 右 Joy-Con 这一侧连上没有、现在动得多猛、映射正在用什么频率"。用户很
自然地把它读成 DG-LAB 通道的连接状态（那是上面那行 `连接`），于是"两行都未连接、设备却有
输出"看起来像自相矛盾。

- 标签改成 `左 Joy-Con` / `右 Joy-Con`（key `motion_joycon_left` / `motion_joycon_right`），
  行序不变：连接 / 左 Joy-Con / 右 Joy-Con / 通道强度 A / 通道强度 B —— 前两行是输入，
  后两行是输出，一眼分开；
- 连接判定同时修过两轮（2026-09-17）：先是"最后一个回答的句柄的最后一条读数"决定整侧（占位
  读数会误判），再是只按读数判断（跟键插回主机、关掉后六轴句柄**照样有读数**，于是一直停在
  "挥动中"）。现在以**主机自己的控制器状态**为准——`hidGetNpadDeviceType` 的按侧设备类型，
  其次是 `padGetStyleSet` / `padGetAttributes` / `padIsHandheld`：拆下来的对应侧 Joy-Con 才
  跟读数走，夹回主机、被关掉或不在的一侧显示未连接、并且**根本不轮询句柄**（所以"显示未连接"
  与"通道有输出"互斥）；六轴句柄的读数只决定波形值与频率，不作连接依据。细节见
  `docs/joycon-input.md`；
- 进入玩法时会往日志写一行 `motion left: handles 2, #0 states … | motion right: …`（落到
  `dglab-net.log`），把主机实际交出的句柄布局记下来；连接状态变化时另写一行
  `motion left connected` / `motion left disconnected`。实机结果与解读见
  `docs/joycon-input.md`。

---

## dglab-socket：睡眠与唤醒的实机经过

原文来自 `docs/dglab-socket.md`：

> 实现是保守的：注册用的模块 id 是 `PscPmModuleId_WlanSockets`，若该 id 已被系统占用
> （`pscmGetPmModule` 返回失败），就只记一行日志并照常运行。
>
> **实机结果：注册被拒绝，返回 `0x0000108A`**（模块 8、描述 0x8A；`WlanSockets` 这个 id
> 由系统自己持有）。
>
> **注册只尝试 `WlanSockets` 这一个 id，而且绝不允许再试别的 id。** 实机证据：拿
> `200`/`201` 之类的自定义 id 去调 `pscmGetPmModule` 会**冻住整台主机**——一次发生在开机
> 路径（卡在开机 logo），一次发生在按 `A` 启动服务时（所有按键无响应，只能长按电源键）。
> `WlanSockets` 则是安全且"失败很快"的：系统自己持有该 id，调用立刻返回 `0x0000108A`。

## dglab-socket：栈缓冲事故的经过

原文来自 `docs/dglab-socket.md`（2026-09-15 与 09-16 两次报告）：

> 实机记录（2026-09-15）：
>
> - 按 `ZL`/`ZR` 会让整个 sysmodule 死掉：`NET_WAVEFORM` 的处理链上，`clear` 和 `pulse`
>   各有一个 1950 字节的栈上缓冲区（`char command[DGLAB_SOCKET_MAX_MESSAGE]`），加上
>   `dglabHandleRequest` 与 WebSocket/socket 写入的调用链就把主线程栈压爆了；
> - 症状是**进程直接消失、所有 IPC 无响应**，客户端拿不到任何 `Result`，所以一开始被当成
>   发送路径的问题查；
> - 把这两个缓冲区挪到 `.bss` 之后，怎么按都不再复现（同一台主机、同一支手机）。
>
> **2026-09-16：第二次报告与结论**。又收到一次同样的症状（按 ZL/ZR → NRO 卡死 → 退出重进
> 提示 sysmodule 未运行）。当前代码里那两个缓冲区已经在 `.bss`（`-fstack-usage` 复核过，这
> 条链上最大的帧是 `dglabNetServerLog` 的 480 字节 + newlib 的 `printf`），**把
> `release/00FF072107210721/` 重新装到 SD 卡并重启之后不再复现**（用户实机确认）。
>
> 这一轮的教训不是某个 API 用法，而是流程：症状组（**NRO 卡死 + 退出重进提示 sysmodule
> 未运行**）的含义是 **sysmodule 进程不在了**，而"SD 卡上装的是旧二进制"与"代码真有 bug"
> 表现**完全一样**。同时补了两条同属这一类的加固：NPDM 的 `main_thread_stack_size` 从
> `0x4000`(16KB) 提到 `0x8000`(32KB)；新增 `make -C tests/stack`。

## dglab-socket：2026-09-15 实机验证记录

原文来自 `docs/dglab-socket.md`（结论已并入主文档的"测试按键"与"关于 3.0 App 的日志"）：

> 优先级 5 在真机上跑通：NRO 显示二维码 → App 扫码绑定 → NRO 用真实强度与波形控制设备，
> 设备有输出。以下几条原先是推断，现在有实机结论：
>
> 1. **指令信封的路由字段**：`clientId` 是**发件人**、`targetId` 是**收件人**（与最初
>    的推断相反，详见"消息外壳"一节）。写成反方向时 App 会记录消息却完全不执行。
> 2. **心跳**：每 30 秒一条、绑定后立刻补发一条的写法，App 接受并保持会话；`message`
>    用 `"200"`。App 不回应心跳。
> 3. **pulse 的 JSON 转义**：转义形式正确，App 能播放我们排队的波形。
> 4. **App 是单向的**：3.0 App 不向服务端发送任何东西（没有 `msg`、没有 `break`、连
>    WebSocket Ping 都没有）。因此 `NET_STATUS` 里 App 上报的强度/上限一直是 0，
>    NRO 的 `app report` 行显示为 `none`。
> 5. **测试按键的强度语义**：设备输出 = 通道强度(0~200) × 波形强度(0~100)。`ZL` 送
>    满强度波形并设置通道强度，所以屏幕上的数字就是实际强度；上限 100（原始值）。
>    （当天两个通道共用一个值、默认 10；之后改成每通道各一个、都从 0 起。）
>
> 仍未解决或未验证：
>
> 1. **服务端运行时休眠会卡死**，只能靠"不自动启动 + 55 秒空闲自动停 + 界面提示"兜底；
>    55 秒自动停这条本身还没有实机确认过。
> 2. **V4 未实现**：`?tid=` 形式在解析里被接受（便于排查），但 V4 的消息外壳没有实现。
> 3. **App 的强度上限不会同步**：见第 4 条，是 App 设计，不是本项目的缺陷。

## joycon-input：连接判定的两轮推翻

原文来自 `docs/joycon-input.md`：

> 早期实现把"最后一个回答的句柄的最后一条读数"直接当成整侧状态：只要有一个句柄回的是
> `IsConnected=0` 的占位读数，整侧就显示"未连接"——而**实机现象（2026-09-16）**正是
> "一对拆下的 Joy-Con、波形输出正常、界面两行却都是未连接"。
>
> 改成"只认读数"之后又反过来（**2026-09-17**）：一旦进了"挥动中"就再也回不到"未连接"
> （关掉、插回主机都不变）——因为 `IsConnected` 位仍然为真而读数已经不来，而规则相信了
> 那个位。

---

## docs-audit：2026-09-17 审计报告原文

下面是 `docs/docs-audit.md` 被压缩前的全文（2026-09-17 的审计快照）。
# 文档规则审计（对照当前代码）

2026-09-17 一次性审计的留档：把仓库里 13 份文档中的规则逐条对照当前代码，标出
**已过时 / 重复 / 可由代码直接推导 / Agent 必须知道 / 仍需保留的历史背景**，并给出
处置建议。清理动作按第 6 节执行，独立于本文提交。

**第 3 节是 2026-09-17 的审计快照**：已处理的条目在行首标 **已处理** 并记下新措辞，
概览表的统计列保持审计当时的数字；本次新增的内容见第 8 节。

结论先说：规则**大部分仍然成立**，问题集中在两处——

1. **随 UI 与构建改动漂移的操作描述**：`README.md`、`docs/nro-ui.md`、
   `docs/joycon-input.md`、`docs/dglab-socket.md` 里有一批"哪个键干什么、菜单叫什么、
   装哪个包"的描述停在了改动之前（第 3.1 节，54 条）；
2. **同一条约束被抄成 2~5 份**：单一连接所有者、发布布局、按键退出规则、`lang/` 用法
   等，改动时只更新了其中一份（第 3.2 节，24 类）。

## 1. 审计基准

| 项 | 值 |
| --- | --- |
| 基准 | 工作区当前状态：`f76d73e` + 8 个未提交改动（`AGENTS.md`、`README.md`、`common/include/dglab/ipc.h`、`docs/dglab-protocol.md`、`docs/dglab-socket.md`、`docs/ipc.md`、`docs/nro-ui.md`、`sysmodule/source/main.c`），按工作区审计，不按 HEAD |
| 范围 | 根 `AGENTS.md`、`common/`、`mods/`、`nro/`、`overlay/`、`sysmodule/` 的 `AGENTS.md`、`README.md`、`docs/*.md`（6 份），共 13 份 |
| 排除 | `third_party/stb/README.md`；代码与 Makefile 中的注释（只作旁证，见附录） |
| 外部协议 | 不联网；只读本机克隆 `../dglab-bluetooth-protocol` 核对引用 |

### 规则口径

一条"规则"= 可独立判断真伪或独立执行的约束句、数值/命名约定、流程与验证要求、行为断言。
不收标题、纯清单表格、纯叙述句和代码示例本身。ID 用文件前缀加序号：

    ROOT 根 AGENTS.md      SYS sysmodule/AGENTS.md   NRO nro/AGENTS.md
    COM common/AGENTS.md   MOD mods/AGENTS.md        OVL overlay/AGENTS.md
    RM  README.md          BLE docs/ble-poc.md       PROTO docs/dglab-protocol.md
    WS  docs/dglab-socket.md                         IPC docs/ipc.md
    UI  docs/nro-ui.md     JOY docs/joycon-input.md

五个标记**可叠加**（一条规则可以既是"重复"又是"Agent 必须知道"），每条另给一条处置建议：
保留 / 改写 / 删除 / 合并到某处 / 下沉到代码或测试。

### 本次实际执行的验证

```
make -C tests/protocol   → 177 + 122 + 64 = 363 checks, 0 failures
make -C tests/ipc        →  51 checks, 0 failures
make -C tests/net        →  47 checks, 0 failures
make -C tests/qr         →  38 checks, 0 failures
make -C tests/lang       → 907 checks, 0 failures
make -C tests/motion     →  23 checks, 0 failures
make -C tests/canvas     → 559 checks, 0 failures
make -C tests/stack      → 116 functions, every stack frame under 1024 bytes
make -C nro package      → release/DGLAB-NX/{DGLAB-NX.nro,lang/*.json}
make -C sysmodule package→ release/00FF072107210721/{exefs.nsp,toolbox.json,flags/boot2.flag}
```

静态核对：文档中出现的路径、符号、常量、命令目标逐个 `rg` / 读文件比对；外部协议来源
核对到本机克隆的 `0155a23cfc123d1243b96482ea475d147573ce8f`（2026-06-11）与
`coyote/v3/README.md`、`coyote/README.md` 都存在，`docs/dglab-protocol.md` 钉的版本有效。

### 不可主机验证

以下内容只能靠实机或 App 侧确认，本次**不据此判过时**，只在文档里标成待确认
（清单见第 7 节）：主机侧 BLE 直连的全部结论、Joy-Con 六轴单位与采样率、DG-LAB App
的实际交互与 V4 消息外壳、底座原生 1080p、按需重绘、睡眠卡死与 55 秒空闲自停。

## 2. 概览

计数口径：**候选条目** = 机械统计的列表项 + 编号项（`^\s*[-*] ` 与 `^\s*[0-9]+[.)] `）；
**①** = 第 3.1 节逐条列出的处置项；**②** = 该文档被卷入的重复组数（同一个重复组涉及多份
文档，故各列之和大于 24）；**③** = 第 3.3 节里属于该文档的处置项；**④/⑤** 指向第 4、5
节的落点。README 的按键表、状态表、文档表按行另计，未算进候选条目。

| 文档 | 候选条目 | ① 已过时 | ② 重复（涉及） | ③ 可推导 | ④ 必须知道 | ⑤ 历史 |
| --- | --- | --- | --- | --- | --- | --- |
| `AGENTS.md` | 137 | 2 | 13 | — | 12 组 | H-13 |
| `sysmodule/AGENTS.md` | 97 | 4 | 7 | P-01 | 10 组 | H-02 |
| `nro/AGENTS.md` | 39 | 2 | 7 | P-10 | 9 组 | — |
| `common/AGENTS.md` | 14 | 2 | 2 | — | 3 组 | — |
| `mods/AGENTS.md` | 14 | 3 | 1 | — | 3 组 | — |
| `overlay/AGENTS.md` | 10 | 2 | 2 | — | 2 组 | — |
| `README.md` | 14 + 表 | 9 | 11 | — | 4 条 | H-13 |
| `docs/nro-ui.md` | 140 | 11 | 8 | P-06 | 第 4 节 | H-04/05/06/09 |
| `docs/joycon-input.md` | 66 | 7 | 4 | P-07 | 第 4 节 | H-06/07 |
| `docs/dglab-socket.md` | 58 | 6 | 4 | P-05 | 第 4 节 | H-02/03/08/10 |
| `docs/ble-poc.md` | 83 | 4 | 2 | P-09 | 第 4 节 | H-01 |
| `docs/dglab-protocol.md` | 47 | 0 | — | P-08 | 第 4 节 | H-12 |
| `docs/ipc.md` | 6 | 2 | 4 | P-02/03/04 | 第 4 节 | H-11 |

最值得先动手的 8 条：`RM-01`~`RM-06`（README 的按键表与菜单名整体漂移）、`UI-01`~`UI-06`
（nro-ui 的旧按键与旧菜单）、`JOY-01`（体感连接判定自相矛盾）、`WS-01`~`WS-03`、
`BLE-01`/`BLE-02`（测试项数与 `make package`）、`ROOT-02`（优先级编号引用全线失效）。

## 3. 需处置清单

### 3.1 已过时

#### README.md

- **RM-01** `README.md:129` — "`Y` | 停止服务端"。现在 `A` 是启停（`nro/source/main.c:719`
  → `toggleServer()`，`main.c:497`：运行中就 `NET_STOP`，否则 `NET_START`），`Y` 是打开
  日志子页（`main.c:710`）。→ 改写。
- **RM-02** `README.md:132` — "`B` | 清空波形（clear A / clear B）"。现在 `X` 清空
  （`main.c:524`，`screen.c:343` 的底栏提示就是 `X clear`），`B` 是返回
  （`main.c:707`）。→ 改写。
- **RM-03** `README.md:135` — "`-` | 切到 BLE PoC 控制台视图"。这个绑定已经取消，入口是
  菜单项（`menu.c:14-20` 的 `DglabMenu_ItemBlePoc`），`-` 只在 PoC 视图内部有效
  （`nro/source/ble_poc_view.c:408`）。→ 删除该行，改到菜单说明里。
- **RM-04** `README.md:111`、`120` — 菜单项名 `Socket test` / `Motion (Joy-Con)` /
  `Advanced (motion)`。现在菜单有 **5 项**，名字取自各页标题：`socket server`、
  `motion (Joy-Con)`、`advanced (motion)`、`about`、`BLE PoC console`
  （`lang/en.json:23,26,32,40,58,85`，`menu.c:14-20`）。→ 改写。
- **RM-05** `README.md:21-26` — "未完成按当前优先级是 13 → 14 → 15 → 16 → 10 → 11 → 17"。
  根 `AGENTS.md` §15 现在分两段从 1 重新编号（已完成 1–9、未完成 1–7），这套 13/14/15/16/10/11/17
  在任何地方都对不上了。→ 改写为功能名（见 ROOT-02）。
- **RM-06** `README.md:178` — "没有空闲超时（只靠 TCP 断开或 `shutdown()`）"。同时又有
  "最后一个客户端离开 55 秒后自动停"（`README.md:151`）与 `NET_IDLE_STOP_MS = 55s`
  （`sysmodule/source/transport/net_socket.c:678`）。两句话说的是不同的东西（对整个服务端
  有超时、对已连接的 App 没有），但写成这样会互相打脸。→ 合并成一句。
- **RM-07** `README.md:128-136` — 按键表缺 `X`（清空）与 `Y`（日志），也没有菜单导航
  （`D-pad` 选择、`A` 进入、`B` 退出），而 `+` 只在 console 页退出的规则没写。
  → 按 `docs/nro-ui.md:639` 的页面结构表重写。（同一处，与 RM-01/02/03 合并处理。）
- **RM-08** `README.md:139` — "界面上 `test` 那一行同时显示两个通道的当前值"。服务端页现在
  的行是 `server / address / app id / channel A / channel B / last cmd`
  （`nro/source/ui/screen.c:268-305`），没有 `test` 或 `strength` 行。→ 改写。
- **RM-09** `README.md:13-18`（状态表） — `nro/` 一栏只提到"测试屏与体感玩法"，缺
  `advanced` / `about` 两屏与日志子页。→ 补一行。

#### docs/nro-ui.md

- **UI-01** `docs/nro-ui.md:300` — "菜单四项定为：连接测试、体感、高级参数、BLE PoC 控制台"。
  现在是 5 项（多一个 `about`，`menu.c:14-20`）。→ 改写，并统一用页面标题的英文名。
- **UI-02** `docs/nro-ui.md:352` — 文件职责表里 `list.c` 写"行、分隔线、聚焦框、**滑块**、
  滚动与滚动条"。滑块已经全部移除（同文件 `:632`），`list.c` 现在只有行、分隔线、聚焦
  框与滚动。→ 改写。
- **UI-03** `docs/nro-ui.md:403` — "进入玩法后 `+` 返回菜单。**`B` 不做返回键**：测试屏里
  它是'清空波形'"。现在正好相反：`B` 是每个 framebuffer 页的返回键，清空是 `X`，
  `+` 在这些页不响应（`main.c:707`、`main.c:524`，`nro/AGENTS.md` 的退出规则）。
  → 改写。
- **UI-04** `docs/nro-ui.md:400` — "启动后先进入菜单：**三行玩法**（`Socket test`、`Motion
  (Joy-Con)`、`Advanced (motion)`、`BLE PoC console`）"。行数与名字都过期：5 项、名字见
  `lang/en.json`。→ 改写。
- **UI-05** `docs/nro-ui.md:428` — "按键提示分两行（`A start`/`Y stop`/`B clear`/`ZL`+`ZR`
  一行，… `- BLE poc`/`+ exit` 一行）"。现在的底栏是 `ZL+ZR test` / `X clear` / `Y log` /
  `B back` / `A start|stop`（`screen.c:341-351`）。→ 改写。
- **UI-06** `docs/nro-ui.md:431-434` — "按键：`A` 启动服务端、`Y` 停止、`B` 清空波形 …"、
  "上一行 `app report`"。同 RM-01/02/08：A 启停、X 清空、Y 日志，`app report` 行已不在
  `screen.c` 的行表里。→ 改写。
- **UI-07** `docs/nro-ui.md:440-470`（"面板：按内容量高度，内容裁剪到框内"整节） — 讲的是
  已删除的 `dglab/ui/panel.h` / `panel.c`（同文件 `:537` 已经记录删除）。这一节现在是
  历史，却按现行规则写。→ 移到"实现记录"并标日期，保留其中仍然成立的纪律（尺寸来自实测、
  标签必须短）。
- **UI-08** `docs/nro-ui.md:518` — 主题表"强调色 …、**滑块的已填充部分**"。无滑块。
  → 改写。
- **UI-09** `docs/nro-ui.md:544` — "光标停在最后一项时，它下面的**滑块**和说明也必须滚进
  视野"。无滑块。→ 改写。
- **UI-10** `docs/nro-ui.md:639`（页面结构表） — "Socket 服务端 | 左栏 x=220 宽 400 …
  右栏 x=620 宽 440"。实测与代码是左栏 80/390、右栏 470/719
  （`nro/include/dglab/ui/screen.h:31-34`，同文件 `:700` 的"第四次修正"已经写对，表格没同步）。
  → 改写。（同一行里的 "6 行参数" 是对的，`screen.c:268-305` 正好 6 行。）
- **UI-11** `docs/nro-ui.md:118,185,189,212,218,343` — "优先级 16 / 8 / 5.4" 等编号引用。
  见 ROOT-02。→ 改写为功能名。

#### docs/joycon-input.md

- **JOY-01** `docs/joycon-input.md:185-199` — "**唯一依据是'读数有没有来'**，句柄自称的
  `IsConnected` 不算"被写成当前规则。现在这一侧能不能用**由主机回答**：
  `hidGetNpadDeviceType` 的按侧设备类型，其次 `padGetStyleSet` / `padGetAttributes` /
  `padIsHandheld`（`nro/source/main.c:1010-1042`），不可用的一侧**根本不轮询句柄**；
  "只认读数"只是都问不出来时的第三兜底。同一份文档前面的"这一侧能不能用"一节
  （`:145-168`）和 `docs/nro-ui.md` 的"第七次修正"（`:717`）写的是新规则，因此这一节
  与本文自相矛盾。→ 改写（保留成历史注记），并删掉与前一节重复的整段。
- **JOY-02** `docs/joycon-input.md:251` — "玩法内部 `+` 返回菜单"。`+` 不响应，`B` 返回
  （`main.c:980`）；菜单本身也是 `B` 退出（`main.c:787`）。→ 改写。
- **JOY-03** `docs/joycon-input.md:277` — 安全表"随时停 | `B`（`clear-A` + `clear-B`）"。
  现在是 `X`（`main.c:524`）。→ 改写。
- **JOY-04** `docs/joycon-input.md:233` — "而 `main.c` 里的 `left[96]` / `right[96]`
  缓冲没跟着改"。现在这两个缓冲已经是 `char left[160]` / `right[160]`
  （`main.c:1065-1066`）。作为历史叙述要改成过去式，否则读者会去代码里找 96。
  → 改写为历史句。
- **JOY-05** `docs/joycon-input.md:87` — "`tests/motion`，72 项"。实际 23 项
  （`make -C tests/motion`）。→ 改写（或删掉项数，见 3.3）。
- **JOY-06** `docs/joycon-input.md:3`、`:77` — "优先级 8"、"优先级 11"。见 ROOT-02。
  → 改写。
- **JOY-07** `docs/joycon-input.md:242-266`（菜单与界面） — "原 `-` 键那个视图，挪进菜单"
  ✓，但"导航：菜单里 `D-pad` 上下选择、`A` 进入、`+` 退出 NRO"里的 `+` 是错的（`B`），
  且菜单项列表缺 `about`。→ 改写。

#### docs/dglab-socket.md

- **WS-01** `docs/dglab-socket.md:191` — "`NET_STOP`（NRO 上按 `Y`）会把它收掉"。现在是
  `A` 启停（`main.c:719`），`Y` 打开日志。→ 改写。
- **WS-02** `docs/dglab-socket.md:213` — 引用的界面文案
  `do not sleep the console while the server runs: press Y first`。实际文案是
  `do not sleep while the server runs`（`lang/en.json:114`，key `sleep_warning`，
  `strings.c:140`），而且它现在是 `server` 行下面的一条 note，不再提按键。→ 改写。
- **WS-03** `docs/dglab-socket.md:249` — "sysmodule 的线程栈只有 16KB"。主线程现在是
  32KB（`sysmodule/DGLAB-NX-Core.json` 的 `main_thread_stack_size: 0x00008000`），
  同一份文档 `:282-283` 已经记录了这次提升。→ 改写（保留 16KB 作为历史）。
- **WS-04** `docs/dglab-socket.md:397` — 测试按键表 "`B` | `clear-A` + `clear-B`"。
  现在是 `X`。→ 改写。
- **WS-05** `docs/dglab-socket.md:402` — "两个值在界面的 `strength` 行上并排显示
  （`A n/100  B n/100`）"。服务端页没有 `strength` 行，两个通道各占一行
  （`channel A` / `channel B`，`screen.c:288-299`）。→ 改写。
- **WS-06** `docs/dglab-socket.md:459` — "优先级 5 在真机上跑通"。见 ROOT-02。→ 改写。

#### docs/ble-poc.md

- **BLE-01** `docs/ble-poc.md:501` — "`make -C tests/ipc` # CMIF 布局 **45 项**检查"。
  实际 51 项。→ 改写（或删掉项数）。
- **BLE-02** `docs/ble-poc.md:505` — "`make -C sysmodule` # 生成 exefs.nsp 与安装目录"。
  `make -C sysmodule` 只构建（`sysmodule/Makefile:155` 的 `all: $(BUILD)`），安装目录
  （`exefs.nsp` + `toolbox.json` + `flags/boot2.flag`）只由 `make package` 生成
  （`sysmodule/Makefile:140-153`）。`make -C nro` 才等价于 `package`（`nro/Makefile:170`）。
  → 改写。
- **BLE-03** `docs/ble-poc.md:17` — "NRO 观察与操作界面 | `nro/source/main.c`"。PoC 的
  视图已经挪到 `nro/source/ble_poc_view.c`（`docs/nro-ui.md:472`、`main.c:1370` 附近的
  console 路径）。→ 改写。
- **BLE-04** `docs/ble-poc.md:6` — "PoC 要回答的是优先级 #5 的问题"。见 ROOT-02。
  → 改写为功能名。

#### 其余文档

- **ROOT-01** `AGENTS.md:296-310`（§11 文档） — 推荐 `docs/architecture.md`、
  `docs/development.md`、`docs/game-mods.md` 三份**不存在**的文档。→ 改成现有 6 份
  `docs/*.md` 的清单，需要新文档时再补。
- **ROOT-02** `AGENTS.md:450-519`（§15） — 已完成用 1–9、未完成用 1–7 两段各自编号，
  而 `README.md:23`、`docs/ipc.md:36,110`、`docs/dglab-socket.md:459`、
  `docs/ble-poc.md:6`、`docs/joycon-input.md:3,77`、`docs/nro-ui.md:118,185,189,212,218,343`
  共 13 处还在引用旧的全局编号（13/14/15/16/10/11、5、6、8、11、16、5.4）。→ 二选一：
  在 §15 给每一项起一个稳定短名（`优先级去重`、`deko3d 后端`…）并让引用点名，或把全部
  引用改成功能名 + 文档链接。推荐后者。
- **SYS-01** `sysmodule/AGENTS.md:18-23`（BLE 所有权） — "不允许 NRO、Overlay 或
  Game Mod 自己建立 DG-LAB **Bluetooth** 连接"。根 `AGENTS.md:81` 已经改成
  "BLE/WebSocket 连接"，现在的设备侧链路是 WebSocket（手机负责 BLE）。→ 改写为该措辞。
  **2026-09-17 已处理**：标题改为「设备侧连接所有权（BLE / WebSocket）」，正文改为
  "连接由 sysmodule 建立、持有、关闭，其它组件只能走 IPC"，删除了"除非有明确的架构理由"例外。
- **SYS-02** `sysmodule/AGENTS.md:25-43`（蓝牙兼容性，8 项验证） — 针对的是已搁置的
  BLE 直连路线（`docs/ble-poc.md:459-472` 的结论）。→ 加一句前置条件"仅在重启 BLE 直连
  路线时适用"，或整节移入 `docs/ble-poc.md`。
- **SYS-03** `sysmodule/AGENTS.md:45-213` — 官方协议参考只覆盖蓝牙协议
  （`dglab-bluetooth-protocol`），现行路线的参考仓库（`dglab-websocket-server`、
  `dglab-kit`、社区 V3 实现，见 `docs/dglab-socket.md:10-16`）在本文里没有对应要求，
  与根 `AGENTS.md:162-178` 的"两条协议线"不一致。→ 把协议来源要求拆成两条线，或指向
  `docs/dglab-*.md`。
- **SYS-04** `sysmodule/AGENTS.md:315-327`（构建验证） — 第 1 条"Title ID 与项目记录一致"。
  实现里只有单一来源：Makefile 从 `DGLAB-NX-Core.json` 读出 `title_id`，校验它存在且是
  16 位十六进制（`sysmodule/Makefile:127-129,142`），没有第二份"项目记录"可比。
  → 改写为"Title ID 只从 `<TARGET>.json` 读，Makefile 不重复写"。
- **COM-01** `common/AGENTS.md:17` — "`common/` 不直接持有 DG-LAB Bluetooth/BLE 连接"。
  同 SYS-01。→ 改写。
  **2026-09-17 已处理**：改为"不建立、也不持有 DG-LAB 设备侧连接（BLE/WebSocket）"。
- **COM-02** `common/AGENTS.md:5-11`（职责） — 列了"错误码；版本信息"，实际
  `common/include/dglab/` 只有 `ipc.h` 与 `ipc_poc.h`：版本在 `ipc.h`，错误码用的是
  libnx 的 `LibnxError_*`（`sysmodule/source/main.c:176` 等），没有自己的错误码定义。
  → 改写，或按 3.3 合并到"IPC 定义"一条。
- **MOD-01** `mods/AGENTS.md:20` — 交互模型里写 `IPC: SendEffect(...)`。不存在这个命令；
  接口是 `NET_SEND`（即时指令）与 `NET_WAVEFORM`（波形流），见 `docs/ipc.md:82,140`。
  → 改写。
- **MOD-02** `mods/AGENTS.md:46` — "或 `docs/game-mods.md`"，该文件不存在。→ 改写。
- **MOD-03** `mods/AGENTS.md:29` — "不得直接持有 DG-LAB BLE 连接"。同 SYS-01。→ 改写。
  **2026-09-17 已处理**：改为"不得自己建立或持有 DG-LAB 设备侧连接（BLE/WebSocket）"。
- **OVL-01** `overlay/AGENTS.md:19,21` — "不负责长期持有 Bluetooth 连接"、
  "不得直接建立 BLE 连接"。同 SYS-01。→ 改写。
  **2026-09-17 已处理**：两条合并成"不得自己建立或持有 DG-LAB 设备侧连接（BLE/WebSocket）"。
- **OVL-02** `overlay/AGENTS.md:30-35` — "修改 Overlay 相关源代码或配置后，应使用该目录
  定义的构建方式重新构建"。`overlay/` 目前没有 `Makefile`（根 `Makefile:36-42` 只在它
  存在时才构建），这条现在无处可依。→ 标注"实现 overlay 时补 `overlay/Makefile`"。→ 改写。
- **NRO-01** `nro/AGENTS.md:40-42` — "应该先用 Console 来呈现字符 UI，待 NRO 的业务逻辑
  全部实现后再实现 GUI"。GUI 已经实现（`nro/source/ui/`，另见 `docs/nro-ui.md:503`）。
  → 删除，或改成历史说明。
- **NRO-02** `nro/AGENTS.md:43-46` — "当前结论是先走 libnx framebuffer 自绘"。现行实现
  仍是 framebuffer ✓，但 §15 已把 deko3d（路线 A）排为未完成第 4 项
  （`AGENTS.md:492-500`、`docs/nro-ui.md:189-195`）。→ 改写为"当前实现是 framebuffer；
  deko3d 只换呈现层，绘制层不动"。
- **IPC-01** `docs/ipc.md:36` — "见 `AGENTS.md` 的优先级 13"。见 ROOT-02。
- **IPC-02** `docs/ipc.md:110` — "接入走下面的 `NET_WAVEFORM`（优先级 6，已完成）"。
  见 ROOT-02。→ 改写为"已完成"。

### 3.2 重复

每条给出唯一归属建议；`→` 后面是处置。

- **D-01 单一连接所有者**：`AGENTS.md:81-105`、`sysmodule/AGENTS.md:18-23`、
  `nro/AGENTS.md:17-21`、`overlay/AGENTS.md:19-21`、`mods/AGENTS.md:27-30`，
  另加 `README.md:36-40` 的"三条规则"。6 处。→ 根 `AGENTS.md` 保留完整表述，
  其余各留一句"设备侧链路只能由 sysmodule 持有，见根 AGENTS.md"。
  **2026-09-17 已处理**：根 `AGENTS.md` §3 保留完整规则（首句改成"只能由 Sysmodule
  建立并持有"），`sysmodule`/`nro`/`mods`/`overlay`/`common` 的 AGENTS 与 `README.md`
  各留一句并指回根。
- **D-02 IPC 变更流程**：`AGENTS.md:107-121`（5 步）、`common/AGENTS.md:20-32`（同一 5 步）。
  → 根保留，`common/AGENTS.md` 留指针。
- **D-03 发布产物布局**：`AGENTS.md:230-261`、`sysmodule/AGENTS.md:273-301`、
  `README.md:66-107`、`docs/ble-poc.md:62-91`。4 处，且 `README.md` 与
  `docs/ble-poc.md` 各画了一遍目录树。→ 根 `AGENTS.md` 保留布局，其余只留"见
  AGENTS.md §8.1" + 各自特有的内容（安装步骤、SD 卡路径）。
- **D-04 按键与退出规则**：`nro/AGENTS.md:94-104`（按键提示与退出规则）、`docs/nro-ui.md:639-645`
  （页面结构表）、`README.md:128-136`（按键表）、`docs/joycon-input.md:251,277`。
  → 归属 `docs/nro-ui.md` 的页面结构表（按页面列全），`nro/AGENTS.md` 保留"Console 用
  `+`、framebuffer 用 `B`"这条不变式，README 只留一句"见 docs/nro-ui.md"。
- **D-05 `lang/` 与文案规则**：`nro/AGENTS.md:122-136`、`AGENTS.md:230-248`（发布布局里的
  `lang/`）、`docs/nro-ui.md:215-305`、`README.md:100-105`。→ `nro/AGENTS.md` 保留
  规则，`docs/nro-ui.md` 保留格式与 key 清单，README 留一句。
- **D-06 SD 卡目录**：`nro/AGENTS.md:137-149`、`README.md:92-99`、`docs/nro-ui.md:250-258`。
  3 处逐字近似。→ `nro/AGENTS.md` 保留，其余留指针。
- **D-07 页面/行/文本规格**：`nro/AGENTS.md:72-121` 与 `docs/nro-ui.md:503-566`、`:617-720`
  大量重叠（两条横线颜色与 y、裁剪区、三种行类型、字号、按键图标"实心挖空"、二维码显示
  条件）。→ 数值与来历留 `docs/nro-ui.md`，`nro/AGENTS.md` 只留"必须遵守"的短条目并指过去。
- **D-08 栈上不要放 KB 级缓冲区**：`sysmodule/AGENTS.md:214-246`、`docs/dglab-socket.md:247-296`。
  → 规则留 `sysmodule/AGENTS.md`，实机经过留 `docs/dglab-socket.md`。
- **D-09 改完必须重装 / 构建标识**：`sysmodule/AGENTS.md:329-349`、`docs/dglab-socket.md:265-295`、
  `AGENTS.md:196-204`。→ 规则留 `sysmodule/AGENTS.md` + 根 §7 一句，实机经过留 docs。
- **D-10 Title ID 单一来源**：`AGENTS.md:230-261`（§8.1）、`sysmodule/AGENTS.md:248-271`、
  `README.md:82-85`、`.gitignore` 注释。→ 规则留 `sysmodule/AGENTS.md`（最具体），根留一句。
- **D-11 禁止猜测 libnx/HOS API**：`AGENTS.md:137-160`、`nro/AGENTS.md:35-38`、
  `sysmodule/AGENTS.md:121-138`（Source of Truth）。→ 根保留通用规则，组件只留"本组件
  该查什么"。
- **D-12 sysmodule 模块名与 NPDM 文件名陷阱**：`sysmodule/AGENTS.md:302-313` 与
  `sysmodule/Makefile:13-17` 注释。→ 文档留一句 + 指针。
- **D-13 DGLAB-NX-Ovl.ovl 产物名**：`AGENTS.md:236-260`、`overlay/AGENTS.md:36-38`。
  → 根保留，overlay 留指针。
- **D-14 "服务端不自动启动 / 55 秒空闲自停 / 睡眠注意"**：`docs/dglab-socket.md:193-245`、
  `README.md:144-152`、`docs/ipc.md:45`（`NET_START` 行）。→ 技术说明留
  `docs/dglab-socket.md`，README 留用户可见的两句。
- **D-15 IPC 命令号不得重排**：`docs/ipc.md:12`、`AGENTS.md:107-121`、`README.md:38-40`、
  `common/include/dglab/ipc.h:16-18`（注释）。→ 规则留根 `AGENTS.md`，头文件注释保留
  （代码旁的约束最有效），docs/README 留指针。
- **D-16 测试清单**：`README.md:154-172` 与各测试目录、`AGENTS.md:205-229`。
  → README 保留（面向人），`AGENTS.md` 只留"改了什么就要重建什么"。
- **D-17 波形槽位/补流参数**：`docs/ipc.md:140-162`、`docs/dglab-socket.md:361-381`、
  `docs/joycon-input.md:116-132`。→ 契约留 `docs/ipc.md`，实现理由留 `docs/dglab-socket.md`，
  joycon 只留"事件源该怎么喂"。
- **D-18 体感玩法参数表**：`docs/joycon-input.md:279-305` 与 `nro/source/motion/motion_settings.c`
  的默认值/范围（代码是权威）、`docs/nro-ui.md:707`（第七次修正）。→ 文档只留"为什么是
  这个默认值"，数值指向代码（见 3.3）。
- **D-19 framebuffer → deko3d 评估**：`docs/nro-ui.md:122-213` 与 `AGENTS.md:492-500`。
  → 评估留在 `docs/nro-ui.md`，§15 只留一句目标与验收。
- **D-20 语言包/翻译范围与术语**：`docs/nro-ui.md:279-305`、`nro/AGENTS.md:122-136`、
  `lang/*.json` 自身。→ 术语表留 `docs/nro-ui.md`（它已声明是"统一译名"的唯一处）。
- **D-21 体感连接判定的历史经过**：`docs/joycon-input.md:170-241`（两轮 09-16/09-17）、
  `docs/nro-ui.md:717-737`（第七次修正）、`nro/source/main.c:1010-1024` 注释。
  → 结论留一处（`docs/joycon-input.md` 的规则段），经过合并成一段历史，UI 文档留一句指针。
- **D-22 NRO 元信息/版本号单一来源**：`AGENTS.md:471-478`（§15 未完成 1）与
  `docs/ipc.md:31-41`（"与发行版本号是两回事"）。→ 规则留 §15，ipc.md 留 IPC 版本表。
- **D-23 BLE PoC 的安装/操作步骤**：`docs/ble-poc.md:69-122` 与 `README.md:87-107`。
  → README 留常规安装，PoC 文档只留 PoC 自己的前提。
- **D-24 "不要提交 release/"**：`AGENTS.md:250-261`、`.gitignore:2-3`、`README.md:64`。
  → 规则留根 `AGENTS.md`，`.gitignore` 是执行者。

### 3.3 可由代码直接推导

这一类删掉的只是"抄一遍代码"，**保留"为什么这样"和违反的后果**。

- **P-01** `sysmodule/AGENTS.md:230-246` 的 `ALLOW` 名单（`netClientThreadMain`、
  `wsConnRecv`、ble_poc 的三项）与"≥1KB 就失败"。名单在
  `tests/stack/Makefile:37`，阈值在 `check_frames.awk`。→ 文档只写"新增栈帧 ≥1KB 会让
  `tests/stack` 失败；例外要写进 `tests/stack/Makefile` 的 `ALLOW` 并注明理由"。
- **P-02** `docs/ipc.md:57-111` 的 `NET_STATUS` / `NET_SEND` 字段表与取值范围。字段在
  `common/include/dglab/ipc.h:57-149`（含注释），越界校验在 `sysmodule/source/main.c:176`
  等处。→ 保留"状态快照语义、通道 0 表示两路、强度是用户设定/波形由事件源设定"这几条
  语义，字段清单改为指向头文件。
- **P-03** `docs/ipc.md:43-55` 命令号表。`common/include/dglab/ipc.h:20-28` 是权威。
  → 保留"号一旦发布不得重排"与新增命令的规矩，表本身可删或降级为"命令名 → 号"的速查。
- **P-04** `docs/ipc.md:14` — "每个回复必须放进 0x100 字节"。`sysmodule/source/main.c:13-23`
  的 `_Static_assert` 与 `sysmodule/include/dglab/ipc_cmif.h:23,58` 已经强制。
  → 保留一句"超出会在编译期失败"。
- **P-05** `docs/dglab-socket.md:340-412` 的补流参数（队列 128 槽、每批 32 槽、提前 200ms）、
  心跳 30 秒、2 条连接、日志环 4KB、1950 字节上限：`sysmodule/include/dglab/net/net_server.h:24-46`、
  `dglab_socket.h:17`、`net_socket.c:678`。→ 保留"为什么是这些值"（App 播完就停、
  留余量避免断音、55s 短于系统 60s 自动休眠），数值指向头文件。
- **P-06** `docs/nro-ui.md:509-534`（量出来的规格表）：颜色在 `nro/source/ui/theme.c`
  与 `nro/include/dglab/ui/theme.h`，坐标在 `nro/include/dglab/ui/page.h:16-40`、
  `screen.h:31-34`。→ 保留"数值来自
  1280×720 截图的逐像素测量"这句话与规格表的作用（改主题时的依据），坐标以代码为准。
- **P-07** `docs/joycon-input.md:279-305` 的参数默认值/范围表：
  `nro/source/motion/motion_settings.c` 与 `motion_feed.c:25-33`。→ 保留"哪些值必须
  贴着皮肤试"的经验判断，数值指向代码。
- **P-08** `docs/dglab-protocol.md:32-93` 的 BLE 特性 UUID 表与 B0/BF/B1 报文布局：
  `sysmodule/include/dglab/protocol/coyote_v3.h:24-28` 与 `coyote_v3.c` 的编解码。
  → 保留"与官方 V3 文档逐字节对齐、已用 4 个官方例子验证"的结论，字节图保留（它是
  对照官方文档的凭据，不是代码的抄写）。
- **P-09** `docs/ble-poc.md:269-286`（里程碑串 `ble scan found client conn svc char
  notify b0 b1 bat`）与 `:123-153`（每一步的预期日志行）：来自 `ble_poc.c` 的日志
 字符串与 `DglabPocStatus`。→ 保留"看哪一行判断卡在哪"，具体文案指向代码。
- **P-10** `nro/AGENTS.md:108-109`（日志子页一次按键 = 一行）与 `:118-120`（跑哪些测试）
  里的数值细节：`main.c:218-219,593-594`、`screen.h:26`。→ 保留不变式，数值去掉。

## 4. Agent 必须知道

这里只列"代码里看不出来、但决定下一次改动对错"的规则，并给出应留在哪一份文件。这也是
第 3 节之外**其余全部条目**的归类：它们既不重复、也不能从代码推导，且不属于历史背景。

### 根 `AGENTS.md`（保留）

1. 单一设备侧链路所有者（BLE 或 WebSocket 都是 sysmodule）；
2. IPC 是内部公共 API：改类型 → 改客户端 → 改文档 → 必要时升版本 → 全量构建；
3. 禁止猜测 libnx / HOS / BLE API，先查 headers、源码、examples；
4. 两条协议线的版本号互不对应（Socket V3/V4 ≠ Coyote V2/V3）；
5. 改 sysmodule 之后必须 `make -C sysmodule package` + 覆盖 SD + 重启，否则验证的是旧二进制；
6. 发布产物一律落在 `release/`，`<TITLE_ID>` 目录名从 `DGLAB-NX-Core.json` 推导；
7. 版本敏感内容必须写明适用版本（HOS、游戏版本、Title ID、Hook 地址）；
8. 不提交构建产物、密钥、个人路径与敏感日志；
9. 不要把某一组件没有实现、或"看起来合理"的 API 当成存在；
10. 发现新知识时：项目级规则才进 `AGENTS.md`，技术细节进 `docs/`；
11. 组件职责边界（sysmodule / nro / overlay / mods / common 各管什么）；
12. 不要为了局部任务破坏 BLE 所有权、IPC 边界、协议层与 UI 解耦。

### `sysmodule/AGENTS.md`（保留，按 SYS-01~03 改写）

1. 协议实现前必须核对官方仓库（两条协议线各自的仓库）并确认版本；
2. 冲突时标记而不是自己挑一个"看起来合理"的答案；
3. 协议版本必须显式区分（`CoyoteV3` / V2 不混层）；
4. 不复制整个外部仓库，协议定义与平台实现解耦；
5. 栈很小：KB 级缓冲区放 `.bss`，每连接的缓冲留在该线程栈上；
6. 开机路径只做三件事（`sm` 注册、互斥锁、会话核心），其余推迟到按需启动；
7. PSC 只尝试 `WlanSockets` 一个 id，绝不再试别的（会冻住整机）；
8. 每个连接自己的缓冲不能共享（两个客户端同时在线会互相踩）；
9. 新增栈上大缓冲前先问"是不是该放 `.bss`"，确要留就进 `ALLOW` 并写理由；
10. 构建产物的目录名/模块名/toolbox 字段都从单一来源推导，不重复硬编码。

### `nro/AGENTS.md`（保留，按 NRO-01/02 改写）

1. 页面由页头 + 行列表 + 底栏组成，没有面板边框；两条横线（y=87/647）是白的；
2. 行只有三种类型，界面里没有滑块，数值用文字；
3. 行数组同一个交给 measure 与 draw，高度/列宽/滚动都来自这次测量；
4. 字号只能用 `text.h` 的五个；
5. 按键提示 = 图标 + 动作文字，按键名不进 `lang/`；
6. Console 页用 `+` 退出，其余 framebuffer 页用 `B`，改了按键必须同时改底栏提示；
7. 无光标又无滚动键的页面必须排进一屏；
8. 布局坐标一律 720p 逻辑单位，缩放只在 canvas 层做；
9. 重建 framebuffer（底座切换、进出 PoC 控制台）必须走
   `appDisplaySuspend()` / `appDisplayReopen()`。

### `common/AGENTS.md`、`mods/AGENTS.md`、`overlay/AGENTS.md`（保留）

1. `common/` 只放真正共享的内容，不放业务逻辑、不碰传输；
2. Game Mod 必须记录 Title ID / 游戏版本 / Build ID / Hook 地址 / 已验证版本；
3. Game Mod 只做"游戏事件 → IPC"的映射，不实现协议；
4. Overlay 通过 IPC 使用 sysmodule，不假设与 NRO 共用渲染环境或生命周期。

### `README.md`（保留）

1. 当前传输路线是 Wi-Fi + WebSocket（手机负责 BLE）；
2. 服务端不会开机自启，最后一个客户端离开 55 秒后自动停；
3. 服务端运行期间不要让主机休眠（睡眠通知被拒，只能长按电源键）；
4. 测试按键会真的输出电压，先确认设备与电极。

### `docs/`（技术细节的归属地）

1. Socket V3 消息外壳的路由字段方向（`clientId` = 发件人）是实机扫出来的，写反了 App
   会记录但不执行；
2. 强度由用户设定、波形由事件源设定，设备输出 = 通道强度 × 波形强度；
3. App 播完就停，所以连续波形必须持续供给；
4. App 3.0 是单向的：不上报强度、不发心跳，所以 `app report` 一直为空不是解析 bug；
5. QR 编码器只做 byte 模式、版本 1..10，与 Apple 参考实现比对过；
6. 中文字形默认阈化成 1-bit，缓存上限与最大字号 × 最大 scale 绑在 `_Static_assert` 上；
7. BLE 直连在 HOS 22.5.0 上不可用，重启该路线前必须先做只读逆向。

## 5. 历史背景（保留，但要标明是历史）

| # | 内容 | 位置 | 建议去向 |
| --- | --- | --- | --- |
| H-01 | BLE 直连的 11 次实机记录与结论 | `docs/ble-poc.md` 全文 | 原地保留（它就是历史文档），只在开头加一句"已搁置，结论见 §结论" |
| H-02 | 主线程栈 16KB → 32KB、两次"看起来像 bug 的旧二进制" | `sysmodule/AGENTS.md:329-349`、`docs/dglab-socket.md:247-296` | 规则留 AGENTS，经过留 docs（D-08/D-09） |
| H-03 | 开机路径最小化（`mkdir` Data Abort、PSC 冻机） | `docs/dglab-socket.md:231-245` | 原地保留 |
| H-04 | 改成 HOS 风格无边框页面之前的面板布局与"量一遍画一遍"事故 | `docs/nro-ui.md:440-502` | 移入"实现记录"并标日期（UI-07） |
| H-05 | 字形左右镜像（位序误判）与图标字母过大的两轮修正 | `docs/nro-ui.md:381-397,617-633` | 原地保留 |
| H-06 | 体感连接判定被推翻的两轮（09-16 占位读数、09-17 关掉仍给读数） | `docs/joycon-input.md:170-241`、`docs/nro-ui.md:717-737` | 合并成一段（D-21） |
| H-07 | `left[96]` 日志截断事故 | `docs/joycon-input.md:227-235` | 改成过去式（JOY-04） |
| H-08 | 消息路由字段写反导致"绑定成功、指令无效" | `docs/dglab-socket.md:46-68,383-395` | 原地保留（它是这条规则的由来） |
| H-09 | 早期"是否要做 deko3d"的复核与触发条件 | `docs/nro-ui.md:157-213` | 原地保留，2026-09-17 的决定已经写在后面 |
| H-10 | `dglab-boot.log` 的来去 | `docs/dglab-socket.md:311-313` | 原地保留 |
| H-11 | IPC 版本 0.1.0 / 0.2.0 的变化表 | `docs/ipc.md:39-41` | 原地保留（升级时的参照） |
| H-12 | 协议层与官方文档的 4 处差异与实现选择 | `docs/dglab-protocol.md:178-192` | 原地保留 |
| H-13 | 旧优先级编号（1–17 全局连续） | `README.md:21-26` 等 13 处 | 改写引用（ROOT-02），旧编号本身只在这一条里留一句 |

`AGENTS.md` 里除了 `sysmodule/AGENTS.md:329-349` 这类"违反会出事"的一句指针外，不再保留
任何经过叙述——历史都放 `docs/`。

## 6. 清理执行清单

按顺序做，每一步之后跑一次第 1 节的验证命令。

**第 1 步：改事实错误（3.1 全部条目）**

1. `README.md`：按键表、菜单名、优先级引用、`test` 行、状态表（RM-01~09）。
2. `docs/nro-ui.md`：菜单项数/名字、`B`/`X`/`+` 语义、底栏提示内容、两处坐标表、
   三处"滑块"、面板小节降级为历史（UI-01~11）。
3. `docs/joycon-input.md`：连接判定段重写 + 与前一节合并、`+`/`B`/`X`、`left[96]` 过去式、
   测试项数、优先级引用（JOY-01~07）。
4. `docs/dglab-socket.md`：`A`/`Y`/`X`、界面文案引用、16KB、`strength` 行、优先级（WS-01~06）。
5. `docs/ble-poc.md`：51 项、`make package`、PoC 视图文件位置、优先级（BLE-01~04）。
6. `AGENTS.md` §11 文档清单 + §15 编号方案（ROOT-01/02）；`sysmodule`、`common`、`mods`、
   `overlay`、`nro` 的 AGENTS 措辞（SYS-01~04、COM-01/02、MOD-01~03、OVL-01/02、
   NRO-01/02、IPC-01/02）。

**第 2 步：单一归属（3.2 的 D-01~D-24）**

按"根 AGENTS 管通用、组件 AGENTS 管局部、docs 管技术细节与来历"落位，重复处改一句指针。
先做 D-01/02/03/04/05/06/07（改动面最大、收益最高），再做其余。

**第 3 步：下沉可推导项（3.3 的 P-01~P-10）**

删字段清单与常量取值，改为指向头文件/测试；保留"为什么"和事故后果。

**第 4 步：历史归位（第 5 节）**

AGENTS 只留一句指针；`docs/nro-ui.md` 的面板小节并入"实现记录"；体感判定两轮合并成一段。

**第 5 步：验证与提交**

- 重跑 8 个测试 + `make -C nro package` + `make -C sysmodule package`；
- 文档中引用的路径/符号再扫一遍（白名单：`release/`、SD 卡路径、外部仓库路径、
  `DGLAB-NX-Ovl.ovl` 等刻意不存在的产物）；
- `git diff` 逐份复核没有无关改动；
- 两个 commit：`docs: audit rules against code`（本文 + README 文档表）与
  `docs: dedupe rules into single home`（第 1~4 步）。

## 7. 待实机确认与不可主机验证

本次不判过时、只登记的地方：

1. BLE 直连的全部结论（`docs/ble-poc.md`）：HOS 22.5.0 + AMS 1.11.2 上不可用；
2. Joy-Con 六轴的物理单位、采样率、`IsInterpolated` 频率（`docs/joycon-input.md:336-351`）；
3. `hidIsSixAxisSensorAtRest` 能否用于零偏归零；applet 模式读六轴的开销；
4. DG-LAB App 的实际交互：V3 心跳内容、App 3.0 单向通信、强度上限不同步；
5. V4 消息外壳的字段名（`controller_disconnected` 与 `client_disconnected` 的差别：
   `docs/dglab-socket.md:121-144` 两个都列了，`AGENTS.md:502-505` 只写了后者）——前置是
   官方 beta 稳定与 App 版本确认；
6. 底座原生 1080p 与按需重绘在真机上的表现（`docs/nro-ui.md:485-501`）；
7. 服务端运行时休眠卡死，以及 55 秒空闲自停是否真的发生过（`docs/dglab-socket.md:459-472`）；
8. `docs/ble-poc.md:488-495` 的已知限制与 `sysmodule/AGENTS.md:25-43` 的 8 项蓝牙兼容性
   验证——都属于"重启 BLE 路线时再验证"。

## 8. 追加（2026-09-17）：两种传输模式的口径

用户确认并要求文档体现的内容：

- **BLE 模式**：sysmodule 直接连接 DG-LAB 设备（Coyote 协议）——**未实现**，该模式已搁置
  （重启前需只读逆向）。
- **WebSocket 模式**：sysmodule 与手机 DG-LAB App 建立 WebSocket 会话——Switch 当服务端、
  App 扫码连入（Switch 不主动外连）；手机负责与设备之间的 BLE，并把波形数据转发给设备
  ——**已实现**（Socket V3）。

落点：

| 文件 | 写了什么 |
| --- | --- |
| `AGENTS.md` | 项目简介加"两种传输模式"表，图上的边标签区分实现状态；§6 两条协议线各标模式与状态；§15 标题「已搁置：BLE 模式（sysmodule 直连设备）」 |
| `README.md` | 开头换成"两种模式"表；状态表把"主机侧 BLE 直连"改成"BLE 模式（sysmodule 直连设备）"；架构图画上"手机 ──BLE──→ 设备" |
| `sysmodule/AGENTS.md` | 职责列表按模式拆分；标题「蓝牙兼容性（仅 BLE 模式，当前未实现）」 |
| `docs/dglab-socket.md` | 开头标注本文对应 WebSocket 模式；"为什么走 WebSocket 模式而不是 BLE 模式" |
| `docs/dglab-protocol.md` | 开头标注 Coyote V3 属 BLE 模式，其编解码被 WebSocket 模式复用 |

写这两段时核对过的两点事实（不能写反）：

1. WebSocket 模式下 **Switch 是服务端**，由 App 扫码连入；
   `sysmodule/source/transport/net_socket.c` 走的是监听路径，Switch 不主动外连。
2. `docs/dglab-protocol.md` 不是"纯 BLE 文档"：它的 B0/BF/B1 与波形频率换算被 WebSocket
   模式的 `pulse` 编码复用（`sysmodule/source/net/dglab_socket.c` 与 `NET_WAVEFORM`）。

## 9. 追加（2026-09-18）：浅色主题的实现口径

需求（用户原话的意思）：参考 HOS 浅色主题截图给 NRO 加浅色模式；先判断能不能跟随系统，
不行就以深色为默认；当前颜色主题的列表项放在关于页语言行下方，按 `Y` 切换。

实现前的两条核对（先查再写，见根 `AGENTS.md` §4）：

1. libnx 确实有 `setsysGetColorSetId()`（`switch/services/set.h:1106`，
   `ColorSetId_Light = 0` / `ColorSetId_Dark = 1`），所以"跟随系统"可以做，不需要
   fallback 成"默认深色"；取不到系统的分支仍然保留，那一条按用户要求落到深色。
2. 用户给的截图是 1280×720 的原生浅色主题系统设置页，与深色那批截图同一来源，因此
   按"逐像素量"的规矩直接量，不猜色。量的工具是一段一次性的 PNG 解码脚本（`sips` 转
   PNG 后在内存里解 filter、按区域取众数/中位数），数值见 `docs/nro-ui.md` 新表。

两条需要用户拍板的取舍（2026-09-18 确认）：

- **跟随系统只在启动与重建画面时读一次**，不做每秒轮询：NRO 在前台时用户进不去系统
  设置，代价是"挂起期间改了系统主题要重启 NRO 才会跟上"，这一点写进了文档；
- **截图里量不到的颜色先沿用深色值**（`error` / `warn` / 对话框三色）：截图里没有错误
  提示、也没有对话框，按"截图到位前不写数值"的规矩不编数，文档标注"未量到"。

落点与踩过的点：

| 文件 | 写了什么 |
| --- | --- |
| `nro/include/dglab/ui/theme.h`、`nro/source/ui/theme.c` | 浅色调色板、`DglabThemeMode`（auto/light/dark）与 `dglabThemeResolve()`；`dglabThemeGet/Set()` 语义不变 |
| `nro/include/dglab/ui/settings.h`、`nro/source/ui/settings.c` | `app.cfg` 改为一个模块拥有两个键（`language=` / `theme=`）；解析从默认值起步，旧文件升级不丢语言 |
| `nro/source/ui/language.c` | `dglabLanguageSerialize/Parse` 删除（语言偏好仍在这里，文件格式归 settings.c） |
| `nro/source/main.c` | `setsysInitialize()` + `appThemeApply()`（启动与 `appDisplayReopen()` 各一次）、`appSettingsSave/Load()`、关于页 `Y` 与主题进入重绘判定 |
| `nro/source/ui/about.c` | 第八行「颜色主题」，Auto 显示成 `跟随系统（深色）`；底栏第三条提示 `[Y] 切换主题` |
| `lang/*.json`、`nro/source/ui/strings.*` | 新增 5 条文案（`action_theme`、`about_theme*`） |
| `tests/canvas/test_canvas.c` | 区域检查与页头状态检查改成"两套调色板 × 两种语言 × 720p/1080p"，新增关于页主题行用例 |
| `tests/lang/test_appcfg.c`（新）、`tests/lang/Makefile` | `app.cfg` 的往返/升级/未知行/未知值，以及主题模式的键、循环与 `Auto + 取不到系统 = 深色` |
| `tests/canvas/tools/render_preview.c` | `PREVIEW_THEME=light`，不用主机就能看浅色各屏 |

写测试时发现的两件事，都是主机侧字体的性质，后面改屏要注意：

- 关于页在主机块字体下**总是比一屏高**，主题行是最后一行，所以那条用例必须把 `offset`
  推到被夹紧的位置才看得到这一行（`offset = 0` 时它根本不在画面里，早先的比较因此全等）；
- 同一个块字体每个字符都是一样宽的黑块，`浅色` 与 `深色` 都是两个全宽字形，**像素完全
  相同**：这条用例只用英文串比较（`Light` / `Dark` 长度不同），值本身的语义由
  `tests/lang` 的解析用例守住。

## 10. 追加（2026-09-18）：NRO 侧自动休眠抑制的口径

需求（用户原话的意思）：服务端运行时让主机的自动休眠定时器不生效；能不能做、怎么做先在
NRO 上研究，放弃 sysmodule 那条路线。

先查再写（根 `AGENTS.md` §4），查证结果：

1. `set:sys` 有直接对口的接口：`setsysGetSleepSettings()` / `setsysSetSleepSettings()`，配合
   `SetSysHandheldSleepPlan_Never` / `SetSysConsoleSleepPlan_Never`（libnx 的
   `switch/services/set.h`）。两个符号在 `libnx.a` 里确实存在（`nm` 查过）。
2. applet 侧也有：`appletSetAutoSleepDisabled()` / `appletIsAutoSleepDisabled()`
   （`applet.h`，`ISelfController`，5.0.0+，头文件**没有**标注 AppletType 限制），以及
   `appletSetMediaPlaybackState()`（`IApplicationFunctions`，注释写明"true 则禁用变暗与自动休眠"，
   但只对 `AppletType_*Application` 可用）。
3. NPDM 里的 `svcSleepSystem` 是 devkitPro sysmodule 模板原样带来的全量 syscall 表里的一项，
   与"能不能阻止睡眠"无关（对照过 `/opt/devkitpro/examples/switch/templates/sysmodule/sysmodule.json`）。

用户拍板的三条（2026-09-18）：

- 走 **NRO 侧抑制**（applet），不改 sysmodule、不碰 `set:sys`：那条路要改用户可见、掉电保持的
  全局设置，写权限还没验证过，而且同样挡不住手动休眠；
- 抑制失败只记日志并保留旧警告文案，不因此拒绝启动服务端；
- 服务端页的警告按状态显示两种文案（抑制生效 / 未生效）。

落点：

| 文件 | 写了什么 |
| --- | --- |
| `nro/include/dglab/nro/auto_sleep.h`、`nro/source/platform/auto_sleep.c`（新） | 唯一所有者：只在服务端状态变化时动作；进入抑制前先读一次，本来就关的不接管也不恢复；失败回传 `Result` 与事件类型 |
| `nro/source/main.c` | socket 页与体感页在轮询到服务端状态后调 `dglabAutoSleepFollowServer()`，结果写一行日志；退出前 `dglabAutoSleepRestore()`；`DglabScreenSnapshot` 与新字段一起进重绘判定 |
| `nro/include/dglab/ui/screen.h`、`nro/source/ui/screen.c` | `DglabScreenState::auto_sleep_suppressed`，服务端行的 note 在两条文案之间切换 |
| `lang/*.json`、`nro/source/ui/strings.*` | 新增 `sleep_warning_auto_off`（en：`Auto sleep is off; sleeping by hand still hangs the console`） |
| `tests/canvas/test_canvas.c` | socket 页的排版用例从 4 种状态扩到 6 种，让两套警告文案都过一遍区域与"内容不贴底"检查 |

已知边界（写进了 `docs/dglab-socket.md` 的「睡眠与唤醒」与 `README.md` 的已知限制）：

- **手动休眠（电源键）仍然会卡死**：自动休眠关得掉，用户按下去的睡眠挡不住；
- **NRO 退出后服务端仍在跑时**，applet 会话结束、抑制失效，自动休眠仍会卡死。这条按用户选择保留，
  等做 Overlay 时一起解决。

**实机结论（2026-09-18）**：applet 模式（相册进入）与 title override 两种启动方式下抑制都生效，
服务端运行期间主机不再自动休眠。这条方案唯一的前提（`appletSetAutoSleepDisabled` 在 NRO 这两种
启动模式下都可用）因此得到确认——早先只能从 libnx 头文件"没有标注 AppletType 限制"推出来。

## 11. 追加（2026-09-18）：反复启停后服务端起不来（0x1759 / 0x559 / 0xD401）

**两次实机现象**（用户，sdmc 上的 `dglab-net.log`）：

- 第一轮：在 socket 页连按 `A`，9 次成功之后第 10 次起每次都失败，此后必须重启主机：

      listening on port 9999
      accept thread failed rc=0x00000559

- 第二轮（换成静态线程栈之后）：**按第二下**就起不来，错误码变成

      listening on port 9999
      accept thread failed rc=0x0000D401

  同一份日志里还第一次出现了 `thread close (accept|tick) rc=0x00001759`。

**诊断**：

- `0x00001759` = `LibnxError_BadInput`，来自 `threadClose()` 的守卫：`Thread.tls_array`
  非零（线程还挂在 libnx 的线程链表里）时它直接返回、什么都不释放。`tls_array` 由
  `_EntryWrap` 在线程启动时写入、由 `threadExit` 在退出时清除；
- 我们的 stop 路径**先复制 `Thread`、再 join、再关副本**，副本里带的正是"复制那一刻线程
  还在跑"的值，而清理只发生在 live 结构上——所以 close **每一轮都失败**，线程的栈、栈镜像
  映射和句柄全部留下；
- 后果链解释了两次现象：栈从堆里要的版本每轮漏 18–22KB，十轮吃光
  `INNER_HEAP_SIZE = 0x80000`（512KB）→ `threadCreate()` 里 `aligned_alloc(0x1000, …)`
  失败，即 `0x00000559`（`LibnxError_OutOfMemory`，反汇编 `threadCreate` 只有这一条来路；
  本 build `__tls_end - __tls_start = 0x418`，栈 `0x4000`，故请求 `0x4660`）；
- 改成静态 `.bss` 栈之后，没被解除的镜像让下一轮 `threadCreate()` 的 `svcMapMemory` 失败，
  即 `0x0000D401`（`KERNELRESULT(InvalidMemoryState)`）——**同一个 bug 换了签名**；
- 代价是我们先按"堆被吃光"下过一次结论，把它当成了根因；真因在两次日志之后才由
  `thread close (…) rc=0x1759` 这一行坐实（第一轮时那两个返回值被直接丢弃，所以什么日志
  都没有）。

**改动**：

- `net_socket.c` 的 `netJoinThread()` 一律作用于 **live** `Thread`：stop 的
  accept/tick/client 三处调用点不再复制结构，`netStartClient()` 复用连接槽时也改成对
  `slot->thread` 等待 + 关闭；等待与关闭的 `Result` 都检查并记日志
  （`thread close (<名字>) rc=0x… (the thread's stack was not released)`）；
- `ble_poc.c` 的 worker 有同一类"复制再关闭"写法（它通常在 join 前线程已退出，所以没暴露），
  一并改成 live 结构并记日志；
- 静态线程栈、常驻 heap 探针（每次启停各一行 `heap start/stop: used/free/arena + delta`）
  保留，定位改成"加固 + 观测"，不再是根因修复；
- `sysmodule/Makefile`、`nro/Makefile`：构建标识（NRO 侧连 `APP_VERSION`）写进
  build 目录的 `build-stamp` 文件并让全部对象依赖它，标识变化时把对象回拨到 1980 年以强制
  重编——第二轮日志里 `dglab 2ac34ef-dirty` 与 `server start, dglab 243d8ba-dirty` 并存，
  就是老 `main.o` 没被重编造成的（make 3.81 只比较秒级 mtime，用回拨而不是单纯比时间戳）；
- 文档：`docs/dglab-socket.md` 的该节按真因重写（三个错误码、两个签名、探针读法），
  `sysmodule/AGENTS.md` 的"线程与栈"加入"只能关 live `Thread`、必须检查 `Result`"这条规则，
  "改完必须重装"一节记上构建标识现在会自动重编。

**主机验证**：`make -C sysmodule`（含 `_Static_assert`；`.bss` 里 5 块页对齐线程栈）、
`tests/net`、`tests/protocol`、`tests/ipc`、`tests/lang`、`tests/motion`、`tests/qr`、
`tests/canvas` 全部 0 failures，`tests/stack` 仍然 every stack frame under 1024 bytes；
两个 Makefile 手工验过：无改动连跑第二次不重编，标识变化时全部源码重编（同一秒内来回改也
能重编），最终 ELF 里只剩一个标识字面量。

**实机结论**：待用户按计划验证（连按 `A` ≥30 次 + 空闲自停一轮 + App 扫码/ZL·ZR 测试波形），
验收标准是没有 `accept thread failed`、没有 `thread close (…)` 行，且 `heap start` 的 delta
一直为 0。

## 12. 追加（2026-09-19）：ZL/ZR 让 sysmodule 进程消失、以及 socket 写路径加固

**现象**（用户实机）：App 扫码绑定之后按 `ZL`/`ZR`，**sysmodule 进程消失但没有崩溃报告**
（`crash_reports/` 与 `atmosphere/fatal_errors/` 都空），NRO 随之卡死、重进提示"sysmodule
未运行"，只能重启主机。

**定位过程**（三轮）：

1. 先在主机侧排除：现有 180 项核心测试 + 照 NRO 的 ZL/ZR 序列（`Replace` + 48 槽 waveform →
   `SetStrength` → 连按 → tick 排流）写的临时复现，在 ASan/UBSan 下 0 错误 ⇒ 问题落在 Switch
   专有的那几层（socket 写 / 线程 / IPC 交付）。
2. 加"发送前后"标记后，sysmodule 自己写的 `dglab-sys.log` 停在
   `waveform tx: ch A, 32 slots, 161 bytes` 而**没有**写完后的 `tx ok` ⇒ 死点被夹在
   `sendCommand()` → `sendText()` → `wsConnSend()` → `netSocketWrite()` → `send()` 里。同一轮把
   连接线程的栈从静态 `.bss` 临时改回 libnx 堆栈，结果**照崩** ⇒ 与静态栈无关。
3. 再加 `write begin`/`write end` 把真正的 `send()` 括起来之后**不再复现**：1508 行日志里每对
   都成对返回、`waveform tx ok: … rc 0` 正常，收尾是一次正常的 `rc -1, errno 32`（EPIPE，
   App 主动断开）→ `app … disconnected` → `stopped`。两轮构建唯一的差别就是多了几行 SD 日志
   ⇒ 判定为**时序相关**，根因没有确定。

**处置**（不声称修掉根因，而是按 `docs/dglab-socket.md` 里原先写好的方向把整类条件去掉）：

- 波形上传只入队：`dglabNetServerUploadWaveform()` 只清队列、置 `clear_pending`、入队，
  `clear-<ch>` 与 batch 一起由 tick 线程的 `waveformPump()` 发（`Replace` 的"立刻重来"只多
  ≤100ms 延迟）；
- 整帧一次写：`WsConn` 增 `tx[WS_MAX_MESSAGE + 16]` 与 `lock`/`unlock` 回调，`wsConnSend()` 在
  锁内拼好帧后只调用一次 `conn->write()`（握手回复同样），传输侧把回调接到连接槽的
  `write_mutex`，并把加锁从 `netSocketWrite()` 移除；
- 非阻塞 + 失败即断开：`send(..., MSG_DONTWAIT | MSG_NOSIGNAL)`，部分写或错误只记一行
  `write failed: … errno … fd …` 并 `shutdown()` 该连接，让连接线程自己 detach；
- 连接线程的栈恢复成静态 `.bss`（bisect 已证明与它无关）；
- 临时探针（`waveform rx` / `waveform tx` / `waveform tx ok` / `write begin|end`）全部删除，
  只保留常驻的 heap 探针、`thread close (…)` 与 `write failed:` 行——再遇到"进程没了、日志
  停在 `tx …` 之后"，先照这轮把 `write begin|end` 加回来。

**顺带修掉的工具问题**：`tests/stack` 用 `-std=c11` 编译 sysmodule 源码，而 sysmodule 自己的
构建不传 `-std`；`-std=c11` 会定义 `__STRICT_ANSI__`，newlib 因此藏掉 libnx `<sys/socket.h>`
里的 `MSG_DONTWAIT`，检查工具直接编译不过。改成 `-std=gnu11`，与真实构建同一个语言模式。

**主机验证**：`tests/net`（62 / 185 / 55，沙箱外回环 38、`test_ws` 71）、`tests/protocol`
（177 / 122 / 64）、`tests/ipc`（51）、`tests/lang`（48 / 835）、`tests/motion`（98 / 23）、
`tests/qr`（38）、`tests/canvas`（1168）全 0 failures；`tests/stack` 118 个函数全部 <1KB。
新增断言：`test_net_server` 检查"上传只入队、下一次 poll 先 `clear-<ch>` 再 `pulse-<ch>`"，
`test_ws` 检查"一帧只产生一次 `write()`"。

**实机结论**：待用户按计划验证（连 App 后 `ZL`/`ZR` 各 10 次、断开重连两次、连按 `A` 30 次
回归）。

## 13. 追加（2026-09-19）：ZL/ZR「卡死」的根因是锁反转

**新证据（用户）**：卡死之后在文件管理器里 **`dglab-sys.log` 打不开、提示"资源被占用中"**，
删掉 sysmodule 并重启才能读里面的内容。⇒ 那条日志文件一直开着 ⇒ **sysmodule 进程还活着**，
是**挂住**，不是崩溃（也解释了为什么 `crash_reports/` 与 `atmosphere/fatal_errors/` 都是空的）。

顺着"挂住"查代码，找到一个锁反转：

- IPC / tick 线程：持 transport 锁（`g_net.mutex`）→ `wsConnSend()` 拿帧锁（连接槽
  `write_mutex`），方向是 **transport → frame**；
- 连接线程：在 `wsConnSend()` 里持着帧锁，`send()` 失败时走 `netSocketWrite()` 的失败分支，
  那里调 `netLog()` 去拿 transport 锁，方向是 **frame → transport**。

两个方向同时发生就互等：连接线程拿帧锁等 transport 锁，tick 线程拿 transport 锁等帧锁。
日志正好停在这种位置上（`tx clear-1` / `waveform tx: …` 之后），而且进程活着、文件被占用、
没有崩溃报告——每一条都对得上。

这条反转是**我自己引入的**：最初 `netSocketWrite()` 的失败路径什么都不记，我在"抓 ZL/ZR
问题"那一轮给它加了 `write failed:` 日志，那时就把 `netLog()` 放进了帧锁里。

**修法**：

- 失败路径不再加锁：把 `errno`/剩余字节/`fd` 记进连接槽（`write_fail_pending` 等字段），
  `shutdown()` 该连接后返回；**由 tick 线程下一次持锁循环时统一打印**
  （`write failed: … bytes left, errno … fd …`），方向恒为 transport → frame；
- probe 线程也修正了同类问题：读探针环时只短暂持有 probe 锁，SD 的 `fwrite`/`fflush` 移到锁外，
  这样"慢/卡住的 SD 写"不会变成链路的一环；
- 规则写进 `sysmodule/AGENTS.md`：锁顺序只能是 transport → frame → probe，连接线程在帧锁里
  不得调用任何会拿 transport 锁的函数；
- `docs/dglab-socket.md` 补上"用 `dglab-sys.log` 能不能打开来判断进程活着还是没了"这条技巧。

**同轮保留的诊断**（用于确认与兜底）：探针环 + `dglabNetServerProbe()`（只 memcpy，不进
`log_sink`）、独立 probe 线程每 25ms 落盘 `dglab-probe.log` 并每秒写 `alive:`、`send begin/end`
两行探针、以及两条粗粒度 SD 标记（`waveform ch …` / `waveform tx: …`）。找到根因并验证稳定后，
这些会一并删除（代码里都标了 `TEMPORARY (ZL/ZR hang diagnosis)`）。

**主机验证**：`tests/net`（62 / 197 / 55，沙箱外回环 38、`test_ws` 71）、`tests/protocol`
（177 / 122 / 64）、`tests/ipc`（51）、`tests/lang`（48 / 835）、`tests/motion`（98 / 23）、
`tests/qr`（38）、`tests/canvas`（1168）全 0 failures；`tests/stack` 全部 <1KB。

**实机结论**：待用户验证（连 App 按 `ZL`/`ZR` 各 10 次、断开重连两次、连按 `A` 30 次）；若仍
挂住，`dglab-probe.log` 的最后几行会直接指出卡在哪一次 `send()`。

## 14. 追加（2026-09-19）：探针抓到卡死点——合并成一次 send() 的 waveform batch

**实机结果**：锁反转修完之后仍然卡死，但探针把位置钉死了。`dglab-probe.log`（77 行）里：

    send begin: tick fd 6, 138 left
    send end:   tick fd 6, 138 left, rc 138, errno 0     ← clear 正常
    send begin: tick fd 6, 308 left                       ← 没有配对的 send end
    alive: probe thread 8 … 66, clients=1, state=2        ← 进程一直活着（66 秒）

全文 6 条 `send begin` 只有 5 条 `send end`。同时用户在文件管理器里看到 `dglab-sys.log` 与
`dglab-probe.log` 都"被占用"——`netNowMs` 之外没有别的解释：**tick 线程卡死在
`send(fd, …, 308, …)` 这一次调用里，进程没死**。`dglab-sys.log` 的最后一行也正好是
`waveform tx: ch A, 32 slots, 161 bytes`（batch 发送前的粗粒度标记）。

**判断**：308 字节＝帧头 4 + payload 304 合并成的一次写，而"已知能工作"的那一轮（加了 SD 探针
就没复现的构建）是**阻塞 `send()` + 帧头/payload 两次写**，同一条 batch 分 4 + 304 两次都正常
返回。这一轮同时改了两件事（`MSG_DONTWAIT` 与"整帧一次写"），两件都退回：

- `netSocketWrite()` 的 `send()` 只带 `MSG_NOSIGNAL`（阻塞，`SO_SNDTIMEO` 5s 兜底）——带
  `MSG_DONTWAIT` 交给 `bsd:u` 的请求可能永远不回，调用者在核里挂着，flag 管不到；
- `wsConnSend()` 回到"帧头 → payload"两次写，互斥仍然由 `WsConn.lock/unlock`（连接槽
  `write_mutex`）保证，因此插帧问题不会回来。

其余全部保留：波形只入队（tick 线程发）、部分写/错误即 `shutdown()` 该连接、失败信息由 tick
线程持锁打印（第 13 条的锁反转修复）、探针环 + `dglab-probe.log` + `alive:` 这套诊断。

**主机验证**：`tests/net`（62 / 197 / 55，沙箱外回环 38、`test_ws` 71）、`tests/protocol`
（177 / 122 / 64）、`tests/ipc`（51）、`tests/lang`（48 / 835）、`tests/motion`（98 / 23）、
`tests/qr`（38）、`tests/canvas`（1168）全 0 failures；`tests/stack` 全部 <1KB。
`test_ws` 的断言相应改回"一帧两次写：帧头在前、payload 在后"。

**实机结论**：待用户验证（连 App 按 `ZL`/`ZR` 各 10 次 → 断开重连两次 → 连按 `A` 30 次）；
`dglab-probe.log` 里应出现成对的 `send begin/end`（帧头 4 字节与 payload 各自一对）。

## 15. 追加（2026-09-19）：实机确认修好，诊断代码全部拆除

**实机结果（用户）**：装上第 14 条的构建后**不再卡死**。证据（`dglab-probe.log` 518 行）：

- **178 条 `send begin` 对 178 条 `send end`**，一条不缺；成对形状正是"帧头 4 字节 + payload"
  （例如 `send begin: client fd 6, 4 left` / `send end … rc 4` 后接
  `send begin: client fd 6, 131 left`）；
- `alive:` 一路写到 162，`state` 走完 1（监听）→ 2（已配对）→ 3（已停止），`clients` 回到 0；
- `dglab-sys.log` 是干净的 `listening → server start → stopped`，`heap start/stop` 都是
  `used=43k free=4k arena=47k`（进出相等，没有泄漏）。

**处置**：结论确认后把诊断全部拆掉，只留修复本身：

- 删：探针环（`DGLAB_NET_PROBE_CAPACITY`、`server->probe*`、`dglabNetServerProbe()`、
  `dglabNetServerReadProbe()`）、`netProbe()`、probe 线程与其静态栈、probe 互斥量、
  `send begin/end` 探针、`netCurrentThreadName()`、`waveform tx: …` 粗粒度标记、
  `tests/net` 的 `testProbeRing` 与 harness 的 `log_sink` 计数；
- 留：波形只入队（tick 线程发）、**一帧两次写 + 帧锁**、`send()` 不带 `MSG_DONTWAIT`（阻塞 +
  `SO_SNDTIMEO`）、部分写/错误即 `shutdown()`、写失败由 tick 线程持锁打印（锁反转修复）、
  静态线程栈、heap 探针、`thread close` 日志；
- 文档：`docs/dglab-socket.md` 的"socket 写路径"保留最终规则，并把这次用的诊断方法
  （文件占用判活/内存环探针/独立落盘/`alive:`）写成"以后遇到同类问题照这个做"的一节；
  `sysmodule/AGENTS.md` 的锁顺序规则去掉了已经不存在的 probe 环节。

SD 卡上的 `logs/dglab-probe.log` 是诊断遗留文件，可以直接删。

**主机验证**：`tests/net`（62 / 185 / 55，沙箱外回环 38、`test_ws` 71）、`tests/protocol`
（177 / 122 / 64）、`tests/ipc`（51）、`tests/lang`（48 / 835）、`tests/motion`（98 / 23）、
`tests/qr`（38）、`tests/canvas`（1168）全 0 failures；`tests/stack` 全部 <1KB。

## 16. 追加（2026-09-19）：拆掉探针后又复现 ⇒ 换"阶段标记 + 看门狗"

**实机结果**：第 15 条那个"只剩修复"的构建，同一个操作**又卡死**，`dglab-sys.log` 被占用
（进程活着），日志停在：

    waveform ch A, 48 slots        ← IPC 线程：上传只入队
    tx clear-1                     ← tick 线程：pending clear（IPC 线程随后被 transport 锁挡住）

而带探针的那一版（第 14 条）跑了一整轮都没卡。⇒ 结论：**热路径里"每帧一次 memcpy + 一把锁"
这种量级的额外工作也足以改变时序**，探针本身在掩盖问题；这不是"探针没用"，而是"探针不能留在
同一个构建里"。

**这一轮的仪器（不改任何行为）**：

- `netStage()`：几个 `volatile` 字段记"最后到达的阶段"（`send begin/end`、
  `log write begin/end`、tick 循环）+ 线程号/fd/字节数/errno，纯 store，不加锁、不写盘；
- 看门狗线程（静态栈，随第一次成功启动创建）：每 500ms 看计数器，**超过 3 秒没动**才往
  `logs/dglab-stall.log` 追加一行 `stall: <阶段> for <n> ms, thread=…, fd=…, bytes=…, errno=…,
  clients=…, state=…`；它只在卡住之后写文件，因此不会掩盖问题；
- 读法：`stall` 行的阶段就是卡住的那一步（`send begin` = socket 写，`log write begin` = SD 日志
  写）；`dglab-stall.log` 缺失或为空 ⇒ 卡住的那条链连 fs 都进不去，看门狗自己也被挡住。

其他一切都保持第 15 条的状态（波形只入队、一帧两次写 + 帧锁、`send()` 不带 `MSG_DONTWAIT`、
部分写/错误即断开、写失败由 tick 线程打印、静态栈、heap 探针）。

**主机验证**：`make` 通过（net_socket.c 手动编译无警告）；`tests/net`（62 / 185 / 55，沙箱外
回环 38、`test_ws` 71）、`tests/protocol`（177 / 122 / 64）、`tests/ipc`（51）、
`tests/lang`（48 / 835）、`tests/motion`（98 / 23）、`tests/qr`（38）、`tests/canvas`
（1168）全 0 failures；`tests/stack` 全部 <1KB。

**实机结论**：待用户复现并回传 `dglab-stall.log`（若存在）、`dglab-sys.log`、`dglab-net.log`。

## 17. 追加（2026-09-19）：卡死变成"卡一下"——看门狗 shutdown 自救生效

**实机结果（两轮）**：

1. 第一阶段（只有阶段标记 + 看门狗记录）抓到根因位置：
   `stall: send begin for 3282 ms, thread=tick, fd=5, bytes=304, errno=0, clients=1, state=2`
   ⇒ 卡在 `bsd:u` 的 socket 写里（阻塞 socket + `SO_SNDTIMEO` 5s 都没兑现），不是 SD/fs 路径
   （`dglab-stall.log` 本身写出来了）。
2. 给看门狗加上"发现卡在 `send begin` 就把这个 fd `shutdown()`"之后，用户实测：
   **会卡一下，但不会卡死了**。那一轮 `dglab-stall.log` 只有一条
   `stall: log write end for 3453 ms, thread=ipc, fd=-1, bytes=0, errno=0, clients=0, state=3`
   （SD 写的一次抖动），`dglab-sys.log` 231 行，App 断开后正常 `stopped`、`heap stop` 与
   `heap start` 相等（`used=42k free=5k arena=47k`）。

**处置**：这套"阶段标记 + 看门狗 + shutdown 自救"从诊断升级为**常驻安全网**（代码注释由
`TEMPORARY` 改成 `Stall safety net`）：热路径只多几个 `volatile` store，看门狗每 500ms 醒一次、
只在卡住 ≥3s 时才写 `logs/dglab-stall.log` 并自救。`docs/dglab-socket.md` 的"诊断 socket 写
卡死"一节记录了它的读法与代价。

**下一步（未做）**：把 socket 写移出 transport 锁，这样即使某次写被 `bsd:u` 挂住，也只有那一个
线程被拖住，IPC（NRO）不再跟着卡几秒；需要把每帧先落到连接自己的缓冲区、锁外再写，改动比
前几轮大，得单独安排。

**主机验证**：`make` 通过；`tests/net`（62 / 185 / 55）、`tests/stack`（120 个函数全部 <1KB）。

## 18. 追加（2026-09-19）：写移出 transport 锁；停服仍给 App 发 close

**实机结果**：加了"锁外 flush"之后不再卡死（用户确认）。但随之暴露一个回归：**NRO 里停止服务端，
App 不同步断开**——`stop` 路径把 `slot->tx` 整个丢掉了，连 `close` 帧一起丢。

**修法**：`stop` 里先丢弃积压的波形数据，再把 `close` 帧排进队列，**在锁外** `netFlushAllPending()`
（只写 6 字节左右的帧）然后才 `shutdown()`/`close()` socket，所以"停服时 App 会显示断开"这条
行为恢复，而 flush 仍然不在 transport 锁里（万一网络栈卡住，只有 flush 的那个线程被挂住）。

**主机验证**：`make` 通过；`tests/net` 55 项、`tests/stack` 122 个函数全部 <1KB。

## 附录：审计中看到的代码注释残留（不在本次范围）

顺手指出来，因为它们是同一批改动的尾巴：

- `nro/include/dglab/ui/screen.h:5`、`:45` 仍写 "the two channels with their sliders"，
  滑块已删；
- `nro/include/dglab/ui/list.h:43,56` 的注释里还有 "Item/Slider"；
- `nro/source/main.c:784-785` 的注释说 `+` 在菜单里仍可用，实际菜单只处理 `B`/`A`/方向键；
- `sysmodule/source/transport/net_socket.c:613-621` 的 `kCandidates[]` 只有一个元素，
  是"只允许这一个 id"规则的载体，注释已经写明，保留即可。
