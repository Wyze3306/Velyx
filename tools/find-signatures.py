#!/usr/bin/env python3
"""Explore a dumped Minecraft image to work out a signature pack.

Takes the memory image tools/dump-image.py produces and gives you the three
things a pack is actually written with:

    strings   <text>        where a string lives, and every place that reaches it
    xrefs     <rva|va>      every rip-relative reference to an address
    pattern   <rva|va>      an IDA byte pattern for the code at an address,
                            with the operands that move between builds wildcarded
    around    <rva|va>      disassembly either side of an address
    scan      <pattern>     how many times an IDA pattern matches, and where — a
                            pack entry is only good when the answer is one
    function  <rva|va>      the bounds of the function containing an address,
                            from .pdata, which is what an anchor resolves to

Bedrock ships with RTTI off, so there are no `.?AVClientInstance@@` descriptors to
walk. What clang did leave behind is better: assertion strings carrying the full
mangled name of the function that asserts, and lambda descriptors naming the
method they were written in. Those are the anchors — find the string, find what
points at it, and you are standing inside a named function.

    ./tools/find-signatures.py strings 'ClientInstance::requestLeaveGame'
    ./tools/find-signatures.py xrefs 0x14ef2b748
    ./tools/find-signatures.py pattern 0x140d40500
"""

import argparse
import re
import sys

try:
    import numpy as np
    import pefile
except ImportError as missing:  # pragma: no cover
    print(f"needs numpy and pefile ({missing})", file=sys.stderr)
    raise SystemExit(1)


