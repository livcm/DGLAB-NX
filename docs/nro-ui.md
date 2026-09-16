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

1. ~~二维码渲染~~、~~文本方案~~：都已落地——二维码是 `nro/source/ui/qr.c` 里的自研
   编码器；中文文本方案见下面「本地化（简中 / 英文）」（`pl` 系统共享字体 +
   `stb_truetype`，位图字体只留给 console）；
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

### 2026-09-15 复核：结论是"先不做"

分层建议已经落地（`nro/source/ui/canvas.c` 是绘制层，`nro/source/platform/framebuffer.c`
是后端），所以真要迁的时候成本比上面估的还低。但复核后**当前不值得做**：

| 想要的收益 | deko3d 能给吗 | 现有方案够不够 |
| --- | --- | --- |
| 省 CPU | 能（GPU 合成） | 已解决：界面改成按需重绘，状态不变就不 Begin/End |
| 双缓冲 / vsync | 能 | libnx 的 `framebufferCreate` 已是 2 缓冲 + `framebufferEnd` 提交 |
| dock 1080p 清晰度 | 能 | 未定：`framebufferCreate` 能否直接开 1920×1080 还没实测（见"实机待确认"第 4 条）；若可行，缺的只是布局缩放，与渲染后端无关 |
| 动画 / 实时曲线 | 能（着色器、批绘） | 目前没有需求：静态文字 + 一个二维码 |
| 文字渲染 | **不能**：deko3d 是 GPU API，字体光栅化仍要自己解决 | 现成的 16×16 位图字体够用 |

复核时实测了工具链，确认"要做就能做"（这是成本证据，不是该做的理由）：

- `deko3d 0.5.0` 已安装：头文件 `libnx/include/deko3d.h`，库 `libnx/lib/libdeko3d.a`
  （另有 `libdeko3dd.a`）；
- 着色器编译器是 `uam -s vert|frag …`（在 `tools/bin`），实测能把示例的 `.glsl`
  编成 `.dksh`；
- 官方示例 `examples/switch/graphics/deko3d/deko_basic` 在本机**完整构建通过**
  （device → linear image → swapchain → RomFS(shaders) → `.nro`）；
- `dkCmdBufCopyBufferToImage` 与 `dkCmdBufBlitImage`（带线性过滤标志）都在
  `deko3d.h` 里，所以"canvas 上传成纹理再缩放到 swapchain"这条**不用写着色器**；
- 示例的 `graphicsInitialize()` 没有额外的服务初始化（不需要手工 `nvInitialize`），
  窗口仍是 `nwindowGetDefault()`，主循环与输入代码不用改。

触发条件（满足任一条再启动；届时走路线 A：只加后端，`screen.c` 不动）：

1. 优先级 8 的 Joy-Con 传感器要在屏幕上显示**实时**波形/曲线；
2. dock 1080p 的清晰度或缩放成为实际痛点，而"canvas 按分辨率缩放"解决不了；
3. 需要成套动画/过渡，或要把位图字体换成矢量字体的观感。

优先级位置：**不插入 1–12 的编号**，当作第 9 项（基础 UI）之后的增强。顺序上建议在
第 8、9 项完成之后、第 10 项（Overlay）开工之前评估一次——"要不要 GPU"取决于前两项
最终要画什么；而 Overlay 是另一个进程里的另一套渲染，两者互不影响。

### 本地化（中文界面）与 deko3d 的关系

**不冲突，前提是 deko3d 只做"呈现"。** 中文界面需要的是：UTF-8 解码、有汉字的字形来源
（`pl` 系统共享字体）、字形缓存，以及按 24px 重排的布局。这些都在 canvas 这一层完成，
像素照旧写进内存；deko3d 将来只是把 canvas 当纹理上传再 blit（上面的路线 A），所以两边
互不干涉。

会冲突的是另一条路线：让 GPU 直接画每个字形（glyph atlas + 着色器往 swapchain 画）。
那样文本渲染要和后端绑在一起，等于每个后端各写一套，主机上的排版预览也会失效。

顺序上建议**先本地化、后 deko3d**（如果 deko3d 真要做）：

1. 本地化会把文本层重构成"文本 → 字形缓存 → 画进 canvas"，这套在路线 A 下原样可用
   （canvas 就是那张要上传的纹理），等于顺手产出 deko3d 路线 B/C 需要的字形图集；
2. 反过来先换后端再本地化，就把"渲染路径变更"和"排版重算"两件高风险的事叠在了一起；
3. 现在 deko3d 的结论仍然是"先不做"，本地化不改变它——字体光栅化是 CPU 的活，GPU 帮不上。

## 本地化（简中 / 英文）

2026-09-16 定的方案。**顺序**：先把体感玩法的参数调稳定 → 做本地化（先菜单、参数页、
体感玩法这三屏）→ 之后再评估 deko3d。

### 语言包

- 只做 **简体中文 + 英文** 两套，但结构上按"key → 文案"存，语言是个枚举；加第三种语言
  是加一条枚举加一个 `lang/<code>.json`，不动界面代码（见下一节）；
- 语言按系统设置自动选（`set:sys` 的 `SetLanguage_ZHCN`，见 libnx `set.h` 的
  `SetLanguage_*`），取不到或不是中文就用英文；
