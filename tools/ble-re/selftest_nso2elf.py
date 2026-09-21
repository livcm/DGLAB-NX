#!/usr/bin/env python3
"""Round-trip check for nso2elf.py using the NSO this repository builds.

`make -C sysmodule package` produces DGLAB-NX-Core.elf, and the libnx tool
chain turns it into DGLAB-NX-Core.nso. Converting that NSO back must reproduce
the original loadable segments byte for byte; if it does not, the converter is
wrong and must not be used on firmware modules.

    usage: selftest_nso2elf.py <sysmodule-dir> [workdir]
"""

import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

PT_LOAD = 1


def load_segments(path):
    with open(path, "rb") as handle:
        data = handle.read()

    _ident = data[:16]
    e_phoff, = struct.unpack_from("<Q", data, 0x20)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x36)

    segments = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = (
            struct.unpack_from("<IIQQQQQQ", data, off)
        )
        if p_type != PT_LOAD:
            continue
        segments.append(
            {
                "vaddr": p_vaddr,
                "memsz": p_memsz,
                "data": data[p_offset:p_offset + p_filesz],
            }
        )
    return segments


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    sysmodule = os.path.abspath(argv[1])
    work = os.path.abspath(argv[2]) if len(argv) > 2 else "/tmp/ble-re"
    os.makedirs(work, exist_ok=True)

    nso = os.path.join(sysmodule, "DGLAB-NX-Core.nso")
    elf = os.path.join(sysmodule, "DGLAB-NX-Core.elf")
    converted = os.path.join(work, "selftest-DGLAB-NX-Core.elf")

    subprocess.run(
        [sys.executable, os.path.join(HERE, "nso2elf.py"), nso, converted],
        check=True,
    )

    original = load_segments(elf)
    rebuilt = load_segments(converted)

    print("original PT_LOAD count=%d, rebuilt=%d" % (len(original), len(rebuilt)))
    if len(original) != len(rebuilt):
        print("FAIL: segment count differs")
        return 1

    failed = False
    for index, (want, got) in enumerate(zip(original, rebuilt)):
        same_vaddr = want["vaddr"] == got["vaddr"]
        same_memsz = want["memsz"] == got["memsz"]
        same_data = want["data"] == got["data"]
        print(
            "  segment %d: vaddr=0x%08X/0x%08X memsz=0x%X/0x%X data=0x%X/0x%X %s"
            % (
                index,
                want["vaddr"], got["vaddr"],
                want["memsz"], got["memsz"],
                len(want["data"]), len(got["data"]),
                "OK" if (same_vaddr and same_memsz and same_data) else "MISMATCH",
            )
        )
        if not (same_vaddr and same_memsz and same_data):
            failed = True

    print("FAIL" if failed else "PASS: NSO -> ELF round trip matches the original ELF")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
