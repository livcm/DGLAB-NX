# NRO UI 方案调研

本文记录在 Switch 上为 NRO 做界面有哪些可选方式、各自依赖与代价，以及本项目的取舍。
（调研时间：2026-09-14，环境：devkitPro + libnx + HOS 22.5.0）

## 结论速览

| 方案 | 额外依赖 | 本机是否可用 | 工作量 | 适合场景 |
| --- | --- | --- | --- | --- |
| libnx console（字符终端） | 无（`-lnx`） | ✅ 已用 | 最低 | 状态、日志、文字菜单 |
| libnx framebuffer（自绘像素） | 无（`-lnx`） | ✅ | 低～中 | 自绘界面、二维码、简单图形 |
| deko3d | libnx 自带 `libdeko3d.a` | ✅ | 高 | 正式的高性能界面、动画 |
| OpenGL ES / EGL | Mesa（`glapi`/`EGL`）、`glad` | ❌ 未安装 | 中～高 | 复用现成 GL 代码、ImGui 后端 |
| SDL2（+ttf/image/mixer） | `switch-sdl2*` portlibs | ❌ 未安装 | 中 | 2D 绘制、输入、音频、文字渲染 |
| Borealis | GLFW + Mesa + GLM，C++17，ROMFS | ❌ 未安装 | 低（用现成组件） | 接近 HOS 观感的完整应用界面 |
| Dear ImGui | 需要渲染后端（SDL2+GL 或 deko3d） | ❌ 需后端 | 中 | 调试面板、参数调节工具 |

"本机是否可用"是按当前 `/opt/devkitpro` 实际安装情况核对的：
`portlibs/switch/` 下只有 `bin/`，**没有任何 portlibs 库**；`libdeko3d.a` 随 libnx 提供。

## 逐项说明

### 1. libnx console（当前 PoC 使用的方案）

- API：`consoleInit` / `consoleUpdate` / `consoleClear` / `consoleExit`，`console.h`
- 字体：默认 `default_font_bin`，8×8 像素、256 个字符（CP437 风格，含块字符 █▀▄），
  可用 `consoleSetFont()` 换成自定义字体
- 支持 ANSI 转义：颜色、光标定位、清屏（本项目的 PoC 日志界面就是这么做的）
- 限制：固定字符网格，不能像素级绘制。想用它显示二维码只能"一个字符一个模块"，
  而字符在屏幕上不是严格正方形（8×8 像素 + 字符间距），扫码成功率没有保障
- 适合：状态显示、日志、简单按键提示

### 2. libnx framebuffer（推荐用于本项目的 QR 与后续 UI）

- API：`nwindowGetDefault()` + `framebufferCreate` / `framebufferMakeLinear`（`switch/display/framebuffer.h`）
- 示例：`/opt/devkitpro/examples/switch/graphics/simplegfx`
- 直接写 RGBA8888 像素：**二维码可以画成严格正方形**（含静区/边距），扫码可靠
- 文本需要自己解决，三种做法：
  1. 自带一张 8×8 或 16×16 位图字体表（几百字节，零依赖，最省事）；
  2. `pl` 服务取系统共享字体（`plGetSharedFontByType`）+ `stb_truetype` 光栅化；
  3. 引入 freetype portlib（依赖最重，但字形质量最好）
- 适合：本项目下一步的 NRO 界面（二维码 + 状态 + 少量按钮/列表）

### 3. deko3d

- libnx 自带：`-ldeko3d`（调试版 `-ldeko3dd`），示例在
  `/opt/devkitpro/examples/switch/graphics/deko3d/`（`deko_basic`、`deko_console` 等）
- Nintendo Switch 原生的现代 GPU API：命令缓冲、DKSH shader、swapchain，性能与
  控制力最好
- 代价：要自己写 shader 与管线；入门成本在本文列出的方案里最高
- 适合：将来要做流畅动画、大量图形元素的正式界面

### 4. OpenGL ES / EGL

- 示例：`/opt/devkitpro/examples/switch/graphics/opengl/*`，
  链接 `-lEGL -lglapi -ldrm_nouveau -lglad`
- 需要 Mesa（`glapi`/`EGL`）与 `glad` portlibs，本机没有安装
- 适合：复用现成的 GL 渲染代码，或作为 ImGui 的后端

### 5. SDL2

- 示例：`/opt/devkitpro/examples/switch/graphics/sdl2/{sdl2-simple,sdl2-demo}`；
  `sdl2-demo` 用到 `SDL2_mixer`、`SDL2_image`、`SDL2_ttf`
- 需要 `switch-sdl2` 系列 portlibs，本机没有安装
- 适合：需要 2D 绘制 + 输入 + 音频 + 文字渲染的常规应用

### 6. Borealis（Switch 风格的成品 UI 库）

- 定位：hardware accelerated、Nintendo Switch inspired UI library（同时支持 PC 与
  Switch），基于 nanovg，作者 natinusala
- Switch 侧依赖：`switch-glfw`、`switch-mesa`、`switch-glm`；需要 C++17，并且要移除
  `-fno-rtti` / `-fno-exceptions`；资源通过 ROMFS（`romfs:/`），集成方式是在 Makefile 里
  `include borealis.mk` 并设置 `BOREALIS_PATH` / `BOREALIS_RESOURCES`
- 优点：现成的视图、主题、动画、手柄与触摸输入，观感接近 HOS
- 缺点：依赖重（GLFW + Mesa + GLM + C++ 运行时）、仍处于早期开发、许可需按仓库
  `LICENSE` 确认
- 适合：确实要做"产品级"应用界面时

### 7. Dear ImGui

- 立即模式 GUI，本身没有平台后端；Switch 上通常配 SDL2+OpenGL3，或配 deko3d 后端
- 因此同样受 portlibs 限制；适合调试面板、参数调节、开发者工具，不太适合当产品 UI

### 8. Tesla / Ultrahand overlay（未来的 overlay 组件）

- overlay 与 NRO 是两套渲染环境：Tesla/Ultrahand 生态有自己的轻量渲染层（libtesla 等），
  与 NRO 的窗口渲染不通用
- 等做到 `overlay/` 那一步再单独调研，本文不做结论

## 本项目的建议

**短期（当前阶段，二维码是硬需求）**：用 **libnx framebuffer 自绘**。
理由：零额外依赖、二维码能画成严格正方形（扫码可靠）、后续要加按钮/列表也有足够控制力；
文本先用一张自带位图字体表解决，避免引入 portlibs。

**中期**：

- 若要做成套、观感接近 HOS 的界面 → 评估 Borealis（省事，但依赖与 C++ 约束较重）；
- 若只做开发者调试面板 → SDL2 + ImGui；
- 若想要最强控制力又不加依赖 → 继续用 framebuffer 自绘，或迁移到 deko3d。

**当前不引入任何 portlibs 依赖**：安装 portlibs 会写入 `/opt/devkitpro` 并且需要联网，
而且会让构建对开发环境产生新的要求，应当在确实需要时再决定。

## 将来若要安装依赖

    dkp-pacman -S switch-sdl2 switch-sdl2_ttf switch-sdl2_image switch-mesa \
        switch-glfw switch-glm switch-freetype

（需要联网与写入 `/opt/devkitpro` 的授权；本条只是记录，当前不执行。）

## 待办与决策点

1. 二维码渲染：framebuffer 自绘（推荐）——落地时需要一个精简 QR 编码器；
2. 文本方案：自带位图字体表（推荐起步）或 stb_truetype + 共享字体；
3. 是否接受引入 C++17 与 portlibs —— 决定 Borealis / ImGui 是否进入候选；
4. overlay 的 UI 方案另行调研。
