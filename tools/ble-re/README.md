# BLE 固件研究工具

用于 [HOS Bluetooth 固件研究](../../docs/ble-re.md)。提取与分析只读固件；
`make_ips.py` 另生成/检查补丁文件，不原地修改固件。密钥、固件、NCA 与提取物不得入库。

## 提取与分析

1. `nca_index.py` 将模拟器分片目录还原为 NCA，列 Title ID、类型与 SDK 版本。
   按 NPDM/module ID 确认目标，不信文件名。
2. 用外部 [hactool](https://github.com/SciresM/hactool) 提取 ExeFS（不随仓库提供）：

   ```sh
   git clone --recursive https://github.com/SciresM/hactool
   cp config.mk.template config.mk
   make -j4
   ./hactool -k <prod.keys> -t nca --exefsdir=<out> <title>.nca
   ```

   系统模块 `<out>/main` 为 NSO0，`main.npdm` 为 NPDM。
3. `python3 tools/ble-re/nso2elf.py <main.nso> <module.elf>` 转为 Ghidra 可加载的 ELF64；
   先通过本仓库 NSO 往返自测：

   ```sh
   python3 tools/ble-re/selftest_nso2elf.py sysmodule
   ```

   输入为 libnx 构建的 `DGLAB-NX-Core.nso/.elf`，必须 PASS（loadable 段一致）再信任转换器。
4. devkitA64 binutils 与地址工具：

   ```sh
   aarch64-none-elf-objdump -d --no-show-raw-insn module.elf > module.txt
   python3 tools/ble-re/find_xref.py module.txt 0x11895b
   python3 tools/ble-re/peek.py module.elf 0x159a28-0x159a60 --pointers
   ```

   find_xref 支持 adrp+add、adrp+ldr、adr；漏 adr 曾误判服务表无引用。
5. Ghidra headless（需 Ghidra 与 Java；macOS 原流程用 `brew install ghidra`）：

   ```sh
   JAVA_HOME=$(brew --prefix openjdk@21)/libexec/openjdk.jdk/Contents/Home \
   analyzeHeadless <projdir> <project> -import module.elf \
     -scriptPath tools/ble-re/ghidra -postScript DecompileAt.java 0x1b5dc

   analyzeHeadless <projdir> <project> -process module.elf -noanalysis \
     -scriptPath tools/ble-re/ghidra -postScript ExportDecompiled.java /tmp/ble-re/module.c

   analyzeHeadless <projdir> <project> -process module.elf -noanalysis \
     -scriptPath tools/ble-re/ghidra -postScript DecompileForce.java 0x1efd0 0x1f420
   ```

   Export 只输出分析器已建函数；IPC case 通过字节/分支表到达，需 Force 按地址反汇编并建函数。
6. `abi_sizes.py` 用已安装 devkitA64 编译探头并读符号尺寸，核对 libnx 类型与固件 case 拷贝量。
   不以虚表下标推命令号；已核对结论见固件文档。

## 补丁工具

```sh
python3 tools/ble-re/make_ips.py --self-test
python3 tools/ble-re/make_ips.py --elf module.elf --module-id <hex> \
  --edit 0x1c750:00200091:00200091 --out <module-id>.ips
python3 tools/ble-re/make_ips.py --verify <module-id>.ips
```

上述 edit 是工具调用示例，不是 BLE 补丁配方。脚本必须验证原字节；IPS 偏移为
0x100＋ELF 地址，字节从解压 ELF 读取，不能直接取压缩 NSO 段。
实际适用构建、补丁字节、安装/撤销与风险见 [诊断补丁](../../docs/ble-re.md#诊断补丁)。

工具参数以各脚本的文件头/usage 为准，使用 argparse 的脚本也提供 `--help`。nso2elf 自带 LZ4 block 解码，无额外 Python 解压库。
可回馈上游的结论及断言边界见 [upstream.md](upstream.md)，尚未提交。
