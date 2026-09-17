# NRO AGENTS.md

## 职责

`nro/` 是普通的 Switch Homebrew 应用程序。

负责：

- 用户界面；
- 接收 Joy-Con 按键和传感器输入；
- 体感玩法；
- DG-LAB 参数配置；
- 调用 Sysmodule IPC；
- 设备状态展示；
- 测试和调试功能。

## 边界

- NRO 不负责实现底层 DG-LAB Bluetooth Protocol。
- NRO 不得自己建立或持有 DG-LAB 设备侧连接（BLE/WebSocket），所有 DG-LAB 通信必须通过
  Sysmodule IPC（见根 `AGENTS.md` 的"单一 DG-LAB 连接所有者"）。
- NRO 的 UI、玩法和业务逻辑应与 BLE/协议层解耦。

## 交互模型示例

    Joy-Con 输入
        ↓
    NRO 计算玩法逻辑
        ↓
    IPC: SendEffect(...)
        ↓
    Sysmodule
        ↓
    DG-LAB

## Joy-Con 输入

Joy-Con 按键输入和六轴传感器应优先使用 libnx 提供的 HID API，而不是自行实现
HOS HID 协议。libnx 和官方 `switch-examples` 都可以作为 API 和用法的参考。

## UI 原则

应该先用 Console 来呈现字符 UI，待 NRO 的业务逻辑全部实现后再实现 GUI。

各可选 UI 方案（console / framebuffer / deko3d / SDL2 / OpenGL / Borealis / ImGui）的
依赖、工作量与取舍见 `docs/nro-ui.md`；当前结论是先走 libnx framebuffer 自绘
（零额外依赖，且二维码需要严格正方形的模块）。

HOS 并没有提供类似 UIKit / SwiftUI / Android Views 的通用
Nintendo 原生 UI Framework 给普通 Homebrew 使用。

因此：

- 不要寻找不存在的“HOS 原生 Button/List/Window API”；
- NRO UI 可以使用自己的 UI / Renderer；
- 可以使用 SDL、OpenGL、deko3d、Dear ImGui 或自定义 Renderer；
- 如果需要 Nintendo 风格 UI，应通过自己的 UI 组件实现。

UI 风格可以模仿 HOS，但实现应与 HOS 系统 UI 解耦。

颜色只有一套来源：`nro/include/dglab/ui/theme.h` 的两套调色板（`dglabThemeDark` /
`dglabThemeLight`），屏幕一律经 `dglabThemeGet()` 取色，**不得写颜色常量**。用户可在
关于页按 `Y` 切换"跟随系统 / 浅色 / 深色"，因此新增或修改屏幕后 `tests/canvas` 会用
两套主题各渲染一遍（含 720p/1080p 与两种语言），浅色下同样要求零像素越界；量色规则与
已量/未量的值见 `docs/nro-ui.md` 的「浅色主题」。

建议逐渐形成自己的组件：

    Screen
    Header
    Sidebar
    List
    ListItem
    Slider
    Toggle
    Button
    Modal

### 页面、行与文本（必须遵守）

界面模仿 HOS（Switch 系统 UI）：**没有面板边框**，页面由页头、行列表、底栏三部分组成。
规格与实测数据见 `docs/nro-ui.md` 的「HOS 风格页面」一节。

- 每一屏的画法：`dglabPageBegin` 铺底与两条分隔线 → `dglabPageHeader` 画标题 →
  `dglabPageHeaderStatus` 画页头右侧的 Sysmodule 状态 → `dglabPageClipContent` 收裁剪 →
  `dglabListDraw` 画行 → `dglabCanvasClearClip` → `dglabListPageScrollBar` →
  `dglabPageHints` 画底栏按键提示；
- **页头右侧一律是 Sysmodule 状态**（`dglabPageHeaderStatus`，文案与配色就是主菜单那两条）。
  版本号之类的东西不进页头：它是每一页共有的"后台还在不在"这一条信息，只有这一个实现；
- 两条横线都是**白色**（`theme->rule`，y=87 与 y=647）；`#4D4D4D` 只用于行与行之间。
  内容裁剪区是 y 88..647，上边界必须在页头线下方——焦点框比行高，第一行的框上沿在 y≈122，
  裁剪区从行顶开始就会把它切掉；
- 行一律用 `nro/include/dglab/ui/list.h` 的 `DglabRow` 数组描述，**同一个数组**
  交给 `dglabListMeasure` 与 `dglabListDraw`。高度、列宽、滚动位置都必须来自这次测量，
  不要另写一份尺寸公式（历史上"量一遍、画一遍"对不上已经出过两次事故）；
- 列表页的可用高度、是否要滚动条、滚动位置夹紧一律走 `dglabListPageLayout()` /
  `dglabListPageScrolls()` / `dglabListPageScrollBar()`：内容高过一屏就自动出滚动条，
  排得进就没有。不要在各屏自己写"内容高 - 视口高"这套算术；
