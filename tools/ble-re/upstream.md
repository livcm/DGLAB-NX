# 回馈给 libnx / switchbrew 的候选（草稿）

> **2026-09-22 重写。** 第一版里的第 1 条（cmd 62 载荷变成 0x40 字节）与第 4 条
> （"20.0.0+ 的 btdrv ABI 与 `btdrv_types.h` 不再一致 / 类型被换掉"）**已作废**：它们
> 建立在"`0x159a28` 是命令表"这个错误前提上。那张表是 btdrv **服务对象的虚表**，真正的
> 命令分派在 `FUN_0001d4b0` 的 259 个 case 里；逐条读完的结果是 libnx 的请求形状与固件
> **一致**（见 `docs/ble-re.md` 的「判定（2026-09-21 夜，更正）」与「命令 → 请求形状」表）。
> 同一套方法后来用在 `btm` 上，结论同样是"形状一致"。所以现在能提的只有下面三条，
> 提交动作仍由人决定。

libnx 里没有 20.0.0+（`bluetooth` 改名 `bluetooth.autog`）的相关记录：`btdrv.h` 的版本
注记停在 12.x，`btmu.c` 最后一次改动是 2020-12-29。下面每一条都写了"能断言什么 / 不能
断言什么"，避免把半成品结论推上去。

## 1. btdrv：cmd 40（`GetChannelMap`）会让固件关掉调用者的会话

能断言：

- HOS 22.5.0（AMS 1.11.2，Switch 1）上，按 libnx 现在的形状
  （`SfBufferAttr_HipcMapAlias|Out|FixedSize`，0x88 字节）调用 cmd 40 之后，**同一会话
  之后的每一次调用都返回 `0x0000F601` = `MAKERESULT(Module_Kernel,
  KernelError_ConnectionClosed)`**；调用之前同一会话上的一切都正常。两次独立实机复现；
- 换成指针缓冲（`HipcPointer|Out|FixedSize`）同样返回 `F601`。

不能断言：是缓冲属性还是缓冲大小的问题（22.5.0 上这条命令到底要什么形状还没定）；
也不能断言这是有意为之还是固件缺陷。

## 2. btm:u：请求带 ARUID，只有该 ARUID 的持有者能用

能断言：

- libnx 的 `btmu*` 封装用 `appletGetAppletResourceUserId()` 填请求，在 **sysmodule** 里
  那个值没有意义：`btmuStartBleScanForSmartDevice` 返回 0，但永远收不到扫描事件/结果
  （`btmuGetBleScanResultsForSmartDevice` 恒为 0）；
- 用同样的载荷、但填**真实的 applet ARUID**（由前台 NRO 提供）时，请求在服务框架层就被
  拒：`0x0000060A` = `Sf/3`；
- 固件侧的原因已经定位到代码：`btm` 模块 cmd 18（`BleConnect`）的 case `0x27b20` 取
  调用者自己的 ARUID（`param_3->vtable[1](param_3, &aruid)`）与请求里的 ARUID 比较，
  `0` 或相等才继续，否则返回 `0x60A`。base `btm` 服务的同类命令（cmd 35 `BleConnect`）
  请求里根本没有 ARUID 字段，它用 `RegisterAppletResourceUserId`(57) 登记。

建议的表述：`btm:u` 是 applet 专用接口，请求里的 ARUID 必须等于调用者自己的 ARUID；
后台进程要拿通用 BLE central 只能走 `btdrv` 或 base `btm`（本仓库两条都在试，
见 `docs/ble-re.md`）。

## 3. 通用注记：20.0.0+ 的命令形状要按 case 读，不能按虚表

能断言：

- 服务对象的虚表（btdrv `0x159a28`、btm 的两张表 `0x76580` / `0x765f8`）不是命令表：
  命令分派由 CMIF 头（`SFCI`）+ 每个命令自己的 case 完成（btdrv `FUN_0001d4b0`、
  base `btm` `0x1bc50`），case 里按 IDL 解参数再调虚方法；
- 用这个方法核对过：btdrv 的扫描链与 GATT 注册、base `btm` 的 13 条 BLE 命令，**载荷与
  出参都与 libnx 一致**（含条目尺寸 0x148 / 0xC / 0x24 / 0x74）；
- 这些 case 只被"字节表 + 分支表"引用，反汇编器不会把它们建成函数；按地址强制反编译的
  脚本放在 `tools/ble-re/ghidra/DecompileForce.java`。

不能断言：有没有命令在 20.0.0+ 被**新增/删除**（本次只核对了 BLE 这一段用到的命令）。
这条更像给 switchbrew 的 `BTM_services` / `Bluetooth_driver_services` 页面加注记，
不是 libnx 的代码问题。
