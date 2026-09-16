# DGLAB-NX

在 Nintendo Switch 上运行的 DG-LAB 设备控制服务。

项目把 DG-LAB 的连接与协议实现收进一个常驻后台服务（sysmodule），前端组件
（NRO / Overlay / Game Mod）只通过 IPC 使用它，不各自实现一遍协议，也不各自去抢设备。

当前传输路线是 **Wi-Fi + WebSocket**：Switch 自己当 WebSocket 服务端，手机上的
DG-LAB App 扫码接入，BLE 由手机负责，Switch 不直接持有蓝牙连接。

## 状态

| 组件 | 状态 |
| --- | --- |
| `sysmodule/` DG-LAB 服务（服务名 `dglab`，Title ID `0x00FF072107210721`） | 可用：Coyote V3 协议层 + Wi-Fi/WebSocket 服务端 + IPC |
| `nro/` 前端 | 可用：菜单选择玩法——测试屏（地址/二维码/测试键/日志）与体感玩法（Joy-Con 驱动波形） |
| `overlay/` | 未实现 |
| `mods/` | 未实现 |
| 主机侧 BLE 直连 | 已搁置，见 `docs/ble-poc.md` |

协议层、IPC 布局、WebSocket/Socket 服务端、二维码与 NRO 绘制都有主机侧测试，见
[测试](#测试)。

## 架构

```
DG-LAB App（手机，负责 BLE）
             ↑  WebSocket（App 是客户端）
   ┌─────────┴─────────┐
   │  dglab sysmodule  │  WebSocket 服务端 / Socket 协议 / Coyote V3
   └─────────┬─────────┘
             │  IPC（服务名 dglab）
             ├──→ NRO（可用）
             ├──→ Overlay（未实现）
             └──→ Game Mod（未实现）
```

围绕这张图的几条规则：

- **单一连接所有者**：只有 sysmodule 接触设备侧通信，其它组件一律走 IPC；
- **IPC 是内部公共 API**：命令号定义在 `common/include/dglab/ipc.h`，一旦发布不重排，
  新增命令用新号；
- **协议层与平台解耦**：`sysmodule/source/protocol`、`sysmodule/source/net` 不含
  libnx / HOS 依赖，所以能在电脑上直接测。

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `sysmodule/` | DG-LAB 设备生命周期、协议实现、WebSocket 服务端、IPC 服务端 |
| `nro/` | Homebrew 前端：界面、二维码、测试按键、日志落盘 |
| `lang/` | 界面文案，一个语言一个 `.json`（构建时复制到 `release/DGLAB-NX/lang/`） |
| `overlay/` | Tesla / Ultrahand overlay（未实现） |
| `mods/` | 特定游戏的联动（未实现） |
| `common/` | 组件间共享的 IPC 定义与公共类型 |
| `tests/` | 主机侧测试（protocol / ipc / net / qr / canvas） |
| `docs/` | 技术文档，见[文档](#文档) |
| `release/` | 构建产物（不提交） |

## 构建

组件构建需要 devkitPro（devkitA64 + libnx）。主机侧测试用系统编译器就能跑，其中
`tests/ipc` 需要读 `$(DEVKITPRO)/libnx` 的头文件（默认 `/opt/devkitpro`）。

```
export DEVKITPRO=/opt/devkitpro
make                 # 构建全部组件，产物统一落在 release/
make clean           # 清除 release/ 与各组件的 build/
```

也可以只构建单个组件：

```
make -C sysmodule package   # release/<TITLE_ID>/{exefs.nsp,toolbox.json,flags/boot2.flag}
make -C nro package         # release/DGLAB-NX/{DGLAB-NX.nro, lang/*.json}
```

sysmodule 的 Title ID 只在 `sysmodule/DGLAB-NX-Core.json` 里写一次，Makefile、安装目录名和
`toolbox.json` 都从它推导，别在别处再抄一份。

## 安装

```
release/00FF072107210721/  →  SD:/atmosphere/contents/00FF072107210721/
release/DGLAB-NX/          →  SD:/switch/DGLAB-NX/      （整个目录一起拷）
```

sysmodule 带 `flags/boot2.flag`（`toolbox.json` 里也是 `requires_reboot: true`），
随系统启动加载，复制完要重启主机。overlay 还没实现，所以现在没有 `DGLAB-NX-Ovl.ovl`。

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
2. 打开 `DGLAB-NX.nro`，菜单里选 `Socket test`，界面显示局域网地址与二维码；
3. 用 DG-LAB App 扫码接入（App 是 WebSocket 客户端）；
4. 用测试按键确认设备有输出。

菜单里的 `Motion (Joy-Con)` 是另一个玩法：左右 Joy-Con 分别驱动 A / B 通道，动得越快
波形值越大、脉冲越密；通道强度仍然是上面的"音量"。做法与参数见
`docs/joycon-input.md`。

同一菜单里的 `Advanced (motion)` 是体感玩法的参数页（死区、灵敏度、包络、频率、波形
强度上限），改完自动存到 `sdmc:/switch/DGLAB-NX/config/motion.cfg`。

按了没反应先看界面上的 `last cmd` 行：它显示最近一次按键的结果（`ok` / `no app bound`
/ `socket error`），红色是失败、黄色是"命令发出去了但听不到"（那一路强度还是 0）。

| 按键 | 动作 |
| --- | --- |
| `A` | 启动 WebSocket 服务端 |
| `Y` | 停止服务端 |
| `ZL` | 测试通道 A（送波形 + A 的强度） |
| `ZR` | 测试通道 B（送波形 + B 的强度） |
| `B` | 清空波形（clear A / clear B） |
| `↑` / `↓` | 调整通道 A 强度（0~100，步进 1，按住连续调整，改完立刻发给 App） |
| `→` / `←` | 调整通道 B 强度（同上） |
| `-` | 切到 BLE PoC 控制台视图（已搁置路线，保留用于诊断） |
| `+` | 退出 |

两个通道的强度各自独立，都从 0 开始；调整即刻发送给 App，所以没有单独的"发送强度"
按键。界面上 `test` 那一行同时显示两个通道的当前值（`A n/100  B n/100`）。

服务端**不会开机自启**（只有按 `A` 才启动），最后一个客户端离开 55 秒后会自动停，
避免长期占用 socket。

两点必须注意：

- **服务端运行期间不要让主机休眠。** 睡眠通知注册被系统拒绝，持有 socket 跨过睡眠会
  让主机挂死，只能长按电源键。原因与应对见 `docs/dglab-socket.md` 的“睡眠与唤醒”。
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
make -C tests/canvas     # canvas 裁剪、字体位序、整屏排版
make -C tests/lang       # JSON 读取、语言文件加载、lang/*.json 是否完整
```

这些测试都不需要 Switch，也不需要 devkitA64（只有 `tests/ipc` 要读 libnx 头文件）。
界面排版还能在电脑上渲染成图片直接看，用法见 `tests/canvas/tools/render_preview.c`
文件头部注释。

实机验证需要用户的手机与 Switch，步骤见 `docs/dglab-socket.md`。

## 已知限制

- 服务端运行期间主机无法正常休眠；
- 只实现 V3 协议，V4 的消息外壳未实现；
- 同时只服务一个 App 连接，且没有空闲超时（只靠 TCP 断开或 `shutdown()`）；
- 部分与 App 的交互约定仍待实机确认，见 `docs/dglab-socket.md` 的“待确认”；
- `overlay/` 与 `mods/` 尚未开始。

## 文档

| 文档 | 内容 |
| --- | --- |
| `docs/dglab-socket.md` | Wi-Fi + WebSocket 传输：绑定流程、服务端实现、平台约束、排查记录 |
| `docs/dglab-protocol.md` | Coyote V3 协议移植范围与验证状态 |
| `docs/ipc.md` | IPC 服务名、版本与命令表 |
| `docs/nro-ui.md` | NRO 界面方案调研与实现记录 |
| `docs/joycon-input.md` | Joy-Con 六轴资料，以及"动作越大波形值越大"这个可选玩法的设计 |
| `docs/ble-poc.md` | 主机侧 BLE 直连的实测记录（已搁置） |

贡献者与 agent 的工作规则在 `AGENTS.md` 以及各组件目录下的 `AGENTS.md`。

## 许可

[MIT](LICENSE)。

本项目是非官方第三方实现，与 DG-LAB 官方（dungeonlab）没有隶属关系。协议实现参照官方
公开仓库，来源与版本记录在 `docs/dglab-protocol.md`；DG-LAB 设备与 App 的使用请遵守
当地法律与设备说明。
