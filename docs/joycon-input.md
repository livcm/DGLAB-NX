# Joy-Con 六轴输入

本文是优先级 8（Joy-Con 传感器输入）的资料与设计。资料部分是查本机 libnx
（`/opt/devkitpro/libnx/include/switch/services/hid.h`）与官方示例
（`examples/switch/hid/sixaxis`、`sevensixaxis`）核对的结果；设计部分是本次确定的玩法：
**Joy-Con 动得越快越剧烈，波形值越大**，左右 Joy-Con 分别驱动 A / B 通道，作为 NRO 里的
一个可选模式，默认关闭。

## libnx 提供的参数

一次采样是 `HidSixAxisSensorState`：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `acceleration` | `HidVector`（float x/y/z） | 三轴加速度 |
| `angular_velocity` | `HidVector`（float x/y/z） | 三轴角速度 |
| `angle` | `HidVector` | 融合后的姿态角（要先开 fusion） |
| `direction` | `float[3][3]` | 姿态矩阵（同一个融合输出） |
| `delta_time` | `u64` | 与上一条采样的间隔 |
| `sampling_number` | `u64` | 采样序号，可用来判断有没有丢采样 |
| `attributes` | `u32` | `HidSixAxisSensorAttribute_IsConnected` / `_IsInterpolated` |

可用/可调的东西：

| 目的 | API |
| --- | --- |
| 取句柄 | `hidGetSixAxisSensorHandles(handles, n, npad_id, style)`：掌机（机身 IMU）、`NpadFullKey`（Pro 手柄）、`NpadJoyDual`（左右 Joy-Con 各一个句柄） |
| 开关 | `hidStartSixAxisSensor` / `hidStopSixAxisSensor` |
| 批量读取 | `hidGetSixAxisSensorStates(handle, states, count)`，`count` 可大于 1，返回实际写出的条数；共享内存里的环形缓冲是 17 条（`HidNpadSixAxisSensorLifo.storage[17]`） |
| 姿态融合 | `hidIsSixAxisSensorFusionEnabled` / `hidEnableSixAxisSensorFusion` / `hidGet`/`SetSixAxisSensorFusionParameters(float unk0, float unk1)` / `hidResetSixAxisSensorFusionParameters` |
| 陀螺零漂 | `hidSetGyroscopeZeroDriftMode(handle, Loose / Standard / Tight)` + get + reset |
| 静止判定 | `hidIsSixAxisSensorAtRest(handle, &out)` |
| 其它 | `hidIsFirmwareUpdateAvailableForSixAxisSensor`；`HidSixAxisSensorUserCalibrationState` 只有类型定义，libnx 没有对应的读写命令 |

没有暴露的：**采样率**（只能从 `delta_time` / `sampling_number` 反推）、**量程**
（±g、±dps）、原始寄存器访问。

几个容易混的点：

- 掌机模式的六轴来自**机身内的 IMU**，不是 Joy-Con；
- `SevenSixAxisSensor` 是另一套 API（主机 + Joy-Con 合并，官方用于 VR 模式）；
- 头文件里**没有任何单位注释**（搜过 `rad/s`、`g`、`deg/s` 都没有）。社区通行说法是
  加速度用 g、角速度用 rad/s、角度用弧度，但按项目规则这条必须实机量一次再写进文档。

## 玩法定义

- **输入**：Joy-Con 的运动剧烈程度；
- **输出**：波形值（波形强度 0~100），越剧烈越大，静止时回落到 0；
- **通道**：左 Joy-Con → A 通道，右 Joy-Con → B 通道，互不影响；
- **启用方式**：NRO 里的一个可选模式，默认关闭，只在需要时打开。

关键点：这里的"波形值"是**波形强度**，不是通道强度。通道强度仍然是用户在 `strength` 行
上调的那个值（相当于音量），设备输出 = 通道强度 × 波形强度。这与项目已有的分工一致
（见 `docs/dglab-socket.md` 的"测试按键"一节），所以运动映射**不需要碰用户的音量**。

## 设计

### 放在哪一层

放在 **NRO**，sysmodule 完全不动：

    Joy-Con IMU
        ↓  每帧读（HID）
    nro: joycon.c        句柄、采样、丢插值样本、断开检测
        ↓  采样
    nro: motion_feed.c   强度 → 25ms 槽位（平台无关，可主机测试）
        ↓  槽位
    NET_WAVEFORM（Append）→ sysmodule → App → 郊狼

理由：

- 这个玩法只在**前台 applet 里**说得通（NRO 占着屏幕时用户才是在"玩"它）；NRO 一退出就
  结束，正好符合"可选玩法"的定位；
- sysmodule 是 boot2 常驻的，去读 HID 会和前台应用抢同一份输入状态，而且它没有界面来
  反馈"现在是不是在动"；
- 将来如果要在**游戏进行中**用（优先级 11，Game Mod），那属于另一个进程、另一套输入
  来源，届时再单独设计——这条路线不阻碍它；
- 现有 IPC 已经够用：`NET_WAVEFORM` 的 `Append` 模式就是为"事件源持续生产波形"准备的，
  **不需要新增 IPC 命令、不需要改协议**。

### 模块划分

