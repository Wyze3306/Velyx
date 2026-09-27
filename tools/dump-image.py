#!/usr/bin/env python3
"""Dump the decrypted Minecraft image out of a running game.

The Minecraft.Windows.exe delivered by the Microsoft Store is encrypted on disk —
no MZ header, 8.00 bits of entropy per byte — so no amount of static analysis
gets anywhere near it. The licensing layer decrypts it into memory at load time,
which is the only place a signature pack can be worked out from.

Under Wine or Proton the game is an ordinary Linux process, so its memory is an
ordinary file. Reading it is read-only and does not touch the game: no ptrace
attach, no breakpoints, nothing written back. It needs ptrace_scope 0 or 1 with
the same user, which is the default on a desktop.

    ./tools/dump-image.py                 # find the game, dump beside this script
    ./tools/dump-image.py -o /tmp/mc.img

The result is a memory image: every section already sits at its virtual address,
so an RVA is also an offset into the file. That is what tools/find-signatures.py
assumes.
"""

import argparse
import os
import re
import subprocess
import sys

# The default image base of a 64-bit Windows executable. Bedrock is not relocated
# under Wine, so this holds; anything mapped here belongs to the game itself.
IMAGE_BASE = 0x140000000


def candidate_pids() -> list[int]:
    try:
        out = subprocess.run(["pgrep", "-af", "Minecraft.Windows.exe"],
                             capture_output=True, text=True).stdout
    except FileNotFoundError:
        return []

    pids = []
    for line in out.splitlines():
        pid, _, command = line.partition(" ")
        # The launcher wrapper's command line ends in the executable too, and so does
        # the shell that went looking. Name matching cannot separate them, so it does
        # not try: the caller keeps whichever one has the image mapped.
        if "Minecraft.Windows.exe" in command and pid.isdigit():
            pids.append(int(pid))
    return pids


def image_extent(pid: int) -> tuple[int, int] | None:
    """Where the game's image starts and ends, from the process map."""
    first = last = None
    with open(f"/proc/{pid}/maps") as maps:
        for line in maps:
            m = re.match(r"([0-9a-f]+)-([0-9a-f]+) ", line)
            if not m:
                continue
            start, end = int(m.group(1), 16), int(m.group(2), 16)
            if start < IMAGE_BASE or start >= IMAGE_BASE + (1 << 32):
                continue
            # The mapping is contiguous from the headers onwards; the first gap ends it.
            if first is None:
                first, last = start, end
            elif start == last:
                last = end
            else:
                break
    return (first, last) if first is not None else None


def dump(pid: int, start: int, end: int, path: str) -> int:
    written = 0
    mem = os.open(f"/proc/{pid}/mem", os.O_RDONLY)
    try:
        with open(path, "wb") as out:
            offset = start
            while offset < end:
                count = min(1 << 22, end - offset)
                try:
                    chunk = os.pread(mem, count, offset)
                except OSError:
                    chunk = b""
                # A hole reads as zeroes rather than shifting everything after it:
                # the whole point is that an RVA stays an offset.
                out.write(chunk + b"\x00" * (count - len(chunk)))
                written += count
                offset += count
    finally:
        os.close(mem)
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-p", "--pid", type=int, help="the game's process id")
    parser.add_argument("-o", "--output", default="mc.img", help="where to write the image")
    args = parser.parse_args()

    pids = [args.pid] if args.pid else candidate_pids()
    if not pids:
        print("no running Minecraft.Windows.exe found; start the game first", file=sys.stderr)
        return 1

    # The one with the image mapped is the game; the rest are its launcher.
    pid = extent = None
    for candidate in pids:
        try:
            found = image_extent(candidate)
        except PermissionError:
            print(f"pid {candidate}: no permission to read its memory", file=sys.stderr)
            continue
        except FileNotFoundError:
            continue
        if found:
            pid, extent = candidate, found
            break

    if not extent:
        print(f"none of {pids} has an image mapped at {IMAGE_BASE:#x}", file=sys.stderr)
        return 1

    start, end = extent
    size = dump(pid, start, end, args.output)

    with open(args.output, "rb") as image:
        head = image.read(0x40)
    if head[:2] != b"MZ":
        print("dumped, but there is no MZ header at the start — wrong region?", file=sys.stderr)
        return 1

    print(f"pid {pid}: {size / 1e6:.1f} MB from {start:#x} to {end:#x} -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
