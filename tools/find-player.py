#!/usr/bin/env python3
"""Finds ClientInstance::localPlayer, and the Actor offsets that hang off it.

The keystone offset. Without it `Game::refreshPlayer` returns at its first line, so
every reading of the player is empty and every other Actor offset in the pack is dead
weight -- there is no object to apply it to.

The method is movement, because nothing static is decisive. A position is three floats,
and three floats that look like a position are everywhere in four gigabytes: a snapshot
alone offered eight hundred thousand candidates. A position that *moves the distance a
walking player moves*, in the seconds the player was walking, is one object.

    1. every 8-byte slot holding a pointer into the game image is a candidate head
    2. read the Vec3 at +0x588 of each, twice, with a walk in between
    3. keep the ones that travelled a walking distance and stayed in the world
    4. the survivor is the player; find which ClientInstance field points at it

Run it with the game up and the player standing in a world, then walk while it counts
down. See docs/ARCHITECTURE.md on game addresses, and assets/signatures/README.md.
"""

import argparse
import json
import math
import re
import struct
import subprocess
import sys
import time

# Proven on 1.26.44.3, and the one offset the whole method leans on. If a future build
# moves it, this tool finds nothing and says so rather than pointing somewhere wrong.
POSITION = 0x588

# Bedrock's image, wherever the loader put it. Anything a real object starts with lands
# in here, which is what separates an object head from three floats that happen to sit
# next to each other.
IMAGE_LO = 0x140000000
IMAGE_HI = 0x160000000

# Read off Flarial's open-source client and each one checked here against a running
# 1.26.44.3 before being trusted. They are facts about Minecraft's binary rather than
# anything of Flarial's, but that project is AGPL, so nothing of its code is used.
#
# levelRenderer earns its place twice over: it is null on a menu and set in a world, so
# it answers "is there anything to look at yet?" without needing the player at all --
# which is the question every failed hunt for the player turned out to be asking.
CI_LEVEL_RENDERER = 0x1B8
CI_MINECRAFT_GAME = 0x1A0
CI_GUI_DATA = 0x648
CI_SERVER_ADDRESS = 0x6D0


def find_pid() -> int:
    out = subprocess.run(["pgrep", "-f", r"Minecraft\.Windows\.exe"],
                         capture_output=True, text=True).stdout.split()
    if not out:
        sys.exit("the game is not running")
    # The launcher spawns helpers that match the same name; the real one is the last to
    # appear and the only one whose maps carry the image.
    for pid in reversed(out):
        try:
            with open(f"/proc/{pid}/maps") as maps:
                if "Minecraft.Windows.exe" in maps.read():
                    return int(pid)
        except OSError:
            continue
    return int(out[-1])


class Process:
    def __init__(self, pid: int):
        self.pid = pid
        self.mem = open(f"/proc/{pid}/mem", "rb", 0)
        self.regions = []
        for line in open(f"/proc/{pid}/maps"):
            parts = line.split()
            if "r" not in parts[1]:
                continue
            lo, hi = (int(x, 16) for x in parts[0].split("-"))
            if hi - lo <= (1 << 32):
                self.regions.append((lo, hi))

    def read(self, address: int, size: int):
        try:
            self.mem.seek(address)
            return self.mem.read(size)
        except OSError:
            return None

    def vec3(self, address: int):
        data = self.read(address, 12)
        if not data or len(data) < 12:
            return None
        return struct.unpack("<fff", data)


def game_string(proc, address: int) -> str | None:
    """An MSVC std::string, short form inline and long form behind a pointer."""
    data = proc.read(address, 32)
    if not data or len(data) < 32:
        return None
    size, capacity = struct.unpack_from("<QQ", data, 16)
    if size == 0 or size > 0x1000 or size > capacity:
        return None
    raw = proc.read(struct.unpack_from("<Q", data, 0)[0], size) if capacity > 15 \
        else data[:size]
    try:
        return raw.decode() if raw else None
    except UnicodeDecodeError:
        return None


