#!/usr/bin/env python3
"""Find code that references a virtual address in an AArch64 listing.

Stripped Switch modules point at rodata with an `adrp` + `add` (or `adrp` +
`ldr` from a literal pool) pair, so a text search for the address never finds
anything. This scans a listing produced by the devkitA64 binutils:

    aarch64-none-elf-objdump -d --no-show-raw-insn module.elf > module.txt
    find_xref.py module.txt 0x11895b
    find_xref.py module.txt 0x118950-0x1189b0

The reported addresses are where the reference is built, which is enough to
jump to the surrounding function in a decompiler by hand or by script.
"""

import re
import sys

ADRP = re.compile(r"^\s*([0-9a-f]+):\s+adrp\s+(x\d+),\s*0x([0-9a-f]+)\s*$")
ADR = re.compile(r"^\s*([0-9a-f]+):\s+adr\s+(x\d+),\s*0x([0-9a-f]+)\s*$")
ADD = re.compile(r"^\s*([0-9a-f]+):\s+add\s+(x\d+),\s+(x\d+),\s*#(0x[0-9a-f]+|\d+)(,\s*lsl\s*#(\d+))?\s*$")
LDR = re.compile(r"^\s*([0-9a-f]+):\s+ldr\s+(x\d+),\s*\[(x\d+),\s*#(0x[0-9a-f]+|\d+)\]\s*$")


def main(argv):
    if len(argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    path = argv[1]
    spec = argv[2]
    if "-" in spec[1:]:
        low, high = spec.split("-")
        low, high = int(low, 16), int(high, 16)
    else:
        low = high = int(spec, 16)
    page = low & ~0xFFF

    pending = {}
    hits = []
    last_page = None

    with open(path, "r", errors="replace") as handle:
        for line in handle:
            match = ADR.match(line)
            if match:
                addr, reg, target_text = match.groups()
                value = int(target_text, 16)
                if low <= value <= high:
                    hits.append((addr, addr, "adr %s (0x%X)" % (reg, value)))
                continue

            match = ADRP.match(line)
            if match:
                addr, reg, adrp_page = match.groups()
                last_page = int(adrp_page, 16)
                if last_page <= high:
                    pending[reg] = (addr, 0, last_page)
                continue

            if not pending:
                continue

            for reg in list(pending):
                addr, steps, _ = pending[reg]
                if steps > 4:
                    del pending[reg]
                    continue
                pending[reg] = (addr, steps + 1, pending[reg][2])

            match = ADD.match(line)
            if match:
                addr, dst, src, imm, _shift, shift_bits = match.groups()
                if src in pending:
                    value = pending[src][2] + int(imm, 0) * (1 << int(shift_bits or 0))
                    if low <= value <= high:
                        hits.append((pending[src][0], addr, "adrp+add %s (0x%X)" % (dst, value)))
                    del pending[src]
                continue

            match = LDR.match(line)
            if match:
                addr, dst, src, imm = match.groups()
                if src in pending:
                    value = pending[src][2] + int(imm, 0)
                    if low <= value <= high:
                        hits.append(
                            (pending[src][0], addr, "adrp+ldr %s (0x%X)" % (dst, value))
                        )
                    del pending[src]

    if not hits:
        print("no reference to 0x%X-0x%X" % (low, high))
        return 1

    for adrp_addr, use_addr, how in hits:
        print("0x%s: %s (adrp at 0x%s)" % (use_addr, how, adrp_addr))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
