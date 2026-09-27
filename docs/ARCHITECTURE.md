# Architecture

How the pieces fit, and why. Most of the decisions that look arbitrary come from
what Bedrock actually is: an MSIX application, rendering through D3D12, whose
addresses move every six weeks.

## Overview

```
                    ┌─────────────────────────────┐
   Velyx.exe ──────▶│  Minecraft.Windows.exe      │
   (launcher)       │                             │
   creates the      │   ┌─────────────────────┐   │
   instance and     │   │      Velyx.dll      │   │
   injects the DLL  │   └─────────────────────┘   │
                    └─────────────────────────────┘
```

The launcher and the client share `velyx_core` (logging, path layout, colour,
strings, processes). Nothing in `core` knows about the game, which is what lets
the launcher link it without pulling in the whole client.

## Startup

`DllMain` does one thing: create a thread. Everything else, creating a D3D
device, scanning memory, touching the disk, would run under the loader lock and
freeze the game.

```
DllMain
  └─ thread → Velyx::start()
       ├─ Paths::ensureLayout()          %APPDATA%/Velyx tree
       ├─ Log::init()                    file, optional console
       ├─ ClientConfig::load()           plus previous-crash detection
       ├─ crash::install()               unhandled exception filter
       ├─ sdk::bindGame()                declares the signatures it needs
       ├─ Signatures::resolveAll()       scan, or reuse the disk cache
       ├─ ThemeManager::load()
       ├─ bindServices()                 clicks, frame times, stats, privacy
       ├─ bindPresence()                 who else here is running Velyx
       ├─ ModuleManager::initialize()    builds the catalogue
       ├─ ProfileManager::load()         and applies the active profile
       └─ HookManager::installAll()      swapchain, window, input, chat
```

From then on the client lives on the game's render thread, inside the Present
detour.

## The frame

```
Present (detour)
  └─ Velyx::onPresent
       ├─ GraphicsContext::attach()      idempotent
       ├─ WindowHook::attach()           first frame only
       ├─ delta time, smoothed FPS
       ├─ emit FrameEvent                SDK, services, animations
       ├─ GraphicsContext::beginFrame()  acquires the back buffer
       │    ├─ emit RenderEvent          HUD elements
       │    └─ emit RenderTopEvent       menu, HUD editor, notifications
       └─ GraphicsContext::endFrame()    presents and flushes
```

Splitting `RenderEvent` from `RenderTopEvent` is not cosmetic. Notifications and
the menu have to sit above the HUD, and nothing else guarantees that ordering.

## The overlay, and why D3D11On12

Bedrock renders with D3D12. Direct2D cannot draw onto a D3D12 resource, so the
chain is:

```
ID3D12Resource (back buffer)
      │  D3D11On12Device::CreateWrappedResource
      ▼
ID3D11Resource ──QueryInterface──▶ IDXGISurface
      │  ID2D1DeviceContext::CreateBitmapFromDxgiSurface
      ▼
ID2D1Bitmap1  ← what Velyx draws into
```

Two details are expensive to get wrong:

* **The command queue has to be the game's.** D3D11On12 requires one, and
  creating our own deadlocks on present. That is why
  `ID3D12CommandQueue::ExecuteCommandLists` is hooked: it is the only place the
  game shows us its queue.
* **Wrapped resources must be released before `ResizeBuffers`,** otherwise DXGI
  refuses the resize. The `ResizeBuffers` detour calls `releaseTargets()` first.

A resource is acquired for the duration of the overlay pass and handed back
immediately after, so the game's own command lists never notice.

There is also a plain D3D11 path, used when the swapchain is a normal D3D11 one.

## Game addresses

Nothing is hard coded. `Signatures` is a registry: features declare what they
need with `require("Actor::position")`, the byte patterns come from JSON, and the
result is cached on disk keyed by game build plus pattern fingerprint. A full
`.text` scan takes a few milliseconds thanks to anchoring `memchr` on the first
concrete byte, and later launches skip the scan entirely.

