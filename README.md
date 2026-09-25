# DGLAB-NX

在 Nintendo Switch 上运行的 DG-LAB 设备控制服务。

项目把 DG-LAB 的连接与协议实现收进一个常驻后台服务（sysmodule），前端组件
（NRO / Overlay / Game Mod）只通过 IPC 使用它，不各自实现一遍协议，也不各自去抢设备。

传输分两种模式，当前只有 **WebSocket 模式**可用：

| 模式 | 连接方式 | 状态 |
| --- | --- | --- |
| WebSocket | sysmodule 与手机 DG-LAB App 建立 WebSocket 会话：Switch 当服务端，App 扫码连入（Switch 不主动外连）；手机负责与设备之间的 BLE，并把波形数据转发给设备 | 已实现（Socket V3） |
| BLE | sysmodule 直接连接 DG-LAB 设备（Coyote 协议） | **扫描 + 连接 + GATT 表已实机跑通，但必须安装 exefs 补丁**（见下）：固件把 BLE 客户端在"控制器层"的激活留给了系统自身的配对流程，第三方客户端会被 `result=0x1A` 挡下；补丁跳过这道检查后，sysmodule 能连上设备并读到 `0x180C`/`0x150A`/`0x150B`（外加 `0x180A` 电量、`0xFE59` DFU）。传输层已接上协议层：BF 与 B0 写入稳定 `rc=0`，且 2026-09-25 的反应测试**实机确认有输出——写入确实到达设备**；但**设备侧一条回包都没有**（通知与读应答全哑），B1 未验证——原因已查清：**连接归 btm 所有，服务层不暴露它的事件**，回读对第三方进程不可得（`docs/ble-re.md` 的「连接所有权在服务层是封的」），因此 BLE 模式按开环设计。完整证据链与补丁说明见 `docs/ble-re.md`，实测记录见 `docs/ble-poc.md` |

两种模式下 Switch 都不直接持有蓝牙连接（WebSocket 模式的 BLE 在手机上）。

## 状态

| 组件 | 状态 |
| --- | --- |
| `sysmodule/` DG-LAB 服务（服务名 `dglab`，Title ID `0x00FF072107210721`） | 可用（WebSocket 模式）：Coyote V3 协议层 + WebSocket 服务端 + IPC |
| `nro/` 前端 | 可用：菜单选择玩法——测试屏（地址/二维码/测试键/日志）、体感玩法（Joy-Con 驱动波形）与触屏玩法（左右半区对应 A/B 通道） |
| `overlay/` | 未实现 |
| `mods/` | 未实现 |
| BLE 模式（sysmodule 直连设备） | 未实现；路径已跑通到"连接 + GATT 表 + BF/B0 写入"，**但依赖 exefs 补丁、且通知路径未通**（设备不回 B1，波形无法出）。当前状态与下一步见 `docs/ble-re.md` 的「当前状态（2026-09-25）」；实测记录见 `docs/ble-poc.md` |

进度与顺序见 `AGENTS.md` §15：骨架、Sysmodule、IPC、Coyote V3 协议层、WebSocket 模式
传输、波形接入、NRO 交互、Joy-Con 输入、基础 UI、NRO 元信息与版本管理、浅色模式都已完成；
触屏玩法的输入层、映射层与玩法页也已实现，就差实机确认触屏能不能读、以及调手感
（见 `docs/touch-input.md`）。接着做 deko3d UI 后端 → Overlay → Game Mod 示例 →
V4 Socket 协议，文档/测试/错误处理是持续项。

