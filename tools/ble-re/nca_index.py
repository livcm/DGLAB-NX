#!/usr/bin/env python3
"""Index the system NCAs of an installed emulator firmware by title ID.

Emulator firmware (Ryujinx and friends) stores every NCA as a directory of
numbered parts, which hactool cannot read directly. This script materialises
each NCA into one file and then asks hactool for its Title ID, content type and
SDK version, so a specific system module can be found without guessing hashes.

    nca_index.py --firmware ~/Library/Application\\ Support/Ryujinx/bis/system/Contents/registered \\
                 --keys <prod.keys> --hactool /tmp/ble-re/hactool/hactool \\
                 [--materialise /tmp/ble-re/ncas] [--grep 010000000000000b]

Without --materialise the script only reports what it finds, reusing files that
are already there. Firmware, keys and the extracted NCA files must never be
committed; see the .gitignore rules.
"""

import argparse
import os
import re
import subprocess
import sys


def materialise(source, target):
    parts = sorted(
        entry for entry in os.listdir(source) if re.fullmatch(r"[0-9]{2}", entry)
    )
    if not parts:
        return False
    with open(target, "wb") as out:
        for part in parts:
            with open(os.path.join(source, part), "rb") as chunk:
                out.write(chunk.read())
    return True


def nca_info(hactool, keys, path):
    result = subprocess.run(
        [hactool, "-k", keys, "-t", "nca", path],
        capture_output=True,
        text=True,
    )
    info = {}
    for line in result.stdout.splitlines():
        match = re.match(r"^(Title ID|Content Type|SDK Version|Distribution type):\s*(.*)$", line)
        if match:
            info[match.group(1)] = match.group(2).strip()
    return info


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--firmware", required=True)
    parser.add_argument("--keys", required=True)
    parser.add_argument("--hactool", required=True)
    parser.add_argument("--materialise", default=None)
    parser.add_argument("--grep", default=None)
    args = parser.parse_args()

    rows = []
    for entry in sorted(os.listdir(args.firmware)):
        if not entry.endswith(".nca"):
            continue

        source = os.path.join(args.firmware, entry)
        if not os.path.isdir(source):  # plain NCA files work as-is
            rows.append((entry, source))
            continue

        if args.materialise is None:
            print("skipping part directory %s (pass --materialise)" % entry, file=sys.stderr)
            continue

        os.makedirs(args.materialise, exist_ok=True)
        target = os.path.join(args.materialise, entry)
        if not os.path.exists(target) and not materialise(source, target):
            continue
        rows.append((entry, target))

    for entry, path in rows:
        info = nca_info(args.hactool, args.keys, path)
        title = info.get("Title ID", "?")
        kind = info.get("Content Type", "?")
        sdk = info.get("SDK Version", "?")
        line = "%s\t%s\t%s\t%s" % (title, kind, entry, sdk)
        if args.grep is None or args.grep.lower() in line.lower():
            print(line)


if __name__ == "__main__":
    main()
