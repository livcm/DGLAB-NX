#!/usr/bin/env python3
"""Print the size of libnx's btdrv/btm structs, to compare with the firmware's.

The firmware modules declare their own argument structs in the per-command
adapters ("copy N bytes from the request"), and on HOS 22.5.0 those sizes do not
always match the types libnx uses (`SetSysBluetoothDevicesSettings` is 0x200 in
libnx while the firmware's command 23 copies 0x2BE; `BtdrvBleAdvertisePacketData`
is 0xCC and ends up in a different command than libnx's). Measuring the libnx side
mechanically is what turns "this does not look right" into a table.

    abi_sizes.py [--devkitpro /opt/devkitpro]

It compiles a probe with the installed devkitA64 toolchain - the sizes come from
the headers, not from anyone's memory.
"""

import argparse
import os
import re
import subprocess
import tempfile

TYPES = [
    "SetSysBluetoothDevicesSettings",
    "BtdrvGattAttributeUuid",
    "BtdrvGattId",
    "BtdrvBleAdvertisePacketData",
    "BtdrvBleAdvertiseFilter",
    "BtdrvBleAdvertisePacketParameter",
    "BtdrvBleScanResult",
    "BtdrvBleConnectionInfo",
    "BtdrvChannelMapList",
    "BtdrvAdapterProperty",
    "BtdrvAdapterPropertySet",
    "BtmBleDataPath",
]

PROBE = """#include <switch.h>
%s
int main(void) { return 0; }
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--devkitpro", default=os.environ.get("DEVKITPRO", "/opt/devkitpro"))
    args = parser.parse_args()

    cc = os.path.join(args.devkitpro, "devkitA64", "bin", "aarch64-none-elf-gcc")
    readelf = os.path.join(args.devkitpro, "devkitA64", "bin", "aarch64-none-elf-readelf")
    inc = os.path.join(args.devkitpro, "libnx", "include")
    if not os.path.exists(cc):
        raise SystemExit("toolchain not found: %s" % cc)

    symbols = "\n".join(
        "char sz_%02d_%s[sizeof(%s)];" % (i, name, name) for i, name in enumerate(TYPES)
    )

    with tempfile.TemporaryDirectory() as work:
        src = os.path.join(work, "probe.c")
        obj = os.path.join(work, "probe.o")
        with open(src, "w") as handle:
            handle.write(PROBE % symbols)

        subprocess.run(
            [cc, "-std=gnu11", "-D__SWITCH__", "-I", inc, "-c", src, "-o", obj],
            check=True,
        )
        # -sW keeps long symbol names intact; the numeric prefix in each name is
        # what maps a symbol back to its type here.
        out = subprocess.run([readelf, "-sW", obj], capture_output=True, text=True, check=True).stdout

    sizes = {}
    for line in out.splitlines():
        match = re.search(r"^\s*\d+:\s+[0-9a-f]+\s+(\d+)\s+OBJECT\s+\S+\s+\S+\s+\d+\s+sz_(\d+)_", line)
        if match:
            sizes[int(match.group(2))] = int(match.group(1))

    for index in sorted(sizes):
        print("%-38s 0x%X" % (TYPES[index], sizes[index]))


if __name__ == "__main__":
    main()