- 界面文案不在 C 代码里：各个 `screen/menu/motion/advanced.c` 只引用 key，文案在 SD 卡上
  的 JSON 里，改文案不需要重新编译 NRO。

### 语言文件（`lang/*.json`）

2026-09-16：文案从 C 数组搬到外部文件。仓库里的 `lang/` 与 `nro/` 同级，构建时复制到
`release/DGLAB-NX/lang/`，安装时和 `DGLAB-NX.nro` 一起进 `SD:/switch/DGLAB-NX/`
（见根 `AGENTS.md` 的发布产物布局）。

文件格式：

```json
{
  "_readme": "以 _ 开头的成员是注释，加载器忽略",
  "language": "en",
  "name": "English",
  "strings": {
    "menu_title": "DGLAB-NX   modes",
    "motion_title": "DGLAB-NX   motion (Joy-Con)"
  }
}
```

- 文件名就是语言码：`en.json` / `zh-Hans.json`，与 `dglabLanguageKey()` 的返回值一致；
  `language` 成员必须和文件名对得上（把成对的文件复制错会当场报错，而不是静默生效）；
- key 的清单是 `nro/include/dglab/ui/strings.h` 里的 `DglabString`，英文 key 名写在
  `nro/source/ui/strings.c` 的 `kKeys[]`（`menu_title`、`desc_deadzone_enter` …）。
  加一条文案 = 枚举加一项 + `kKeys[]` 加一行 + 两份 JSON 各加一条，`tests/lang` 会检查
  三方对得上；
- 加载器 `dglabStringsLoadJson()` 在 `nro/source/ui/strings.c`，JSON 读取器在
  `nro/source/util/json.c`。两者都不碰 libnx，所以 `tests/lang` 能在电脑上直接跑。

启动时**只**在 `sdmc:/switch/DGLAB-NX/lang/` 找（`DGLAB_LANG_DIR`，见
`nro/include/dglab/platform/langfiles.h`，读取代码在 `nro/source/platform/langfiles.c`）：
每个语言读 `<目录>/<语言码>.json`，没有别的候选目录，也没有"和 NRO 当前所在目录"这种
位置——NRO 的运行期文件统一在 `SD:/switch/DGLAB-NX/` 下分层：

    SD:/switch/DGLAB-NX/DGLAB-NX.nro
    SD:/switch/DGLAB-NX/lang/     界面文案（随发布一起拷，缺失即启动报错）
    SD:/switch/DGLAB-NX/config/   app.cfg、motion.cfg、dglab-ble-address.txt
    SD:/switch/DGLAB-NX/logs/     dglab-net.log、dglab-sys.log、dglab-ble-poc.log

`config/` 与 `logs/` 由 NRO 启动时创建（sysmodule 自己创建 `logs/`）；`lang/` 不创建，
里面必须有文件。

**有任意一个文件能用就正常启动**：另一套语言缺的 key 全部回落到已加载的语言，屏幕上不会
出现空标签。一个都读不出来时，NRO 在创建 framebuffer 之前用 console 打出英文错误页
（列出找过的目录和每个文件的具体问题），等 `+` 退出——这条路径不能依赖 JSON，所以它和
其它 console 输出一样是 ASCII 英文。

文件存在但不完整（缺 key、多出未知 key、同一个 key 写两遍）不是致命错误：缺的回落，
问题写进 `sdmc:/switch/DGLAB-NX/logs/dglab-net.log`，同时出现在连接测试页的日志面板里。

### 翻译范围与术语

除专有名词外全部翻译。**保留原文**：`DGLAB`/`DG-LAB`、`Joy-Con`、`sysmodule`、
`BLE`、`PoC`、`Socket`（作为协议名）、`App`、`sdmc:/...` 路径、按键名（`A`/`B`/`ZL`/
`+`/十字键）。

统一译名（避免以后同一个概念两种说法）：

| 英文 | 中文 |
| --- | --- |
| strength（概念是 channel strength；体感页的标签只有一格，写全了会撞到数值） | 通道强度 |
| waveform strength / level | 波形值（数值语境）／波形强度（概念语境） |
| dead zone enter / exit | 死区进入 / 死区退出 |
| gyro range / accel range | 陀螺仪量程 / 加速度量程 |
| attack / release | 上升时间 / 释放时间 |
| idle stop | 静止停发延时 |
| frequency fast / still | 最快频率 / 静止频率 |
| strength max | 波形强度上限 |
| left / right Joy-Con | 左 / 右 Joy-Con（体感页的输入行；**不要**译成"通道 A/B"） |
| mode | 模式（菜单里的四项） |

菜单四项定为：**连接测试**（原来的 Socket test）、**体感（Joy-Con）**、
**高级参数**、**BLE PoC 控制台**。

### 例外：console 视图保持英文

BLE PoC 控制台和启动失败提示走的是 libnx 的 `console`（`consoleInit` + `printf`），
它用的是和 canvas 同一套 256 字形位图字体，**没有汉字**，除非换自定义字体。这部分
明确不投入本地化工作，保持英文（`-` 键那个入口现在也在菜单里，用户平时不会碰到）；
sysmodule 的日志行同理（另一进程 + 落盘日志，供排查用）。所以"界面文案进 JSON"指的是
framebuffer 自己画的那几屏。

### 渲染前提

