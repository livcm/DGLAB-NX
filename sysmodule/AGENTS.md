# Sysmodule AGENTS.md

## 职责

`sysmodule/` 是项目的核心后端。

负责（传输分两种模式，当前只实现 WebSocket 模式）：

- **WebSocket 模式**：与手机 DG-LAB App 的 WebSocket 会话（Switch 是服务端、App 扫码连入）、
  Socket 协议、把事件源的波形数据转发给 App（见 `docs/dglab-socket.md`）；
- **BLE 模式**：直接连接 DG-LAB 设备（Coyote 协议）——**未实现，该模式已搁置**，
  见 `docs/ble-poc.md`；
- DG-LAB 设备发现、连接、断开；
- 设备状态管理；
- Effect / Wave / Command 等协议层功能；
- 对外提供稳定的 IPC API；
- 管理 DG-LAB 连接生命周期；
- 处理来自多个客户端的请求。

## 设备侧连接所有权（BLE / WebSocket）

Sysmodule 是 DG-LAB 设备侧连接（BLE 或 WebSocket）的唯一所有者：连接由它建立、持有，
也由它关闭。其它组件只能通过 IPC 与 Sysmodule 通信，不得自己建立或持有这类连接。

规则与理由见根 `AGENTS.md` 的"单一 DG-LAB 连接所有者"。

## 蓝牙兼容性（仅 BLE 模式，当前未实现）

只要HOS支持，DG-LAB BLE 连接必须与 Nintendo Switch 无线控制器
和蓝牙音频共存。

不要假设该 BLE 连接是无害的。

在考虑完成蓝牙实现之前，验证：

1. 在 DG-LAB 连接时，Joy-Con L/R 可以保持连接。
2. 现有的无线控制器仍然有效。
3. 新的无线控制器可以从`控制器->更改握把/顺序`完成配对。
4. 在 HOS 允许的情况下，蓝牙音频可以与 DG-LAB 共存。
5. HOS 强加的控制器限制并没有错误地归因于 DG-LAB。
6. 当NRO退出时，系统保持稳定，而 Sysmodule 保持 DG-LAB 的连接。
7. 睡眠/唤醒行为经过测试。
8. DG-LAB 断开/重新连接不会干扰控制器重新连接。

不要仅仅基于成功的 BLE 连接来声明蓝牙是完美兼容的。

## DG-LAB 官方协议参考仓库（BLE 模式 / Coyote）

本节只管蓝牙这条协议线（Coyote V2 / V3）。本项目的 DG-LAB Bluetooth Protocol 实现必须
以官方开源协议仓库作为主要协议参考（简称 `dglab-bluetooth-protocol`）：

    https://github.com/dungeonlab-open/dglab-bluetooth-protocol

（WebSocket 那条协议线的来源见 `docs/dglab-socket.md`。）

### 外部协议实现约束

任务涉及 DG-LAB Bluetooth Protocol 时，实现、修改或调试之前必须先检查
`dglab-bluetooth-protocol` 的相关文档与实现，步骤固定为：

    1. 定位官方协议仓库   2. 确认设备型号   3. 确认协议版本   4. 阅读对应协议文档
    5. 提取所需协议细节   6. 检查当前项目已有实现   7. 再开始编码

不得跳过第 1~4 步直接写协议代码；不得仅凭模型已有知识、其他第三方项目或对协议的猜测实现
DG-LAB Protocol。如果本项目代码与官方文档不一致，先确认差异原因，不要直接假设本项目的
实现是对的。

### 优先参考的内容

Coyote 设备优先看仓库里的 `coyote/README.md`、`coyote/v2/README.md`、
`coyote/v3/README.md`，按实际设备与协议版本选对应文档。不要因为其他 DG-LAB 设备的协议
结构相似就把它的协议套到 Coyote 上。

不要因为其他设备的协议结构相似，就将其协议直接用于 Coyote。

### 实现时必须核对

实现 DG-LAB Bluetooth Protocol 时，至少核对：

- Bluetooth service UUID；
- characteristic UUID；
- characteristic properties；
- Bluetooth device name；
- MTU / packet size；
- command header；
- command payload；
- packet length；
- byte order；
- sequence number；
- channel A / B 数据格式；
- strength 数据格式；
- waveform 数据格式；
- response / notification 格式；
- reconnect 后需要重新设置的参数；
- protocol version differences。

