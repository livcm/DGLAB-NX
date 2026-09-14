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

## framebuffer → deko3d 迁移评估

**结论：不难，但取决于"搬到哪一层"。** libnx 的 framebuffer 与 deko3d 都用同一个
`nwindowGetDefault()`，所以窗口、输入、主循环这些代码完全不变；变的只是"像素怎么送上屏"。

| 路线 | 做法 | 一次性工作量 | 绘制代码 | 适合 |
| --- | --- | --- | --- | --- |
| A. 保留 CPU 光栅化 | deko3d 里建 PitchLinear 图像，CPU 直接写像素，再 blit/全屏 quad 到 swapchain | 小 | **不用改** | 我们的界面（静态二维码 + 文字 + 少量控件） |
| B. 半 GPU | 二维码/位图当纹理（CPU 生成一次），矩形与文字用 quad 批绘 | 中 | 部分重写 | 需要频繁重绘或动画 |
| C. 全 GPU / 成品 UI | 字体图集、精灵批、动画、vsync 管理；或直接用 Borealis | 大 | 重写绘制层 | 追求观感的正式界面 |

### 核对过的关键事实

- deko3d 随 libnx 提供；最小骨架见 `examples/switch/graphics/deko3d/deko_basic`
  （233 行：device → mem block → image → swapchain → command list → present）
- 图像布局可选 `DkImageFlags_PitchLinear`（线性）或默认的 BlockLinear；
  **要 CPU 直接写像素必须显式选线性**（BlockLinear 需要 swizzle）
- `dkMemBlockGetCpuAddr()` + `dkMemBlockFlushCpuCache()` 提供了"CPU 写、GPU 读"的
  正规路径，不需要自己做缓存魔术
- libnx 的 console 是**可插拔渲染器**（`ConsoleRenderer` 的
  init/deinit/drawChar/scrollWindow/flushAndSwap 回调）。官方示例里已经有现成的
  deko3d 版渲染器（`examples/switch/graphics/deko3d/deko_console/source/gpu_console.c`，
  486 行）和 OpenGL 版（`opengl/gpu_console`，467 行）——**纯文字界面迁移成本很低**
- 待实测的一点：swapchain 的图像是否接受 PitchLinear；若不行，就退化成
  "CPU 写纹理 + blit 到 swapchain"，仍然属于路线 A

### 建议

现在就把 NRO 的 UI 写成三层：

    内容/布局（QR、状态、按钮） → canvas 抽象（fillRect / drawText / drawBitmap / present） → 后端

后端先实现 libnx framebuffer。这样将来换 deko3d 只是"再实现一个 canvas 后端"，
UI 逻辑一行都不用动。按这个结构估计：路线 A 约半天，路线 B 约 1~2 天，路线 C 是另立项目。

---

## 实现记录（优先级 5.4）

按上面的建议落地了三层结构，文件如下：

| 文件 | 层 | 说明 |
| --- | --- | --- |
| `nro/source/ui/qr.c` | 内容 | QR 编码器（byte 模式、纠错等级 L/M/Q/H、版本 1..10，最大 57×57） |
| `nro/source/ui/canvas.c` | canvas | 纯 RGBA8888 缓冲区的矩形、位图字体、二维码绘制，带裁剪 |
| `nro/source/ui/screen.c` | 内容/布局 | 标题、服务器状态、二维码、日志、按键提示的排版 |
| `nro/source/platform/framebuffer.c` | 后端 | libnx framebuffer 的 begin/end 与字体来源 |
| `nro/source/main.c` | 应用 | IPC 轮询、按键处理、视图切换 |

`nro/source/ui` 全部与 libnx 无关，因此界面可以在电脑上渲染检查。

### 二维码编码器是怎么验证的

QR 编码器是自己写的（devkitPro 里没有可用 QR 库），所以它必须被独立实现验证，
而不是"看起来画对了"：

- 参考实现用 macOS CoreImage 的 `CIQRCodeGenerator`，工具在
  `tests/qr/tools/gen_reference.m`；
- 生成的矩阵放在 `tests/qr/golden/`，测试逐模块比对（版本、纠错分块、掩码选择、
  模块布局任何一处错了都会 diff 出来）；
- 约定：参考实现返回的符号带 1 模块静区，生成器会去掉；样例必须是**纯小写、
  无数字**的文本，否则 CoreImage 会做混合模式分段（numeric/alphanumeric），
  那是本项目不会做的优化。

