# NRO AGENTS.md

## 职责

`nro/` 是普通的 Switch Homebrew 应用程序。

负责：

- 用户界面；
- 接收 Joy-Con 按键和传感器输入；
- 体感玩法；
- DG-LAB 参数配置；
- 调用 Sysmodule IPC；
- 设备状态展示；
- 测试和调试功能。

## 边界

- NRO 不负责实现底层 DG-LAB Bluetooth Protocol。
- NRO 不得自己建立 DG-LAB BLE 连接，所有 DG-LAB 通信必须通过 Sysmodule IPC。
- NRO 的 UI、玩法和业务逻辑应与 BLE/协议层解耦。

## 交互模型示例

    Joy-Con 输入
        ↓
    NRO 计算玩法逻辑
        ↓
    IPC: SendEffect(...)
        ↓
    Sysmodule
        ↓
    DG-LAB

## Joy-Con 输入

Joy-Con 按键输入和六轴传感器应优先使用 libnx 提供的 HID API，而不是自行实现
HOS HID 协议。libnx 和官方 `switch-examples` 都可以作为 API 和用法的参考。

## UI 原则

应该先用 Console 来呈现字符 UI，待 NRO 的业务逻辑全部实现后再实现 GUI。

各可选 UI 方案（console / framebuffer / deko3d / SDL2 / OpenGL / Borealis / ImGui）的
依赖、工作量与取舍见 `docs/nro-ui.md`；当前结论是先走 libnx framebuffer 自绘
（零额外依赖，且二维码需要严格正方形的模块）。

HOS 并没有提供类似 UIKit / SwiftUI / Android Views 的通用
Nintendo 原生 UI Framework 给普通 Homebrew 使用。

因此：

- 不要寻找不存在的“HOS 原生 Button/List/Window API”；
- NRO UI 可以使用自己的 UI / Renderer；
- 可以使用 SDL、OpenGL、deko3d、Dear ImGui 或自定义 Renderer；
- 如果需要 Nintendo 风格 UI，应通过自己的 UI 组件实现。

UI 风格可以模仿 HOS，但实现应与 HOS 系统 UI 解耦。

建议逐渐形成自己的组件：

    Screen
    Header
    Sidebar
    List
    ListItem
    Slider
    Toggle
    Button
    Modal

## NRO Identity

普通 NRO 不需要像 NSP/NCA 那样分配 Nintendo Title ID，
也不使用 Atmosphère `contents/<titleid>/` 安装结构。

但是 NRO 可以包含 NACP metadata，其中的
PresenceGroupId / SaveDataOwnerId 等字段可能被 loader 或模拟器
用作 ProgramId。

因此：

- 不要将 NRO 的 metadata identity 与 Sysmodule Title ID 混淆；
- 如果项目包含多个 NRO，它们应使用独立且稳定的 metadata identity；
- 不要让多个需要被独立识别的 NRO 无意中共享同一个 ProgramId；
- 如果需要兼容 Ryujinx，应验证多个 NRO 能否同时被正确识别。

## 构建

修改 NRO 源代码、headers、IPC 定义、Makefile 或 linker 配置后，应至少执行：

    make -C nro

并说明实际执行的验证结果。

输出文件名应为：

    DGLAB-NX.nro