例：实现 Coyote V3 时不得直接套用 V2 的数据格式——V3 文档明确说明两者的数据处理方式
存在差异，以 V3 文档为准。

### Source of Truth

优先级：① DG-LAB 官方 `dglab-bluetooth-protocol` 仓库；② 本项目已验证的实现与测试结果；
③ libnx / Switch 官方接口文档；④ 其他第三方实现；⑤ Agent 自身知识。

来源冲突时：不要自行选一个"看起来合理"的答案 → 标记冲突 → 检查官方仓库对应版本 →
必要时实机确认 → 在代码或 `docs/dglab-protocol.md` 里记录结论。

### 协议版本

协议版本必须显式记录，不要造一个含糊的 `DGLabProtocol` 把 V2、V3 的行为混在一起：用
`DglabCoyoteV2Protocol` / `DglabCoyoteV3Protocol` 这类命名，或在协议层用明确的版本结构
区分。只支持一个版本时也必须在代码与文档里写明。

### 外部仓库的使用方式

能上网时：打开官方仓库 → 读根 README → 定位当前设备与协议版本的目录 → 读对应 README →
按需检查其他相关文件 → 再开始实现。

不能上网时：不得假装已经读过官方仓库；改用 `docs/dglab-protocol.md` 或本地缓存资料，
资料不足就明确指出缺少官方协议依据。

### 不要复制整个外部仓库

`dglab-bluetooth-protocol` 是参考来源，不是要整仓复制进本项目；按 Switch / libnx 的架构
重新实现，保持协议定义与平台实现解耦：

    Official DG-LAB Protocol → Protocol Layer → BLE Transport → Sysmodule → IPC

### 协议文档

协议移植完成后，在 `docs/dglab-protocol.md` 记录：支持的设备与协议版本、官方协议来源、
Switch 侧 BLE 实现方式、官方协议到本实现的映射、已验证 / 尚未验证的指令、已知限制，并注明
官方参考仓库地址：

    https://github.com/dungeonlab-open/dglab-bluetooth-protocol

## 线程与栈

sysmodule 的线程栈都很小：主线程由 NPDM 的 `main_thread_stack_size` 决定
（`sysmodule/DGLAB-NX-Core.json`，**32KB**，最初是 16KB），网络线程在
`sysmodule/source/transport/net_socket.c` 里由 `NET_THREAD_STACK_TOTAL` 的**静态 `.bss`
栈**提供（可用栈 `NET_THREAD_STACK_SIZE`，页对齐，`ble_poc.c` 的 worker 同样写法）。这条
路径上叠着 libnx 的 IPC/服务调用、newlib 的 `printf` 和 fs 写入，**不要在这些函数里放大
缓冲区**。线程栈也**不要交回给堆**：sysmodule 的堆是 `main.c` 里固定的 `INNER_HEAP_SIZE`
（512KB），libnx 的 `threadCreate()` 拿到 NULL 栈时会用 `aligned_alloc()` 从这里要一块
（本 build 每线程 18–22KB），accept/tick 这种每次启停都新建的线程会把堆当成它的栈池。

线程的回收有个必须记住的坑（`docs/dglab-socket.md` 的"反复启停后服务端起不来"）：

- `threadWaitForExit()`/`threadClose()` 必须传 **live** 的 `Thread`，**不能传 join 之前的
  副本**——libnx 的 `threadClose()` 在 `tls_array != 0`（线程还挂在它的线程链表里）时直接
  返回 `LibnxError_BadInput` = `0x00001759` 并且什么都不释放，而副本里留着的正是线程运行
  时的值；这样每一轮启停都会漏掉那个线程的栈、栈镜像映射和句柄，症状是"反复启停后服务端
  起不来、必须重启主机"（堆栈版本 `rc=0x00000559`，静态栈版本 `rc=0x0000D401`）；
- 两个调用（等待、关闭）的 `Result` 都要检查并记日志：这正是上面那条 bug 藏了很久的原因。

