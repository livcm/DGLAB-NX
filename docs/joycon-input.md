# Joy-Con 六轴输入

本文是 Joy-Con 传感器输入玩法的资料与设计。资料部分是查本机 libnx
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
- **输出**：波形值（波形强度 0~100）**和频率**都跟着动作走——越剧烈波形值越大、脉冲越密，
  静止时回落到 0 与最慢频率；
- **通道**：左 Joy-Con → A 通道，右 Joy-Con → B 通道，互不影响；
- **启用方式**：NRO 菜单里选择的一种玩法，默认不进入。

关键点：这里的"波形值"是**波形强度**，不是通道强度。通道强度仍然是用户在 `strength` 行
上调的那个值（相当于音量），设备输出 = 通道强度 × 波形强度。这与项目已有的分工一致
（见 `docs/dglab-socket.md` 的"测试按键"一节），所以运动映射**不需要碰用户的音量**。

强度行写成 **`值/上限`**（例如 `20/80`）：右边的上限就是 D-pad 停住的地方。Socket、体感、
触屏三页优先用 App 上报的上限（`DglabNetStatus::app_limit_*`，那才是真正在钳制的那个），
App 还没上报时用参数页里的通道强度上限；蓝牙页始终用参数页的那一对
（`dglabChannelCeiling()`，见 `docs/ipc.md`）。

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
- 将来如果要在**游戏进行中**用（Game Mod），那属于另一个进程、另一套输入
  来源，届时再单独设计——这条路线不阻碍它；
- 现有 IPC 已经够用：`NET_WAVEFORM` 的 `Append` 模式就是为"事件源持续生产波形"准备的，
  **不需要新增 IPC 命令、不需要改协议**。

### 模块划分

| 文件 | 内容 | 能否主机测试 |
| --- | --- | --- |
| `nro/source/motion/joycon.c`（+ `dglab/nro/joycon.h`） | libnx 侧：拿句柄、启动/停止、把 `HidSixAxisSensorState` 转成平台无关采样、丢连接/插值样本 | 否（要实机） |
| `nro/source/motion/motion_feed.c`（+ `dglab/nro/motion_feed.h`） | 纯逻辑：运动采样 → 25ms 槽位；含强度公式、死区、平滑、峰值保持、频率、静止停流、节奏 | **是**（`tests/motion`，23 项） |
| `nro/source/ui/motion.c` | 玩法界面（平台无关） | **是**（`tests/canvas`） |
| `nro/source/main.c` | 每帧驱动、把产生的槽位用 `NET_WAVEFORM` 上传、菜单分发 | 否 |

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
- 静止时按释放曲线回落，不瞬间归零（体感上是"跟着动作走"，而不是"啪"地一下断掉）；
- 攻击/释放用两个不同时间常数（建议 ~50ms 起、~300ms 落）；
- `IsInterpolated` 的采样直接丢（那是主机插值出来的，不是真实运动）。

**死区（必须有）**：手上握着不动也不可能是理想静止，IMU 噪声、心跳式的微抖都会给出非零
读数。所以每个采样的强度先过死区：低于**进入阈值**直接当 0，高于**退出阈值**才重新算
"在动"，两个阈值不同（滞后）是为了避免在阈值附近来回跳。建议初值 进入 0.05 / 退出 0.02，
实机调。必要时可以配合 `hidIsSixAxisSensorAtRest` 或在静止期间估计零偏来辅助判断。

### 槽位与节奏

- 每槽 = 25ms：`strength = round(强度 × 100)`，写满 0~100；
- **频率也跟着强度走**：`frequency_ms = 100 - (100 - 30) × 强度`，即静止 100ms（与测试波形
  同款）、最剧烈 30ms。V3 的频率在程序侧 10~100ms 是 1:1 映射到设备值，所以这个区间
  无损；30 与 100 都是初值，实机调；
- NRO 每 200~400ms 用 `Append` 上传一批（一批 ≤ 48 槽，IPC 上限）；服务端已经负责
  "每批最多 32 槽、提前 200ms 补流、队列 128 槽"，NRO 不需要自己算提前量；
- **静止时的策略**：死区内继续发槽位，让强度沿释放曲线衰减；当"已经静止超过 250ms"**且**
  "释放曲线掉到强度 0"时**停止上传**，队列自然放空、输出停下、流量归零。**必须等它归零**：
  只按静止时长停发等于把释放曲线截断，那正是释放曲线要避免的突然一下。再次超过进入阈值
  时立刻恢复上传（第一批立即下发，服务端立即补流，所以起播只差一帧加网络）。
  为什么不一直发 0：静止也发的话 IPC/WebSocket 会一直跑（40 槽/秒 ≈ 每秒 2~3 条 pulse
  命令），App 的日志会被无意义的 `tx` 刷满；停发省掉这些唤醒，代价只是重新起播的几十
  毫秒，而释放曲线已经把它衰减到 0，所以不会在停的那一刻突然断掉；
