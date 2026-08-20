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
       ├─ ModuleManager::initialize()    builds the catalogue
       ├─ ProfileManager::load()         and applies the active profile
       └─ HookManager::installAll()      swapchain and window
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
       │    └─ emit RenderTopEvent       menu, notifications, palette
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

## Events

`EventBus` is typed, synchronous, priority ordered and cancellable.

Two properties are worth knowing:

1. **Handlers run without the lock.** A module may subscribe, unsubscribe or emit
   from inside a handler; mutations are queued and applied once the emit
   finishes.
2. **A disabled module costs nothing.** `Module::on()` registers a subscription
   *factory*, not the subscription. It is created on enable and destroyed on
   disable, which is what makes a large catalogue viable.

## Modules

```
Module                      identity, settings, keybind, permissions
 ├─ HudModule               placement, anchoring, rotation, opacity, group
 │   └─ TextHud             label/value rows, alignment, measurement
 │       ├─ FpsHud
 │       ├─ CpsHud
 │       └─ …
 ├─ Zoom, FreeLook, …       movement modules
 └─ ClickGui, HudEditor, …  client surfaces, marked essential
```

`HudModule` implements placement once for everyone. A subclass answers "how big
are you?" and "draw yourself here", and inherits the rest. `TextHud` goes further
for the common case. That is why adding a HUD readout is twenty lines and why
they all line up to the pixel when stacked.

Settings are described `std::variant`s (label, range, unit, visibility
condition, keywords). That single description drives the menu, the JSON round
trip, the search and the command palette, so there is no second list to keep in
sync.

## Profiles

A profile holds every module's state and settings, the HUD layout and the theme.
Switching disables everything and reloads, so no state leaks between profiles.

* **Automatic switching**: each profile declares substrings matched against the
  address and name of the server you join. Longest match wins, otherwise the
  default profile applies.
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

`Theme` is a serialisable struct holding every visual decision: thirteen
colours, four shape values, typography, effects and animation speed. No drawing
code invents a colour or a radius; it all goes through `theme()`.

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