| 文件 | 内容 | 能否主机测试 |
| --- | --- | --- |
| `nro/source/motion/joycon.c` | libnx 侧：拿句柄、启动/停止、把 `HidSixAxisSensorState` 转成平台无关采样、左右句柄到 A/B 的映射、断开检测 | 否（要实机） |
| `nro/source/motion/motion_feed.c`（+ `nro/include/dglab/nro/motion_feed.h`） | 纯逻辑：运动采样 → 25ms 槽位；含强度公式、平滑、峰值保持、上限、节奏 | **是**（`tests/motion`） |
| `nro/source/main.c` | 开关按键、每帧驱动、把产生的槽位用 `NET_WAVEFORM` 上传、界面显示 | 否 |

把纯逻辑单独拆出来是照搬协议层的做法：映射公式是这个玩法里唯一需要反复调的东西，放到能
跑主机测试的地方，调参数时不必每次都上真机。

### 强度怎么算

每个采样先算一个 0~1 的"剧烈程度"：

    强度 = clamp01( w_gyro * |ω| / ω_ref + w_accel * |Δa| / a_ref )

- `|ω|` 是角速度模长（甩动、转手腕），`|Δa|` 是加速度相对上一条的变化量（抖动、撞击）；
- `ω_ref` / `a_ref` 是参考尺度（把"满量程"定在多大的动作上），权重与参考值都放配置里；
- `w_accel` 主要负责"甩一下"的冲击，`w_gyro` 负责持续挥动，两个都给默认值，实机调。

再按 25ms 窗口聚合：

- 窗口内取**峰值**而不是平均——"抡一下"的尖峰不能被静止采样平均掉；
- 静止时按释放曲线回落，不瞬间归零（听起来是"跟着动作走"，而不是开关咔哒）；
- 攻击/释放用两个不同时间常数（建议 ~50ms 起、~300ms 落）；
- `IsInterpolated` 的采样直接丢（那是主机插值出来的，不是真实运动）。

### 槽位与节奏

- 每槽 = 25ms：`frequency_ms` 默认固定 100（与测试波形同款），只改 `strength`；
- `strength = round(强度 × 上限)`，**上限默认 30**（不是 100）：这是贴在身上的东西，
  默认值应当保守，用户自己往上调；
- NRO 每 200~400ms 用 `Append` 上传一批（一批 ≤ 48 槽，IPC 上限）；服务端已经负责
  "每批最多 32 槽、提前 200ms 补流、队列 128 槽"，NRO 不需要自己算提前量；
- 静止时**继续发 0 强度的槽位**（保持节奏恒定，避免断续和队列空/满的边界情况）；
- 关闭模式：停传感器 + `Clear`（复用已有的 `B` 键逻辑）。

### 通道与手柄

- `NpadJoyDual` 一次拿两个句柄，按官方示例的约定第一个是左、第二个是右；左 → A，右 → B；
- 只插了一只 Joy-Con 时，另一路保持 0（界面显示"未连接"）；
- 掌机 / Pro 手柄的 IMU **暂不映射**（可选做法是映射到 A+B，等有需求再加）；
- 手柄断开或休眠：`IsConnected` 变 0 → 该通道立即静音，并在界面上说明。

### 界面

- 开关：新增一个按键（候选 `X` 或 `L`，footer 加一项），开启时面板显示模式状态；
- 新增一行 `motion`：左右通道的实时强度、是否已连接、上限值；
- **显示节流**：实时值以约 5Hz 刷新（整屏重绘只在快照变化时发生，见 `docs/nro-ui.md`
  的按需重绘），但采样与上传仍是每帧进行；
- 安全提示：模式开启时明确显示"会真实输出电压"和当前上限；
- 已有的 `last cmd` 行继续负责显示"服务端没起 / 没有 App 绑定"这类失败。

### 安全与默认值

| 项 | 默认 | 理由 |
| --- | --- | --- |
| 模式状态 | 关闭 | 可选玩法，不跟着 NRO 启动 |
| 波形强度上限 | 30 / 100 | 贴在身上的设备，默认保守 |
| 攻击 / 释放 | ~50ms / ~300ms | 避免跳变，也让手感跟得上动作 |
| 静止 | 强度 0（继续发槽位） | 输出停，节奏不乱 |
| 随时停 | `B`（`clear-A` + `clear-B`） | 已有按键，不需要新模式专属急停 |

## 实现顺序

1. `motion_feed` 纯逻辑 + `tests/motion`（合成采样：静止、单次冲击、持续挥动、超上限、
   插值样本、节奏与峰值保持）；
2. `joycon.c`：句柄、启动/停止、采样读取、左右映射（先只打印在屏幕上，确认单位与方向）；
3. 接上 `NET_WAVEFORM` 上传与 `B` 清除；
4. 界面：模式开关、`motion` 行、显示节流、安全提示；
5. 实机调参：权重、参考值、上限、攻击/释放时间；
6. 文档：把实机量到的单位、采样率、轴方向补回本文，README 的按键表加新模式。

## 待实机确认（写进文档前不许猜）

1. `acceleration` / `angular_velocity` / `angle` 的**单位**；
2. 实际采样率，以及每帧能读到几条（决定一个 25ms 窗口里有几条样本）；
3. 左右 Joy-Con 的坐标轴方向（左右是镜像的），确认"越剧烈越大"的符号取对；
4. `IsInterpolated` 出现的频率；
5. `hidIsSixAxisSensorAtRest` 的判据是否够稳（能否用来做零偏归零）；
6. NRO 在 applet 模式下读六轴的可用性与开销（每帧 60 次读是否明显耗电）；
7. Joy-Con 断开/休眠时 `attributes` 的变化时机。