- 行类型只有三种：`DglabRow_Item`（左标签右值，可聚焦）、`DglabRow_Note`（灰色小字说明，
  缩进、带 ◆）、`DglabRow_Paragraph`（页面自己的白色正文，24px，无 ◆，可换行）。
  **界面里没有滑块**，数值一律用文字显示；
- 行以外的控件（二维码、日志正文）由屏幕自己按列/按行距画在内容区里，位置必须来自
  `dglabListMeasure` 之类的实测结果，不要另写常数；
- 字号只能用 `text.h` 里的五个（`DGLAB_TEXT_TITLE` 28 / `BODY` 24 / `VALUE` 22 /
  `ICON` 20 / `NOTE` 18），由 `DglabFontSet` 一起传给屏幕。不要在屏幕里挑新字号，也不要
  假设只有一个字体实例；
- 按键提示只能是"**按键图标 + 动作文字**"（`dglab/ui/button.h` + `dglab/ui/page.h` 的
  `DglabHint`，一个提示可以带两个图标，例如 ZL+ZR、←+→）。**按键名不进 `lang/`**：
  图标不是可翻译内容，`lang/` 里只放"返回""启动服务端"这类动作；
  新动作 = `strings.h` 加 `DglabString_Action*`；
- 按键图标一律"**实心形状 + 字母挖空**"（字母用页面背景色画进实心盘里），不要画细圆环；
  字母用 `DGLAB_TEXT_ICON`(20px) 并**按字形墨迹盒居中**（`dglabTextInkTop`/`InkHeight`）：
  用行字号或按 line box 居中都会让字母顶到 26px 的图标框边（docs/nro-ui.md 有实测）；
  `dglabHintDraw`/`dglabPageHints` 因此要显式传两个字体（图标一个、动作文字一个）；
- 退出规则：**Console 页（BLE PoC console、`showNotice`、`showStartupNotice`）用 `+`**，
  **其余 framebuffer 页面一律 `B`**，`+` 在这些页面不响应。改按键语义必须同时改底栏提示，
  两者不一致比没有提示更糟；
- 需要多于一屏内容的页面必须给出滚动方式：有光标的页面用光标驱动，无光标的页面用
  专门的滚动键（日志子页、About 页用 ↑↓）。**滚动提示不进底栏**：内容超过一屏时右缘会
  出现滚动条，那就是提示，底栏再写一条 `↑↓ 滚动` 只是重复（2026-09-17 需求）；
  **无光标又无滚动键的页面（连接测试、体感）必须把内容排进一屏**，`tests/canvas` 的
  "内容不贴底"检查会守住这一条；
- 日志子页的一次按键 = 一行（`DGLAB_SCREEN_LOG_PITCH`），连发用 `LOG_SCROLL_*` 自己的时序，
  不要复用体感强度的连发时序；
- 二维码只在服务端 `Listening`/`Paired` 时显示（`screen.c` 的 `qrCode()`）；
  `NET_QR` 在只有局域网地址时也会成功，不要用它的成败当作"服务端在运行"；
- 布局坐标一律是 720p 逻辑单位。底座 1080p 由 `platform/framebuffer.c` 建 1920×1080 的
  framebuffer + canvas 层的 `scale_num/scale_den` 完成，**屏幕代码不得自己乘比例**；
  缩放会把逻辑坐标按边换算，测量函数返回的仍然是逻辑单位；
- 重建 framebuffer（底座切换、进出 BLE PoC 控制台）必须走 `main.c` 的
  `appDisplaySuspend()` / `appDisplayReopen()`：它同时重开共享字体并按当前 scale 重新光栅化。
  重建后靠 `g_display_generation` 强制每一屏重画一帧；
- **重绘判定必须包含所有会改变画面的输入**（光标、滚动位置、语言、状态、generation…）：
  About 页曾经漏掉语言，按 ←/→ 改了语言却不重画，要等下一次按 ↑/↓ 带动滚动位置才显示出来
  （看起来像按键串了）；
- **按键要用的状态必须来自本页当帧拿到的值**，不要用只有别的页面才更新的全局：体感页的 A
  启停曾经读一个只有 socket 页写的全局，于是底栏写着"停止"、按键却发 `NET_START`
  （sysmodule 对已运行的服务端直接返回成功、什么都不做），服务端因此停不掉；
- 新增或修改屏幕后跑 `make -C tests/canvas`：`testEveryPageStaysInItsRegions` 会用
  两份语言文件把每一屏渲染一遍，检查没有任何像素画到页头/内容列/底栏/滚动条之外
  （原委见 `docs/nro-ui.md`）。

### 文案（必须遵守）

