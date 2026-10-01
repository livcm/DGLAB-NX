# 开发与构建

用户安装与操作见 [README](../README.md)，开发约束见 [根 AGENTS](../AGENTS.md)。

## 架构与状态

```text
NRO / Overlay / Game Mod → IPC → sysmodule
                                  ├─ WebSocket 服务端 → 手机 App → BLE → 设备
                                  └─ BLE → 设备（需 exefs 补丁，开环）
```

设备侧连接由 sysmodule 独占；IPC 是内部公共 API。协议与网络核心不依赖 libnx/HOS，
可在电脑测试；平台传输、UI 和事件源各自解耦。

| 组件 | 当前状态 |
| --- | --- |
| sysmodule | WebSocket/Socket V3、Coyote V3 协议层、IPC 可用；BLE 入口已跑通，依赖补丁且无回读 |
| NRO | 菜单、连接测试、BLE 直连、体感、触屏、参数、关于及诊断控制台可用 |
| Overlay / Game Mod | 未实现，计划一起开发，用 Overlay 管理游戏期间的服务与 Mod |

目录职责见根 AGENTS。`common/` 放共享接口，`lang/` 是运行期文案，`tests/` 是主机测试，
`config/` 放项目配置，`.github/workflows/` 管理 CI/发布；`build/` 为不提交的产物。

## 构建与安装验证

组件需要 devkitPro（devkitA64 + libnx）：

```sh
export DEVKITPRO=/opt/devkitpro
make                        # 全部组件并组装 build/
make clean                  # 清除产物
make -C sysmodule package   # 只组装后台安装目录
make -C nro package         # 只组装前端与语言包
```

当前产物：`build/<TITLE_ID>/` 与 `build/DGLAB-NX/`；Overlay 实现后才生成
`build/DGLAB-NX-Ovl.ovl`。
Title ID 从 `sysmodule/DGLAB-NX-Core.json` 推导，不复制常量。
NRO 的 `lang/` 必须与 `.nro` 一起安装。改 sysmodule 后覆盖其 SD 安装目录、弹出整盘、
重启，并核对日志构建标识，再做实机测试。

| 标识 | 唯一来源 | 用途 |
| --- | --- | --- |
| 发行版本 | 根 `VERSION` | NACP、About、toolbox.json |
| IPC 接口版本 | `common/include/dglab/ipc.h` | GET_VERSION；与发行版本无关 |
| 构建标识 | `git describe --always --dirty` | About 与服务日志，用于确认安装版本 |

## 发布

正式 tag 必须为 `v<VERSION>` 且是 annotated；CI 在编译前检查一致性。
发布脚本从 `build/` 推导 Title ID，zip 根目录对应 SD 根布局：
`atmosphere/contents/<TITLE_ID>/` 与 `switch/DGLAB-NX/`。

| 入口 | 行为 |
| --- | --- |
| 手工 tag | 改 VERSION 并提交，`git tag -a v<版本> -m "DGLAB-NX <版本>"`，推送 tag，触发 release.yml |
| tag-release.yml | 在分支上手动输入版本；更新 VERSION、提交、打 annotated tag、推送并调度 release.yml；已有 tag 则重新发布该 tag |
| dev.yml | push main 或手动触发：复用 CI 构建测试，更新滚动 pre-release，资产 `DGLAB-NX-dev-sd.zip`；不决定正式版本 |

`dev` 是 lightweight tag，因此移动它不会改变构建标识的 annotated tag 选择。
PR/main 的 CI 做构建与测试；main 还触发 dev 包。
`release.yml` 手动运行在分支上是 dry run（只上传 artifact），在 tag 上可发布。
实现及权限要求以 [.github/workflows](../.github/workflows/) 为准。

## 测试与实机边界

```sh
make -C tests/protocol
make -C tests/ipc
make -C tests/net
make -C tests/qr
make -C tests/canvas
make -C tests/lang
make -C tests/motion
make -C tests/touch
make -C tests/stack
```

主机测试覆盖范围以各测试目录为准，不在文档重复用例与数量。
`tests/ipc` 需要 libnx headers；`tests/stack` 需要 devkitA64，其余用系统编译器。
UI 图片预览命令见 `tests/canvas/tools/render_preview.c` 文件头。

已实机确认：Socket V3 控制输出、Joy-Con 左右驱动对应通道、触屏可运行、两种 NRO
启动方式的自动休眠抑制；BLE 连接/GATT/写入及默认波形灯闪。
BLE 跨页玩法、可感强度档位、蓝牙共存/睡眠兼容及补丁发布仍待完成。
具体环境、证据和未验证项见各专题，不能用主机测试代替实机结论。

## 后续方向

- BLE：验证跨页玩法与可感输出，补丁纳入发布；保持开环，不继续在现有服务层尝试回读。
- Overlay 与 Game Mod：共同开发，支持游戏与 NRO 无法同时运行的场景。
- Socket V4：因 V4 App 向下兼容 V3 暂搁置；待官方 beta 稳定与 App 版本确认后，
  做外壳、绑定、二维码、主机回环及实机验证。
- deko3d 呈现层：收益较低暂搁置；已核对限制与备选路线见 UI 文档。
- 文档、测试和错误处理随改动同步。

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [IPC](ipc.md) | 公共 API 的语义、兼容边界与调用入口 |
| [Socket](dglab-socket.md) | Socket V3、App 交互、平台限制与排查 |
| [Coyote](dglab-protocol.md) | 蓝牙协议来源、实现选择与验证边界 |
| [BLE 验证](ble-poc.md) | 直连与探针操作、实机证据、验收 |
| [BLE 固件研究](ble-re.md) | HOS 22.5.0 的服务限制、补丁依据与风险 |
| [NRO UI](nro-ui.md) | 布局/字体/主题依据及后端取舍 |
| [Joy-Con](joycon-input.md) | 输入设计、实机观察与调参边界 |
| [触屏](touch-input.md) | 触点设计、参数复用与实机验收 |
| [文档约定](docs-audit.md) | 分层、事实保留与检查规则 |
| [历史结论索引](history.md) | 代码仍引用的历史节点及现行结论入口 |
| [BLE 工具](../tools/ble-re/README.md) | 固件读取与复核流程 |
| [上游候选](../tools/ble-re/upstream.md) | 可回馈 libnx/switchbrew 的证据及断言边界 |