中文要能显示，得先做前面说的渲染层改造：UTF-8 解码 + `pl` 系统共享字体 +
`stb_truetype` 光栅化 + 字形缓存 + 按 24px 重排布局（现在全部按 16px 硬编码）。
主机上的 `tests/canvas` 预览要换成同一套文本层，否则排版回归失去意义。

方案定稿前用系统字体（CoreText，24px）出过三张示意：菜单、体感、高级参数（只用于评审，
不提交二进制图片）。据此确认的排版规则：正文 24px、行距 34px、菜单行高 44px、参数页行高
32px、中文按字符数折行（无空格），面板宽度沿用现在的值（560 / 720 / 760）。

### 字形光栅化：默认 1-bit（保持点阵观感）

`stb_truetype` 光栅化出来是 8-bit 覆盖度，怎么用由我们决定：

- **阈值化成 1-bit（默认选它）**：硬边、块状，和现在英文界面的位图字体是同一路观感；
  而且 canvas 目前只有 `dglabCanvasFill`（直接写实色像素，**没有 alpha 混合**），1-bit 能
  直接复用，不必新增绘制原语；
- 保留灰度抗锯齿：更平滑，但要在 canvas 里加一个混合原语，而且和现有界面观感割裂。

已知代价：把为矢量优化的字体在 24×24 上硬阈值化，笔画密的字可能出现粘连或丢笔画（经典的
点阵中文字体是逐字手调的，我们拿不到这种）。所以实现顺序是：

1. 先按 1-bit、24px 做出来；
2. 生成一张"界面里用到的每个字"的对照图，逐字检查；
3. 糊的字多就把字号提到 32px（面板与行高按 32px 重排）；
4. 仍然不行再考虑路线 B（带授权的点阵中文字体）。

细节：阈值和落点都要对齐到整数像素——1px 的方块落在小数坐标上会被抗锯齿糊回去，这一点
在出示意图时就踩到过一次。

---

## 实现记录（优先级 5.4）

按上面的建议落地了三层结构，文件如下：

| 文件 | 层 | 说明 |
| --- | --- | --- |
| `nro/source/ui/qr.c` | 内容 | QR 编码器（byte 模式、纠错等级 L/M/Q/H、版本 1..10，最大 57×57） |
| `nro/source/ui/canvas.c` | canvas | 纯 RGBA8888 缓冲区的矩形、位图字体、二维码绘制，带裁剪 |
| `nro/source/ui/page.c` | 内容/布局 | 页头、标题线、内容列、底栏按键提示（HOS 风格页面的外壳） |
| `nro/source/ui/list.c` | 内容/布局 | 行、分隔线、聚焦框、滑块、滚动与滚动条 |
| `nro/source/ui/button.c` | 内容/布局 | 手柄按键图标（A/B/X/Y、L/R/ZL/ZR、±、方向键） |
| `nro/source/ui/screen.c` | 内容/布局 | 服务端页的行、二维码块、日志块 |
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

- 启动后先进入**菜单**（`nro/source/ui/menu.c`）：三行玩法（`Socket test`、`Motion
  (Joy-Con)`、`Advanced (motion)`、`BLE PoC console`）+ 选中项的说明；标题栏右侧显示 sysmodule 是否还答
  `PING`（每个玩法都依赖它，进去才发现要失败就白跑一趟）。`D-pad` 上下选择、`A` 进入、
  `+` 退出；进入玩法后 `+` 返回菜单。**`B` 不做返回键**：测试屏里它是"清空波形"；
  原先绑在 `-` 上的 BLE PoC 视图挪成了菜单项；
- `Advanced (motion)`（`nro/source/ui/advanced.c`）把体感玩法的全部参数放在一页上：
  `D-pad` 上下选参数、左右改值（**按一下只走一格**，按住 0.5 秒后才开始连发、每 0.2 秒
  一格——参数改飞了没法靠反向点一下找回来）、`Y` 恢复默认、`+` 保存返回。每次改动都写进
  `sdmc:/switch/DGLAB-NX/config/motion.cfg`，体感玩法进入时读取；参数清单见
  `docs/joycon-input.md`；
- `Motion (Joy-Con)` 玩法（`nro/source/ui/motion.c`）见 `docs/joycon-input.md`：显示
  左右通道的连接状态、实时强度与频率、作为"音量"的通道强度、链路状态与最近一次上传
  结果；
- 服务器状态：状态、局域网地址、控制器 uuid、App uuid、连接/指令/上报计数、
  App 上报的强度与上限、最近一次协议错误；端口号显示在标题栏（值那一列只有 21 个
  字符宽，`192.168.1.161:9999` 正好占满）；
- `last cmd` 行是最近一次按键命令的结果（`A test  ok`、`clear  no app bound`、
  `A test  ok (A is 0)`，失败为红色、空通道为黄色）：sysmodule 是设备链路的唯一
  所有者，命令没送出去时那边没有任何日志，这一行是唯一能看到原因的地方。
  以前所有按键的 `Result` 都被丢掉，"按了没反应"和"按成功了"在屏幕上完全一样；
- App 的 `feedback` 字段（`DGLAB_NET_FEEDBACK_*`）不再单独占一行：3.0 App 是单向的，
  它永远是 `none`。字段仍在 `NET_STATUS` 里，将来有 App 上报再恢复那一行；