Failure is always local:

* an optional signature missing means its feature goes quiet;
* a required one missing means reduced mode, a clear message, and the client
  still starts;
* an invalid pointer at runtime means `memory::readable()` returns a default
  instead of dereferencing.

`sdk::Game` is the only façade that reads the game. Modules never read memory
themselves; they read `game().player()` or `game().world()`, refreshed once per
frame.

## The world snapshot

Three things sit behind that façade, refreshed in this order at the top of every
frame:

```
FrameEvent
  ├─ Game::onFrame        (First)  client instance, player, world
  ├─ Camera::onFrame      (First)  view-projection, camera basis
  └─ Entities::onFrame    (High)   the level's actor list, near to far
```

`Entities` copies the list out rather than handing modules a pointer into it. That
is not caution for its own sake: eight modules drawing over the same entities read
it once between them, and an entity that dies halfway through the frame cannot
take a module down with it. The list is capped, sorted by distance and validated —
an end pointer before the begin, a span that is not a multiple of eight, a position
outside the world: each one means no list at all rather than a walk into nothing.

`Camera` answers `project(world) -> screen` two ways. With
`ClientInstance::viewMatrix` in the pack it multiplies through the game's own
matrix and is exact whatever the camera is doing. Without it, it builds a basis
from the player's eye, yaw and pitch and an assumed field of view. The second is a
few pixels off in third person and right in first — which is the difference
between a category that needs one more signature to exist and one that needs two.
`exact()` says which one answered, and the Hitboxes calibration settings hide
themselves the moment it returns true.

## Events

`EventBus` is typed, synchronous, priority ordered and cancellable.

Two properties are worth knowing:

1. **Handlers run without the lock.** A module may subscribe, unsubscribe or emit
   from inside a handler; mutations are queued and applied once the emit
   finishes.
2. **A disabled module costs nothing.** `Module::on()` registers a subscription
   *factory*, not the subscription. It is created on enable and destroyed on
   disable, which is what makes a large catalogue viable.

## Finding other Velyx users

There is no server behind Velyx, no account and no directory, so the question
"who else here is running this?" has only two honest answers, and `Presence`
holds both.

```
Presence
 ├─ chat handshake        [Velyx] <version> <name>, said once per world
 │    ├─ sent   sdk::Game::sendChat, six seconds after joining
 │    └─ heard  ChatReceiveEvent, cancelled so nobody ever sees it
 └─ the roster            %APPDATA%/Velyx/cache/presence/<pid>.json
      ├─ written every three seconds by every running client
      └─ read on the same tick, entries older than fifteen seconds ignored
```

The two differ in more than mechanism. The handshake reaches anyone on the
server and costs a chat message the server logs; the roster reaches only the
instances this launcher started on this machine and costs nothing at all. Both
are off until the `velyx_users` module is on, and the setting that *sends* is a
separate one from the setting that *listens*.

Three things are worth knowing:

* **Answering is what makes one line enough.** Whoever arrives says it, whoever
  was already there says it back — once, because a client that has already
  spoken in this world stays quiet. A server with ten Velyx users costs ten
  lines, not a hundred.
* **Nothing crosses a thread boundary un-owned.** The handshake is heard on the
  game's thread inside the chat detour, the roster on its own thread, and both
  do no more than push a name into a queue behind a mutex. `PeerFoundEvent` is
  emitted from the frame, which is the only place a handler may notify or draw.
* **The registry answers one question**, `knows(name)`, and does not decide what
  that looks like. The chat inserts a formatted tag in front of the sender; the
  nametag draws a coloured chip inside the pill and shifts the name to make room.
  Neither knows about the other.

## Chat

