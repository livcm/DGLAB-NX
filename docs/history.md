# 历史结论索引

实验流水账和旧方案由 Git 历史保留。本页保留代码仍引用的历史节点；
现行事实、证据与限制以对应专题为准，不再追加原文。

## 28. BLE 两段会话的边界（2026-09-25）

btm 会话重复 InitializeBle/EnableBle/RegisterGattClient 的四轮失败，对照不碰的两轮成功。
因此驱动级开栈与 btm 连接必须分两段，btm 段不再操作 btdrv 全局栈或注册。
仅打开服务并在测量后只读事件队列，与重复开栈/注册不是同一操作。
现行边界、0x668F 和连接所有权依据见 [BLE 固件研究](ble-re.md)。
源码中的 `docs/history.md §28` 继续指向此结论。

## 其它节点

| 历史事项 | 现行结论与依据 |
| --- | --- |
| btdev/btm:u 扫描成功但无事件、ARUID 探测 | [BLE 固件研究](ble-re.md#btmu-与-aruid) |
| vtable 当命令表，注册/ABI 推断被撤回 | [更正](ble-re.md#判定2026-09-21-夜更正) |
| 驱动级事件形状、扫描与广播观察 | [扫描与 GATT](ble-re.md#扫描与-gatt) |
| 回包/配对/拨轮探测收束 | [连接所有权](ble-re.md#连接所有权在服务层是封的) |
| 协议、绑定、写入与灯闪实机里程碑 | [BLE 验证](ble-poc.md#当前状态) |
| 栈溢出、socket 卡住与启停泄漏 | [Socket 平台依据](dglab-socket.md#平台实现与故障依据) |
| UI、字体、主题和 Joy-Con 多轮修正 | [UI](nro-ui.md)、[Joy-Con](joycon-input.md) |
| 文档审计、发布流程与版本分层 | [文档约定](docs-audit.md)、[发布](development.md#发布) |

历史提及的“BLE 未实现/搁置”“无补丁即可连”“返回就停会话”不再代表当前行为。
不能用已撤回的参数形状或旧探针按键进行新实验。