- 右侧：二维码 + 其内容（自动换行，模块大小按剩余高度自适应）；
- 下方：sysmodule 日志（`NET_LOG` 增量读取），同时写入
  `sdmc:/switch/DGLAB-NX/logs/dglab-net.log`，方便测试后把文件发回来；
- 重绘策略：状态、二维码、强度和 `last cmd` 打包成快照，只有快照变化才
  `framebufferBegin/End`（整屏 1280×720 + 二维码以前是每帧都画）；日志轮询也从每帧
  降到每 3 帧一次（约 20Hz），SD 卡上的日志文件不受影响；
- 按键提示分两行（`A start`/`Y stop`/`B clear`/`ZL`+`ZR` 两个测试键一行，
  `D-pad` 调强度/`- BLE poc`/`+ exit` 一行）：一行放不下，之前会把最后的 `+ exit`
  挤出屏幕；
- 按键：`A` 启动服务端、`Y` 停止、`B` 清空波形、`ZL` 测试通道 A、`ZR` 测试通道 B、
  `↑`/`↓` 调通道 A 强度、`→`/`←` 调通道 B 强度（改完立刻发给 App；按住约 0.4 秒后
  才以每 0.1 秒一步连发——早先是按住 6 帧（约 100ms）就连发，而正常按键时长就在这个
  量级，导致"按一下"经常走两格）、`+` 回菜单；
- 两个通道的强度各自独立、默认都是 0，并排在 `strength` 行显示（`A n/100  B n/100`）；
  上一行 `app report` 是 App 自己上报的强度与上限，3.0 单向所以显示 `none`；
  服务端那栏的值列从"最宽的标签"之后开始（标签宽度按实际字体量），`A 100/100  B
  100/100` 是它要装下的最宽一行。

### 面板：按内容量高度，内容裁剪到框内

2026-09-16 实机截图暴露的问题：面板高度、标签列宽都是**按 16px 位图字体写死的常数**，
换成 24px 系统字体后行高从 16 变成 34，于是「服务端运行时不要休眠」的字形下沿压在
服务端面板的下框线上、日志行的尾部画到二维码那栏、体感页的说明文字掉出面板、关于页
最后一行的 `Language` 压在下框线上，标题也因为没有用统一的居中算式而偏低。

现在的规则：

- 每个面板的高度由**要放进去的内容**用 `dglabTextCountLines` 量出来（服务端面板：标题 +
  8 行 + 休眠警告的换行数；体感页：标题 + 5 行 + 说明的换行数；关于页：一张行表量出来的
  总和），不再有按字体写死的高度；
- 标签列 / 值列按**当前字体的实测宽度**定位（`dglabTextWidth`），列宽不再按字符数假
  设。值列从"最宽标签 + 间距"开始，所以**标签必须短到能让最宽的那行值放在它右边**
  （体感页的英文标签因此是 `strength`，不是 `channel strength`；写成后者时要么撞上
  数值，要么数值被框线切掉，两种都被测试挡住）；
- 标题栏的标题在每一屏都用同一个算式 `(TITLE_HEIGHT - cell_height) / 2` 居中；
- `dglab/ui/panel.h` 是唯一的画框入口：先画底 + 框 + 标题，再用 `dglabPanelClip` 把裁剪
  区收到框内侧。所以即使某一屏量错了，文字也只会被框线切断，不会画到框外
  （`canvas` 的裁剪矩形是这一条的兜底保证）。

主机侧用 `tests/canvas` 的 `testNothingLeavesItsPanel` 守住这几点：它用**实机字体的度量**
（24px 字身、34px 行距、ASCII 半宽、汉字全宽）把五屏**在两种语言下各渲染一遍**，然后检查
①每条框线上没有别的颜色（文字没压到框线）、②框内侧第一圈没有别的颜色（文字没被切）、
③面板之外没有文字（标题栏、面板下方的安全提示与页脚除外）、④关于页标题与体感页标题
在同一行、⑤体感页的标签加最宽的数值仍放得下（英文写 `channel strength` 就会失败）、
⑥面板下方的安全提示能在面板与页脚之间排完。这几条正是上面那些截图里的问题：拿旧代码
跑这套测试会全部失败。

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
4. ~~1080p dock 模式下 `nwindowGetDefault()` 的分辨率~~：现在底座会显式
   `nwindowSetDimensions(1920, 1080)` 并按 1.5 倍布局渲染，见下面「dock（1080p）与缩放」；
   仍需实机确认的是这条路径在真实底座上确实给出原生 1080p（而不是被 scaler 再放大一次）。
5. 按需重绘：不调用 `framebufferBegin/End` 的那些帧，实机上画面应当保持不动
   （而不是闪烁或变黑）。这一条只有实机能验，电脑上只能验证布局与像素。

如果没有 sysmodule，或者 framebuffer 建不起来，NRO **不使用 framebuffer**，而是走
console（`consoleInit` + `printf` + 等 `+` 退出）打印服务名、`smGetService` 的返回值
与安装路径。这样做的原因来自实机反馈：console 是本项目里**已验证能在真机上正常显示**
的渲染路径（`-` 键那个视图），而 framebuffer 路径曾经给出过黑屏；错误信息再重要也
不能依赖一条还没被实机确认过的路径。

## HOS 风格页面（2026-09-16）

