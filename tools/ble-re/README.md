# ble-re

Tools used for the read-only reverse engineering of the HOS Bluetooth stack that
`docs/ble-re.md` describes. Everything here only reads firmware; no firmware,
key or extracted NCA belongs in git.

## Pipeline

1. `nca_index.py` — turn the emulator's part-directory NCAs back into files and
   list Title ID / content type / SDK version for each of them.
2. Build hactool (not vendored) and extract the ExeFS of the module of interest:

       git clone --recursive https://github.com/SciresM/hactool
       cp config.mk.template config.mk && make -j4
       ./hactool -k <prod.keys> -t nca --exefsdir=<out> <title>.nca

   For system modules `<out>/main` is an NSO0 and `<out>/main.npdm` the NPDM.
3. `nso2elf.py` — convert that NSO0 into an ELF64 file that Ghidra can load.
4. Disassemble with the devkitA64 binutils and use `find_xref.py` / `peek.py`
   for address-level work:

       aarch64-none-elf-objdump -d --no-show-raw-insn module.elf > module.txt
       find_xref.py module.txt 0x11895b
       peek.py module.elf 0x159a28-0x159a60 --pointers
5. Decompile with Ghidra headless (`brew install ghidra`), passing the scripts
   in `ghidra/` and a script search path:

       JAVA_HOME=$(brew --prefix openjdk@21)/libexec/openjdk.jdk/Contents/Home \
       analyzeHeadless <projdir> <project> -import module.elf \
         -scriptPath tools/ble-re/ghidra \
         -postScript DecompileAt.java 0x1b5dc

       ... -process module.elf -noanalysis -postScript \
         ExportDecompiled.java /tmp/ble-re/module.c

6. `make_ips.py` — build (or inspect) an Atmosphère `exefs_patches` IPS32 patch
   for a system module, guarded by the module's original bytes:

       python3 tools/ble-re/make_ips.py --self-test
       python3 tools/ble-re/make_ips.py --elf module.elf --module-id <hex> \
           --edit 0x1c750:00200091:00200091 --out <module-id>.ips
       python3 tools/ble-re/make_ips.py --verify <module-id>.ips

   The IPS offset is `0x100 + address` (Atmosphère protects the first 0x100
   bytes of the mapped module) and the bytes must come from the decompressed ELF
   - the NSO's segments are LZ4 compressed.

7. `abi_sizes.py` — print the size of libnx's btdrv/btm structs by compiling a
   probe with the installed devkitA64 and reading the symbol sizes, so the
   firmware's "copy N bytes" can be compared against the libnx side instead of
   against memory. On HOS 22.5.0 several of them do not match
   (`BtdrvGattAttributeUuid` 0x14 vs the firmware's 0x40-byte registration block,
   `SetSysBluetoothDevicesSettings` 0x200 vs 0x2BE), which is why the BLE work
   needs per-command struct derivation rather than a numbering fixup.

8. `upstream.md` — the findings from this work that are new to libnx
   (`btdrv.h`'s version notes stop at 12.x and `btmu.c` was last touched in
   2020-12), written as ready-to-submit drafts: the 0x40-byte payload of command
   62, command 40 closing the caller's session, `btm:u` being applet-only, and the
   ABI/type mismatch table. Each item says what is proven and what is not.

## Validation

`selftest_nso2elf.py` converts the NSO this repository builds itself
(`sysmodule/DGLAB-NX-Core.nso`, produced by the libnx toolchain from
`DGLAB-NX-Core.elf`) back into an ELF and compares the loadable segments with
the original. It must print `PASS` before the converter is trusted on firmware
modules:

    python3 tools/ble-re/selftest_nso2elf.py sysmodule

## Notes

- `find_xref.py` understands the three ways a stripped AArch64 module reaches
  data: `adrp`+`add`, `adrp`+`ldr`, and a plain `adr`. Missing the `adr` case
  once made a service-name table look unreferenced.
- `nso2elf.py` implements the LZ4 block decoder itself, so the only external
  tools are the binutils and Ghidra.
