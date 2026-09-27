# Signature packs

Velyx hard codes no game addresses. Everything it reads from Minecraft goes
through a symbolic name (`Actor::position`, `ClientInstance::instance`, and so
on) resolved at startup from a JSON file.

The point is that a Mojang update needs a new file here, not a rebuild.

## Where the file is looked up

In this order, and the three sources merge with the last one winning:

1. `<folder containing Velyx.dll>/assets/signatures/<major.minor>.json`, which
   is where a development build keeps its pack;
2. `%APPDATA%/Velyx/assets/signatures/<major.minor>.json`, where the launcher
   unpacks the template and where a pack you download goes;
3. `%APPDATA%/Velyx/config/signatures.json`, local overrides, handy for fixing a
   single entry without rewriting the pack.

`<major.minor>` comes from the version resource of the running
`Minecraft.Windows.exe`. For `1.21.44.1`, Velyx looks for `1.21.json`.

## Format

```jsonc
{
  "signatures": {
    // Short form: an IDA pattern, and the match address is the target.
    "LocalPlayer::sendChatMessage": "48 89 5C 24 ? 57 48 83 EC ?",

    // Long form: the pattern lands on an instruction that *references* the
    // target (call rel32, lea rip+disp32). Velyx follows the displacement.
    "ClientInstance::instance": {
      "pattern": "48 8B 0D ? ? ? ? 48 85 C9 74 ?",
      "kind": "relative",
      "operand": 3,   // distance from the instruction start to the disp32
      "length": 7,    // total instruction length
      "addend": 0     // added to the final address
    }
  },

  "offsets": {
    // Field offsets in bytes from the start of the object.
    "ClientInstance::localPlayer": 168,
    "Actor::position": 88
  }
}
```

`?` and `??` are wildcards. Case does not matter.

Two more kinds carry no byte of code at all, which is what makes them survive a
rebuild:

```jsonc
    // The function that carries an assertion: the text is found in .rdata, the one
    // instruction that references it in .text, and the function around it in the
    // exception table. Nothing in it moves when the game is rebuilt.
    "LevelRendererPlayer::setupCamera": {
      "kind": "anchor",
      "text": "void LevelRendererPlayer::setupCamera(mce::Camera &, const float)"
    },

    // A slot of a vtable another entry names, read once that entry has resolved.
    "GameMode::attack": { "kind": "vtable", "vtable": "GameMode::vtable", "slot": 14 }
```

A `pattern` may also be a list, tried in order until one matches exactly once —
for two builds of the same version, such as a release and a Preview. `"function":
true` on a pattern walks from the match to the start of the function containing it,
so a distinctive instruction in the middle of a function can name the function, and
`"addend"` moves the result by that many bytes, to land on an instruction inside the
match.

A pack covers a minor version, and a build inside it sometimes moves one thing and
nothing else. Entries under `builds`, keyed by the exact version the game reports,
are read over the shared ones when that build runs:

```jsonc
  "builds": {
    "1.26.51.1": {
      "signatures": {
        // slot 14 on 1.26.44/45, a thunk in slot 15 here
        "GameMode::attack": { "pattern": "4D 89 C1 41 B0 01 E9 ? ? ? ?", "kind": "relative", "operand": 7, "length": 11 }
      }
    }
  }
```

## What happens when an entry is missing

Nothing dramatic, and that is on purpose:

- a missing **optional** signature only disables the feature that needs it;
- a missing **required** signature starts the client in reduced mode: the menu,
  themes, profiles and every purely client side module (FPS, CPS, clock,
  keystrokes, performance graph and the rest) still work, while modules that
  read game state show `--`;
- the **Diagnostics** page lists every entry, its owner and the resolved address.
  That is where you start when filling in a pack.

A client that refuses to boot because the game moved by four bytes is a broken
client. This one tells you and carries on.

## What each optional entry buys