把界面从"带边框的面板 + 面板标题"改成模仿 HOS（Switch 系统 UI）的页面。参考图是
系统设置的截图（设置首页、主机/SD 卡、画面的亮度、好友通知设置、休眠确认框、EULA），
下面的数值都是从这些 1280×720 截图里**逐个像素量出来的**。

### 量出来的规格

| 项 | 值 | 用途 |
| --- | --- | --- |
| 背景 | `#2D2D2D` | 页面底色 |
| 抬升面 | `#323232` | 导航列表的选中块（本 UI 目前只在菜单用到） |
| 分隔线 | `#4D4D4D`，1px | 只用于行与行之间 |
| 页头线 | `#FFFFFF`，1px，y=87 | 标题栏下方，左右各留 24px |
| 正文 / 次级 | `#FFFFFF` / `#AAAAAA` | 标签与值 / 说明文字 |
| 强调色 | `#00FFC8` | 值、选中项文字、滑块的已填充部分 |
| 聚焦框 | 描边 `#66D5EE` 3px、填充 `#1F2328`、圆角 6px | 光标所在行 |
| 按键图标 | 实心形状 + 字母挖空，图标框 26px，字母 20px（`DGLAB_TEXT_ICON`）、按字形墨迹居中（四边留白≈6px） | 底栏与页内提示行；字母用页面背景色挖出 |
| 滚动条 | `#565656`，4px，屏幕右缘（x=1263） | 内容超过一屏时 |
| 对话框 | 底 `#464646`，按钮分隔线 `#676767`，聚焦按钮 `#3A3E43` | 确认框（尚未实现，数值先记下） |
| 内容列（单栏页） | x=220..1060（宽 840） | 行的左右边界，与截图里的分隔线一致 |
| 内容带（两栏页） | 左栏从 x=80 起，右栏 470..1189；外边距分别是 80 与 91 | 两栏页的外边距比单栏页小得多，见下 |
| 内容裁剪 | y 88..647 | 上边界在页头线下方一像素：第一行的焦点框上沿（约 y=122）才不会被切 |
| 段落 vs 说明 | 段落：白色 24px、无 ◆；说明：`#AAAAAA` 18px、缩进 16px、聚焦行下方带 ◆ | 图 1 顶部"只有当您在线时…"是段落，行下面的小字才是说明 |
| 行高 | 71px | 聚焦框 82px，故意比行高一点，像 HOS 那样压住上下分隔线 |
| 底栏 | **白色** 1px 分隔线 y=647，提示图标区 y=672..698，右对齐到 x=1216 | 按键提示；和页头线同色 |
| 字号 | 28 / 24 / 22 / 20 / 18 | 页头 / 行与标签 / 值 / 按键图标字母 / 说明（`text.h`） |

HOS 的侧栏聚焦框偏蓝（`#1A9AD5`），内容行的聚焦框偏青（`#66D5EE`）。按需求这里统一成
一个颜色（`theme->focus_ring`），所以菜单（导航列表）和内容行看起来一致；要还原 HOS
的差别，只需要改这一个主题字段加一处绘制。

### 与旧实现的关系

- `nro/include/dglab/ui/panel.h` / `panel.c` 已删除：HOS 没有面板，行靠分隔线和留白划分；
- 面板的"量一遍、画一遍"纪律保留下来，并换成 `dglabListMeasure` + `dglabListDraw`
  共用同一个 `DglabRow` 数组（同一个坑：量错就会画错）；
- 三处细节是这次踩出来的，写在这里免得重来：
  1. **行的值必须各有各的缓冲**：行是先记账、后统一绘制的，共用一个 `char buffer[]`
     会让所有行显示最后写进去的那串字；
  2. **滚动要跟着光标走**：没有光标就没有滚动，页面一旦超过一屏就会有内容永远看不到
     （高级参数页尤其明显：光标停在最后一项时，它下面的滑块和说明也必须滚进视野）；
  3. **页头的标题与右侧状态要按字体实测高度居中**，写死 y 会把 28px 标题放低。

### 按键提示

底栏的提示是"按键图标 + 动作文字"，因此：

- 图标由 `dglab/ui/button.c` 画（圆形描边 + 字母、圆角框 + 字母、± 与方向箭头），
  不需要图片资源；
- **按键名不再进 `lang/`**：`lang/*.json` 里只有动作（"返回""启动服务端""切换语言"），
  `strings.h` 里对应 `DglabString_Action*`；
- `B` 统一是返回、`A` 是执行当前行，菜单里 `B` 直接退出；每屏的提示由该屏自己给出，
  与按键处理写在同一个文件附近，避免提示和实际行为对不上。

### 主机侧检查

- `tests/canvas` 的 `testEveryPageStaysInItsRegions` 把五屏 × 两种语言 × **两种分辨率**
  （掌机 720p、底座 1080p）渲染一遍（外加菜单每个条目、服务端页四种状态、体感页两种状态、
  高级参数页首尾两项），检查没有任何像素落在"页头带 / 内容列 / 底栏 / 滚动条列"之外——
  这是旧版 `testNothingLeavesItsPanel`（"文字不许压到面板框线"）在无边框界面里的对应物；
- 预览出图仍然是 `tests/canvas/tools/render_preview.c`，会用 `PREVIEW_TTF` 开出全部字号，
  页面名后面加 `dock` 就渲染底座用的 1920×1080 画面（`normal` / `menu` / `motion` /
  `advanced` / `about` / `log` / `nowifi` / `stopped`）。

