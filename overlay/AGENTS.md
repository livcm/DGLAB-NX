# Overlay AGENTS.md

## 职责

`overlay/` 用于 Tesla / Ultrahand 等 Overlay 环境。

负责：

- 快速查看 DG-LAB 连接状态；
- 快速连接 DG-LAB 设备；
- 快速发送测试 Effect；
- 修改常用参数；
- 开关功能；
- 调试信息；
- 运行时控制。

## 边界

- Overlay 不负责长期持有 Bluetooth 连接。
- Overlay 不实现底层 DG-LAB Bluetooth Protocol。
- Overlay 必须通过 Sysmodule IPC 使用 DG-LAB 能力，不得直接建立 BLE 连接。

## 与 NRO 的关系

Overlay 与 NRO 可以共享业务逻辑和 IPC 定义，但不要假设两者的 UI、
渲染环境和生命周期完全相同。

## 构建

修改 Overlay 相关源代码或配置后，应使用该目录定义的构建方式重新构建，
并在最终报告中说明实际执行过的验证。

输出文件名应为：

    DGLAB-NX-Ovl.ovl
