#!/usr/bin/env python3
"""Inspect mapped data inside an ELF produced by nso2elf.py.

    peek.py module.elf 0x61150            # hexdump around one address
    peek.py module.elf 0x61150-0x61190    # hexdump of a range
    peek.py module.elf 0x61150 --pointers # resolve 8-byte values to strings

This exists because the modules are stripped: reading a table means asking
"what is at this address" over and over, and the devkitA64 binutils cannot map
a virtual address back to a file offset for us.
"""

import struct
import sys

PT_LOAD = 1


class Elf:
    def __init__(self, path):
        with open(path, "rb") as handle:
            self.data = handle.read()

        e_phoff, = struct.unpack_from("<Q", self.data, 0x20)
        e_shoff, = struct.unpack_from("<Q", self.data, 0x28)
        e_phentsize, e_phnum = struct.unpack_from("<HH", self.data, 0x36)
        e_shentsize, e_shnum = struct.unpack_from("<HH", self.data, 0x3A)

        self.segments = []
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            p_type, p_flags, p_offset, p_vaddr, _paddr, p_filesz, p_memsz, _align = (
                struct.unpack_from("<IIQQQQQQ", self.data, off)
            )
            if p_type == PT_LOAD:
                self.segments.append((p_vaddr, p_offset, p_filesz, p_memsz))

        self.sections = []
        for i in range(e_shnum):
            off = e_shoff + i * e_shentsize
            name, kind, flags, addr, offset, size = struct.unpack_from("<IIQQQQ", self.data, off)
            self.sections.append((name, kind, flags, addr, offset, size))

    def to_offset(self, vaddr):
        for vaddr_base, file_off, filesz, _memsz in self.segments:
            if vaddr_base <= vaddr < vaddr_base + filesz:
                return file_off + (vaddr - vaddr_base)
        return None

    def read(self, vaddr, size):
        off = self.to_offset(vaddr)
        if off is None:
            return None
        return self.data[off:off + size]

    def cstring(self, vaddr, limit=64):
        off = self.to_offset(vaddr)
        if off is None:
            return None
        end = self.data.find(b"\0", off)
        if end < 0 or end - off > limit:
            return None
        raw = self.data[off:end]
        if not raw or any(byte < 32 or byte > 126 for byte in raw):
            return None
        return raw.decode("ascii")


def hexdump(elf, low, high):
    start = low & ~0xF
    for vaddr in range(start, high + 1, 16):
        chunk = elf.read(vaddr, 16)
        if chunk is None:
            print("0x%08x  <unmapped>" % vaddr)
            continue
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print("0x%08x  %-47s  %s" % (vaddr, " ".join("%02x" % b for b in chunk), text))


def pointers(elf, low, high):
    for vaddr in range(low, high + 1, 8):
        raw = elf.read(vaddr, 8)
        if raw is None:
            continue
        value, = struct.unpack("<Q", raw)
        text = elf.cstring(value) if value else None
        if value and (text is not None or elf.to_offset(value) is not None):
            print("0x%08x -> 0x%016x %s" % (vaddr, value, ('"%s"' % text) if text else ""))


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    elf = Elf(argv[1])
    spec = argv[2]
    if "-" in spec:
        low, high = (int(part, 16) for part in spec.split("-"))
    else:
        low = high = int(spec, 16)
        if len(argv) == 3:
            high = low + 0x3F

    if "--pointers" in argv:
        pointers(elf, low, high)
    else:
        hexdump(elf, low, high)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