Most of the pack is one offset for one readout. A few entries are worth more
than the rest, because whole categories hang off them:

| Entry | Without it | With it |
| --- | --- | --- |
| `Level::runtimeActorList` | Nothing in **Combat** draws: no hitboxes, nametags, tracers, radar or target card | All of it |
| `Actor::entityTypeId` | Anything wearing a nametag counts as a player, everything else as unknown | Players, hostiles, passives, items and projectiles told apart, and filtered separately |
| `Actor::aabbDimensions` | Every box is drawn player sized, 0.6 by 1.8 | Boxes match the entity |
| `ClientInstance::viewMatrix` | The camera is derived from the player's eye, rotation and an assumed field of view. Accurate in first person; a few pixels off in third | Exact, whatever the camera is doing |
| `GuiData::addMessage` (or `GuiData::displayChatMessage` on older builds) | Nothing in Velyx hears the chat: the streamer mode filter passes everything, and other Velyx users can only be found through the launcher's own instances on this machine | The chat reaches the client, so a line can be read, badged, censored or dropped |
| `LocalPlayer::applyTurnDelta` | Turns are shaped at the input door — raw input under Wine, GameInput on Windows — where a count is only nominally a fraction of a degree, so SnapLook carries a calibration and FreeLook waits | Every turn is shaped in the game's own degrees, on every machine; SnapLook and FreeLook land exactly; and the player object announces itself on the first turn, so no `ClientInstance::localPlayer` offset is needed |
| `LevelRendererPlayer::getFov`, `::fovTangent` or `::setupCamera` | Zoom, Custom FOV and Dynamic FOV wait | All three work, and the derived camera projects through the field of view the game actually draws with |
| `Options::getViewPerspective` | The perspective stays the game's; FreeLook cannot switch to third person | The perspective is answered for every time the game asks |
| `GameMode::attack` | Hits are read off the entities' health, which needs the list and `Player::health`; the reach readout waits | A hit is heard the moment it registers: hit marker, reach, target card and hit colour work with no entity list at all |

The derived camera is why the Combat category works on a pack holding little
more than `Actor::position` and `Level::runtimeActorList`. Hitboxes carries an
**assumed field of view** and **eye height** under its advanced settings for
calibrating it; both disappear from the menu the moment `viewMatrix` resolves,
because nothing reads them any more.

The four hooks are functions Velyx stands on rather than reads, so their shape
matters as much as their address:

```cpp
void  LocalPlayer::applyTurnDelta(Vec2 const& delta)      // x pitch, y yaw, degrees
float LevelRendererPlayer::getFov(float, bool, ...)       // degrees; radians are converted
int   Options::getViewPerspective()                        // 0, 1 or 2
void  GameMode::attack(Actor& target, ...)                 // every register past the target is passed on whole
```