已经踩过两次的坑：一条 `char command[1950]`（`DGLAB_SOCKET_MAX_MESSAGE`）或
`char frame[WS_MAX_MESSAGE]` 放在栈上时，加上已有的调用链会把主线程栈压爆。症状是
**整个 sysmodule 直接死掉**（进程消失、IPC 全部无响应），不是返回一个错误——所以从
客户端看不到任何 `Result`，只能靠日志和实机表现判断。实机复现记录见
`docs/dglab-socket.md` 的"栈上不要放 KB 级缓冲区（血泪教训）"。

规则：

- `net_server.c` 这类被 IPC 线程、tick 线程、客户端线程共用的代码里，KB 级缓冲区放
  `.bss`（`static`），并在注释里写明"调用方持有 transport 锁"，因为一份缓冲区就够；
- 每个线程的栈都放在 `.bss`（`NET_THREAD_STACK_TOTAL` 大小、页对齐），`threadCreate()`
  一律传 `stack_mem` + `sizeof()`，不要让它去堆里分配；
- socket 写：帧层用 `WsConn.lock/unlock` 把**帧头 + payload 两次写**放进同一把锁（传输侧接连接
  槽的 `write_mutex`），`send()` **不要带 `MSG_DONTWAIT`**（实测带它交给 `bsd:u` 的请求可能
  永远不回，调用者挂在核里），超时靠 `SO_SNDTIMEO`；部分写或错误即 `shutdown()` 该连接；
  **IPC 线程不做可能阻塞的 socket 写**——波形上传只入队，发送由 tick 线程做
  （见 `docs/dglab-socket.md` 的"socket 写路径"）；
- **锁顺序只能有一个方向：transport（`g_net.mutex`）→ frame（连接槽 `write_mutex`）**。
  IPC / tick 线程按这个方向拿；**连接线程只拿 frame，因此它在帧锁里绝不能调用 `netLog()` 之类
  会去拿 transport 锁的东西**——那会与该方向反向，和 tick 线程互等，整个 sysmodule 就这么挂住
  （现象：NRO 卡死、`dglab-sys.log` 被占用打不开、没有崩溃报告）。需要记日志时把数据放进连接槽，
  由 tick 线程下一次持锁时打印；
- 停止路径必须检查 `threadWaitForExit()`/`threadClose()` 的 `Result` 并记日志：线程没被
  回收时，泄漏的是它那 18–22KB 的栈，而日志里只会显示"若干次之后才失败"；
- 每个连接自己的缓冲区（`netClientThreadMain` 的接收缓冲、`wsConnRecv` 的重组缓冲）
  必须留在那个线程的栈上，不能共享——两个客户端同时在线时共享会互相踩；
- 栈帧由 `make -C tests/stack` 自动检查：它用 devkitA64 的 gcc 以 `-fstack-usage` 编译
  sysmodule 的全部源码，除 `netClientThreadMain`、`wsConnRecv` 与 ble_poc 里那三个
  scan 结果结构外，**任何函数栈帧 ≥ 1KB 就失败**（名单与理由写在
  `tests/stack/Makefile` 的 `ALLOW` 里）。新增的大缓冲区要先想想是不是该放 `.bss`，
  确实要留的再加进名单并写明理由；
- `tests/stack` 用 `-std=gnu11` 编译：sysmodule 自己的构建不传 `-std`，而 `-std=c11` 会定义
  `__STRICT_ANSI__`，让 newlib 藏掉 libnx `<sys/socket.h>` 里的 BSD 声明（`MSG_DONTWAIT`
  就是其中之一）——两边的语言模式必须一致，否则检查的是另一份代码；
- 手工量单个文件时（需要看完整列表、或想比较不同编译选项）：

      aarch64-none-elf-gcc -std=gnu11 -O2 -fstack-usage -D__SWITCH__ \
          -I$(DEVKITPRO)/libnx/include -Isysmodule/include -Icommon/include \
          -c sysmodule/source/net/net_server.c -o /tmp/net_server.o

  然后看 `/tmp/net_server.su`：核心代码里不该再出现接近 2KB 的栈帧。
  主线程栈从 16KB 提到 32KB 之后这一类事故的余量更大，但规则不变——栈不是无限的地方。

## Sysmodule Title ID

本项目的 Sysmodule 必须使用项目专属的 64-bit Title ID，指定为：

    00FF072107210721

禁止：