- 关闭玩法：停传感器 + `Clear`（复用已有的 `B` 键逻辑）。

### 通道与手柄

- `NpadJoyDual` 一次拿两个句柄，按官方示例的约定第一个是左、第二个是右；左 → A，右 → B；
- 只插了一只 Joy-Con 时，另一路保持 0（界面显示"未连接"）；
- 掌机 / Pro 手柄的 IMU **暂不映射**（可选做法是映射到 A+B，等有需求再加）；
- 手柄断开或休眠：`IsConnected` 变 0 → 该侧立即静音，并在界面上说明。

实现上同时持有 `NpadJoyDual` 的一对句柄和 `JoyLeft`/`JoyRight` 的单只句柄，轮询时逐个
尝试——系统怎么分配手柄都能读到。

**顺序是有意义的：同一侧只用一个句柄。** 这两组句柄说的是**同一只物理手柄**，所以
`joycon.c` 按 `NpadJoyDual` 在前、单只风格在后的顺序轮询，**一旦某个句柄真的给出了采样就
停止**。否则同一只手柄的读数会被送进 feed 两次，而"重复的一条"看起来正好是"没有变化"，
会把加速度差分那一路稀释掉。

**"这一侧能不能用"由主机回答，不能问六轴句柄。** 这是 2026-09-17 三轮回合才弄清楚的一条：
Joy-Con 插回主机、或者被按同步键关掉之后，**六轴句柄照样在给读数**（所以按"有没有读数"判断
连接是错的，UI 才会一直停在"挥动中"）。真正回答这个问题的是主机自己的控制器状态，而它就在
NRO 每帧都在用的那个 `PadState` 旁边：

| 来源 | 回答什么 |
| --- | --- |
| `hidGetNpadDeviceType(HidNpadIdType_No1)` | **按侧**的设备类型：`JoyLeft`/`JoyRight`（拆下来的 Joy-Con，本玩法可用）、`HandheldLeft`/`HandheldRight`（夹在主机上）、`FullKey`（Pro 手柄） |
| `padGetStyleSet()` / `padGetAttributes()` / `padIsHandheld()` | 同一件事的粗粒度版本（生效风格 + `IsLeftConnected`/`IsRightConnected` + 是否处于掌机模式） |
| 六轴句柄的读数 | 只回答"现在动得多猛、频率多少"，**不回答连接** |

规则因此是分层的（`main.c` 里按信任度取第一个有回答的来源）：

1. 设备类型可用 → 左侧可用 = 有 `JoyLeft` 位，右侧可用 = 有 `JoyRight` 位；
2. 否则 PadState 有回答 → 非掌机模式、风格含对应的一半、且该半边的 `Is*Connected` 置位；
3. 都不回答（连 PadState 都是空的）→ 回到"只看读数"的老办法。

**不可用的一侧根本不轮询句柄**：不产生采样、通道不出声、界面显示"未连接"——所以"显示未连接"
和"通道有输出"仍然不可能同时成立（这一条是硬不变量）。

其余几条是这几轮回合留下的（保留）：

- **两侧的句柄永远来自同一次调用**（`hidGetSixAxisSensorHandles(pair, 2, …, NpadJoyDual)`），
  之前每侧各取一次，只有一只拆下时可能把一侧绑到**另一只手柄**上——用户看到的"右手柄的数值跑
  到左手柄那一行、然后两行定住"就是这个；
- **不按侧重新取句柄**（那是串线的来源，实机日志里 `rescans 24` / `158` 就是这种 churn）；只有
  两侧同时静默 `RESCAN_AFTER_QUIET_POLLS`(120，约 2 秒)才重取一次，另外可以按 `Y`
  （底栏「重新扫描手柄」）手动重取；
- 一对句柄可用时**只用它**，单只风格的句柄只在"这台机器根本取不到一对句柄"时兜底；
- **手柄不再有读数时动作也会结束**：`motion_feed` 的 `moving` 以前只在采样进来时更新，手柄一走
  行上会停成"挥动中 / 波形值 0 / 100ms"（实机反馈里的"定住"就是它）。现在释放曲线走完、窗口里
  又没有任何采样时，`moving` 复位成静止。