`ChatHook` stands on the last call before a received line is drawn and turns it
into `ChatReceiveEvent`. On older builds that is `GuiData::displayChatMessage`,
which takes the sender and the message as two `const&` strings. From 1.26.50 every
display call builds a `GuiMessage` and passes it by value to `GuiData::addMessage`,
which destroys it: the hook reads the two strings out of it, destroys a dropped line
itself with the game's own `GuiMessage` destructor, and gives a rewritten line new
strings from the game's heap (`memory::assignGameString`, through the allocator the
pack names), because the game is the one that frees them. Either way the sender and
the message are both rewritable, which is what lets a handler badge a name, censor a
word, or drop the line by cancelling; anything a handler leaves alone reaches the
game byte for byte.

Two details, both learned the hard way:

* **The game's `std::string` is not necessarily ours.** Bedrock is built with
  MSVC and `Velyx.dll` may not be, so every string crossing the line goes
  through `memory::readString` and `memory::GameString`, which lay out sixteen
  bytes of buffer, a size and a capacity by hand. Passing the client's own
  `std::string` across worked only as long as both were built by the same
  compiler.
* **The detour runs on the game's thread.** An exception escaping it reaches no
  handler at all and ends the process through `std::terminate`, so every
  listener runs inside a `try` and a throw costs the line its rewrite, nothing
  more.

## The game's own functions

Five hooks stand on functions of the game itself, each behind a pack entry and each
optional: missing, it costs the modules that listen for its event and nothing else.
`hooks::declareSignatures` names them to the registry before the pack is read, so
the Diagnostics page lists them with everything else.

| Hook | Stands on | Emits |
| --- | --- | --- |
| `TurnHook` | `LocalPlayer::applyTurnDelta` | `TurnDeltaEvent`, in the game's own degrees; also names the player object |
| `FovHook` | `LevelRendererPlayer::getFov` where a build has it; otherwise the C runtime's `tanf`, answering only the two calls the camera takes the tangent of the field of view through; `LevelRendererPlayer::setupCamera` as a last resort | `FovEvent`, in degrees whichever |
| `PerspectiveHook` | `Options::getViewPerspective`, which the camera asks once a frame | `PerspectiveEvent` |
| `AttackHook` | `GameMode::attack`, a vtable slot, or the function behind the slot where a build moved it | nothing itself: it hands the target to `sdk::Signals` |
| `ChatHook` | `GuiData::addMessage` or `GuiData::displayChatMessage`, above | `ChatReceiveEvent` |

The camera door deserves a word. Since 1.21.100 the game does not ask anyone what
field of view to draw with: a system computes it into `CameraComponent`, and two
functions build a projection from it, `setupCamera` and the one it hands the
component to, which builds the projection the frame is actually drawn with. Both
take `tanf` of half the field of view through the same import. So the hook stands on
`tanf` and answers differently only for those two callers, recognised by the address
they return to: the game builds both projections, near and far planes and all, with
the field of view the handlers asked for. The first attempt rescaled the matrix
after `setupCamera` returned, which reads correctly in the log and changes nothing
on screen, because the frame is drawn from the copy made inside it; that road is
kept only for a pack that names `setupCamera` and nothing better.

What the game says nothing about is read rather than heard. `sdk::Signals` watches
the snapshot every frame: the player's health dropping is `HurtEvent`, reaching zero
`DeathEvent`, coming back `RespawnEvent`; an entity's health dropping is
`ActorHurtEvent`; the screen name changing is `ScreenChangeEvent`; and twenty times a
second there is a `TickEvent`. The one thing heard is the hit: `GameMode::attack`
runs on the game's thread the moment a hit registers, the hook only remembers the
target, and the next frame turns it into `AttackEvent` and `ActorHurtEvent` with an
entity a handler can draw over — this frame's snapshot entry, or a copy read off the
address the game named when the pack cannot walk the list.

### Three roads to the player

`ClientInstance::localPlayer` is an offset no pack carries yet, so the SDK takes
whichever road the pack opens: the offset; the turn hook, which is called on the
LocalPlayer and names it on the first turn; the LocalPlayer's own vtable, which a
thread of its own looks for on the heap while the player is wanted and unknown; and
the GameMode, which keeps its player at `+8` and names it on the first hit. All four
land in `Game::adoptLocalPlayer`, which keeps the object for as long as it still
starts with the vtable it had.

