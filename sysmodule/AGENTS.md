# Sysmodule 工作规则

## 职责与边界

管理 DG-LAB BLE/WebSocket、协议、设备生命周期和稳定 IPC；遵循 [根规则](../AGENTS.md)。
平台无关协议层不能依赖 libnx/HOS。当前状态见 [开发文档](../docs/development.md)。

## BLE 约束

实现或研究前读 [固件研究](../docs/ble-re.md)、[操作验证](../docs/ble-poc.md) 和
[Coyote 协议](../docs/dglab-protocol.md)。

- 一个开机周期只走一条 BLE 路径；驱动级探针打开栈后，btm 会话不得再
  `InitializeBle`、`EnableBle` 或 `RegisterGattClient`。不要冒用 applet ARUID，
  不要给 btm 留无法完成的连接；完成实验后重启。
- BLE 依赖对应固件构建的 exefs 补丁，按开环设计：状态只表达请求值，UI 不显示真实强度/电量，
  不做基于设备当前值的相对加减。
- 通道强度与通道强度上限是两组独立的 A/B 值，禁止混字段；上限为 0 的通道无幅度输出。
  有效波形是输出的前提，不能只发强度或 BF 来判断写入是否生效。
- HOS 允许时必须与无线控制器、蓝牙音频共存。不能仅凭连接成功宣称兼容：实机验证
  Joy-Con L/R 保持连接、现有控制器工作、新控制器配对、蓝牙音频、NRO 退出后的稳定性、
  睡眠/唤醒及 DG-LAB 断连/重连不干扰控制器重连；区分 HOS 自身限制。

## 官方协议参考与实现

蓝牙主要依据：[官方仓库](https://github.com/dungeonlab-open/dglab-bluetooth-protocol)。
Socket 来源见 [Socket 文档](../docs/dglab-socket.md)。

1. 实现、修改或调试前：定位官方仓库 → 确认型号与协议版本 → 读根 README 及对应设备/版本文档
   → 提取细节 → 查现有实现 → 编码。Coyote 优先读 `coyote/README.md` 和对应 `v2/v3/README.md`。
2. 不得跳过来源、型号、版本和文档核对；不凭记忆、第三方实现或其它相似设备协议编码。
3. 至少核对服务/特性 UUID、properties、设备名、MTU/包长、header/payload、字节序、
   序列号、A/B 强度与波形格式、回包、重连参数及版本差异。不得把 V2 格式套到 V3。
4. 来源优先级：DG-LAB 官方 → 本项目已验证实现/测试 → libnx/Switch 接口文档 →
   第三方实现 → Agent 知识。冲突先标记、查对应版本，必要时实机确认，再记录结论。
5. 无法联网时明确说明，使用本地缓存或协议文档；资料不足就指出缺少依据，不假装已核对。
6. 显式区分协议版本，即使只支持一个也要在代码和文档注明。官方仓库是参考，不整仓复制；
   保持 Protocol → Transport → Sysmodule → IPC 分层。
7. 移植后在协议文档记录设备/版本、官方来源、Switch 实现映射、已验证/未验证指令和限制。

## 线程与栈

- 线程栈放页对齐的静态 `.bss`，`threadCreate()` 显式传栈地址与大小，不从固定堆分配。
- 共用路径的 KB 级缓冲放 `.bss`，注明调用方持有 transport 锁；连接自己的接收/重组缓冲
  必须独立，不能让同时在线的客户端共享。
- `threadWaitForExit()` / `threadClose()` 必须用 live `Thread`，不能用 join 前副本；
  两个 Result 均检查并记日志。否则线程链表状态导致关闭失败，泄漏栈映射与句柄。
- socket 帧头和 payload 放同一帧锁；`send()` 禁用 `MSG_DONTWAIT`，超时用 `SO_SNDTIMEO`，
  部分写/错误立即 `shutdown()`。IPC 线程不做可能阻塞的写，波形上传只入队，由 tick 发送。
- 锁顺序固定为 transport → frame。连接线程在帧锁内不能调用会拿 transport 锁的日志函数；
  失败先记到连接槽，由 tick 持锁输出。
- 开机路径最小，文件访问前显式挂载 SD，不能在 boot2 早期写路径；睡眠与诊断约束见
  [Socket 文档](../docs/dglab-socket.md#睡眠与唤醒)。PSC 注册只允许既有 WlanSockets ID，
  不试自定义 ID；没有可靠新机制前不再做 PSC 实验。
- 大缓冲/线程改动跑 `make -C tests/stack`；例外需在测试 `ALLOW` 中写明理由。
  栈检查语言模式必须与构建一致（gnu11），不能改成会隐藏 newlib BSD 声明的严格 c11。

## Sysmodule Title ID

必须沿用 `DGLAB-NX-Core.json` 的项目专属 64-bit Title ID，所有构建脚本、toolbox 和
安装目录从它推导，不在 Makefile、脚本、README、发布脚本或 toolbox 里另写一份。
禁止随机生成、使用 Nintendo/其它 Homebrew ID、照其它 ID 改一位，或不查配置自行决定。

## Sysmodule 构建与发布

- 模块名 `DGLAB-NX-Core`；`TARGET` 与 NPDM 的 `name`/配置文件名一起保持一致。
  libnx 只自动匹配 `<TARGET>.json` 或 `config.json`，漏改会退化为普通 NRO。
- `make` 编译；`make package` 编译并生成根 `build/<TITLE_ID>/` 的安装目录。
  布局及发行版本规则见 [根规则](../AGENTS.md#81-发布产物布局)。
- `toolbox.json` 的 name 从 TARGET、tid 从 NPDM、version 从根 VERSION 推导，
  不与 `GET_VERSION` 的 IPC 接口版本混用。
- package 必须检查 Title ID、输出目录、`exefs.nsp`、`toolbox.json`、`flags/boot2.flag`
  及 toolbox 的 name/tid/version 一致；任何失败必须让构建失败。
- 改完必须重新 package、安装、重启。实机验证先比对启动/开服日志的构建标识与 checkout；
  不匹配就重装。构建标识变化必须触发相关对象重编，避免混入旧对象。