- 随机生成 Title ID；
- 使用 Nintendo 系统 Title ID；
- 使用其他 Homebrew Sysmodule 已使用的 Title ID；
- 根据其他项目的 Title ID 直接修改一个数字后使用；
- 在没有检查现有项目配置的情况下自行决定 Title ID。

推荐在合适的配置文件中定义该 Title ID，所有构建脚本、toolbox.json
和安装目录都必须从这个唯一配置来源获取 Title ID。

禁止在以下位置重复硬编码 Title ID：

- Makefile；
- toolbox.json；
- shell script；
- README；
- release script。

## Sysmodule 构建与发布

Sysmodule 的构建必须区分：

    make
        ↓
    构建 Sysmodule

    make package
        ↓
    构建 + 生成 Atmosphère 安装目录

最终安装结构必须为：

    <TITLE_ID>/
        ├── exefs.nsp
        ├── toolbox.json
        └── flags/
            └── boot2.flag

该目录位于仓库根目录的 `release/` 下（见根 `AGENTS.md` 的“发布产物布局”），
Title ID 目录名由 `DGLAB-NX-Core.json` 推导。

其中：

- `exefs.nsp` 是构建产生的 Sysmodule NSP；
- `toolbox.json` 用于 Sysmodule Toolbox/Overlay；
- `flags/boot2.flag` 用于 Atmosphère 启动阶段加载 Sysmodule。

### Sysmodule 模块名称

指定为

    DGLAB-NX-Core

`Makefile` 中的 `TARGET` 与 `sysmodule/DGLAB-NX-Core.json` 中的 `name` 都是此名称；
`toolbox.json` 由 `Makefile` 从 `TARGET` 生成，禁止另写一份。

NPDM 配置文件名必须与 `TARGET` 一致：libnx 模板只自动匹配 `<TARGET>.json` 或
`config.json`。改名时漏掉文件不会直接报错，而是静默退化成生成普通的 homebrew
`.nro`（只在 `make package` 最后报 `.nsp` 不存在），所以两者必须一起改。

### 构建验证

`make package` 必须验证：

1. Title ID 与项目记录一致；
2. `exefs.nsp` 存在；
3. `toolbox.json` 存在；
4. `toolbox.json` 中的 `name` 与模块名称一致；
5. `toolbox.json` 中的 `tid` 与目录 Title ID 一致；
6. `flags/boot2.flag` 存在；
7. 输出目录结构正确。

如果任何检查失败，构建必须失败，而不是生成一个可能无法安装的目录。

### 改完必须重装（血泪教训）

Sysmodule 是 **boot2 常驻**进程：它不从 SD 卡上的 NRO 启动，而是开机时由 Atmosphère
从 `atmosphere/contents/<TITLE_ID>/` 加载。因此

    make -C sysmodule package  →  覆盖 SD 卡上的 <TITLE_ID>/  →  重启主机

这三步少任何一步，跑的都还是旧二进制，而**旧二进制的行为与代码 bug 无法区分**：
2026-09-16 那次"按 ZL/ZR 就卡死、退出重进提示 sysmodule 未运行"，最后查明是"SD 卡上装的
是旧版本"（旧版本的 16KB 主线程栈会被 `NET_WAVEFORM` 那条链压爆，进程直接消失）。最容易
犯的是只更新 NRO：NRO 一拷就生效，sysmodule 还是老的。

为了下次一眼能看出来，sysmodule 会在启动与每次开服时把构建标识写进日志：

- 标识来自 Makefile 注入的 `git describe --always --dirty`
  （`sysmodule/include/dglab/build.h`，拿不到 git 时是 `unknown`）；
- `dglab-sys.log` 的第一行形如 `server start, dglab 645f698-dirty`；
- 标识是通过 `-D` 注入的，make 看不到命令行变化，所以 `sysmodule/Makefile` 把标识写进
  `build/build-stamp` 并让全部对象依赖它：换提交、变 dirty（NRO 侧还有 `VERSION`）时会自动
  重编，不会出现 main.o 与 net_socket.o 各报一个标识的旧二进制（2026-09-18 踩过）；
- **日志里没有这一行 = 装的是 2026-09-16 之前的版本**。

实机验证之前先确认这一行与当前 checkout 对得上；对不上就先重装再验，否则验证的是旧代码。
完整事件记录见 `docs/dglab-socket.md` 的"栈上不要放 KB 级缓冲区"。