### 确认对话框：只记规格，不实现

按需求**不做确认框**，规格先记在这里，将来某个动作真的需要二次确认时直接照做（截图：
HOME 菜单的"主机将进入休眠模式"）：

- 底板 `#464646`，主文本框约 910×294，屏幕居中（截图里 x=118..1028、y=214..508）；
- 正文第一行与第二行同为 24px 白色居中，第三行是说明，用 18px `#AAAAAA`；
- 按钮区上方一条 1px `#676767` 分隔线；
- 两个按钮等宽、相邻排成一对，各约 394×82：聚焦的那个是 `#3A3E43` 填充 +
  3px `#289AD3` 圆环 + 强调色文字；未聚焦的只画强调色文字，没有框；
- **确认在右、取消在左**（需求指定），聚焦默认落在右侧那个；
- 主题字段已经留好（`theme->dialog` / `dialog_rule` / `dialog_button`），落地时只需要一个
  `dglab/ui/dialog.c`。

### dock（1080p）与缩放（2026-09-16 第五次修正：已实现）

**掌机 1280×720，底座 1920×1080 原生渲染**，由 `nro/source/platform/framebuffer.c` 按
`appletGetOperationMode()` 决定：底座时先 `nwindowSetDimensions(nwindowGetDefault(), 1920, 1080)`
再建 framebuffer；1080p 建不起来就把窗口尺寸改回 720p 再建一次（宁可让系统的 scaler 放大，
也不要黑屏）。

布局仍然只有一套坐标。缩放放在 **canvas 层**，`DglabCanvas` 带一个 `scale_num/scale_den`
（1/1 或 3/2），屏幕代码一行都不用改：

- 所有图元把逻辑坐标换算到 buffer 像素，**每条边各换一次**（`x` 与 `x + width` 分开），
  这样共享边的两个矩形换算后仍然共享那条边，不会留缝也不会叠一像素；
- 裁剪矩形按同样方式换算后**以 buffer 像素保存**，`insideClip`/`clipRect` 继续按像素工作；
- 文本按**物理**尺寸光栅化（28/24/22/20/18 × 1.5 = 42/36/33/30/27px），但
  `DglabGlyph`/`DglabGlyphSource` 报给布局的度量（advance、bearing、ascent、行高）
  仍然是**逻辑**单位（`text_ttf.c` 的 `toLogical`），字形位图按逻辑笔位 × scale 落在物理像素上。
  测量与绘制共用同一套累加，所以"量一遍、画一遍对不上"这个历史事故不会因为缩放回来；
- 字形缓存的上限 `MAX_GLYPH_PIXELS` 从 40 提到 48：42px 档的汉字墨迹会超过 40，而超限的字形
  不是被裁掉而是**整字消失**（缓存夹住盒子后跳过光栅化），`_Static_assert` 把这个数字和
  最大字号 × 最大 scale 绑在一起；缓存槽位 160 → 128，`.bss` 约 1.2MB；
- 二维码的模块尺寸在缩放后再取整（`floor`），并在调用方保留的逻辑框里重新居中：模块跨在
  像素边界上会糊，而扫码是这屏唯一没法重试的消费者；
- 底座切换：`appletHook(AppletHookType_OnOperationMode, …)` 的回调只置标志，主循环在帧之间
  做 `appDisplaySuspend()` + `appDisplayReopen()`（重建 framebuffer + 按新 scale 重新光栅化字体）
  并靠 `g_display_generation` 强制每一屏重画一帧。

代价与已知限制：

- 掌机保持原生 720p，不做 1080p 下采样；显存与填充率只在底座时是 2.25 倍；
- 共享字体加载失败时的兜底位图字体不参与缩放（它是 16×16 的点阵），1.5× 下会比按字号预期小，
  但不会溢出——这条路只在系统共享字体整份取不到时才会走；
- 电脑上能验证的只有布局与像素（`tests/canvas` 会把每一屏在 720p 与 1080p 各渲染一遍，
  检查像素仍然落在页头/内容列/底栏/滚动条之内，并单独检查共享边无缝隙、字形落点正确）；
  `nwindowSetDimensions` 在真机上是否真的给出原生 1080p 仍只有实机能验。

### 第二次修正（2026-09-16 晚）：按键、图标与页面结构

参考图 1（好友的通知设置）复核后又改了一轮，几个之前量错或没量到的地方：

- **底栏线是白的**：之前把内容行的 `#4D4D4D` 当成了底栏线（还写在 y=620）。全宽扫描 5 张参考图，
  页头线（y=87）与底栏线（**y=647**）都是纯白 `#FFFFFF` 1px；`#4D4D4D` 只出现在行与行之间。
- **首行焦点框被切**：焦点框比行高（82 vs 71），第一行的框上沿在 y≈122，而裁剪区从 y=128 开始，
  上边框被切掉。现在裁剪区从 y=88（页头线下方一像素）开始。
- **按键图标是"实心挖空"**：把参考图脚标的 A 键像素画出来后可以看到，它是**实心圆盘 + 把字母挖掉**
  （字母是背景色的洞），字母墨迹约 15px，对应 22px 字号。之前的"细圆环 + 18px 字"是错的。
  （**2026-09-16 第六次修正**：22px 字母在 26px 的图标框里只剩约 1px 留白，实机截图看得很明显，
  所以字母改成 `DGLAB_TEXT_ICON` 20px 并按字形墨迹居中，四边留白约 6px；图标框尺寸不变，
  见本文件末尾的"第六次修正"。）