### What survives a rebuild

A pattern entry may be a list of alternatives, tried until one matches exactly once,
which is how one `1.26.json` covers a release and a Preview. Two kinds carry no byte
of code at all: an **anchor** names the function that carries an assertion string —
the string is found in `.rdata`, the one instruction referencing it in `.text`, and
the function around it in the exception table — and a **vtable slot** reads an entry
of a vtable another signature names. `"function": true` walks from any match to the
start of its function, so a distinctive instruction in the middle of one can name it.

Two facts about the builds Velyx runs on, learned on 2026-09-09 and 2026-09-12: the
Xbox app builds (`gamecore_x64_desktop`) are compiled almost without stack cookies,
so every published pattern that carries the cookie load fails on them; and the
release strips its assertion strings while the Preview keeps them, so anchors work on
the Preview and patterns are the only way in on the release.

## The mouse

Every turn the player makes passes through `velyx::turn` before the game hears about
it. What made that worth building as its own thing is that the *door* is not the same
on every machine:

```
Windows            IGameInputReading::GetMouseState   running totals
WineGDK            GetRawInputData / GetRawInputBuffer per-event deltas
                                  │
                                  ▼
                          velyx::turn::shape
                       ├─ emit TurnDeltaEvent          degrees, one constant each way
                       ├─ cancelled → nothing moves
                       ├─ + whatever a module asked for after the event, never scaled
                       └─ keep the fraction owed
```

Bedrock reads the mouse through Microsoft GameInput on Windows. Under WineGDK the
redistributable is present and answers — the client can even create an instance and
patch the reading's vtable off it — but the game never asks it for anything, taking its
mouse off the message pump instead. `GameInputHook` and `UserInputHook` therefore both
hand their movement to the same shaper, and a module scales, smooths or cancels a turn
without knowing which door was used.

That distinction is the trap, and it cost a wrong diagnosis once: **being able to hook a
door is not the game walking through it.** The log line that says the readings will come
out empty is written by the probe at install time and proves only that the
redistributable is here. Only a reading the game itself asked for calls `markCarried`,
and only that makes `carried()` true — which is what the sensitivity multiplier, the
cinematic camera and SnapLook check before claiming they can do anything.

Neither door needs a signature. GameInput belongs to the redistributable and raw input
to user32, so nothing here moves when the game rebuilds — which is why these modules
work on a build whose pack is otherwise empty.

Four details are load-bearing:

* **The movement comes from the device, never from what the game was last told.**
  GameInput hands out running totals, so the delta is taken against the device's own
  previous total; feeding a scaled turn back in as the next baseline compounds the
  multiplier every frame.
* **Fractions are kept.** Whole counts are all a reading or a raw input message can
  carry, so a multiplier below one would round every small movement to nothing and the
  mouse would feel stuck rather than slow. The remainder is paid into the next one.
* **An untouched axis is passed through untouched.** Degrees are a `float`; an axis that
  goes out through the constant and back unchanged still loses a count to rounding, so
  the identity case is spotted and short-circuited.
* **An injected turn is added after the event.** A scripted about-face is an angle
  already decided on, and has no business passing under the player's own sensitivity
  multiplier on the way out.

What the constant cannot do is tell you what a count is worth. That is the game's own
sensitivity slider, and measuring it needs `Actor::rotation`, which no pack carries
yet. Nothing that only scales or smooths a turn is affected — the value cancels — so it
costs exactly one module, SnapLook, a calibration setting until the offset lands.

FreeLook stands on the same pipeline: while the key is held it records what the game
applies, last in line so that it records the shaped value, and when the key goes up it
asks for the exact opposite — through the game door in degrees, through an input door
in counts, and exact either way because the way back goes out through the same
constant the way out came in by. The automatic perspective still waits: the
perspective hook answers the game's question, but which state the player is in —
swimming, gliding, riding — is a flag no pack carries an offset for yet.

