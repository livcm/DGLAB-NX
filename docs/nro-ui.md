# NRO UI 设计依据

当前后端是 libnx framebuffer，布局/绘制与平台呈现分层。
菜单、连接、BLE、体感、触屏、参数、关于及日志已实现；console 用于 PoC 和启动错误。
强制规则见 [NRO AGENTS](../nro/AGENTS.md)，用户操作见 [README](../README.md)。
本文只保留设计理由、实测依据与平台限制，不重复组件/参数清单。

## 呈现与字体选择

普通 Homebrew 没有可直接调用的 HOS 通用控件框架。framebuffer 零额外依赖，
能让 QR 模块保持整数正方形，也使 UI 在电脑上渲染验证。
SDL/OpenGL/Borealis/ImGui 等完整方案成本更高，Overlay 的环境另行评估。

自绘文本采用 UTF-8、pl 系统共享字体、stb_truetype 与字形缓存；
默认 1-bit 阈值光栅化，匹配点阵观感并复用无 alpha 混合的 canvas。
密笔画可能粘连，改字体/字号需逐字看图，不能仅靠像素越界测试判清晰。

console/字体加载失败的兜底使用 libnx 256 字形字体，不能显示中文；
默认字体符号未公开声明，曾以 nm 与 console 渲染器核对大小和位序，回归见 tests/canvas。
位图兜底在 1.5 倍底座下不随字号缩放，会偏小，不能当原生大字路径。

## 布局与颜色依据

HOS 风格来自用户提供的 1280×720 系统设置截图逐像素测量（2026-09-16 深色、09-18 浅色）。
page/list/theme/text 是尺寸与配色唯一实现来源，不在文档再维护常量表。

- 页头、列表、底栏，无面板；焦点框比行高，裁剪从页头线下开始，不能从首行顶裁。
- 单栏与两栏使用不同内容带，日志用宽带，触屏可声明整块玩法区；不能强行用同一边距。
- 值文字各用独立缓冲；所有行先测量后绘制，滚动随光标；About 等无光标页有独立滚动键。
  About 真实字体实机排不进一屏，不能为省滚动删内容。
- 按键图标按墨迹盒居中，图标与文字两字体分开；line box 居中曾让字母贴边。
- HOS 导航与内容聚焦颜色不同，本项目按用户要求统一成一个主题字段。
- 浅色截图没有对话框或错误状态，因此 dialog/dialog_rule/dialog_button/warn/error
  未量到，暂沿深色值；未来修改必须用含该状态的截图，不凭观感。
- 确认对话框按需求尚不实现；若未来实施，确认右、取消左、默认右焦点是既有选择，
  其它旧量图细节可查 Git 历史。

颜色值已在 theme 中，界面通过 dglabThemeGet 取色。主题取不到系统设置时 Auto=深色；
只在启动/重建读系统设置，挂起时改系统主题需重启 NRO 才跟上，不轮询。

## 本地化与文案

简中/英文 JSON 在根 lang，运行期只读 `SD:/switch/DGLAB-NX/lang/`。
语言自动选择失败或非中文时用英文；有一套可用即可启动，缺 key 回落。
文件名与 language 必须对应；全不可用在 framebuffer 创建前用英文 console 提示并等 + 退出。
未知/重复/缺 key 可记录日志，不能让屏幕空标签。格式与加载细节查 strings/langfiles 及 tests/lang。

专有名词 DG-LAB、Joy-Con、sysmodule、BLE、PoC、Socket、App 和路径保留原文；
统一用“通道强度”“波形值/波形强度”“通道强度上限”“波形强度上限”，不混概念。
菜单名与页面标题共用字符串；DGLAB-NX 只用于菜单标题，版本进 About 行，页头右侧只留服务状态。
语言/主题偏好共存 config/app.cfg，旧文件缺主题时用 auto，不丢原语言。

## 二维码验证

QR 编码独立与 macOS CoreImage 的 CIQRCodeGenerator 逐模块比对，工具和参考矩阵在 tests/qr。
参考返回一模块静区，需要移除；比对样例用纯小写无数字，避免 CoreImage 混合模式分段。
本项目 byte 模式能编码真实地址，只可能比 Apple 多一个版本，不影响扫码。
屏上模块按物理像素取整、静区保持、在可用框内居中；不能用照片观感替代编码验证。