本项目只实现 byte 模式，带数字的地址（例如真实的
`ws://192.168.1.161:9999/<uuid>`）同样能编码，只是**可能比 Apple 的编码器大一个
版本**（45×45 而不是 41×41）。这只是占用更多屏幕，不影响可扫。

工作过程中被这些测试抓到的真实 bug：格式信息两块的位置写反（等价于转置）、
版本 1 被当成有校正图案（越界写内存）、暗模块被格式信息保留区覆盖、以及信封构造
没有转义 `pulse` 命令里的引号。

### 字体

文本用 libnx 自带的那张 16×16 点阵字体（`default_font_bin`），没有引入
freetype 之类的依赖：

- 该符号没有公开声明，只在 `console.h` 的注释里出现，因此代码里显式 `extern`；
  `nm libnx.a` 显示它是 0x2000 字节（256 个字形 × 32 字节），与 16×16 一致；
- 位序：每行 2 字节，**一行是一个小端 16 位值，最高位是最左边的像素**，也就是
  列 `c` 取该行的第 `tile_width - 1 - c` 位。这与 libnx 自己的 console 渲染器
  （`ConsoleSwRenderer_drawChar` 从 `0x8000` 开始逐位右移）一致。

  这一条踩过坑：第一版按"行内 LSB 优先"实现，结果**整个界面的字形左右镜像**
  （排版正常、只有字反了），是实机测试才发现的。当时我抽字体数据渲染字形时把
  镜像的 `L`/`r` 误读成了正常字形——字形是逐像素画出来的，判断方向必须真的看图
  （`tests/canvas/tools/render_preview.c` 现在就是干这个用的），不能只看符号。
  `tests/canvas` 里有一个合成字体的回归测试，字形画反会直接失败。

### 界面内容与按键

- 服务器状态：状态、局域网地址、控制器 uuid、App uuid、连接/指令/上报计数、
  App 上报的强度与上限、最近一次协议错误；端口号显示在标题栏（值那一列只有 21 个
  字符宽，`192.168.1.161:9999` 正好占满）；
- 右侧：二维码 + 其内容（自动换行，模块大小按剩余高度自适应）；
- 下方：sysmodule 日志（`NET_LOG` 增量读取），同时写入
  `sdmc:/switch/DGLAB-NX/dglab-net.log`，方便测试后把文件发回来；
- 按键提示分两行（`A start`/`Y stop`/`X strength`/`B clear` 一行，`ZL pulse`/
  `L/R value`/`- BLE poc`/`+ exit` 一行）：一行放不下，之前会把最后的 `+ exit`
  挤出屏幕；
- 按键：`A` 启动服务端、`Y` 停止、`X` 测试强度、`B` 清空波形、`ZL` 测试波形、
  `L`/`R` 调整测试强度、`-` 打开 BLE PoC 控制台视图、`+` 退出。

BLE PoC 的控制台视图挪到了 `nro/source/ble_poc_view.c`：它需要独占屏幕和 console，
所以进入前会释放 framebuffer，退出后再建。

### 主机侧检查

```
make -C tests/qr        # QR 与 CoreImage 参考矩阵逐模块比对
make -C tests/canvas    # canvas 裁剪/字体位序/二维码绘制/整屏排版
```

排版还能直接渲染成图片查看：

```
tests/canvas/tools/render_preview.c   # 用法见文件头部注释
```

### 实机待确认

1. framebuffer 与 console 视图来回切换（`framebufferClose` → `consoleInit` →
   `consoleExit` → `framebufferCreate`）是否稳定；
2. 二维码在真实屏幕上以 8~10 像素/模块渲染时的扫码成功率；
3. 手柄按键提示是否符合实际使用习惯；
4. 1080p dock 模式下 `nwindowGetDefault()` 的分辨率（当前按 720p 固定布局）。

如果没有 sysmodule，或者 framebuffer 建不起来，NRO **不使用 framebuffer**，而是走
console（`consoleInit` + `printf` + 等 `+` 退出）打印服务名、`smGetService` 的返回值
与安装路径。这样做的原因来自实机反馈：console 是本项目里**已验证能在真机上正常显示**
的渲染路径（`-` 键那个视图），而 framebuffer 路径曾经给出过黑屏；错误信息再重要也
不能依赖一条还没被实机确认过的路径。
