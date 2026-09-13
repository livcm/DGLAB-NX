# AGENTS.md

## 项目简介

本项目是一个 Nintendo Switch Homebrew 项目，目标是在 Nintendo Switch 上提供
DG-LAB Bluetooth Protocol 的实现，并通过统一的后台服务向 DG-LAB 设备发送控制信号。

项目核心不是某一个特定游戏，而是：

    DG-LAB BLE Device
        ↑
    DG-LAB Sysmodule
        ↑ IPC
    ┌───┼───────────────┐
    │   │               │
   NRO Overlay       Game Mods
    │   │               │
 Joy-Con/UI          游戏事件

Sysmodule 负责长期运行的 DG-LAB Bluetooth/BLE 连接及协议实现；
NRO、Overlay 和 Game Mod 负责具体的用户交互或游戏事件，并通过 IPC 使用 Sysmodule
提供的功能。

---

## 1. 项目结构

推荐使用以下结构：

    DGLAB-NX/
    ├── AGENTS.md
    ├── README.md
    ├── Makefile
    │
    ├── common/
    │   ├── ipc/
    │   └── types/
    │
    ├── sysmodule/
    │   ├── source/
    │   ├── include/
    │   └── Makefile
    │
    ├── nro/
    │   ├── source/
    │   ├── include/
    │   └── Makefile
    │
    ├── overlay/
    │   ├── source/
    │   └── Makefile
    │
    ├── mods/
    │   ├── <game-a>/
    │   └── <game-b>/
    │
    ├── docs/
    │   ├── architecture.md
    │   ├── ipc.md
    │   ├── dglab-protocol.md
    │   └── development.md
    │
    └── config/

目录可以随着项目发展调整，但组件职责必须保持清晰。

---

## 2. 组件职责

- `sysmodule/`：DG-LAB BLE 连接、协议实现与设备生命周期；见 `sysmodule/AGENTS.md`
- `nro/`：Switch Homebrew 前端、用户交互与调试；见 `nro/AGENTS.md`
- `overlay/`：Tesla / Ultrahand 环境下的快速查看与控制；见 `overlay/AGENTS.md`
- `mods/`：特定游戏的联动与事件检测；见 `mods/AGENTS.md`
- `common/`：组件间共享的 IPC 定义和公共类型；见 `common/AGENTS.md`

---

## 3. 核心架构原则

### 单一 DG-LAB 连接所有者

DG-LAB Bluetooth/BLE 连接只能由 Sysmodule 管理。

不要出现：

    NRO → BLE
    Overlay → BLE
    Game Mod → BLE

正确结构：

    NRO ──────┐
    Overlay ──┼── IPC → Sysmodule → BLE → DG-LAB
    Game Mod ─┘

这样可以避免：

- 多个客户端争抢 Bluetooth；
- 连接状态不同步；
- 重复实现协议；
- Game Mod 与前端耦合；
- 后续增加新客户端时修改 BLE 层。

---

### IPC 是内部公共 API

Sysmodule 与其他组件之间的 IPC 必须视为正式 API。

修改 IPC 时：

1. 修改公共类型；
2. 更新客户端；
3. 更新文档；
4. 必要时增加协议版本；
5. 构建并验证所有受影响组件。

不要为了一个简单功能随意破坏已有 IPC 接口。

---

## 4. Nintendo Switch / libnx 开发原则

本项目使用：

- devkitPro；
- devkitA64；
- libnx；
- Nintendo Switch Homebrew 环境。

libnx 是本项目主要的 Switch 系统服务/硬件访问接口。

例如 Joy-Con 输入和六轴传感器应优先使用 libnx 提供的 HID API，而不是自行实现
HOS HID 协议。libnx 和官方的 `switch-examples` 都可以作为 API 和用法的参考。

### 禁止猜测 API

尤其禁止凭记忆或猜测编写：

- libnx API；
- HOS service 名称；
- IPC command；
- HID API；
- Bluetooth API；
- Sysmodule API；
- Nintendo 系统接口；
- Switch 内部结构。

如果 API 是否存在、参数、返回值或行为不确定：

1. 搜索当前安装的 libnx headers；
2. 搜索 libnx 源码；
3. 搜索 switch-examples；
4. 必要时查看相关项目源码；
5. 确认后再实现。

不要因为“API 名字看起来合理”就直接使用。

---

## 6. DG-LAB 官方协议参考仓库

