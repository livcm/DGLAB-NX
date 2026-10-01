# Common 工作规则

`common/` 只放真正跨组件共享的 IPC 命令/数据结构、Effect、协议无关类型、错误码与版本。
不放组件业务逻辑，不实现 BLE Transport 或 DG-LAB Protocol。

连接所有权、IPC 变更流程和通用开发要求遵循 [根规则](../AGENTS.md)。
修改 IPC 时必须更新公共类型、受影响客户端、文档，必要时增加版本，并构建验证所有受影响组件。
