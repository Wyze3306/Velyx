<div align="center">

# Velyx

A utility client for Minecraft Bedrock Edition: an injected DLL, plus a launcher
that runs several copies of the game side by side, each signed in to its own
Microsoft account.

[![build](https://github.com/Wyze3306/Velyx/actions/workflows/build.yml/badge.svg)](https://github.com/Wyze3306/Velyx/actions/workflows/build.yml)
[![licence](https://img.shields.io/badge/licence-GPL--3.0-3DDC84)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-0B1F17)](CMakeLists.txt)

**[wyze3306.github.io/Velyx](https://wyze3306.github.io/Velyx/)**

</div>

Velyx ships as a single `Velyx.exe`. The client, the fonts and the signature
template are packed inside it and get copied to `%APPDATA%\Velyx` the first time
you run it. There is nothing to install and no folder to keep next to the exe.

## What it does

**In game.** The interface is in English, and French comes as a translation table
(`assets/lang/fr.json`) you can pick from the menu. Adding a language means adding
a file. The menu has a fuzzy search over every module and every setting (`Ctrl+K`
opens it directly on the search), a HUD editor with grid snapping, alignment
guides and element groups, a live theme editor and a notification centre.

**Profiles.** A profile holds the modules and the HUD layout. The look of the
interface belongs to the client and stays the same whatever profile is active.
Four profiles come with it (Global, PvP, Performance, Survival). You can keep
restore points and export a profile or a theme as one line of text to send to a
friend.

**62 modules.** Movement and camera, HUD readouts, a crosshair designer,
accessibility options, screen filters (colour blindness aids included),
screenshots with a thumbnail gallery, clip markers, a benchmark, and privacy and
streamer modes. On top of that:

*Combat.* Hitboxes over players and mobs in four styles, nametags with health,
tracers, a target card with a health bar that trails the damage you just dealt, a
radar that turns with you, a hit marker with a streak counter, a reach readout,
and a low health warning around the edge of the screen.

*Performance.* A frame limiter that paces against the clock and can use a lower
cap when the window is in the background, process tuning (priority, performance
cores, 1 ms timer), a system monitor for processor, memory and video memory, and
an overlay cost readout that shows how many microseconds Velyx itself takes per
frame.

*The rest.* Fullbright done with the overlay's colour matrix instead of touching
the game, waypoints that stay visible at the edge of the screen, chat macros,
coordinates you can copy or post in chat with one key, and a badge next to other
people who use Velyx.

**Instances.** The launcher makes isolated copies of the game so you can run
several at the same time, each with its own account, and injects the client when
it starts them.

**Other Velyx users.** There is no server listing them and no account to create.
Velyx finds them in two ways. On a server, a client posts one line in the chat a
few seconds after joining. Other Velyx clients read it, hide it so nobody sees it,
and reply once if they have not posted yet. Instances running on your own machine
find each other through a local file, so nothing is sent at all. Everyone found
this way gets a badge before their name in the chat and on their nametag.

Velyx also writes a crash report that names the module that was running, checks
GitHub for new releases, and walks you through a short setup the first time.

## Signature packs

Velyx has no Minecraft memory addresses built in. Everything it reads from the
game goes through a name that is looked up at startup in
`assets/signatures/<version>.json`. When Bedrock updates, what needs to change is
that JSON file, not the client. If a signature is missing, the feature that needs
it turns off and the Diagnostics page says which one, and the game keeps running.

Packs are not in the repository. Each release has the pack for the game versions
it was tested on attached next to the exe: put it in
`%APPDATA%\Velyx\assets\signatures\`.

Without a pack, everything that does not read the game still works: the menu,
themes, profiles, the HUD editor, FPS, CPS, clock, keystrokes, memory, the
performance graph, screenshots, filters, fullbright, the frame limiter, process
tuning, the system monitor, playtime and the benchmark.

The whole Combat category depends on one entry, `Level::runtimeActorList`. The
camera used to draw on top of the world is a second one. With
`ClientInstance::viewMatrix` the projection is exact. Without it, Velyx rebuilds
one from the player's eye position and rotation, which is accurate in first person
and can be calibrated in the Hitboxes settings.

See [`assets/signatures/README.md`](assets/signatures/README.md) to write one.

## Running several accounts

Bedrock is an MSIX app, so Windows will not start two copies of it. The launcher
works around this by giving every instance its own package identity: hard linked
game files, a rewritten `AppxManifest.xml` and a loose file registration. Windows
then sees each copy as a separate app with its own data folder, and so with its
own Xbox sign in.

Velyx stores no passwords and no tokens. An account in the launcher is just a
label on an instance. You sign in inside the game, and the sign in stays there.

Creating instances needs Windows Developer Mode. The launcher checks for it
before you start.

## Where the line is

No killaura, no aim assist, no auto clicker, no fly, no reach extension, no
automation of any kind. Velyx does not play for you and does not change what the
server receives. The reach readout only measures your hits, it does not make them
longer. Each module lists what it touches (network, files, synthetic input, game
memory, clipboard, system) and the menu shows it before you turn the module on.

Only one module sends anything: `velyx_users` posts a single chat line when you
join a server. The server logs it like any other message, and players without
Velyx see it as plain text. The module is off by default, sending and listening
are two separate settings, and you can turn sending off and still see the others.

The Combat category does show information the game does not give you. Velyx
cannot read the world's collision, so it cannot know if an entity is behind a
wall: hitboxes, nametags and tracers are drawn from the entity's position, visible
or not. In multiplayer that is a real advantage, and servers that care about it
will treat it as one. None of it is on until you enable it, or pick the PvP
profile which does. The range and entity filters let you limit it to what you
actually want.

## Docs

[Building](docs/BUILDING.md) ·
[Architecture](docs/ARCHITECTURE.md) ·
[Roadmap](docs/ROADMAP.md)

Licensed under [GPL-3.0](LICENSE). No Flarial code was copied. That project is
AGPL-3.0 and was only used as a reference for which game functions a Bedrock
client needs to reach. Bundled dependencies are MinHook (BSD-2-Clause) and
nlohmann/json (MIT).

Minecraft is a trademark of Mojang AB. Velyx is not affiliated with or endorsed
by Mojang or Microsoft.