**连接状态是整侧判断的，不由某一个句柄说了算。** `dglabMotionSensorConnected()`
（`nro/source/motion/motion_feed.c`，`tests/motion` 覆盖）的兜底只看读数，主机回答了
"这一侧能不能用"时就不会走到它：

1. 这一轮有任何采样 → **已连接**（读数只有在自称已连接时才会进入 feed，所以"显示未连接"
  与"正在输出"不可能同时成立）；
2. 否则**连续 `DGLAB_MOTION_SENSOR_QUIET_POLLS`(20，约 0.33 秒)** 次轮询一条读数都没有 →
   **未连接**；
3. 还没到那个次数 → 保持上一次（刚进玩法的那几帧还没有读数；中间偶尔冒出来的占位读数也不会
  让这一行闪）。

活的传感器每帧都把 LIFO 填满（实测每次轮询 16 条），安静下来就是不在了；用**轮询次数**
而不是毫秒计数，是为了让一次卡帧/一次 IPC 停顿看起来只是"一次轮询"。这条规则被推翻过两轮
（占位读数误判、关掉手柄后仍显示"挥动中"），经过见 `docs/history.md`。

**单只风格的句柄只在取不到一对句柄时才用。** 一对 Joy-Con 插回主机后一对句柄会回占位读数，
而单只风格句柄（同一只物理手柄的另一个名字）还在给数据，那一侧就会永远活着。所以一对句柄
可用时**只用它**，单只句柄只作为"这台机器根本取不到一对句柄"时的兜底。

进入玩法时 NRO 会把每侧的句柄数、连续空轮询次数与本轮每个句柄的 states/采样/connected 写
一行日志到 `sdmc:/switch/DGLAB-NX/logs/dglab-net.log`；没轮到的句柄会写成 `#n not polled`
（"没有读数"和"没问它"是两件事）。**实机实测（2026-09-17，一对拆下的 Joy-Con，进去就挥）**：

    motion left: handles 2, quiet 0, #0 states 16, samples 16, connected 1, #1 not polled
      | right: handles 2, quiet 0, #0 states 16, samples 16, connected 1, #1 not polled

由此确定的实机事实：

- **每侧确实拿到两个句柄**（`NpadJoyDual` 的一个 + 单只风格的一个），顺序如设计；
- **真正给数据的是 `NpadJoyDual` 那个**：一次轮询 16 条（后续帧 14~15 条是有效读数，差的那
  一两条是插值样本，被丢掉）；深度正好是系统 LIFO 的上限附近（17 条），所以每帧都把环形缓冲
  取空。这条"每帧都被填满"是上面第 4 条规则的依据；
- **单只风格的句柄在这套玩法下没有数据**（新规则下根本不去轮询它）——这也解释了为什么
  "一个句柄判整侧"的旧规则会出错；
- 连接状态变化时会另写一行 `motion left connected` / `motion left disconnected`（只在变化时
  写，静止会话不刷屏），并且**那一行带着该侧当时的完整描述**（quiet、rescans、每个句柄的
  states/采样/connected），所以在日志里能直接看出"为什么"变：`sdmc:/switch/DGLAB-NX/logs/dglab-net.log`
  里既能看到手柄掉没掉，也能看到是哪条规则判的。

**诊断行本身踩过一次坑**：给描述加了 `quiet`、`#n` 之后行变长了，而当时 `main.c` 里的
`left[96]` / `right[96]` 缓冲没跟着改，于是日志里那行被从中间截断（`connecte | right: …`）
——诊断信息自己变成噪音（现在两个缓冲都是 160）。加字段时记得连缓冲一起量一遍。

注意：这个文件是 **NRO** 落盘的，而 NRO 只在**服务端页**轮询 sysmodule 日志；直接进体感页
时文件里就只有上面这几行（sysmodule 自己的 `dglab-sys.log` 不受影响）。

**左右坐标轴方向不需要标定**：强度用的是 `|ω|` 和 `|Δa|` 的模长，左右手柄镜像的坐标系
对模长没有影响（原来的待确认清单里有一条，现在可以划掉）。

### 菜单与界面

玩法**不绑按键**，而是 NRO 启动后先进一个菜单：

    DGLAB-NX   玩法
      > 测试（socket 面板）     现状那一屏：服务端状态、二维码、测试键、日志
        体感（Joy-Con）         本文这个玩法
        BLE PoC 控制台          原 `-` 键那个视图，挪进菜单