class Image:
    def __init__(self, path: str):
        self.data = open(path, "rb").read()
        pe = pefile.PE(data=self.data, fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.sections = [(s.Name.rstrip(b"\x00").decode(errors="replace"),
                          s.VirtualAddress,
                          max(s.Misc_VirtualSize, s.SizeOfRawData))
                         for s in pe.sections]
        self.text = next(((va, size) for name, va, size in self.sections if name == ".text"), None)

    def section_of(self, rva: int) -> str | None:
        for name, va, size in self.sections:
            if va <= rva < va + size:
                return name
        return None

    def va(self, rva: int) -> int:
        return self.base + rva

    def rva(self, value: int) -> int:
        # Accepts either, so a virtual address pasted from a disassembler just works.
        return value - self.base if value >= self.base else value

    def xrefs(self, target_rva: int, limit: int = 64) -> list[int]:
        """Every place in .text holding a 32-bit displacement that lands on target_rva.

        Scanned as raw displacements rather than by disassembling 240 MB: an
        instruction referencing an address ends with `disp32` four bytes before the
        next instruction, whatever the opcode, so `p + 4 + disp == target` finds
        lea, mov, call and jmp in one pass.
        """
        if not self.text:
            return []
        start, size = self.text
        buf = self.data[start:start + size]
        found: list[int] = []

        for phase in range(4):
            tail = buf[phase:]
            tail = tail[:len(tail) - (len(tail) % 4)]
            values = np.frombuffer(tail, dtype="<i4")
            positions = np.arange(values.size, dtype=np.int64) * 4 + phase
            wanted = (target_rva - start) - (positions + 4)
            for index in np.nonzero(values == wanted)[0]:
                found.append(start + int(positions[index]))
                if len(found) >= limit:
                    return sorted(found)
        return sorted(found)


def cstring_at(image: Image, rva: int, limit: int = 400) -> bytes:
    end = image.data.find(b"\x00", rva, rva + limit)
    return image.data[rva:end if end >= 0 else rva + limit]


def cmd_strings(image: Image, args) -> None:
    needle = args.text.encode()
    for match in re.finditer(re.escape(needle), image.data):
        start = match.start()
        while start > 0 and 0x20 <= image.data[start - 1] < 0x7F and match.start() - start < 300:
            start -= 1
        text = cstring_at(image, start)
        print(f"\n{image.va(start):#x} [{image.section_of(start)}] {text[:180].decode(errors='replace')}")
        for ref in image.xrefs(start, limit=args.limit):
            print(f"    referenced from {image.va(ref):#x}")


def cmd_xrefs(image: Image, args) -> None:
    target = image.rva(int(args.address, 0))
    refs = image.xrefs(target, limit=args.limit)
    print(f"{len(refs)} reference(s) to {image.va(target):#x} [{image.section_of(target)}]")
    for ref in refs:
        print(f"    {image.va(ref):#x}")


def disassemble(image: Image, rva: int, count: int):
    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    except ImportError:
        print("needs capstone for disassembly", file=sys.stderr)
        return []
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    return list(md.disasm(image.data[rva:rva + count * 16], image.va(rva), count))


def cmd_around(image: Image, args) -> None:
    rva = image.rva(int(args.address, 0))
    for insn in disassemble(image, rva, args.count):
        print(f"{insn.address:#x}  {insn.bytes.hex():<24} {insn.mnemonic} {insn.op_str}")


def cmd_pattern(image: Image, args) -> None:
    """An IDA pattern for the code at an address.

    Anything rip-relative or absolute is wildcarded: those are exactly the bytes a
    rebuild moves, and leaving them concrete is how a pattern survives one update
    and dies on the next.
    """
    rva = image.rva(int(args.address, 0))
    parts: list[str] = []
    for insn in disassemble(image, rva, args.count):
        raw = insn.bytes
        wild = [False] * len(raw)
        for size in (4, 8):
            # A displacement or immediate of this size, at the tail of the instruction.
            if len(raw) >= size and ("rip" in insn.op_str or "0x1" in insn.op_str):
                for i in range(len(raw) - size, len(raw)):
                    wild[i] = True
                break
        parts += ["?" if w else f"{b:02X}" for b, w in zip(raw, wild)]
        if len(parts) >= args.bytes:
            break
    print(" ".join(parts[:args.bytes]))


def cmd_scan(image: Image, args) -> None:
    parts = []
    for tok in args.pattern.split():
        parts.append(b"." if tok in ("?", "??") else re.escape(bytes([int(tok, 16)])))
    rx = re.compile(b"".join(parts), re.DOTALL)
    start, size = image.text
    hits = [start + m.start() for m in rx.finditer(image.data[start:start + size])]
    print(f"{len(hits)} match(es)")
    for rva in hits[:args.limit]:
        bounds = function_bounds(image, rva)
        where = f" in {image.va(bounds[0]):#x}-{image.va(bounds[1]):#x}" if bounds else ""
        print(f"    {image.va(rva):#x}{where}")


def function_bounds(image: Image, rva: int):
    """(begin, end) RVAs of the .pdata entry covering rva, or None."""
    pdata = next(((va, size) for name, va, size in image.sections if name == ".pdata"), None)
    if not pdata:
        return None
    start, size = pdata
    entries = np.frombuffer(image.data[start:start + size - size % 12], dtype="<u4").reshape(-1, 3)
    entries = entries[entries[:, 0] != 0]
    order = np.argsort(entries[:, 0], kind="stable")
    begins = entries[order, 0]
    index = int(np.searchsorted(begins, rva, side="right")) - 1
    if index < 0:
        return None
    begin, end = int(entries[order[index], 0]), int(entries[order[index], 1])
    return (begin, end) if begin <= rva < end else None


def cmd_function(image: Image, args) -> None:
    rva = image.rva(int(args.address, 0))
    bounds = function_bounds(image, rva)
    if not bounds:
        print("no function covers that address")
        return
    print(f"{image.va(bounds[0]):#x}-{image.va(bounds[1]):#x} ({bounds[1] - bounds[0]} bytes)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-i", "--image", default="mc.img", help="the dumped image")
    parser.add_argument("--limit", type=int, default=32, help="most references to print")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("strings"); p.add_argument("text"); p.set_defaults(run=cmd_strings)
    p = sub.add_parser("xrefs");   p.add_argument("address"); p.set_defaults(run=cmd_xrefs)
    p = sub.add_parser("around")
    p.add_argument("address"); p.add_argument("-n", "--count", type=int, default=20)
    p.set_defaults(run=cmd_around)
    p = sub.add_parser("pattern")
    p.add_argument("address"); p.add_argument("-n", "--count", type=int, default=12)
    p.add_argument("-b", "--bytes", type=int, default=32)
    p.set_defaults(run=cmd_pattern)
    p = sub.add_parser("scan");    p.add_argument("pattern"); p.set_defaults(run=cmd_scan)
    p = sub.add_parser("function"); p.add_argument("address"); p.set_defaults(run=cmd_function)

    args = parser.parse_args()
    args.run(Image(args.image), args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
