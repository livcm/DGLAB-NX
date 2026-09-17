# BLE Transport PoC

本文记录 **BLE 模式**（sysmodule 直接连接 DG-LAB 设备）的实机可行性验证，环境
HOS 22.5.0 / AMS 1.11.2 / Switch 1。**结论：这条路走不通，模式已搁置**（见根
`AGENTS.md` §15）；现行传输模式是 WebSocket，见 `docs/dglab-socket.md`。

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

BLE 模式已搁置，安装步骤见 `README.md`（`make` 或 `make -C sysmodule package`）；PoC
只要 sysmodule 装好、NRO 能启动即可。

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
| `Left` | 运行一次 btdrv 驱动级探针（见归档的第八次实机） |
| `-` | 停止 PoC（清理并退出） |
| `+` | 退出 NRO |

动作键在 PoC 未运行时（界面显示 `state: idle`）会自动先启动一次运行，
所以空闲界面直接按 `Up`／`Down` 也能工作。

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

- `events=0`：BLE 事件没收到，问题在事件通路，与设备无关；
- `events>0, results=0`：广播被过滤或设备没有在广播，按 `ZL` 关闭过滤器重扫；
- `results>0` 但没有 `coyote 3.0 found`：广播内容与预期 AD 类型不一致，
  日志里的 `ad type=.. data=..` 就是它实际广播的内容，此时可以按 `ZR` 直接连接，
  先把 GATT 与通知部分验证掉。

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

### 结论（截至 HOS 22.5.0 / AMS 1.11.2）

已经排除的可能：扫描过滤器 UUID、扫描参数（interval/window）、事件源选择（managed 与
LE HID 两个队列）、轮询与事件两种读取方式、按地址直连、以及权限/调用顺序。

剩下的解释是：libnx 的 BLE 绑定（btdrv/btm）面向 HOS 5~9 时代实现，在 22.5.0 上要么
事件载荷布局已改变（读到的是空/错位数据），要么 Nintendo 只对系统自身的流程开放
通用 BLE 客户端能力（btm 的 general 过滤固定为 Nintendo company ID `0x0553`、
smart device 过滤为空、连接请求被直接拒绝）。

**因此在本机环境下，"Switch 后台 sysmodule 直连 BLE 外设"这条路暂时走不通。**
继续深入只有两个方向：逆向 HOS 22.5 的 BLE ABI（工作量大、结果不确定，且违反
"不猜测 API" 的项目原则），或者换传输层。

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
