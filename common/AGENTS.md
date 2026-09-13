# Common AGENTS.md

## 职责

`common/` 存放组件之间真正需要共享的内容：

- IPC command 定义；
- IPC 数据结构；
- Effect 数据结构；
- 协议无关的公共类型；
- 错误码；
- 版本信息。

## 边界

- 不要把具体组件的业务逻辑塞进 `common/`。
- `common/` 不直接持有 DG-LAB Bluetooth/BLE 连接。
- `common/` 不直接实现 BLE Transport 或 DG-LAB Protocol。

## 修改要求

IPC 定义属于项目内部公共 API。修改 `common/` 中的 IPC 类型、命令或数据结构时，
必须遵循根 `AGENTS.md` 中“IPC 是内部公共 API”的流程：

1. 修改公共类型；
2. 更新所有受影响的客户端；
3. 更新 IPC 文档；
4. 必要时增加协议版本；
5. 构建并验证所有受影响组件。

不要为了一个简单功能随意破坏已有 IPC 接口。

通用开发流程、libnx 规则、依赖管理、Git 和安全要求遵循根 `AGENTS.md`。