def status(proc, instance: int) -> bool:
    """Says whether there is a world to look at. Returns True when there is."""
    word = proc.read(instance, 8)
    if not word:
        print(f"ClientInstance {instance:#x} is not readable -- stale address?")
        return False

    print(f"ClientInstance   {instance:#x}")
    print(f"server           {game_string(proc, instance + CI_SERVER_ADDRESS) or '-'}")

    for name, off in (("minecraftGame", CI_MINECRAFT_GAME), ("guiData", CI_GUI_DATA)):
        pointer = struct.unpack("<Q", proc.read(off + instance, 8) or b"\0" * 8)[0]
        print(f"{name:16s} {pointer:#x}" if pointer else f"{name:16s} null")

    renderer = struct.unpack("<Q", proc.read(instance + CI_LEVEL_RENDERER, 8)
                             or b"\0" * 8)[0]
    print()
    if not renderer:
        print("IN A WORLD:      no -- the level renderer is null.")
        print()
        print("Being connected to a server is not the same thing. Nothing about the")
        print("player exists in memory yet, so there is no offset to find and no HUD")
        print("that could show anything. Spawn in, then run this again.")
        return False

    print(f"IN A WORLD:      yes -- level renderer at {renderer:#x}")
    return True


def in_world(p) -> bool:
    x, y, z = p
    if not all(map(math.isfinite, (x, y, z))):
        return False
    if abs(x) > 3e7 or abs(z) > 3e7:
        return False
    return -64.0 <= y <= 320.0


def candidate_heads(proc: Process) -> list[int]:
    """Every slot holding a pointer into the game image, i.e. every plausible object."""
    try:
        import numpy as np
    except ImportError:
        sys.exit("needs numpy: pip install numpy")

    heads = []
    chunk = 16 << 20
    for lo, hi in proc.regions:
        address = lo
        while address < hi:
            size = min(chunk, hi - address) & ~7
            if size < 8:
                break
            buf = proc.read(address, size)
            if not buf or len(buf) < 8:
                address += size or 8
                continue
            words = np.frombuffer(buf[: len(buf) & ~7], dtype="<u8")
            hits = np.nonzero((words >= IMAGE_LO) & (words < IMAGE_HI))[0]
            heads.extend(address + int(i) * 8 for i in hits)
            address += size
    return heads


def snapshot(proc: Process, heads) -> dict[int, tuple]:
    shot = {}
    for head in heads:
        position = proc.vec3(head + POSITION)
        if position and in_world(position):
            shot[head] = position
    return shot


def client_instance(proc: Process, log: str) -> int | None:
    """Whatever Velyx said last. Cheaper and surer than finding it again."""
    try:
        text = open(log, encoding="utf-8", errors="replace").read()
    except OSError:
        return None
    found = re.findall(r"ClientInstance at (0x[0-9a-f]+)", text)
    return int(found[-1], 16) if found else None


def offsets_into(proc: Process, base: int, target: int, span: int) -> list[int]:
    data = proc.read(base, span)
    if not data:
        return []
    return [off for off in range(0, len(data) - 8, 8)
            if struct.unpack_from("<Q", data, off)[0] == target]


