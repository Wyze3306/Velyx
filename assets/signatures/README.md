# Signature packs

Velyx hard codes no game addresses. Everything it reads from Minecraft goes
through a symbolic name (`Actor::position`, `ClientInstance::instance`, and so
on) resolved at startup from a JSON file.

The point is that a Mojang update needs a new file here, not a rebuild.

## Where the file is looked up

In this order, and the three sources merge with the last one winning:

1. `<folder containing Velyx.dll>/assets/signatures/<major.minor>.json`, the pack
   shipped with a build;
2. `%APPDATA%/Velyx/assets/signatures/<major.minor>.json`, a pack the user
   dropped in;
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

## Writing a pack

`template.json` lists every expected name with empty patterns. Fill in the ones
you need; empty entries are ignored quietly.

To validate a pack, launch the game and open **Menu → Diagnostics**. Green rows are
resolved, red rows are required signatures that no longer match.