DG-LAB Bluetooth Protocol 的官方参考仓库为：

    https://github.com/dungeonlab-open/dglab-bluetooth-protocol

协议实现规则、验证要求和版本区分要求见 `sysmodule/AGENTS.md`。
任何涉及 DG-LAB Bluetooth Protocol 的任务，必须先阅读该文件，再开始编码。

## 7. 开发流程

在修改代码之前：

1. 阅读相关目录和现有实现；
2. 确定修改属于哪个组件；
3. 检查已有 API 和代码模式；
4. 检查相关 headers / examples；
5. 明确修改范围；
6. 采用最小必要修改。

完成修改后：

1. 构建受影响的组件；
2. 运行已有测试；
3. 必要时进行 Switch 实机验证；
4. 检查 Git diff；
5. 确认没有无关修改；
6. 在最终报告中说明实际执行过的验证。

不要声称“已测试”而实际上没有运行测试。

---

## 8. Build / Test

每个组件都应尽量提供明确的构建方式。

例如：

    make

或：

    make -C sysmodule
    make -C nro

如果新增组件，应同时提供最基本的构建说明。

修改以下内容后必须重新构建相关组件：

- C/C++ 源代码；
- headers；
- IPC 定义；
- Makefile；
- linker 配置；
- sysmodule 配置；
- NRO 配置。

## 8.1 发布产物布局

所有编译产物统一生成到仓库根目录的 `release/`，用根目录的 `make` 一次构建：

    release/
    ├── <TITLE_ID>/        Atmosphère sysmodule 目录
    │   ├── exefs.nsp
    │   ├── toolbox.json
    │   └── flags/boot2.flag
    ├── DGLAB-NX.nro       前端 NRO
    └── DGLAB-NX-ovl.ovl   Overlay

规则：

- 根目录 `make` 构建全部组件并生成上述布局；`make clean` 清除这些产物；
- 各组件用 `make -C <component> package` 只生成自己那一部分；
- `<TITLE_ID>` 目录名必须由 `sysmodule/DGLAB-NX.json` 推导，禁止在 Makefile、
  脚本或文档里另写一份；
- `release/` 属于构建产物，不提交到 Git。

如果当前没有自动化测试，应至少进行：

- 编译检查；
- 静态检查；
- 实机启动验证（由用户进行测试）。

---

## 9. 版本敏感内容

Switch Homebrew 的以下内容可能高度依赖版本：

- HOS 版本；
- 游戏版本；
- Title ID；
- Build ID；
- Hook 地址；
- 内存结构；
- Sysmodule 行为；
- Bluetooth 行为。

任何版本敏感代码都必须明确记录适用版本。

不要为了让代码“看起来通用”而隐藏版本限制。

---

## 10. 依赖管理

添加第三方依赖之前：

1. 确认现有依赖无法满足需求；
2. 确认依赖支持 Nintendo Switch / devkitA64；
3. 确认许可证；
4. 确认构建方式；
5. 尽量避免为了很小的功能引入大型依赖。

优先复用 devkitPro / libnx 已有能力。

---

## 11. 文档

根 AGENTS.md 只描述项目规则和架构。

详细技术资料放在：

    docs/

推荐：

    docs/architecture.md
    docs/ipc.md
    docs/dglab-protocol.md
    docs/development.md
    docs/game-mods.md

如果某个组件变得复杂，可以在该目录增加自己的：

    <component>/AGENTS.md

更深层的 AGENTS.md 可以补充或覆盖本文件中与该组件有关的规则。

---

## 12. Git

Commit 应该：

- 小而明确；
- 一个 commit 尽量只做一类事情；
- 不提交构建产物；
- 不提交密钥；
- 不提交个人机器路径；
- 不提交临时调试文件。

推荐：

    feat: add DG-LAB IPC service
    feat: add Joy-Con motion input
    fix: handle BLE disconnect
    refactor: separate protocol layer
    docs: document IPC protocol

---

## 13. 安全与隐私

禁止将以下内容提交到 Git：

- API key；
- Bluetooth 配对密钥；
- 私人配置；
- 私人设备信息；
- 个人路径；
- 调试日志中的敏感信息。

测试所需的配置应通过：

- 本地配置文件；
- 环境变量；
- 示例配置文件；

提供。

---

## 14. Agent 工作原则

Agent 应优先修改现有代码，而不是重复实现已有功能。

