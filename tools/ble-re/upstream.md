# 回馈给 libnx / switchbrew 的候选（草稿）

这些是这次逆向里**已经有实机证据**的发现，libnx 目前完全没有 20.0.0+（`bluetooth` 改名
`bluetooth.autog`）的相关记录：`btdrv.h` 的版本注记只到 12.x，`btmu.c` 最后一次改动是
2020-12-29。下面每一条都写了"能断言什么 / 不能断言什么"，避免把半成品结论推上去。

## 1. btdrv：cmd 62 的载荷在 20.0.0+ 变成 0x40 字节（`RegisterGattClient`）

能断言：

- HOS 22.5.0（AMS 1.11.2，Switch 1）上，用 libnx 现在的 0x14 字节
  （`BtdrvGattAttributeUuid`）发 cmd 62，固件回
  `ClientRegistration result=0x00000037 / client_if=0xFF`（无效接口号），重复多次稳定复现；
- 把整块换成 **0x40 字节内联载荷**（块首放 libnx 原来的 `{u32 size=0x10; u8 uuid[16]}`，其余补
  零）后，固件回 `ClientRegistration result=0x00000000 / client_if=0x02`；
- 固件侧同一命令的适配层是"从请求里拷 0x40 字节结构、再交给管理器方法"，与 0x40 吻合；
- 改成指针缓冲（`SfBufferAttr_HipcPointer|In|FixedSize`）反而不行：返回
  `0xF601`（`KernelError_ConnectionClosed`），说明这条要内联数据。

不能断言：0x40 字节块的字段含义（目前只验证了"块首是原来的 0x14 字节结构"可用）；成功还
需要什么会话前置条件（另一次测试里，同样的 0x40 形状在做了若干其它调用之后返回
`0x29E71` = `Bluetooth/0x14F`）。

## 2. btdrv：cmd 40（`GetChannelMap`）会让固件关掉调用者的会话

能断言：

- 用 libnx 现在的形状（`SfBufferAttr_HipcMapAlias|Out|FixedSize`，0x88 字节）调用后，
  **同一会话之后的每一次调用都返回 `0xF601` = `KernelError_ConnectionClosed`**；调用前同一
  会话上的一切都正常。两次独立实机复现；
- 换成指针缓冲（`HipcPointer|Out|FixedSize`）同样返回 `F601`。

不能断言：是缓冲属性还是缓冲大小不对（22.5.0 上这条命令到底要什么形状还没定）。

## 3. btm:u：请求带 ARUID，只有拥有该 ARUID 的 applet 能用

能断言：

- libnx 的 `btmu*` 封装用 `appletGetAppletResourceUserId()` 填请求；在 **sysmodule** 里那个值
  没有意义。此时 `btmuStartBleScanForSmartDevice` 返回 0，但**永远收不到扫描事件/结果**
  （`btmuGetBleScanResultsForSmartDevice` 恒为 0）；
- 用同样的载荷、但填**真实的 applet ARUID**（由前台 NRO 提供）时，请求在服务框架层就被拒：
  `0x0000060A` = `Sf/3`（module 10 = `Sf`，description 3）；
- 连接同理：sysmodule 调 `btmuBleConnect` 得到 `0x0005568F` = `Btm/0x2AB`。

建议的表述：btm:u 的扫描/连接是 applet 专用接口；后台 sysmodule 想拿通用 BLE central 必须走
btdrv（本仓库的做法见 `docs/ble-re.md`）。

## 4. 通用注记：20.0.0+ 的 btdrv ABI 与 `btdrv_types.h` 不再一致

适配层"要拷多少字节"和 libnx 的类型尺寸对照（`tools/ble-re/abi_sizes.py` 量的 libnx 侧）：

| 命令 | libnx 类型/大小 | 固件要拷 | 结论 |
| --- | --- | --- | --- |
| 62 `RegisterGattClient` | `BtdrvGattAttributeUuid` 0x14 | 0x40 | 不一致（见 §1） |
| 23 `TriggerConnection` | `{addr; u16}` 8 | 0x2BE | 不一致 |
| 57/53 过滤器/广播数据 | `BtdrvBleAdvertiseFilter` 0x3E / `...PacketData` 0xCC | 0xCC | 类型落到别的命令号上 |
| 61 `EnableBleScanFilter` | `bool` | 0x40 | 不一致 |
| 24 `AddPairedDeviceInfo` | `SetSysBluetoothDevicesSettings` 0x200 | 0x2BE | 不一致 |

也就是说这不是"整体挪几个命令号"能修的：20.0.0+ 把这一层重新生成过，**类型也被换掉**。
可行的贡献方式是先在文档/issue 里给出"哪些命令的形状变了 + 怎么验证"的方法（从每个命令的
适配层读出'拷多少字节、交给哪个方法'），等结构字段逐个对出来再提 PR。
