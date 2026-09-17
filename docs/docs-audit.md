# 文档约定与审计清单

本文只保留两类内容：**文档该放哪里**，以及 **2026-09-17 审计的处置结果**。
逐条规则的分类、证据与删除/合并清单已归档到 `docs/history.md`（「docs-audit：
2026-09-17 审计报告原文」）。

## 文档归属

- 根 `AGENTS.md`：跨组件的通用规则与架构约束；
- `<component>/AGENTS.md`：该组件内部的约定与边界；同一条规则只留一句并指回根；
- `docs/*.md`：技术细节、实测数据、协议事实与来历；过程与迭代日志放 `docs/history.md`；
- 数值、名单、接口形状以代码为准，文档写"为什么"与指针。

## 已处理（2026-09-17）

| 项 | 处置 |
| --- | --- |
| 单一连接所有者措辞（SYS-01、COM-01、MOD-03、OVL-01、D-01） | 统一为"BLE/WebSocket 连接只能由 sysmodule 建立并持有，其它组件只能通过 IPC 与 sysmodule 通信"，根 `AGENTS.md` 为唯一归属处 |
| 两种传输模式口径 | 根 `AGENTS.md`、`README.md`、`sysmodule/AGENTS.md`、`docs/dglab-socket.md`、`docs/dglab-protocol.md` 写明 BLE 模式（未实现、已搁置）与 WebSocket 模式（已实现，Switch 是服务端、App 扫码连入） |
| 54 条过时句 | 按当前代码改写：`A` 启停、`X` 清空、`Y` 日志、`B` 返回、`+` 只在 console 页；菜单 5 项与新英文名；`tests/motion` 23 项、`tests/ipc` 51 项；`make -C sysmodule package`；主线程栈 32KB；两栏坐标 80/390 与 470/719；去掉"优先级 N"编号引用 |
| 重复解释与迭代过程 | 移入 `docs/history.md`，主文档只留结论与指针 |

## 待处理

- 压缩尚未覆盖的文档（`joycon-input`、`dglab-socket`、`README`、`ipc`、`dglab-protocol`
  与两份 AGENTS）仍保持原样；
- 实机待确认项（见各文档的"待实机确认"一节）：BLE 直连结论、Joy-Con 单位与采样率、
  App 交互与 V4 消息外壳、底座原生 1080p、按需重绘、服务端运行时休眠。
