# Sysmodule AGENTS.md

## 职责

`sysmodule/` 是项目的核心后端。

负责：

- DG-LAB Bluetooth/BLE 连接；
- DG-LAB Bluetooth Protocol；
- DG-LAB 设备发现、连接、断开；
- 设备状态管理；
- Effect / Wave / Command 等协议层功能；
- 对外提供稳定的 IPC API；
- 管理 DG-LAB 连接生命周期；
- 处理来自多个客户端的请求。

## BLE 所有权

Sysmodule 是 DG-LAB 设备连接的唯一所有者。

除非有明确的架构理由，不允许 NRO、Overlay 或 Game Mod 自己建立
DG-LAB Bluetooth 连接。

## 蓝牙兼容性

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

## DG-LAB 官方协议参考仓库

本项目的 DG-LAB Bluetooth Protocol 实现必须以官方开源协议仓库作为主要协议参考：

https://github.com/dungeonlab-open/dglab-bluetooth-protocol

简称：

    dglab-bluetooth-protocol

### 外部协议实现约束

如果任务涉及 DG-LAB Bluetooth Protocol，在实现、修改或调试 DG-LAB Bluetooth Protocol 相关代码之前，
必须先检查 `dglab-bluetooth-protocol` 中的相关文档和实现。操作步骤必须是：

    1. 定位官方协议仓库
    2. 确认设备型号
    3. 确认协议版本
    4. 阅读对应协议文档
    5. 提取所需协议细节
    6. 检查当前项目已有实现
    7. 再开始编码

不得跳过第 1～4 步直接编写协议代码。

不要仅凭模型已有知识、其他第三方项目或对协议的猜测实现 DG-LAB Protocol。

如果本项目代码与官方协议文档不一致，应首先确认差异原因，而不是直接假设
本项目的实现是正确的。

### 优先参考的内容

对于 DG-LAB 郊狼设备，优先检查：

    coyote/
    ├── README.md
    ├── v2/
    │   └── README.md
    └── v3/
        └── README.md

根据实际设备和协议版本选择对应文档。

官方仓库目前包含：

- Coyote V2 Bluetooth Protocol；
- Coyote V3 Bluetooth Protocol；
- Pulse Waveform 相关说明；
- 其他 DG-LAB 设备的 Bluetooth Protocol。

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

例如，如果实现 Coyote V3，不得直接套用 V2 的数据格式。
V3 文档明确说明其数据处理方式与 V2 存在差异，应以 V3 文档为准。

### Source of Truth

协议相关信息的优先级：

1. DG-LAB 官方 `dglab-bluetooth-protocol` 仓库；
2. 本项目已经验证过的实现和测试结果；
3. libnx / Switch 官方相关接口文档；
4. 其他第三方实现；
5. Agent 自身知识。

如果不同来源存在冲突：

- 不要自行选择一个看起来合理的答案；
- 标记冲突；
- 检查官方协议仓库对应版本；
- 必要时通过实机测试确认；
- 在代码或 `docs/dglab-protocol.md` 中记录结论。

### 协议版本

协议版本必须显式记录。

不要创建一个含糊的：

    DGLabProtocol

然后把 V2、V3 的行为混在一起。

推荐：

    DGLabCoyoteV2Protocol
    DglabCoyoteV3Protocol

或者在协议层通过明确的版本结构进行区分。

如果当前只支持一个版本，也必须在代码和文档中明确说明。

### 外部仓库的使用方式

当 Agent 可以访问网络时：

1. 打开官方仓库；
2. 阅读根 README；
3. 定位到当前设备和协议版本的目录；
4. 阅读对应 README；
5. 根据需要检查仓库中的其他相关文件；
6. 再开始实现。

如果网络不可用：

- 不得假装已经阅读官方仓库；
- 应使用项目已有的 `docs/dglab-protocol.md` 或本地缓存资料；
- 如果资料不足，应明确指出缺少官方协议依据。

### 不要复制整个外部仓库

`dglab-bluetooth-protocol` 是协议参考来源，不意味着应该把整个仓库直接复制到本项目。

本项目应根据 Switch / libnx 的架构重新实现协议：

    Official DG-LAB Protocol
             ↓
       Protocol Layer
             ↓
      BLE Transport
             ↓
        Sysmodule
             ↓
           IPC

协议定义与 Switch 平台实现应该保持解耦。

### 协议文档

当本项目完成协议移植后，应在：

    docs/dglab-protocol.md

记录：

- 支持的 DG-LAB 设备；
- 支持的协议版本；
- 官方协议来源；
- Switch 侧 BLE 实现方式；
- 官方协议与 Switch 实现之间的映射；
- 已验证的指令；
- 尚未验证的指令；
- 已知限制。

文档中应注明官方参考仓库：

    https://github.com/dungeonlab-open/dglab-bluetooth-protocol

## 线程与栈

sysmodule 的线程栈都很小：主线程由 NPDM 的 `main_thread_stack_size` 决定
（`sysmodule/DGLAB-NX-Core.json`，**32KB**，最初是 16KB），网络线程在
`sysmodule/source/transport/net_socket.c` 里由 `NET_THREAD_STACK_SIZE` 指定。
这条路径上叠着 libnx 的 IPC/服务调用、newlib 的 `printf` 和 fs 写入，
**不要在这些函数里放大缓冲区**。

已经踩过两次的坑：一条 `char command[1950]`（`DGLAB_SOCKET_MAX_MESSAGE`）或
`char frame[WS_MAX_MESSAGE]` 放在栈上时，加上已有的调用链会把主线程栈压爆。症状是
**整个 sysmodule 直接死掉**（进程消失、IPC 全部无响应），不是返回一个错误——所以从
客户端看不到任何 `Result`，只能靠日志和实机表现判断。实机复现记录见
`docs/dglab-socket.md` 的"栈上不要放 KB 级缓冲区（血泪教训）"。

规则：

- `net_server.c` 这类被 IPC 线程、tick 线程、客户端线程共用的代码里，KB 级缓冲区放
  `.bss`（`static`），并在注释里写明"调用方持有 transport 锁"，因为一份缓冲区就够；
- 每个连接自己的缓冲区（`netClientThreadMain` 的接收缓冲、`wsConnRecv` 的重组缓冲）
  必须留在那个线程的栈上，不能共享——两个客户端同时在线时共享会互相踩；
- 栈帧由 `make -C tests/stack` 自动检查：它用 devkitA64 的 gcc 以 `-fstack-usage` 编译
  sysmodule 的全部源码，除 `netClientThreadMain`、`wsConnRecv` 与 ble_poc 里那三个
  scan 结果结构外，**任何函数栈帧 ≥ 1KB 就失败**（名单与理由写在
  `tests/stack/Makefile` 的 `ALLOW` 里）。新增的大缓冲区要先想想是不是该放 `.bss`，
  确实要留的再加进名单并写明理由；
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
- **日志里没有这一行 = 装的是 2026-09-16 之前的版本**。

实机验证之前先确认这一行与当前 checkout 对得上；对不上就先重装再验，否则验证的是旧代码。
完整事件记录见 `docs/dglab-socket.md` 的"栈上不要放 KB 级缓冲区"。
