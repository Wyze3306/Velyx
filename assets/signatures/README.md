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

The derived camera is why the Combat category works on a pack holding little
more than `Actor::position` and `Level::runtimeActorList`. Hitboxes carries an
**assumed field of view** and **eye height** under its advanced settings for
calibrating it; both disappear from the menu the moment `viewMatrix` resolves,
because nothing reads them any more.

The list is expected to be a `std::vector<Actor*>`: a begin pointer at the
offset and an end pointer eight bytes after it. Anything that does not look like
one — an end before the begin, a span that is not a multiple of eight, more than
four thousand entries — is treated as no list at all rather than walked.

## Writing a pack

`template.json` lists every expected name with empty patterns. Fill in the ones
you need; empty entries are ignored quietly.

To validate a pack, launch the game and open **Menu → Diagnostics**. Green rows are
resolved, red rows are required signatures that no longer match.