导航：菜单里 `D-pad` 上下选择、`A` 进入、`B` 退出 NRO；玩法内部 `B` 返回菜单
（framebuffer 页一律 `B` 返回，`+` 只在 console 页生效；清空波形是 `X`）。原先把 BLE PoC
绑在 `-` 上的快捷键随菜单一起取消（它变成一个菜单项）。菜单本身是一屏 canvas 内容
（列表 + 选中高亮 + 一行说明），按项目惯例放在 `nro/source/ui/menu.c` 里，用
`tests/canvas` 一起做排版回归。

体感玩法自己的界面：

- 两行输入状态，标签就是手柄本身（**不是** DG-LAB 的通道）：`左 Joy-Con` / `右 Joy-Con`，
  值是这一侧的实时状态——`未连接` / `静止 波形值 0 Nms` / `挥动中 波形值 N Nms`。
  早期这两行叫"通道 A / 通道 B"，用户会把它读成 DG-LAB 通道的连接状态（那是上面的
  `连接` 行），所以才改名（`motion_joycon_left` / `motion_joycon_right`）；
- **显示节流**：实时值以约 5Hz 刷新（整屏重绘只在快照变化时发生，见 `docs/nro-ui.md`
  的按需重绘），但采样与上传仍是每帧进行；
- 安全提示：进入玩法时明确显示"会真实输出电压"，并提示"觉得太强就调通道强度"；
- 已有的 `last cmd` 行继续负责显示"服务端没起 / 没有 App 绑定"这类失败。

### 安全与默认值

| 项 | 默认 | 理由 |
| --- | --- | --- |
| 玩法状态 | 菜单里选择才进入 | 不跟着 NRO 启动 |
| 波形强度范围 | 0~100（写满） | 强弱由用户自己调**通道强度**控制，波形值不额外设上限 |
| 死区 | 进入 0.05 / 退出 0.02 | 手握不动的噪声必须被吃掉，两个阈值避免抖动 |
| 攻击 / 释放 | ~50ms / ~300ms | 避免跳变，也让手感跟得上动作 |
| 静止 | 衰减到 0 且静止超过 250ms 后停发 | 输出停、流量归零，重新起播只差几十毫秒 |
| 随时停 | `X`（`clear-A` + `clear-B`） | 已有按键，不需要新模式专属急停 |

### 参数页（Advanced）

上表这些值都能在 NRO 的 `Advanced (motion)` 里改，不用重新编译：`D-pad` 上下选、左右改
（**按一下一格**，按住 0.5 秒后才连发、每 0.2 秒一格），`Y` 恢复默认，`B` 保存返回。
改动即时写入 `sdmc:/switch/DGLAB-NX/config/motion.cfg`，体感玩法进入时读取（文件损坏或缺失就
回到默认值）。

| 参数 | 默认 | 范围 | 一格 |
| --- | --- | --- | --- |
| deadzone enter | 0.05 | 0.01~0.50 | 0.01 |
| deadzone exit | 0.02 | 0.01~0.50（不超过 enter） | 0.01 |
| gyro range | 6.00 | 1~30 | 0.5 |
| accel range | 2.00 | 0.25~10 | 0.25 |
| gyro weight | 1.00 | 0~3 | 0.1 |
| accel weight | 1.00 | 0~3 | 0.1 |
| attack | 50ms | 10~1000ms | 10ms |
| release | 300ms | 25~2000ms | 25ms |
| idle stop | 250ms | 100~2000ms | 50ms |
| **freq fast** | **30ms** | 10~500ms | 5ms |
| freq still | 100ms | 10~1000ms | 10ms |
| density | 可变 | 可变 / 固定 | 切换 |
| fixed density | 65ms | 10~500ms | 5ms |
| strength max | 100 | 1~100 | 1 |
| channel limit A | 100 | 0~100 | 1 |
| channel limit B | 100 | 0~100 | 1 |

最后两行不是体感参数：它们是**通道强度上限**（设备侧 BF 强制的天花板，一个通道一个），
体感/触屏自己不读，BLE 会话启动时按这一对写 BF（见 `docs/ipc.md` 的 `BLE_*`）。放在这一页
是因为"参数"只有这一个家，两种玩法与 BLE 会话共用同一份 `motion.cfg`（见 `docs/touch-input.md`
的「参数复用」）。上限和强度是两回事：强度是页面上拨的那个数，上限是它上不去的天花板，
设成 0 时那一通道不会有任何输出。

`density` 是这一页唯一的开关（值显示成词而不是数字）：**可变**＝脉冲间隔跟着玩法自己的
驱动走（体感跟着挥动幅度），**固定**＝间隔恒为 `fixed density`，只有波形值还在动。两种
玩法共用这一个开关，详见 `docs/touch-input.md` 的「参数复用」。