## 底座与显示生命周期

布局始终 720p 逻辑坐标，底座 framebuffer 为 1920×1080（scale=1.5），失败回退 720p。
矩形两边分别换算保持共享边；字体按物理尺寸光栅化、度量返回逻辑单位，QR 模块保持整数像素。
字形缓存容量不足会让整字消失而非裁掉，需与最大字号×scale 一起核对。

操作模式回调只置标志，帧间通过 appDisplaySuspend/Reopen 重建后端与字体，
以 generation 强制所有屏重画；console 共用同一窗口，必须先释放后端再初始化 console。
只重建 framebuffer 不重开字体曾导致新字符全空。
按需重绘要覆盖语言、主题、状态、滚动与 generation；按键必须读本页当帧状态，
不能靠其它页才更新的全局决定启停。

## 日志与 BLE 跨页

Socket 和 BLE 的屏上日志各自一条环与 cursor，不能互相替换标题后共用内容。
原始行保留，显示再按真实像素宽裁并加省略号，不能提前按字符数截断。
NRO 自己的诊断进 net 环，BLE 会话/探针进 PoC 环；net 磁盘镜像可以合写。
BLE 内存日志必须关机前由 NRO 读出。

2026-09-27 选择 BLE 会话跨页存活，菜单提示如何停，玩法连接行显示蓝牙；
退出 NRO 停会话，sysmodule 看门狗兜底。参数页改上限后回蓝牙页会补 BLE_LIMIT，
不为改上限重连。设计/代码已落实，跨页实机仍待验，见 [BLE 验证](ble-poc.md)。

## deko3d 后端评估（暂搁置）

迁移收益较低暂不实施。路线 A 保留 CPU canvas，只替换呈现，不改布局/字体，
用 deko3d C API，不需 C++17/portlibs/着色器。

2026-09-19 核对 deko3d 0.5.0 的 dk_swapchain.o 与 libnx framebuffer.o：
交换链按格式表配置 tiled NvGfxFormat，不看 PitchLinear 标志，**不能直接呈现线性图像**。
RGBA8 表值 `0x0000000100532120` 与 libnx 相同，线性数据需 GPU 拷进 tiled 交换链图。
这已解决早期“先确认是否接受 PitchLinear”的疑问。

可行路径：CPU 写线性 memblock → CopyBufferToImage → present；
CopyBufferToImage/BlitImage 已在 deko3d.h 核对，deko_basic 本机完整构建通过。
参考官方 deko_basic、deko_console/gpu_console，字体本地化保持与后端无关。

- CPU/GPU 共享写用 MemBlockGetCpuAddr/FlushCpuCache；每交换链槽独立画布避免 GPU 未读完就覆盖。
  单画布需 QueueWaitIdle，省内存但串行；双画布在底座额外约 8.29MB，原评估总约 33MB 对现行约 25MB。
- swapchain 析构会 ReleaseBuffers；device 建销与 libnx 同一 nv 生命周期，
  console/底座切换可沿用显示重建纪律，仍须反复实机验证。
- deko3d 内部 RaiseError 会进入致命页，不保证返回失败给 console 回退。
  尺寸要先用返回 Result 的 nwindowSetDimensions 探测；图像 flag/2D 拷贝组合仍待 spike。
- 若恢复实施，先保留 framebuffer 后端可选，验菜单后再扩展全部页面；
  比对两分辨率截图、扫码、反复底座/console 切换、按需重绘和帧时间，再决定默认后端。

## 验证入口与待验

`make -C tests/canvas` 覆盖两语言×两主题×720p/1080p 的区域与布局；
`make -C tests/lang`、`make -C tests/qr` 验证语言及编码。预览命令见
`tests/canvas/tools/render_preview.c` 文件头，可设 PREVIEW_THEME/PREVIEW_LANG。

主机图片只能验证布局与像素；仍需实机确认底座真实原生 1080p、无重绘帧画面保持、
console 来回切换稳定、二维码成功率、共享字体的可读性及手柄提示。
缺服务/显示建立失败时的 console 诊断路径已有实机基础，不能让错误信息依赖可能黑屏的后端。
