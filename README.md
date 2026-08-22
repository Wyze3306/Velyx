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

**In game.** The interface is in English, with French shipped as a translation table
(`assets/lang/fr.json`) you pick from the menu — add a file to add a language. One
menu with fuzzy search over every module and every setting —
`Ctrl+K` opens it straight on that search — a HUD editor with grid snapping,
alignment guides and element groups, a live theme editor, and a notification
centre.

**Profiles.** A profile carries the modules and the HUD, and nothing else: how the
interface itself looks is the client's, the same whichever profile is active. Velyx
ships four to start from — Global, PvP, Performance, Survival — keeps restore points,
and exports either a profile or a theme as a single line you can paste to a friend.

**61 modules.** Movement and camera, HUD readouts, a crosshair designer,
accessibility, screen filters including colour blindness aids, screenshots with a
thumbnail gallery, clip markers, benchmark, privacy and streamer modes — plus:

*Combat.* Hitboxes over players and mobs in four styles, nametags with health,
tracers, a target card whose bar trails the damage you just did, a radar that
turns with you, a hit marker with a streak counter, a reach readout and a
low-health alert around the edge of the screen.

*Performance.* A frame limiter that paces against the clock and caps separately
when the window is not in front of you, process tuning (priority, performance
cores, one millisecond timer), a system monitor reading processor, memory and
video memory, and an overlay cost readout that says in microseconds what Velyx
itself is charging you per frame.

*The rest.* Fullbright done through the overlay's own colour matrix rather than
through the game, waypoints that keep pointing at a place from the edge of the
screen, chat macros, and coordinates copied or announced in one press.

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
performance graph, screenshots, filters, fullbright, the frame limiter, process
tuning, the system monitor, playtime and the benchmark.

The Combat category hangs off one entry, `Level::runtimeActorList`. The camera it
projects through is a second: with `ClientInstance::viewMatrix` the projection is
exact, and without it Velyx derives one from the player's eye and rotation, which
is accurate in first person and calibrated from the Hitboxes settings.

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

## Where the line is

No killaura, no aim assist, no auto-clicker, no fly, no reach extension, no
gameplay automation. Nothing here plays for you, and nothing here changes what
the server sees — the reach readout *measures* your hits, it does not lengthen
them. Every module declares what it touches (network, files, synthetic input,
game memory, clipboard, system) and the menu shows that before you enable it.

The Combat category does put information on screen that the game does not give
you. Velyx has no access to the world's collision, so it cannot tell whether an
entity is behind a wall: a hitbox, a nametag or a tracer is drawn from the
entity's position whether you can see it or not. That is a real advantage in
multiplayer, and servers that care will treat it as one. None of it is on until
you switch it on — or pick the PvP profile or preset, which does — and the range
and entity filters are there so you can keep it to what you actually want.

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
