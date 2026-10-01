# libnx / switchbrew 上游候选

草稿，尚未提交。证据限定 HOS 22.5.0 / AMS 1.11.2 / Switch 1；
地址与复核材料见 [固件研究](../../docs/ble-re.md)。提交动作仍由人决定。

2026-09-22 已撤回“cmd 62 要 0x40 字节”“整体 btdrv ABI 漂移”：
`0x159a28` 是虚表，不是命令表。BLE 相关请求形状按 case 核对与 libnx 一致。
当时查阅的 btdrv.h 版本注记停于 12.x、btmu.c 修改记录止于 2020-12-29，
这些是研究时的资料状态，不代表上游今天仍未更新。

## GetChannelMap 关闭会话

两次独立实机，cmd 40 使用 0x88 字节 MapAlias/Out/FixedSize 后，会话关闭，
其后所有调用 `0xF601`（Kernel ConnectionClosed）；换指针缓冲同样复现。
可以报告现象及最小复现，不能断言缓冲大小/属性是原因，也不能断言有意行为或固件缺陷。

## btm:u 的调用者身份

sysmodule 用 btmu 封装扫描返回 0，但无事件/结果；借 NRO ARUID 则 `0x60A`。
固件 cmd 18 case `0x27b20` 比较调用者与请求 ARUID，0 或相等放行，不同则拒绝。
base btm 对应 cmd 35 不带该字段，用 cmd 57 登记。
可建议文档明确 applet/调用者身份约束；不能建议后台冒用前台身份。

## case 与虚表的区别

btdrv FUN_0001d4b0、base btm 0x1bc50 从 CMIF SFCI 分派到 case，
IDL 解参数后调用虚方法；btdrv 0x159a28、btm 0x76580/0x765f8 都不是命令表。
核对过扫描/GATT 与 base btm 13 条 BLE 命令及条目尺寸 0x148/0xC/0x24/0x74，形状匹配。
未核对全部 20.0.0+ 命令增删，不能作全量兼容结论。
case 不一定由分析器自动建函数，复核用 DecompileForce.java。
此项适合接口资料的方法注记，不构成 libnx 代码 bug。
