#!/usr/bin/env python3
"""Build (or inspect) an Atmosphère exefs IPS32 patch for a system module.

Atmosphère's loader applies `exefs_patches/<any-dir>/<module-id-hex>.ips` to the
mapped NSO while loading it (stratosphere/loader/source/ldr_patcher.cpp), after
the signature and segment-hash checks, so a patch needs no re-signing:

    NsoPatchesDirectory      = "exefs_patches"
    NsoPatchesProtectedSize  = sizeof(NsoHeader)   /* 0x100 */
    NsoPatchesProtectedOffset = sizeof(NsoHeader)  /* 0x100 */

and the patcher applies `mapped_module[ips_offset - 0x100]`, i.e.

    IPS 偏移 = 0x100 + 目标在 NSO 映射里的偏移

where the mapping starts at the text segment (text base = 0, exactly the
addresses the decompiler shows). The bytes must come from the *decompressed*
module - this repository gets them from the ELF that nso2elf.py produces, never
from the NSO file itself (its segments are LZ4 compressed).

    # Prepare a patch for the bluetooth module of HOS 22.5.0
    make_ips.py --elf bluetooth.elf --module-id c91c6fc8aa4c39222d6ccfe0fff468543105e59b \
                --edit 0x1c9c0:8b0300f9:0b0300f9 \
                --out atmosphere/exefs_patches/DGLAB-NX-BLE/<module-id>.ips

Every --edit is `address:old-hex:new-hex`; the old bytes are read from the ELF and
must match, which is what stops a patch from silently landing in the wrong place
when the firmware moves. `--verify <file.ips>` parses a patch back.
"""

import argparse
import os
import struct
import sys

PT_LOAD = 1


class Elf:
    """Minimal reader: enough to map an address to file bytes."""

    def __init__(self, path):
        with open(path, "rb") as handle:
            self.data = handle.read()
        e_phoff, = struct.unpack_from("<Q", self.data, 0x20)
        e_phentsize, e_phnum = struct.unpack_from("<HH", self.data, 0x36)
        self.segments = []
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            p_type, _flags, p_offset, p_vaddr, _paddr, p_filesz, _memsz, _align = struct.unpack_from(
                "<IIQQQQQQ", self.data, off
            )
            if p_type == PT_LOAD:
                self.segments.append((p_vaddr, p_offset, p_filesz))

    def read(self, vaddr, size):
        for vaddr_base, file_off, filesz in self.segments:
            if vaddr_base <= vaddr < vaddr_base + filesz:
                start = file_off + (vaddr - vaddr_base)
                return self.data[start:start + size]
        raise SystemExit("address 0x%X is not inside a loadable segment" % vaddr)


def parse_edits(values, elf=None):
    edits = []
    for value in values:
        parts = value.split(":")
        if len(parts) != 3:
            raise SystemExit("--edit wants address:old-hex:new-hex, got %r" % value)
        vaddr = int(parts[0], 16)
        old = bytes.fromhex(parts[1]) if parts[1] != "-" else None
        new = bytes.fromhex(parts[2])
        if len(new) == 0:
            raise SystemExit("--edit %s has no new bytes" % value)
        if old is not None and elf is not None:
            actual = elf.read(vaddr, len(old))
            if actual != old:
                raise SystemExit(
                    "0x%X: expected %s but the module has %s - wrong firmware build?"
                    % (vaddr, old.hex(), actual.hex())
                )
        edits.append((vaddr, old, new))
    return edits


def build_ips(edits, protected=0x100):
    out = bytearray(b"IPS32")
    for vaddr, _old, new in edits:
        offset = protected + vaddr
        if offset < protected:
            raise SystemExit("0x%X is inside the protected NSO header" % vaddr)
        if len(new) >= 0x10000:
            raise SystemExit("one --edit may change at most 0xFFFF bytes")
        out += struct.pack(">I", offset)
        out += struct.pack(">H", len(new))
        out += new
    out += b"EEOF"
    return bytes(out)


def parse_ips(data):
    """Parse back what build_ips produced (enough for the self test)."""
    if data[:5] != b"IPS32":
        raise SystemExit("not an IPS32 file")
    pos = 5
    records = []
    while True:
        if data[pos:pos + 4] == b"EEOF":
            break
        offset, = struct.unpack_from(">I", data, pos)
        size, = struct.unpack_from(">H", data, pos + 4)
        pos += 6
        if size == 0:
            rle_size, = struct.unpack_from(">H", data, pos)
            value = data[pos + 2]
            pos += 3
            records.append((offset, bytes([value]) * rle_size))
        else:
            records.append((offset, data[pos:pos + size]))
            pos += size
    return records


def module_id_bytes(text):
    raw = bytes.fromhex(text)
    if len(raw) > 0x20:
        raise SystemExit("module id is at most 0x20 bytes")
    return raw + bytes(0x20 - len(raw))


def self_test():
    edits = [(0x1234, None, bytes.fromhex("1f2003d5"))]
    data = build_ips(edits)
    records = parse_ips(data)
    assert records == [(0x100 + 0x1234, bytes.fromhex("1f2003d5"))], records
    # Trailing zeroes may be trimmed from the file name; the loader rebuilds them.
    assert module_id_bytes("c91c6fc8aa4c3922") == bytes.fromhex(
        "c91c6fc8aa4c3922" + "00" * 0x18
    )
    # An edit shorter than the original is fine too: only the listed bytes change.
    edits = [(0x40, None, bytes.fromhex("c0035fd6"))]
    assert parse_ips(build_ips(edits)) == [(0x140, bytes.fromhex("c0035fd6"))]
    print("self test: OK (offset 0x100 + address, IPS32 round trip, module id padding)")


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf")
    parser.add_argument("--module-id")
    parser.add_argument("--edit", action="append", default=[])
    parser.add_argument("--out")
    parser.add_argument("--verify")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv[1:])

    if args.self_test:
        self_test()
        return 0

    if args.verify:
        with open(args.verify, "rb") as handle:
            for offset, data in parse_ips(handle.read()):
                print("0x%08X  %s" % (offset, data.hex()))
        return 0

    if not (args.elf and args.module_id and args.edit):
        parser.error("need --elf, --module-id and at least one --edit (or --verify/--self-test)")

    elf = Elf(args.elf)
    edits = parse_edits(args.edit, elf)
    data = build_ips(edits)

    if args.out:
        out_dir = os.path.dirname(args.out)
        if out_dir:
            os.makedirs(out_dir, exist_ok=True)
        with open(args.out, "wb") as handle:
            handle.write(data)
        print("wrote %s (%d bytes, %d edit(s))" % (args.out, len(data), len(edits)))
        print("install as: <sd>:/atmosphere/exefs_patches/<any-name>/%s" %
              os.path.basename(args.out))
    else:
        sys.stdout.buffer.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