- NRO 自绘界面的文字一律来自 `lang/*.json`，屏幕代码只写
  `dglabString(DglabString_...)`，不要在 C 代码里放界面字面量。新增一条文案 =
  `nro/include/dglab/ui/strings.h` 加枚举 + `nro/source/ui/strings.c` 的 `kKeys[]` 加 key
  + `lang/en.json` 与 `lang/zh-Hans.json` 各加一条；`tests/lang` 会检查三方对得上；
- `lang/` 在仓库根目录（与 `nro/` 同级），`make -C nro package` 复制到
  `release/DGLAB-NX/lang/`。它是**运行期**读的：必须和 `DGLAB-NX.nro` 一起装到
  `SD:/switch/DGLAB-NX/`，否则 NRO 启动即打印错误退出（有任意一个语言文件可用就继续
  启动，缺的 key 回落到已加载的语言）；
- console 输出（BLE PoC 控制台、启动失败提示）保持 ASCII 英文：libnx 的 console 用的是
  256 字形位图字体，画不出汉字；sysmodule 的日志行同样不进 `lang/`；
- 改文案不用重新编译 NRO，但排版按字体实测宽度算，改完要跑 `make -C tests/canvas` 和
  `make -C tests/lang`。

### SD 卡目录（必须遵守）

NRO 的运行期文件只有一处，按用途分层，路径写在 `nro/source/main.c` 顶部：

    SD:/switch/DGLAB-NX/DGLAB-NX.nro
    SD:/switch/DGLAB-NX/lang/     界面文案（`DGLAB_LANG_DIR`，缺失即启动报错）
    SD:/switch/DGLAB-NX/config/   app.cfg、motion.cfg、dglab-ble-address.txt
    SD:/switch/DGLAB-NX/logs/     dglab-net.log、dglab-ble-poc.log（sysmodule 写 dglab-sys.log）

- 新增设置文件放 `config/`，新增日志放 `logs/`，不要在 `DGLAB-NX/` 根目录或 SD 根目录
  直接写文件（`config/`、`logs/` 由 NRO 启动时创建，sysmodule 自己创建 `logs/`）；
- 不要在别处再找 `lang/`：`langfiles.c` 只读 `DGLAB_LANG_DIR` 一处。

## NRO Identity

普通 NRO 不需要像 NSP/NCA 那样分配 Nintendo Title ID，
也不使用 Atmosphère `contents/<titleid>/` 安装结构。

但是 NRO 可以包含 NACP metadata，其中的
PresenceGroupId / SaveDataOwnerId 等字段可能被 loader 或模拟器
用作 ProgramId。

因此：

- 不要将 NRO 的 metadata identity 与 Sysmodule Title ID 混淆；
- 如果项目包含多个 NRO，它们应使用独立且稳定的 metadata identity；
- 不要让多个需要被独立识别的 NRO 无意中共享同一个 ProgramId；
- 如果需要兼容 Ryujinx，应验证多个 NRO 能否同时被正确识别。

## 元信息与版本

NACP 里的应用名、作者与发行版本由 `nro/Makefile` 生成：应用名 `DGLAB-NX`、作者
`livcm`、版本来自仓库根 `VERSION`。

- **发行版本只有一个来源**：仓库根 `VERSION`。根 `Makefile` 读它并作为 `APP_VERSION`
  传给 `nro/Makefile`，`make -C nro` 也读同一个文件；不要在 Makefile、代码或文档里
  再写一个版本号（`nro/Makefile` 在 `switch_rules` 之前读它，就是为了不让 libnx 的
  `1.0.0` 默认值悄悄变成发行版本）；
- 这个号同时进 NACP 和 About 页的「应用版本」行（`-DDGLAB_APP_VERSION`，默认值见
  `nro/include/dglab/nro/version.h`），所以改 `VERSION` 之后必须重新构建 NRO；
- **IPC 接口版本不是它**：`DGLAB_IPC_PROTOCOL_VERSION`（`common/include/dglab/ipc.h`）
  由 sysmodule 通过 `GET_VERSION` 报告，About 页用单独一行显示（`docs/ipc.md` 的
  “版本”）——两个版本号在同一页上，混用就是把 sysmodule 的接口版本当成 NRO 的发行版；
- About 页的构建标识是 `git describe --always --dirty`（`-DDGLAB_BUILD_STAMP`），
  回答“卡上的是不是我刚编的那一份”，做法与 sysmodule 的 `dglab/build.h` 相同；
- 图标是 `nro/DGLAB-NX.jpg`（256×256 JPEG，配色同界面主题）。重画用
  `nro/tools/make_icon.c`，文件头写着编译与转换的两条命令。

## 构建

修改 NRO 源代码、headers、IPC 定义、Makefile 或 linker 配置后，应至少执行：

    make -C nro

并说明实际执行的验证结果。

输出文件名应为：

    DGLAB-NX.nro
