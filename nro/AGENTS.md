# NRO 工作规则

## 职责与边界

NRO 负责 UI、输入玩法、参数和调试，所有 DG-LAB 通信走 sysmodule IPC；
遵循 [根规则](../AGENTS.md)。UI/玩法与传输、协议解耦。
同进程只持有一个 `dglab` IPC 会话，子页复用菜单的 `Service*`，禁止重新 `smGetService`。

## 输入

- Joy-Con 按键/六轴优先用 libnx HID，核对 headers 和官方示例，不自行实现 HOS HID。
- 触屏只在玩法页初始化和读取，其它页面不碰；唯一平台 API 入口是
  `platform/touchscreen.c`，映射放平台无关 `touch/touch_feed.c`，用 `tests/touch` 验证。
- 触屏与体感共用 `config/motion.cfg`，由 `motion_settings.c` 统一读写；不新建触屏参数文件。
- BLE 会话跨页存活，蓝牙页 X 停、退出 NRO 补 BLE_STOP；菜单/玩法页必须提示活动会话。
  当前实机验证边界见 [BLE 验证](../docs/ble-poc.md)。

## 页面、行与文本（必须遵守）

采用 libnx framebuffer 自绘，console 留给 PoC/出错页。HOS 没有供普通 Homebrew 使用的
通用原生 Button/List/Window 框架；仿 HOS 风格也要与系统 UI 解耦。
设计依据与实测规格见 [UI 文档](../docs/nro-ui.md)。

- 页面为页头、行列表、底栏，无面板边框；共用 `page` 的 Begin/Header/HeaderStatus、
  ClipContent、列表绘制、ClearClip、列表滚动条与 Hints 流程。
- 页头右侧只显示统一的 sysmodule 状态，版本号进列表。页头/底栏线取 `theme->rule`，
  行分隔取对应主题字段；颜色一律通过 `dglabThemeGet()`，不得写颜色常量。
- 同一个 `DglabRow` 数组交给 Measure 和 Draw，尺寸/列宽/滚动来自测量，
  不另写公式；列表布局、夹紧与滚动条用共用 PageLayout/PageScrolls/PageScrollBar。
- 行只用 Item、Note、Paragraph；没有滑块，数值用文字。各行值有独立缓冲，
  不能让延后绘制的行共享会被覆盖的字符串。
- 二维码/日志等非行内容也按测量定位，不写位置常数。二维码只在 Listening/Paired 显示，
  `NET_QR` 成功只代表有地址，不能用来判断服务端运行。
- 玩法页可声明 y=88..646 的满幅玩法区，但墨迹不能越过两条分隔线；新页面在 canvas
  测试选择 `PageRegion_Playfield`，使用对应玩法区检查，不能套列表底栏检查。
- 字号只能取 `text.h` 的五档，通过 `DglabFontSet` 传递；不要自选字号或假设只有一个字体。
- 按键提示必须是 `DglabHint` 的图标＋动作；按键名不翻译，动作进 strings/lang。
  图标实心、字母用背景色挖空，按墨迹盒居中；显式传图标与文字两份字体。
- Console 页用 + 退出，其余 framebuffer 页用 B，+ 不响应。改行为同时改提示。
- 超过一屏必须可滚：有光标随光标，无光标用专用键。滚动条即提示，不在底栏重复写滚动。
  无光标且无滚动键的页面必须排进一屏；菜单移动夹紧，不循环。
- 日志每次按键滚一行，连发用自己的时序，不复用体感强度的连发状态。
- 坐标全是 720p 逻辑单位，1080p 缩放交 framebuffer/canvas，屏幕不乘比例。
- 重建 framebuffer（底座切换、console 切换）必须走 `appDisplaySuspend/Reopen`，
  同时重开共享字体并按 scale 光栅化，以 display generation 强制所有页面重画。
- 重绘条件覆盖光标、滚动、语言、主题、状态和 generation 等所有画面输入；
  按键使用本页当帧状态，不读仅由别页更新的全局。
- 新增/修改屏幕跑 `make -C tests/canvas`，两语言、两主题、720p/1080p 都不得越界。

## 文案与运行期目录（必须遵守）

- 自绘 UI 文案全部来自根 `lang/*.json`，代码用 `dglabString`；新增文案同步
  `strings.h` 枚举、`strings.c` 的 kKeys 和 en/zh-Hans JSON，跑 `tests/lang`。
- 语言文件只从 `DGLAB_LANG_DIR` 加载，必须与 NRO 一起安装。任意一语言可用即可启动，
  缺 key 回落；全不可用在 framebuffer 创建前给出英文 console 错误页。
- Console 输出保持 ASCII 英文（默认字体无汉字），sysmodule 日志不进语言包。
- 改文案无需重编，但必须跑 `tests/canvas` 与 `tests/lang`。
- 所有运行期文件放 `SD:/switch/DGLAB-NX/`：语言在 `lang/`，设置在 `config/`，日志在
  `logs/`。新增设置/日志不能散落应用或 SD 根目录；config/logs 由程序创建。

## 元信息与版本

- 普通 NRO 不使用 sysmodule 的 contents/Title ID 安装模型；NACP identity 不等于 sysmodule ID。
  多个需独立识别的 NRO 要有独立稳定的 identity；兼容 Ryujinx 时验证能分别识别。
- 应用名/作者来自 `nro/Makefile`；发行版本唯一来源为根 VERSION，进入 NACP/About，
  改 VERSION 必须重编，禁止另写版本值或使用 libnx 默认值。IPC 版本由 GET_VERSION 单独显示。
- About 构建标识用 `git describe --always --dirty` 核对安装版本；图标修改工具见
  `nro/tools/make_icon.c` 文件头。

## 构建

源码、headers、IPC、Makefile 或 linker 改动后至少 `make -C nro`，报告实际验证；
发布用 `make -C nro package`，产物为 `DGLAB-NX.nro` 与运行期语言包。