遵循：

    Inspect
      ↓
    Understand
      ↓
    Plan
      ↓
    Minimal Change
      ↓
    Build
      ↓
    Test
      ↓
    Review Diff

如果遇到不确定的 Nintendo / libnx / Bluetooth API，

不要猜，先查资料、headers、源码和 examples。

如果发现当前架构与任务要求冲突，应先说明冲突，再决定是否修改架构。

不要为了完成一个局部任务而破坏：

- Sysmodule 的单一 BLE 所有权；
- IPC 边界；
- 协议层与 UI 解耦；
- Game Mod 与核心服务解耦。

## 知识沉淀与 AGENTS.md 更新

Agent 在研究外部资料、阅读源码或实际开发过程中，可能发现新的项目知识。

不要因为发现新信息就自动修改 AGENTS.md。

只有当某条信息满足以下条件时，才考虑更新 AGENTS.md：

1. 对后续多个任务都有持续影响；
2. 属于项目开发规则、架构约束、工作流程或重要约定；
3. 不属于普通 API 文档或实现细节；
4. 已通过可靠来源或实际测试验证；
5. 如果不写入 AGENTS.md，未来 Agent 很可能重复犯同样的错误。

例如：

应该写入 AGENTS.md：

- DG-LAB BLE 连接只能由 Sysmodule 持有；
- 不允许猜测 libnx API；
- 修改 IPC 必须同步更新客户端；
- Game Mod 必须记录游戏版本；
- DG-LAB Protocol 必须先参考官方协议仓库；
- 某个目录应该使用特定构建命令。

不应该写入 AGENTS.md：

- 某个 BLE characteristic 的 UUID；
- 某个 DG-LAB 指令的具体 payload；
- 某个游戏某版本的 hook 地址；
- 某个 API 的完整参数说明；
- 一般性的第三方库使用方法。

上述技术知识应分别记录到：

- docs/dglab-protocol.md
- docs/architecture.md
- docs/ipc.md
- docs/game-mods.md
- 或对应组件的文档

### 更新原则

如果 Agent 发现某个现有规则不正确：

1. 先确认事实；
2. 判断这是规则错误还是普通技术知识；
3. 如果属于持久性项目规则，可以修改 AGENTS.md；
4. 修改 AGENTS.md 时保持最小 diff；
5. 不要因为一次偶然情况添加过度具体的规则。

如果只是普通技术知识，应更新对应的 docs，而不是 AGENTS.md。

---

## 15. 当前项目优先级

开发优先级：

1. 建立可编译的 Switch 项目骨架；
2. 建立 Sysmodule；
3. 建立 IPC；
4. 实现可离线测试的 DG-LAB V3 协议层（编解码、强度状态机、会话层）并在电脑上测试；
5. 实现 Wi-Fi + WebSocket 传输：Switch 作为 WebSocket 服务端，手机 DG-LAB App 通过
   局域网接入（BLE 由手机负责，Switch 不直接持有蓝牙连接）：
   1. WebSocket 握手与帧（平台无关 + 主机测试）；
   2. DG-LAB Socket 协议消息（绑定、强度、波形、清空、心跳）；
   3. Sysmodule 网络模块与 IPC 命令；
   4. NRO 界面（连接信息、地址/二维码、测试按钮）；
6. 把 V3 波形/强度数据接到 Socket 协议（复用协议层已有的波形编码）；
7. 实现最小 NRO Client 的完整交互；
8. 实现 Joy-Con / 六轴输入；
9. 实现基础 UI；
10. 实现 Overlay；
11. 实现 Game Mod 示例；
12. 完善文档、测试和错误处理。

### 已搁置：主机侧 BLE 直连

在 HOS 22.5.0 + AMS 1.11.2 上，Switch 后台 Sysmodule 直连 BLE 外设不可用
（btm/bt 只面向任天堂自家设备；btdrv 的 BLE 事件载荷为空、GATT 客户端注册失败）。
完整实测记录见 `docs/ble-poc.md`。

这条线只有在需要时才会重启，且必须先做**只读逆向**：从固件提取 `bluetooth` 进程，
确认失败点属于"结构/调用顺序变化"（改绑定即可，不需要补丁）还是"缺少通用 GATT
central"（需要 exefs patch 或直接放弃）。不要在没有逆向结论前尝试补丁方案。

除非任务明确要求，否则不要在核心功能尚未稳定前过早实现复杂 UI
或大量 Game Mod。