协议层、IPC 布局、WebSocket/Socket 服务端、二维码与 NRO 绘制都有主机侧测试，见
[测试](#测试)。

## 架构

```
DG-LAB App（手机，负责 BLE）  ──BLE──→  DG-LAB 设备
             ↑  WebSocket（App 是客户端，扫码连入 Switch）
   ┌─────────┴─────────┐
   │  dglab sysmodule  │  WebSocket 服务端 / Socket V3 / Coyote V3 协议层
   └─────────┬─────────┘
             │  IPC（服务名 dglab）
             ├──→ NRO（可用）
             ├──→ Overlay（未实现）
             └──→ Game Mod（未实现）
```

围绕这张图的几条规则：

- **单一连接所有者**：BLE/WebSocket 连接只能由 sysmodule 建立并持有，其它组件一律走 IPC
  （见 `AGENTS.md` §3）；
- **IPC 是内部公共 API**：命令号定义在 `common/include/dglab/ipc.h`，一旦发布不重排，
  新增命令用新号；
- **协议层与平台解耦**：`sysmodule/source/protocol`、`sysmodule/source/net` 不含
  libnx / HOS 依赖，所以能在电脑上直接测。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `sysmodule/` | DG-LAB 设备生命周期、协议实现、WebSocket 服务端、IPC 服务端 |
| `nro/` | Homebrew 前端：界面、二维码、测试按键、日志落盘 |
| `VERSION` | 发行版本，唯一来源（进 NRO 的 NACP 与 About 页，以及 sysmodule 的 `toolbox.json`） |
| `lang/` | 界面文案，一个语言一个 `.json`（构建时复制到 `build/DGLAB-NX/lang/`） |
| `overlay/` | Tesla / Ultrahand overlay（未实现） |
| `mods/` | 特定游戏的联动（未实现） |
| `common/` | 组件间共享的 IPC 定义与公共类型 |
| `tests/` | 主机侧测试（protocol / ipc / net / qr / canvas / lang / motion / stack） |
| `docs/` | 技术文档，见[文档](#文档) |
| `build/` | 构建产物（不提交） |
| `.github/workflows/` | CI：PR / push 的构建检查与 tag 发版，见[发布](#发布) |

## 构建

组件构建需要 devkitPro（devkitA64 + libnx）。主机侧测试用系统编译器就能跑，其中
`tests/ipc` 需要读 `$(DEVKITPRO)/libnx` 的头文件（默认 `/opt/devkitpro`）。

```
export DEVKITPRO=/opt/devkitpro
make                 # 构建全部组件，产物统一落在 build/
make clean           # 清除 build/ 与各组件自己的 build/
```

也可以只构建单个组件：

```
make -C sysmodule package   # build/<TITLE_ID>/{exefs.nsp,toolbox.json,flags/boot2.flag}
make -C nro package         # build/DGLAB-NX/{DGLAB-NX.nro, lang/*.json}
```

sysmodule 的 Title ID 只在 `sysmodule/DGLAB-NX-Core.json` 里写一次，Makefile、安装目录名和
`toolbox.json` 都从它推导，别在别处再抄一份。

前端的发行版本同样只有一个来源：仓库根 `VERSION`（当前 `0.3.0`）。根 `make` 把它传给
`nro/Makefile`，进 NACP 与 About 页；`make -C nro package` 读的是同一个文件。IPC 接口
版本是另一回事（`common/include/dglab/ipc.h`，见 `docs/ipc.md` 的"版本"）。

## 发布

发布由 GitHub Actions 完成，规则只有一条：**tag 必须等于根目录 `VERSION`**
（tag 形如 `v<VERSION>`，当前 `VERSION` 是 `0.3.0`，所以 tag 是 `v0.3.0`）。

```
# 1. 改 VERSION（唯一版本来源，进 NACP 与 About 页）并提交
# 2. 打 annotated tag（About 页的 build stamp 才会显示 tag 而不是短 SHA）
git tag -a v0.3.0 -m "DGLAB-NX 0.3.0"
git push origin v0.3.0
```

推 tag 后 `.github/workflows/release.yml` 会：

1. 调用 `.github/workflows/ci.yml`：在 `devkitpro/devkita64` 容器里跑 `tests/` 下的全部
   主机侧测试，再用根 `make` 构建；tag 与 `VERSION` 不一致会在编译前失败并打印两个版本号；
2. 把 `build/` 组装成 SD 卡布局并校验（`exefs.nsp` / `toolbox.json` / `flags/boot2.flag` /
   NRO / `lang/`，以及 NRO 里确实带着这个 `VERSION`）；Title ID 目录名仍从构建产物推导，
   workflow 里没有另写一份；
3. 打包成 `DGLAB-NX-<VERSION>-sd.zip` 与 `SHA256SUMS`，创建 Release 并附自动生成的
   release notes。

zip 里就是 SD 卡的根布局，解压到 SD 卡根目录即可（覆盖安装同理）：

    atmosphere/contents/<TITLE_ID>/  →  SD:/atmosphere/contents/<TITLE_ID>/
    switch/DGLAB-NX/                 →  SD:/switch/DGLAB-NX/

PR 和 `main` 上的 push 只跑 `ci.yml`（构建 + 全部主机侧测试），不发版。
`workflow_dispatch` 手动跑 `release.yml` 是 dry run：构建、打包、上传 artifact，但不建 Release，
用于验证 workflow 本身。

## 安装

```
build/00FF072107210721/    →  SD:/atmosphere/contents/00FF072107210721/
build/DGLAB-NX/            →  SD:/switch/DGLAB-NX/      （整个目录一起拷）
```

sysmodule 带 `flags/boot2.flag`（`toolbox.json` 里也是 `requires_reboot: true`），
随系统启动加载，复制完先**弹出整块磁盘**（`diskutil eject /dev/diskN`，或 Finder 里"推出"）
再拔卡、然后重启主机。
`toolbox.json` 的 `version` 是这份包的发行版本
（根 `VERSION`），模块管理器类应用会显示它；IPC 接口版本不在这里，它由模块在运行时
通过 `GET_VERSION` 报给 NRO。overlay 还没实现，所以现在没有 `DGLAB-NX-Ovl.ovl`。

NRO 的运行期文件都在这一个目录下，并且固定分层：

    SD:/switch/DGLAB-NX/DGLAB-NX.nro     NRO 本体
    SD:/switch/DGLAB-NX/lang/            界面文案，运行时读，不是编进 NRO 的
    SD:/switch/DGLAB-NX/config/          app.cfg、motion.cfg 等设置（自动创建）
    SD:/switch/DGLAB-NX/logs/            dglab-net.log、dglab-sys.log 等（自动创建）

`lang/` 必须和 NRO 一起拷过去：一个 `.json` 都找不到时 NRO 会在启动时打印路径与原因后
退出。文案格式和 key 清单见 `docs/nro-ui.md`。

需要 Atmosphère。当前实机验证环境是 HOS 22.5.0 + AMS 1.11.2。

## 使用

1. Switch 与手机连同一个局域网；
2. 打开 `DGLAB-NX.nro`，菜单里选 `socket server`，界面显示局域网地址与二维码；
3. 用 DG-LAB App 扫码接入（App 是 WebSocket 客户端）；
4. 用测试按键确认设备有输出。

菜单里的 `motion (Joy-Con)` 是另一个玩法：左右 Joy-Con 分别驱动 A / B 通道，动得越快
波形值越大、脉冲越密；通道强度仍然是上面的"音量"。做法与参数见
`docs/joycon-input.md`。

菜单里的 `touch (screen)` 把屏幕左右均分：左半区是 A 通道、右半区是 B 通道，手指越高
波形值越大、在每个半区里越靠右脉冲越密；抬手后输出按释放曲线淡出。它和体感玩法共用同一
份参数与同一个槽位发生器，做法见 `docs/touch-input.md`。**触屏只在玩法页里读**，
菜单用 `D-pad` 与 `A` 操作；**主机（底座）模式下触摸屏够不到，玩法页会把玩法区留白并
在中间提示取下主机**。

同一菜单里的 `advanced (motion)` 是两种玩法共用的参数页（死区、灵敏度、包络、频率、波形
强度上限，以及「波形密度」开关；死区/量程/权重只对体感玩法有效）。波形密度选「可变」时
各玩法照旧驱动脉冲间隔（体感跟随挥动幅度、触屏跟随手指左右），选「固定」时间隔恒为
「固定密度」那一项、只有波形值还在动。改完自动存到
`sdmc:/switch/DGLAB-NX/config/motion.cfg`。

按了没反应先看界面上的 `last cmd` 行：它显示最近一次按键的结果（`ok` / `no app bound`
/ `socket error`），红色是失败、黄色是"命令发出去了但听不到"（那一路强度还是 0）。

菜单本身：`D-pad` 上下选择、`A` 进入；**`B` 退出 NRO**（`+` 只在 console 页——
BLE PoC 与启动出错提示——有效）。菜单有 7 项：`socket server`、`bluetooth (direct)`、
`motion (Joy-Con)`、`touch (screen)`、`advanced (motion)`、`about`、`BLE PoC console`。
其中 **bluetooth (direct)** 是 BLE 模式的正式入口（起会话、看状态、调软上限；玩法页照旧用，
数据会自动走 sysmodule 的 BLE 会话；见 `docs/ble-poc.md` 与 `docs/ipc.md` 的 `BLE_*`）。
它有两条规矩：需要 exefs 补丁；**一个开机周期只走一条 BLE 路径**，跑完想再连一次（或再跑
探针实验）就先重启。重启用在 BLE 那条路上——BT 栈/btm 被我们动过，混着来会崩整机；
Socket 模式是手机连设备、Switch 只当 WebSocket 服务端，不碰这些，停掉会话后数据就自动回到
Socket 转发，不需要为它重启。

| 按键 | 动作 |
| --- | --- |
| `A` | 启动 / 停止 WebSocket 服务端（底栏提示跟着切换） |
| `Y` | 打开 / 关闭 sysmodule 日志子页 |
| `X` | 清空波形（clear A / clear B） |
| `B` | 返回菜单 |
| `ZL` / `ZR` | 测试通道 A / B（送波形 + 该通道强度） |
| `↑` / `↓` | 调整通道 A 强度（0~100，步进 1，按住连续调整，改完立刻发给 App） |
| `→` / `←` | 调整通道 B 强度（同上） |

两个通道的强度各自独立，都从 0 开始；调整即刻发送给 App，所以没有单独的"发送强度"
按键。服务端页上 `channel A` / `channel B` 两行各显示一个当前值（`A n/100`）。

服务端**不会开机自启**（只有按 `A` 才启动），最后一个客户端离开 55 秒后会自动停，
避免长期占用 socket。

两点必须注意：

- **服务端运行期间不要让主机休眠。** 睡眠通知注册被系统拒绝，持有 socket 跨过睡眠会
  让主机挂死，只能长按电源键。自动休眠在服务端运行期间由 NRO 关掉（applet 的
  `AutoSleepDisabled`），所以"人走开"这条路径是安全的；**手动休眠仍然会卡死**，
  停服之后再睡。原因、边界与还未解决的漏洞见 `docs/dglab-socket.md` 的“睡眠与唤醒”。
- **测试按钮会真的输出电压。** `ZL`/`ZR` 是测试波形 + 强度，请先确认设备和电极连接
  正常，强度从 0 开始一点点加。

排查实机问题时看 `sdmc:/switch/DGLAB-NX/logs/dglab-net.log`：NRO 会把服务端的收发日志
增量写到那里，出问题直接把这个文件发回来最有用。

## 测试

```
make -C tests/protocol   # Coyote V3 编解码、强度状态机、会话层
make -C tests/ipc        # 手写 CMIF 服务端与 libnx 客户端的发法是否对得上
make -C tests/net        # WebSocket 帧、Socket 协议、服务端会话、真实回环端到端
make -C tests/qr         # 二维码与 CoreImage 参考矩阵逐模块比对
make -C tests/canvas     # canvas 裁剪、字体位序、整屏排版（720p 与 1080p 各一遍）
make -C tests/lang       # JSON 读取、语言文件加载、lang/*.json 是否完整
make -C tests/motion     # 体感映射与参数文件
make -C tests/stack      # sysmodule 的线程栈预算（用 devkitA64 的 gcc 量栈帧）
```

这些测试都不需要 Switch（`tests/stack` 要 devkitA64 的编译器来量 aarch64 的栈帧，
`tests/ipc` 要读 libnx 的头文件）。
界面排版还能在电脑上渲染成图片直接看，用法见 `tests/canvas/tools/render_preview.c`
文件头部注释。

实机验证需要用户的手机与 Switch，步骤见 `docs/dglab-socket.md`。

## 已知限制

- 服务端运行期间不能手动休眠（自动休眠已由 NRO 抑制；NRO 退出后服务端仍在跑时，
  自动休眠这条路径也仍然会卡死）；
- 只实现 V3 协议，V4 的消息外壳未实现；
- 同时只绑定一个 App 连接（服务端最多接受 2 条 TCP，其中一条留给重连过渡）；已连接的
  App 没有单独的空闲超时，只靠 TCP 断开或 `shutdown()`；
- 部分与 App 的交互约定仍待实机确认，见 `docs/dglab-socket.md` 的“待确认”；
- `overlay/` 与 `mods/` 尚未开始。

## 文档

| 文档 | 内容 |
| --- | --- |
| `docs/dglab-socket.md` | WebSocket 模式（Socket 协议）：绑定流程、服务端实现、平台约束、排查记录 |
| `docs/dglab-protocol.md` | Coyote V3 协议移植范围与验证状态 |
| `docs/ipc.md` | IPC 服务名、版本与命令表 |
| `docs/nro-ui.md` | NRO 界面方案调研与实现记录 |
| `docs/joycon-input.md` | Joy-Con 六轴资料，以及"动作越大波形值越大"这个可选玩法的设计 |
| `docs/touch-input.md` | 触屏玩法：libnx 触屏资料、左右半区与两轴映射、与体感共用的参数、实机验收清单 |
| `docs/ble-poc.md` | BLE 模式（sysmodule 直连设备）的实测记录（未完成：连接与写入已通、通知路径待解） |
| `docs/ble-re.md` | BLE 直连的只读固件逆向：模块归属、固件侧 IPC 形状与当前判定进度 |
| `docs/docs-audit.md` | 文档约定（谁放哪一层）与 2026-09-17 审计的处置结果 |
| `docs/history.md` | 文档压缩时移出的历史原文（各文档的迭代过程与审计明细） |

贡献者与 agent 的工作规则在 `AGENTS.md` 以及各组件目录下的 `AGENTS.md`。

## 许可

[MIT](LICENSE)。

本项目是非官方第三方实现，与 DG-LAB 官方（dungeonlab）没有隶属关系。协议实现参照官方
公开仓库，来源与版本记录在 `docs/dglab-protocol.md`；DG-LAB 设备与 App 的使用请遵守
当地法律与设备说明。
