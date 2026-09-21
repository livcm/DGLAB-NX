#!/usr/bin/env python3
"""Convert a Switch NSO0 (shared object) into an ELF64 file.

Only the Python standard library is used, and the LZ4 block decoder below is
written from the LZ4 block format, so the script also runs on machines without
lz4 tooling. `tools/ble-re/README.md` records how this was validated against
the NSO this repository builds itself.

Header layout follows hactool's `nso.h` (nso0_header_t) and is not guessed.

    usage: nso2elf.py <input.nso> <output.elf>
"""

import struct
import sys

NSO_MAGIC = 0x304F534E  # "NSO0"

PAGE = 0x1000

# ELF constants
ET_EXEC = 2
EM_AARCH64 = 183
PT_LOAD = 1
SHT_PROGBITS = 1
SHT_NOBITS = 8
SHT_STRTAB = 3


def lz4_decompress_block(src: bytes, expected: int) -> bytes:
    """Decode a raw LZ4 block (no frame header), as Nintendo's NSO0 uses."""
    dst = bytearray()
    pos = 0
    size = len(src)

    while pos < size:
        token = src[pos]
        pos += 1

        literal_len = token >> 4
        if literal_len == 15:
            while True:
                value = src[pos]
                pos += 1
                literal_len += value
                if value != 255:
                    break

        dst += src[pos:pos + literal_len]
        pos += literal_len

        if pos >= size:  # last block has no match part
            break

        offset = src[pos] | (src[pos + 1] << 8)
        pos += 2
        if offset == 0:
            raise ValueError("invalid LZ4 offset 0")

        match_len = token & 0x0F
        if match_len == 15:
            while True:
                value = src[pos]
                pos += 1
                match_len += value
                if value != 255:
                    break
        match_len += 4

        start = len(dst) - offset
        if start < 0:
            raise ValueError("LZ4 match before start of stream")
        for i in range(match_len):
            dst.append(dst[start + i])

    if len(dst) != expected:
        raise ValueError(
            "LZ4 size mismatch: got 0x%X, header says 0x%X" % (len(dst), expected)
        )
    return bytes(dst)