def name_offsets(proc: Process, obj: int, tag: bytes, span: int = 0x2000) -> list[int]:
    """Where an MSVC std::string holding the gamertag sits inside the object."""
    body = proc.read(obj, span)
    if not body:
        return []
    found = []
    start = 0
    while True:
        i = body.find(tag, start)
        if i < 0:
            return found
        if i + 32 <= len(body):
            size, capacity = struct.unpack_from("<QQ", body, i + 16)
            if size == len(tag) and capacity == 15:
                found.append(i)
        start = i + 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pid", type=int, default=0)
    ap.add_argument("--walk", type=float, default=12.0,
                    help="seconds to walk for (default 12)")
    ap.add_argument("--least", type=float, default=4.0,
                    help="blocks that must have been covered (default 4)")
    ap.add_argument("--most", type=float, default=400.0,
                    help="blocks past which it was a teleport, not a walk")
    ap.add_argument("--tag", default="", help="gamertag, to confirm and place nameTag")
    ap.add_argument("--log", default="", help="velyx.log, to read the ClientInstance from")
    ap.add_argument("--save", default="",
                    help="take the first snapshot into this file and stop, instead of "
                         "counting down. Walk, then rerun with --compare.")
    ap.add_argument("--compare", default="",
                    help="take the second snapshot and diff it against --save's file")
    ap.add_argument("--status", action="store_true",
                    help="just say whether there is a world loaded, and stop")
    args = ap.parse_args()

    pid = args.pid or find_pid()
    proc = Process(pid)
    print(f"game is pid {pid}, {len(proc.regions)} readable region(s)")

    instance = client_instance(proc, args.log) if args.log else None

    if args.status:
        if not instance:
            return print("pass --log <velyx.log> so the ClientInstance can be read") or 1
        return 0 if status(proc, instance) else 1

    # The hunt is pointless without a world, and saying so costs one read.
    if instance and not status(proc, instance):
        return 1

    print("\ncollecting candidates...", flush=True)
    heads = candidate_heads(proc)
    print(f"{len(heads)} object head(s)")

    if args.compare:
        before = {int(k): tuple(v) for k, v in json.load(open(args.compare)).items()}
        print(f"{len(before)} position(s) from the earlier snapshot")
    else:
        before = snapshot(proc, heads)
        print(f"{len(before)} carry something shaped like a position at +{POSITION:#x}")
        if not before:
            print("\nNothing to watch. Are you actually spawned in a world, not on a "
                  "loading or disconnect screen?")
            return 1

        if args.save:
            json.dump({str(k): list(v) for k, v in before.items()}, open(args.save, "w"))
            print(f"\nsaved to {args.save}. Walk, then rerun with --compare {args.save}")
            return 0

        print(f"\n>>> WALK NOW, in a straight line, for {args.walk:.0f} seconds <<<\n",
              flush=True)
        for left in range(int(args.walk), 0, -1):
            print(f"  {left}...", end="\r", flush=True)
            time.sleep(1)
        print("            ")

    movers = []
    for head, was in before.items():
        now = proc.vec3(head + POSITION)
        if not now or not in_world(now):
            continue
        travelled = math.dist(was, now)
        if args.least <= travelled <= args.most:
            movers.append((head, was, now, travelled))

    movers.sort(key=lambda m: -m[3])
    print(f"{len(movers)} object(s) moved a walking distance\n")
    for head, was, now, travelled in movers[:20]:
        print(f"  {head:#x}  ({was[0]:8.2f},{was[1]:7.2f},{was[2]:8.2f})"
              f" -> ({now[0]:8.2f},{now[1]:7.2f},{now[2]:8.2f})   {travelled:6.2f} m")

    if not movers:
        print("Nothing moved. Walk further, or raise --walk.")
        return 1

    if not instance:
        print("\nNo ClientInstance to match against: pass --log <velyx.log>.")
        return 0

    print(f"\nClientInstance {instance:#x}")
    print("\n-- which field points at a mover --")
    hit = False
    for head, _, _, travelled in movers:
        for off in offsets_into(proc, instance, head, 0x8000):
            hit = True
            print(f"  ClientInstance::localPlayer = {off}   ({off:#x})"
                  f"   -> {head:#x}, moved {travelled:.2f} m")
            if args.tag:
                for name in name_offsets(proc, head, args.tag.encode()):
                    print(f"    Actor::nameTag = {name}   ({name:#x})")

    if not hit:
        print("  none directly. The player is reached through something else here;\n"
              "  rerun with the movers above and sweep one level deeper.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
