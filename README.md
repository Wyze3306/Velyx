<div align="center">

# Velyx

A utility client for Minecraft Bedrock Edition: an injected DLL plus a launcher
that runs several game instances side by side, each on its own Microsoft account.

[![build](https://github.com/Wyze3306/Velyx/actions/workflows/build.yml/badge.svg)](https://github.com/Wyze3306/Velyx/actions/workflows/build.yml)
[![licence](https://img.shields.io/badge/licence-GPL--3.0-3DDC84)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-0B1F17)](CMakeLists.txt)

**[wyze3306.github.io/Velyx](https://wyze3306.github.io/Velyx/)**

</div>

Velyx ships as a single `Velyx.exe`. The client, the fonts and the signature
template travel inside it and land in `%APPDATA%\Velyx` the first time you run
it, so there is no folder to keep together and nothing to install.

## What it does

**In game.** One menu with fuzzy search over every module and every setting, a
HUD editor with grid snapping, alignment guides and element groups, a live theme
editor, a `Ctrl+K` command palette, and a notification centre.

**Profiles.** Module state, HUD layout and theme travel together. Velyx switches
profile on its own based on the server you join, keeps restore points, and
exports a profile as a single line you can paste to a friend.

**45 modules.** Movement and camera, HUD readouts, a crosshair designer,
performance and battery modes, accessibility, screen filters including colour
blindness aids, screenshots with a thumbnail gallery, clip markers, benchmark,
privacy and streamer modes.

**Instances.** The launcher builds isolated copies of the game so you can run
several at once, one account each, with the client injected on launch.

It also keeps a crash report that names the module that was running, checks
GitHub for new releases, and walks you through a short setup on first launch.

## Signature packs

Velyx contains no Minecraft memory addresses. Everything it reads from the game
goes through a symbolic name resolved at startup from
`assets/signatures/<version>.json`, so a Bedrock update needs a new JSON file
rather than a rebuild. A missing signature disables the feature that needs it and
says so on the Diagnostics page instead of taking the game down.

Without a pack, everything that does not depend on the game still works: the
menu, themes, profiles, the HUD editor, FPS, CPS, clock, keystrokes, memory,
performance graph, screenshots, filters, playtime and the benchmark.

See [`assets/signatures/README.md`](assets/signatures/README.md) to write one.

## Running several accounts

Bedrock is an MSIX app, so Windows refuses to start two copies of it. The
launcher gets around that by giving each instance a genuinely distinct package
identity: hard linked game files, a rewritten `AppxManifest.xml`, and a
loose file registration. Windows then treats each one as a separate application
with its own data container, which is what gives it its own Xbox sign in.

Velyx stores no credentials and no tokens. An account in the launcher is a label
you attach to an instance; the sign in happens in the game and stays there.

Creating instances needs Windows Developer Mode enabled. The launcher checks
before you try.

## Not a cheat client

No killaura, no fly, no reach, no wallhack, no gameplay automation. The practice
modules time and count, they do not play for you. Every module declares what it
touches (network, files, synthetic input, game memory) and the menu shows that
before you enable it.

## Docs

[Building](docs/BUILDING.md) ·
[Architecture](docs/ARCHITECTURE.md) ·
[Roadmap](docs/ROADMAP.md)

Licensed under [GPL-3.0](LICENSE). No Flarial code was copied; that project is
AGPL-3.0 and was only used as a reference for which game functions a Bedrock
client has to reach. Bundled dependencies are MinHook (BSD-2-Clause) and
nlohmann/json (MIT).

Minecraft is a trademark of Mojang AB. Velyx is not affiliated with or endorsed
by Mojang or Microsoft.