- **页面段落 ≠ 行说明**：图 1 顶部的"只有当您在线时，才会收到发送给您的通知。"是**白色 24px 段落、无 ◆**；
  行下面的小字才是灰色 18px 说明（缩进 16px）。
- **滑块全部去掉**：连接测试的通道强度、体感页强度、高级参数的档位都改回文字值。

### 页面结构（第二次修正后）

| 页面 | 结构 | 按键 |
| --- | --- | --- |
| 菜单（标题 `DGLAB-NX 模式`） | 导航列表：5 行 + 选中项的说明 note | ↑↓ 选择，A 进入，B 退出 |
| Socket 服务端 | 左栏 x=220 宽 400：提示行 + 二维码 + 三行小字计数；右栏 x=620 宽 440：页内提示行 + 6 行参数 | X 清空、Y 日志、A 启停、B 返回、ZL/ZR 测 A/B、↑↓ 调 A、←→ 调 B |
| 日志子页 | Y 打开：页头 + 32 行日志（系统字体、24px、**行距 37px**、白色），打开时停在最新 | ↑↓ 滚动、Y 关闭、B 返回 |
| 体感（Joy-Con） | 页内提示行 + 5 行（连接、左/右 Joy-Con、通道强度 A/B） | 与连接测试相同，多一个 `Y` 重新扫描手柄（`docs/joycon-input.md`：手柄关掉再打开、或从主机上拔下来之后，用它在原地重新取句柄） |
| 高级参数（体感） | 12 行 + 选中项的说明 note，按光标滚动 | ↑↓ 选择、←→ 改值（按住连发）、Y 恢复默认、B 返回 |
| 关于 | 两个白色段落 + 源码行 + 语言行 | ←→ 切换语言、B 返回 |

按键约定：**Console 页（BLE PoC console、启动/出错提示页）一律 `+` 退出**；
**其余 framebuffer 页面一律 `B` 返回/退出，`+` 无效**。

页内提示行（Socket 服务端与体感页顶部那行 `[▲][▼] 调整 A  [◀][▶] 调整 B`）用的是 18px 白字 + 按键图标，
这样"上/下管 A、左/右管 B"能在一行里放下，也还是"图标 + 动作文字"的形式，不需要把按键名写进文案。

### 日志环与二维码

- 日志环从 12 行提到 **32 行**（`DGLAB_SCREEN_LOG_LINES`），否则日志子页根本没有可滚动的历史；
  SD 卡上的镜像文件仍是完整的。
- 二维码的模块大小同时受两栏的宽与高约束：`module = clamp(⌊可用宽 / 总模块数⌋, 4, 8)`，再按高度收一次；
  算不出 4 就直接显示"二维码太长"。左栏 400px 宽时，典型的 45 模块（版本 7）码是 7px/模块、371px。

### 标题与命名（2026-09-16 第三次修正）

- **`DGLAB-NX` 只出现在主菜单**（标题 `DGLAB-NX 模式`）；子页面标题只有自己的名字：
  `Socket 服务端` / `体感（Joy-Con）` / `高级参数（体感）` / `关于`，日志子页是 `sysmodule 日志`。
- **菜单项的名字就是子页面的标题**，而且是同一个字符串：`menu.c` 的 `kItemKeys[]` 直接指向
  `DglabString_SocketTitle` 等，`item_socket`/`item_motion`/`item_advanced`/`item_about`
  四个键已删除。名字要改就改一处，列表和页面不可能再对不上（BLE PoC 控制台没有自己的页面，
  仍用 `item_ble_poc`）。
- 说明文案里提到别的页面时用新名字（例如"在「Socket 服务端」里设置"）。
- **主菜单标题只留 `DGLAB-NX`**（不带"模式"），因为它是唯一显示应用名的地方。

### 单栏与两栏的边距不一样（2026-09-16 第四次修正）

参考图并排看就很明显：单栏页（好友的通知设置）的内容列是 **x=220..1059**，左右各约 220；
两栏页（设置/数据管理）的**左栏从 x=80 起、分隔线到 379**，**右栏 470..1189**——右边距只剩 91，
文字起点 482、值的右端 1174。也就是说两栏页并没有把内容挤在中间，而是几乎铺满整屏。

因此 `page.h` 多了一个宽版内容带：

- `dglabPageClipContent()`：单栏页用，x=220..1060（菜单、体感、高级参数、关于、日志子页）；
- `dglabPageClipWide()`：两栏页用，x=80..1190（目前只有 Socket 服务端页）。

Socket 服务端页的两栏直接采用实测数字：左栏 `DGLAB_SOCKET_QR_X 80` / 宽 390（二维码 371 居中），
右栏 `DGLAB_SOCKET_INFO_X 470` / 宽 719（行内文字从 486 起、值右端 1173，和参考图的 482 / 1174 对齐）。
`tests/canvas` 的区域检查也按页区分：单栏页超出 220..1060 就失败，两栏页允许到 80..1190。

### 第六次修正（2026-09-16）：日志滚动、二维码、菜单、图标与字体生命周期

一轮实机 / 模拟器反馈后的修正，逐条记在这里，免得以后重踩：

