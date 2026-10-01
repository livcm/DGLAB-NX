# Overlay 工作规则

面向 Tesla/Ultrahand：通过 sysmodule IPC 查看连接、连接设备、发送测试 Effect、
调整常用参数、开关功能及显示调试信息。不自行连接设备或实现底层协议，遵循 [根规则](../AGENTS.md)。

可与 NRO 共享业务逻辑和 IPC，但不能假设 UI、渲染环境或生命周期相同。
修改源码或配置后按本目录构建方式验证并报告实际结果；输出名为 `DGLAB-NX-Ovl.ovl`。
