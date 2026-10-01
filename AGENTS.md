# Agent 工作规则

## 项目与组件

DGLAB-NX 是 Atmosphère 环境下的 Nintendo Switch Homebrew。Sysmodule 常驻管理
DG-LAB 连接与协议；NRO、Overlay、Game Mod 通过 IPC 使用它。

| 目录 | 职责 | 局部规则 |
| --- | --- | --- |
| `sysmodule/` | BLE/WebSocket、协议、设备生命周期、IPC 服务 | [AGENTS](sysmodule/AGENTS.md) |
| `nro/` | 用户界面、Joy-Con/触屏玩法、调试 | [AGENTS](nro/AGENTS.md) |
| `overlay/` | 游戏期间的快速查看与控制 | [AGENTS](overlay/AGENTS.md) |
| `mods/` | 游戏事件检测与 IPC 映射 | [AGENTS](mods/AGENTS.md) |
| `common/` | 公共 IPC 与类型 | [AGENTS](common/AGENTS.md) |

当前状态、构建、发布和测试入口见 [开发文档](docs/development.md)。

## 3. 核心架构原则

### 单一 DG-LAB 连接所有者

设备侧 BLE/WebSocket 连接只能由 sysmodule 建立并持有；其它组件一律通过 IPC。
WebSocket 模式下 Switch 是服务端、App 扫码连入，设备 BLE 由手机持有；BLE 模式下
sysmodule 直连设备。保持协议层与 UI、Game Mod 与核心服务解耦，不为新增客户端重复实现传输。

### IPC 是内部公共 API

修改 IPC 必须同步公共类型、所有受影响客户端和 [IPC 文档](docs/ipc.md)，必要时增加
接口版本，并构建、验证所有受影响组件。已发布命令号不得重排，新命令用新号；避免破坏兼容。

## 开发流程

1. 修改前阅读相关目录的 AGENTS 与实现，确认组件归属、已有 API 和最小修改范围。
2. 优先修改现有代码；发现架构冲突先说明，不能为局部功能破坏连接所有权或 IPC 边界。
3. 修改后构建受影响组件、运行已有相关测试、检查 diff，并报告实际执行的验证。
   无自动化测试时至少编译、静态检查，实机启动由用户验证。
4. 源码、headers、IPC、Makefile、linker 或组件配置变化都必须重新构建相关组件。
5. 改 sysmodule 后，实机验证前必须 `make -C sysmodule package`、覆盖 SD 卡上的
   `atmosphere/contents/<TITLE_ID>/`、重启；仅更新 NRO 不会更新常驻服务。先核对日志构建标识。
6. 写完 SD 卡必须 `diskutil eject /dev/diskN` 弹出整块磁盘并确认设备已消失再拔卡。
   `unmount` 和 `unmountDisk` 不等于弹出。

## API、协议与版本

- 使用 devkitPro、devkitA64、libnx；HID 等系统访问优先用 libnx。
- 禁止猜 libnx API、HOS 服务名/命令、HID/Bluetooth/Sysmodule 接口或内部结构。
  不确定时依次查已安装 headers、libnx 源码、官方 switch-examples，必要时查相关项目源码。
- 涉及 DG-LAB 官方协议时先读 [sysmodule 规则](sysmodule/AGENTS.md)，按设备和版本核对
  官方资料。蓝牙协议是 Coyote V2/V3，Socket 协议是 V3/V4，二者版本不对应。
- BLE 实现或研究前先读 [固件研究](docs/ble-re.md) 的当前状态、连接所有权和诊断补丁，
  遵守一个开机周期一条 BLE 路径、btm 会话不碰 btdrv、不冒用 ARUID、不留悬置连接的约束。
- 版本敏感代码必须写明 HOS/游戏版本、Title ID、Build ID、Hook 和内存结构的适用范围，
  不隐藏兼容限制。
- 新依赖先确认现有能力不足、Switch/devkitA64 支持、许可证和构建方式；优先复用
  devkitPro/libnx，避免为小功能引入大型库。

## 8.1 发布产物布局

根 `make` 构建全部组件，组件 `make -C <component> package` 只组装自己的部分；
产物统一在根 `build/`，不提交。`make clean` 清除产物。新增组件必须提供基本构建说明。

```text
build/
├── <TITLE_ID>/{exefs.nsp,toolbox.json,flags/boot2.flag}
├── DGLAB-NX/{DGLAB-NX.nro,lang/*.json}
└── DGLAB-NX-Ovl.ovl
```

- Title ID 唯一来源是 `sysmodule/DGLAB-NX-Core.json`；Makefile、脚本和文档不得另写一份。
- `lang/` 源头在仓库根目录，随 NRO 发布、一起安装；不能只复制 `.nro`。
- 发行版本唯一来源是根 `VERSION`。正式 tag 必须是相应的 `v<VERSION>`，且为 annotated；
  不一致时 CI 必须在编译前失败，发布脚本从 `build/` 推导 Title ID。
- 手工 tag 与 `tag-release.yml` 遵循同一规则；`dev.yml` 的滚动 pre-release 不决定发行版本，
  固定 `dev` tag 必须是 lightweight，避免污染 `git describe --always --dirty`。
  具体入口见 [发布](docs/development.md#发布)。

## 文档与知识沉淀

- [README](README.md) 面向用户；`docs/*.md` 放技术依据、设计原因、实机结论与限制；
  AGENTS 只放持久的开发规则。归属与删改标准见 [文档约定](docs/docs-audit.md)。
- 不重复抄录代码和测试能说明的细节；临时过程交给 Git 历史。构建与开发流程放
  `docs/development.md`，组件约束放对应 AGENTS。
- 不因发现新知识自动改 AGENTS。只有信息持续影响多个任务、属于规则/架构/流程、
  有可靠验证且不记录容易重犯时，才做最小更新。普通 API、UUID、payload、Hook 地址和
  第三方库用法放技术文档；发现规则错误先核实再修正。

## Git 与隐私

Commit 小而明确，一次尽量一类改动，可用 `feat:`、`fix:`、`refactor:`、`docs:`。
禁止提交构建产物、密钥、配对密钥、私人配置/设备信息、个人机器路径或临时调试文件；
日志中的敏感信息也不得提交。测试配置用本地文件、环境变量或示例文件提供。