实机反馈（2026-09-16）：**默认的 30ms + 通道强度 50 感觉偏弱**，而同一支设备用 App 自带
波形、通道强度 40 多就已经很强。这条对得上"密度也是强度"这一条——波形值一样时，脉冲越密
单位时间送出的电荷越多，所以 `freq fast` 往下调（10ms 是设备下限）大概率是最有效的那个
旋钮；`gyro range` 调小、`strength max` 保持 100 也可以一起试。这也是把这页做出来的原因：
这类数值只有贴着皮肤试才知道，编译一遍调一次太慢。

## 实现顺序

1. `motion_feed` 纯逻辑 + `tests/motion`（合成采样：静止与死区、单次冲击、持续挥动、
   插值样本、节奏与峰值保持、频率随强度变化、静止后停发的时机）——**已完成**；
2. 菜单视图 `nro/source/ui/menu.c` + `tests/canvas` 的排版回归，`main.c` 改成"菜单 →
   各玩法"的分发结构（BLE PoC 也挪进菜单）——**已完成**；
3. `joycon.c`：句柄、启动/停止、采样读取、左右映射（先只显示在屏幕上，确认单位与方向）；
   ——**已完成**，读数已经接到强度映射上（尚未实机验证）；
4. 接上 `NET_WAVEFORM` 上传、频率映射、`B` 清除；
   ——**已完成**（全零的批次不上传，这就是"静止停流"的落地方式）；
5. 玩法界面：`motion` 行、显示节流、安全提示；
   ——**已完成**（`nro/source/ui/motion.c`，实时值 5Hz 刷新）；
5b. 参数页：`Advanced (motion)`（`nro/source/motion/motion_settings.c` +
   `nro/source/ui/advanced.c`）——**已完成**，参数存 SD 卡，`tests/motion` 覆盖默认值、
   单步加减、范围钳制、关联项约束与文件往返；
6. 实机调参：权重、参考值、死区、攻击/释放、频率区间；
7. 文档：把实机量到的单位、采样率、轴方向补回本文，README 加菜单与玩法说明。

剩下的就是第 6、7 步（调参与文档收尾）。**实机记录（2026-09-16）：体感玩法工作正常**
（用户确认）——移动左右 Joy-Con 能驱动对应的通道输出。下面这些细节仍待逐步确认：

- 握在手里不动时 `motion` 一行的 level 是不是 0（死区够不够大）；
- 甩一下能不能立刻感到输出、松手后是不是平滑地回落；
- ~~两个通道分别对应左右手柄~~：已确认（左 → A、右 → B）；界面上的"未连接"判定 bug 也已
  修（见上面「通道与手柄」一节），下次实机再看一次日志里 `motion left: handles …` 那行就能
  把主机实际交出的句柄布局记进文档；
- 静止超过 250ms 后 `last cmd` 不再出现新的 `waveform X  ok`（说明确实停流了）；
- 手感太猛或太钝时，调 `motion_feed` 的默认配置（`dglabMotionFeedDefaultConfig`）。

## 待实机确认（写进文档前不许猜）

1. `acceleration` / `angular_velocity` / `angle` 的**单位**；
2. ~~实际采样率，以及每帧能读到几条~~：**部分回答**——进入玩法后第一次轮询拿到 16 条
   （LIFO 深度 17，所以每帧基本取空），按 60fps 算大约每秒 960 条；稳态数值与是否有丢失
   还没统计（要看多帧的 states 计数）；
3. ~~左右 Joy-Con 的坐标轴方向~~：不需要——强度用模长，左右镜像不影响；
4. `IsInterpolated` 出现的频率：**部分回答**——目前抓到的轮询里一条都没有（所以实测频率
   很低，但还没有长时间统计）；
5. `hidIsSixAxisSensorAtRest` 的判据是否够稳（能否用来做零偏归零）；
6. NRO 在 applet 模式下读六轴的可用性与开销（每帧 60 次读是否明显耗电）；
7. ~~Joy-Con 断开/休眠时六轴 `attributes` 的变化时机~~：**已确认这条路问不出连接状态**——
   插回主机、或者按同步键关掉之后，句柄**照样给读数**（2026-09-17 实机三轮才对上），所以
   连接判定改问主机自己的控制器状态（设备类型 / `PadState`，见上面「这一侧能不能用」）。
   六轴 `attributes` 只剩下"这条读数能不能用"这一个用途，`motion … connected /
   disconnected` 的日志行也改成跟着主机状态走。