class Nso:
    def __init__(self, data: bytes):
        if len(data) < 0x100:
            raise ValueError("file too small for an NSO0 header")

        magic = struct.unpack_from("<I", data, 0)[0]
        if magic != NSO_MAGIC:
            raise ValueError("not an NSO0 file (magic 0x%08X)" % magic)

        self.version = struct.unpack_from("<I", data, 0x04)[0]
        self.flags = struct.unpack_from("<I", data, 0x0C)[0]
        self.build_id = data[0x40:0x60]
        self.compressed_sizes = struct.unpack_from("<III", data, 0x60)

        # segments[] : file_off, dst_off, decomp_size, align_or_total_size
        self.segments = []
        for i in range(3):
            self.segments.append(struct.unpack_from("<IIII", data, 0x10 + 0x10 * i))

        self.sections = []
        for index, (file_off, dst_off, decomp_size, extra) in enumerate(self.segments):
            compressed = (self.flags >> index) & 1
            blob_size = self.compressed_sizes[index] if compressed else decomp_size
            blob = data[file_off:file_off + blob_size]
            if len(blob) != blob_size:
                raise ValueError("segment %d is truncated" % index)

            if compressed:
                content = lz4_decompress_block(blob, decomp_size)
            else:
                content = blob

            self.sections.append(
                {
                    "index": index,
                    "vaddr": dst_off,
                    "content": content,
                    "size": decomp_size,
                    "bss": extra if index == 2 else 0,
                }
            )


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def build_elf(nso: Nso) -> bytes:
    # Section indices: 0 = null, then .text/.rodata/.data/.bss/.shstrtab.
    names = b"\0.text\0.rodata\0.data\0.bss\0.shstrtab\0"
    name_off = {name: names.index(name) for name in
                (b".text", b".rodata", b".data", b".bss", b".shstrtab")}

    headers_size = 64 + 56 * 3  # ELF header + 3 program headers
    offset = align_up(headers_size, 0x10)

    placed = []
    for section in nso.sections:
        content = section["content"]
        placed.append({"section": section, "offset": offset})
        offset = align_up(offset + len(content), 0x10)

    shstrtab_offset = offset
    offset = align_up(offset + len(names), 0x10)

    content_names = [b".text", b".rodata", b".data"]

    section_headers = []
    for entry in placed:
        section = entry["section"]
        section_headers.append(
            {
                "name": name_off[content_names[section["index"]]],
                "type": SHT_NOBITS if section["size"] == 0 else SHT_PROGBITS,
                "flags": 0x6 if section["index"] == 0 else (0x2 if section["index"] == 1 else 0x3),
                "addr": section["vaddr"],
                "offset": entry["offset"],
                "size": section["size"],
            }
        )

    # .bss carries only the zero-initialized tail of the rwdata segment.
    bss_size = nso.sections[2]["bss"]
    data_section = nso.sections[2]
    data_end = data_section["vaddr"] + data_section["size"]
    if bss_size:
        section_headers.append(
            {
                "name": name_off[b".bss"],
                "type": SHT_NOBITS,
                "flags": 0x3,
                "addr": data_end,
                "offset": shstrtab_offset,
                "size": bss_size,
            }
        )

    section_headers.append(
        {
            "name": name_off[b".shstrtab"],
            "type": SHT_STRTAB,
            "flags": 0,
            "addr": 0,
            "offset": shstrtab_offset,
            "size": len(names),
        }
    )

    shoff = align_up(shstrtab_offset + len(names), 0x8)
    shnum = 1 + len(section_headers)
    shstrndx = shnum - 1

    blob = bytearray()
    blob += b"\x7fELF"
    blob += bytes([2, 1, 1, 0])          # 64-bit, little-endian, current version
    blob += bytes(8)                      # padding
    blob += struct.pack(
        "<HHIQQQIHHHHHH",
        ET_EXEC,                       # e_type
        EM_AARCH64,                    # e_machine
        1,                             # e_version
        0,                             # e_entry
        64,                            # e_phoff
        shoff,                         # e_shoff
        0,                             # e_flags
        64,                            # e_ehsize
        56,                            # e_phentsize
        3,                             # e_phnum
        64,                            # e_shentsize
        shnum,                         # e_shnum
        shstrndx,                      # e_shstrndx
    )

    for entry in placed:
        section = entry["section"]
        blob += struct.pack(
            "<IIQQQQQQ",
            PT_LOAD,
            0x5 if section["index"] == 0 else (0x4 if section["index"] == 1 else 0x6),
            entry["offset"],
            section["vaddr"],
            section["vaddr"],
            len(section["content"]),
            section["size"] + section["bss"],
            PAGE,
        )

    for entry in placed:
        content = entry["section"]["content"]
        blob += bytes(entry["offset"] - len(blob))
        blob += content

    blob += bytes(shstrtab_offset - len(blob))
    blob += names

    blob += bytes(shoff - len(blob))
    blob += struct.pack("<IIQQQQIIQQ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    for header in section_headers:
        blob += struct.pack(
            "<IIQQQQIIQQ",
            header["name"],
            header["type"],
            header["flags"],
            header["addr"],
            header["offset"],
            header["size"],
            0, 0,
            0x10, 0,
        )

    return bytes(blob)


def main(argv):
    if len(argv) != 3:
        print("usage: nso2elf.py <input.nso> <output.elf>", file=sys.stderr)
        return 2

    with open(argv[1], "rb") as handle:
        data = handle.read()

    nso = Nso(data)
    print(
        "NSO0 v%d flags=0x%X build_id=%s"
        % (nso.version, nso.flags, nso.build_id.hex())
    )
    for section in nso.sections:
        print(
            "  segment %d: vaddr=0x%08X size=0x%X bss=0x%X"
            % (
                section["index"],
                section["vaddr"],
                section["size"],
                section["bss"],
            )
        )

    elf = build_elf(nso)
    with open(argv[2], "wb") as handle:
        handle.write(elf)
    print("wrote %s (%d bytes)" % (argv[2], len(elf)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