## Modules

```
Module                      identity, settings, keybind, permissions
 ├─ HudModule               placement, anchoring, rotation, opacity, group
 │   └─ TextHud             label/value rows, alignment, measurement
 │       ├─ FpsHud
 │       ├─ CpsHud
 │       └─ …
 ├─ Zoom, FreeLook, …       movement modules
 ├─ Hitboxes, Radar, …      combat modules, over sdk::Entities and sdk::Camera
 └─ ClickGui, HudEditor, …  client surfaces, marked essential
```

One trap is worth naming, because it is invisible until something depends on it:
`HudModule::onRender` calls `relevantNow()` first and only measures the element if
it says yes. Work that decides *whether* an element should be on screen therefore
cannot live in `contentSize()` — once faded out it would never be asked again.
`TargetHud` resolves its target on `FrameEvent` for exactly that reason.

`HudModule` implements placement once for everyone. A subclass answers "how big
are you?" and "draw yourself here", and inherits the rest. `TextHud` goes further
for the common case. That is why adding a HUD readout is twenty lines and why
they all line up to the pixel when stacked.

Settings are described `std::variant`s (label, range, unit, visibility
condition, keywords). That single description drives the menu, the JSON round
trip and the search, so there is no second list to keep in sync.

## Profiles

A profile holds every module's state and settings, and the HUD layout. It does not
hold the theme or the interfaces' own settings: those live in `config/client.json`
and stay put whichever profile is active. Switching disables everything and reloads,
so no state leaks between profiles — everything except the interfaces, whose being on
screen is session state and never travels in a profile.

* **Automatic switching**: a profile can declare substrings matched against the
  address and name of the server you join. Longest match wins; when nothing
  matches, the profile you chose stays. None of the profiles Velyx ships with
  declares any, so this only ever does what you asked it to.
* **Versioning**: every switch, import or reset writes a restore point into
  `profiles/<name>/versions/` first. Twenty are kept.
* **Sharing**: `VELYX1:<base64 json>`, one line, safe to paste into a chat.

## Interface

`Ui` is an immediate mode layer: the menu is rebuilt every frame. There is no
widget tree to keep in sync with the module list, so a module added by a plugin
shows up with no registration step.

What is retained is interaction state only: what is hovered, what is being
dragged, where each scroll area sits, and one animated value per widget. That
last one is what makes the interface move rather than merely redraw.

Widget ids are hashes of a name plus an index, so a row inside a loop keeps its
hover state from one frame to the next.

## Themes

`Theme` is a serialisable struct holding every visual decision: fourteen
colours, four shape values, typography, effects and animation speed. No drawing
code invents a colour or a radius; it all goes through `theme()`.

The surfaces are neutral graphite, not tinted. Mint is the accent and nothing
else uses it: an element is green because it is active, selected or primary. That
is the whole colour rule, and it is why the client reads as calm rather than as a
wall of one hue.

That is what makes the theme editor a real feature rather than an accent colour
picker. A theme can drop the blur, round differently, enlarge the text and
disable animation, which is exactly what accessibility mode needs.

## The launcher

See the README for the principle. In code:

* `InstanceManager` finds the installed game, clones it with hard links,
  rewrites the manifest, registers, activates and injects;
* `AccountStore` holds account labels and their binding to an instance, never a
  token or a credential;
* `main.cpp` is a Win32 window on an `ID2D1HwndRenderTarget`, with nothing to
  install.

The only two operations that go through PowerShell are `Get-AppxPackage` and
`Add-AppxPackage -Register`. There is no reasonable C++ equivalent without
pulling all of WinRT into the binary.

## Conventions

* C++23, `namespace velyx`, `PascalCase` types, `camelCase` functions and
  variables, `member_` for private fields.
* Comments are rare and explain decisions, not syntax. If the code needs a
  paragraph, the code is usually wrong.
* Developer facing text (code, comments, logs, crash reports, commits, docs) is
  English. The in game interface is French, which is its audience.
