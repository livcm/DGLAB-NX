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

- Overlay 不实现底层 DG-LAB Bluetooth Protocol。
- Overlay 不得自己建立或持有 DG-LAB 设备侧连接（BLE/WebSocket），只能通过 Sysmodule IPC
  使用 DG-LAB 能力（见根 `AGENTS.md` 的"单一 DG-LAB 连接所有者"）。

## 与 NRO 的关系

Overlay 与 NRO 可以共享业务逻辑和 IPC 定义，但不要假设两者的 UI、
渲染环境和生命周期完全相同。

## 构建

修改 Overlay 相关源代码或配置后，应使用该目录定义的构建方式重新构建，
并在最终报告中说明实际执行过的验证。

输出文件名应为：

    DGLAB-NX-Ovl.ovl