- **日志页一次按键滚一行**：`log_offset` 一直是像素单位，而 `logScrollFromDirections()` 每按一次
  只加 `1`，所以"滚一行"实际是滚一个像素。现在一步 = `DGLAB_SCREEN_LOG_PITCH`(37px)，单击永远
  只走一行；连发用它自己的时序（`LOG_SCROLL_HOLD_NS` 300ms 后每 `LOG_SCROLL_REPEAT_NS` 50ms
  一行，约 20 行/秒），不再借用体感强度的 `TEST_STRENGTH_*` 时序，也不再共用它的 hold 状态变量。
  打开日志页仍然停在最新一行（`offset = logMaxOffset()`），所以往上翻时最后一行可能只露出一截
  ——"停在最新"和"对齐行网格"只能选一个。
- **二维码只在服务端运行时显示**：`NET_QR` 只要有局域网地址就返回 payload（地址本身有用，
  docs/ipc.md 的契约不变），但界面以前拿到就画码，于是出现"服务端还没启动就显示二维码"。
  现在 `screen.c` 的 `qrCode()` 还要求 `status_ok && (Listening || Paired)`，否则显示
  `qr_not_running` 的说明。`tests/canvas` 用"这一屏唯一的纯黑像素"数守住它：未运行时左栏黑色
  模块数必须是 0，运行时 > 1000。
- **按键图标的字母缩小并居中**：字母以前用 24px body 画进 26px 圆盘/方框，而且按 line box
  居中——line box 带着基线以下的 descender 空间，实测字母离边框只有 1px（实机截图就是这个
  观感）。现在字母用新增的 `DGLAB_TEXT_ICON`(20px)，并按**字形墨迹盒**
  (`dglabTextInkTop` / `dglabTextInkHeight`) 在框内居中，四边留白约 6px。图标几何没变，
  `dglabHintDraw` / `dglabPageHints` 因此要显式传两个字体（图标一个、动作文字一个）；
  `tests/canvas` 有两条回归：留白不足、以及"图标没有用传进来的图标字体"都会失败。
- **主菜单不循环**：`dglabMenuMove()` 从"回绕"改成"夹紧"到 `0..count-1`，到底再按不会跳回顶端。
- **体感页的灰色小字删掉**：那条 note 讲的是"通道强度是音量、在 Socket 服务端里设置"，而这一页
  自己有通道强度行、也能直接改，属于过期说明。`motion_safety` 这个 key 从 `strings.h` /
  `strings.c` / `lang/*.json` 三处一起删掉（`tests/lang` 保证三方一致）。
- **字体生命周期**：进 BLE PoC 控制台前会 `dglabFontClose()`（`plExit()` 把共享字体的映射释放
  掉），回来后**必须**重新 `dglabFontOpen()`。以前只重建 framebuffer，字形源里的
  `stbtt_fontinfo` 仍指向已释放的映射：缓存里已有的字形还能画，新字符全是空，整屏文字看起来
  像"丢了"，只有重启 NRO 才能恢复。这一步现在统一在 `main.c` 的 `appDisplaySuspend()` /
  `appDisplayReopen()` 里（BLE PoC 与底座切换共用）：重建 framebuffer → 重新 ApplyLanguage
  （重开共享字体、按当前 scale 重新光栅化全部字号）→ `g_display_generation++`；每一屏都把
  generation 计入"是否要重绘"，所以重建之后一定重画一帧。

### 第七次修正（2026-09-16）：体感页那两行的名字与判定

实机截图暴露的是**命名**问题：体感页原本把输入侧的两行叫 `通道 A` / `通道 B`，而它们显示的
其实是"左 Joy-Con / 右 Joy-Con 这一侧连上没有、现在动得多猛、映射正在用什么频率"。用户很
自然地把它读成 DG-LAB 通道的连接状态（那是上面那行 `连接`），于是"两行都未连接、设备却有
输出"看起来像自相矛盾。

- 标签改成 `左 Joy-Con` / `右 Joy-Con`（key `motion_joycon_left` / `motion_joycon_right`），
  行序不变：连接 / 左 Joy-Con / 右 Joy-Con / 通道强度 A / 通道强度 B —— 前两行是输入，
  后两行是输出，一眼分开；
- 判定同时修掉：每侧最多持有两个 HID 句柄（`NpadJoyDual` 的一个 + 单只风格的一个），而
  "最后一个回答的句柄的最后一条读数"曾经直接决定整侧状态；单只风格的句柄只回
  `IsConnected=0` 的占位读数时，整侧被判成未连接，而采样早已进了映射——正是截图里的现象。
  现在每侧只用一个句柄（`NpadJoyDual` 优先，只有它没给出采样才回退），状态用
  `dglabMotionSensorConnected()` 判定："有采样 → 已连接；连续 20 次轮询没有读数 → 未连接；
  再否则保持上一次"——句柄自称的 `IsConnected` 不算数（2026-09-17 两轮实机反馈：相信那个位
  会让"挥动中"再也回不到"未连接"），细节见 `docs/joycon-input.md`；
- 进入玩法时会往日志写一行 `motion left: handles 2, #0 states … | motion right: …`（落到
  `dglab-net.log`），把主机实际交出的句柄布局记下来；连接状态变化时另写一行
  `motion left connected` / `motion left disconnected`。实机结果与解读见
  `docs/joycon-input.md`。