Newer builds have no `getFov`. `setupCamera` reads the field of view out of the
camera component and builds the projection from `tanf(fov / 2)` before handing the
projection on. `LevelRendererPlayer::fovTangent` names that call (`call [rip+x]`,
through the import of the C runtime's `tanf`): the hook stands on `tanf` and answers
differently only when the call returns into `setupCamera`, so the game builds its
projection with the field of view the handlers asked for. Rescaling the projection
after `setupCamera` returns, which is what `setupCamera` alone falls back to, came too
late on 1.26.51.1: the frame is drawn from the copy made inside the function.

What the game says nothing about — the player being hurt, dying, respawning, an
entity being hurt by someone else — is read off the health in the snapshot rather
than hooked, so it needs `Player::health` and, for entities, the list.

The chat is hooked rather than called, in one of two shapes. Older builds:

```cpp
void GuiData::displayChatMessage(std::string const& sender, std::string const& message)
```

Two strings, sender first, and it is the last thing that runs before the line is
drawn. The client reads both through the game's own `std::string` layout rather
than its own, so which compiler built `Velyx.dll` makes no difference.

From 1.26.50 every display call builds a `GuiMessage` and ends in:

```cpp
void GuiData::addMessage(GuiMessage message, int kind)   // by value: the callee destroys it
```

It needs `GuiMessage::author` and `GuiMessage::message` (the two strings inside the
message), `GuiMessage::~GuiMessage` (a dropped line is destroyed by the hook instead
of by `addMessage`) and `Bedrock::allocator` (the global the game's allocator lives
behind: a rewritten line has to be made of blocks the game can free).

The list is expected to be a `std::vector<Actor*>`: a begin pointer at the
offset and an end pointer eight bytes after it. Anything that does not look like
one — an end before the begin, a span that is not a multiple of eight, more than
four thousand entries — is treated as no list at all rather than walked.

## Writing a pack

`template.json` lists every expected name with empty patterns. Fill in the ones
you need; empty entries are ignored quietly.

To validate a pack, launch the game and open **Menu → Diagnostics**. Green rows are
resolved, red rows are required signatures that no longer match.

### Getting at the binary

`Minecraft.Windows.exe` as the Microsoft Store delivers it is **encrypted on
disk**: no MZ header, eight bits of entropy per byte, nothing to disassemble. The
licensing layer decrypts it into memory at load, and that is the only copy worth
looking at.

Under Wine or Proton the game is an ordinary Linux process, so:

```sh
./tools/dump-image.py -o mc.img      # with the game running
```

That reads `/proc/<pid>/mem` and nothing else — no ptrace attach, no breakpoints,
nothing written back, so it cannot disturb the session it reads. The result is a
*memory* image: every section already sits at its virtual address, so an RVA is
also an offset into the file.

### 1.26.51.1, the release of the 1.26.50 line

Laid out like the Preview, stripped of its assertions like the older release, and
worked out on 2026-09-27 against the running game with gdb's hardware watchpoints
rather than by reading code. Each entry was then checked by using it:

| | |
| --- | --- |
| `LocalPlayer` vtable | `0x14e8e1bc0`. The other player object in a world of your own, with a GameMode of its own, is the integrated server's (`0x14e83c0e0`) |
| `LocalPlayer::applyTurnDelta` | `0x1447617c0`, found by breaking there while the mouse moved: `[rdx]` read `(-0.1635, 0.4906)` for `(30, 10)` counts |
| `Options::getViewPerspective` | `0x140fb7470`, the only reader of the value F5 cycles, once a frame |
| `GuiData::addMessage` | `0x14155adc0`, the writer of the end of GuiData's message vector while a line was typed |
| `GameMode::attack` | slot 15, a thunk into `0x142599f10`; slot 14 is another use-on-entity routine, and the one that took the attack hook down with a pointer cut to a byte |
| `LevelRendererPlayer::fovTangent` | the `tanf` call at `0x1446d022b`; writing a third of the field of view into the camera component zoomed three times |

### 1.26.50.27 Preview is another build

Same version, different compiler flags: the Preview ships **without stack cookies**,
so every pattern that carried the cookie load (`48 8B 05 ? ? ? ? 48 33 C4`) stops
matching there, and the object layouts shift (`ClientInstance` stores its base
vtables at `+0x18`, `+0x20`, `+0x88`, `+0x90` instead of `+0x18` and `+0x80`;
`LevelRendererPlayer::cameraPos` sits at `0x654` instead of `0x704`). The pack
carries both constructor patterns as alternatives and leans on anchors and vtable
slots where it can. Found on that image, 2026-09-09:

| | |
| --- | --- |
| `ClientInstance` vtable | `0x150280110`, 421 slots; `requestLeaveGameAsync` and `requestLeaveGame` still at 14 and 15 |
| `LocalPlayer` vtable | `0x1501f30e0`, from the constructor named by its own assertion |
| `GameMode` vtable | `0x150133720`; `continueDestroyBlock` at 3, `attack` at 14 (`0x1428b3370`, builds an ItemUseOnEntity transaction) |
| `LevelRendererPlayer::setupCamera` | `0x144e95770`, reads the camera component (`fov` in radians at `+0x50`) and writes the projection to the top of the `mce::Camera` stack at `+0x80` |
| `LocalPlayer::applyTurnDelta` | not found: the turn goes through ECS systems on this build, and the ±90° pitch clamp turns up in `Mob::_applyYRot`, not in a method taking a `Vec2` |
| Perspective | lives in `MinecraftCamera::CameraPerspectiveOptionComponent`, read by the camera systems directly; no getter the game asks through |

### What is already known about 1.26.44.3

Worked out with the tools above, so the next attempt does not start from nothing.

| | |
| --- | --- |
| Image base | `0x140000000`, `.text` 240 MB, `.rdata` 50 MB, `.data` 8.5 MB |
| Functions | 905 854, with exact bounds, from `.pdata` |
| RTTI | **off** — no `.?AVClientInstance@@` descriptors; the usual vtable-by-RTTI route is closed |
| `ClientInstance` vtable | `0x14e774db0`, 421 virtual methods |
| `ClientInstance::requestLeaveGame` | slot 15, `0x140d404c0` |
| `ClientInstance::requestLeaveGameAsync` | slot 14, `0x140d402b0` |
| `ClientInstance::setupClientGame` | slot 29, `0x140d4a700` |
| Most-read field of `ClientInstance` | `+0x1a0`, read 48 times across the vtable; a sub-object with a 229-method vtable that most wrappers delegate through |

**There is no `ClientInstance::instance` global.** Verified against the running
game: exactly one object carries the vtable, zero pointers to it anywhere in the
image, 351 on the heap. It is owned by a `shared_ptr` inside `MinecraftGame`.
That is why the client finds it by scanning for the vtable instead.

**`localPlayer` is not a direct field of `ClientInstance`.** A histogram of every
`mov r64, [rcx+disp]` across all 421 virtual methods turns up no offset pointing
at an Actor-sized object. It is reached through a sub-object — `+0x1a0` is the
first place to look.

Four approaches that did **not** work, so as not to repeat them:

* diffing the heap for a float triple that moves like a walking player: four
  gigabytes contain thousands of decoys — render buffers, chunk grids at exact
  16-block steps, camera-relative entity positions;
* ranking heap objects by vtable length: the length walk over-counts, running off
  the end of a real vtable into adjacent pointer data;
* looking for `std::vector<Actor*>` by shape: thousands of homogeneous vectors
  match, and the real actor list holds *mixed* subtypes anyway, so a
  same-vtable test excludes it;
* tiny-getter extraction from the vtable: every short method is a forwarding
  wrapper that delegates, not a field accessor.

### Finding the anchors

Bedrock ships with RTTI off, so there are no `.?AVClientInstance@@` descriptors to
walk — that route, which most guides start from, is closed. What clang did leave
behind is better: assertion strings carrying the full mangled name of the function
that asserts.

```sh
./tools/find-signatures.py strings 'ClientInstance::requestLeaveGame'
#   0x14ef2b748 [.rdata] virtual void __cdecl ClientInstance::requestLeaveGame(bool, bool)
#       referenced from 0x140d4054b
```

One reference, unambiguous, and it is *inside* the named function. From there:

```sh
./tools/find-signatures.py around 0x140d4054b -n 40    # read the code
./tools/find-signatures.py xrefs 0x140d40000           # who reaches this
./tools/find-signatures.py pattern 0x140d40000         # an IDA pattern, operands wildcarded
```

`xrefs` scans raw 32-bit displacements rather than disassembling 240 MB, so it
catches `lea`, `mov`, `call` and `jmp` alike in one pass over the section.

Wildcard every rip-relative displacement and every absolute address in a pattern.
Those are precisely the bytes a rebuild moves, and leaving them concrete is how a
pattern survives one update and dies on the next.
