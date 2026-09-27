# Roadmap

Every feature on the original wish list, with where it stands and where the code
goes.

| | Meaning |
| --- | --- |
| ✅ | Written, compiled, usable |
| 🟡 | The groundwork exists; a module or a screen is still missing |
| ⬜ | Not written yet |

## Configuration and profiles

| Feature | | Where |
| --- | --- | --- |
| Profile manager | ✅ | `dll/config/ProfileManager.*` and the Profiles page |
| Auto profile switch | ✅ | `ProfileManager::profileForServer`, longest match wins |
| Config sharing (code and file) | ✅ | `exportCode` / `importCode`, `VELYX1:` format |
| Config versioning | ✅ | `snapshot` / `restore`, 20 restore points kept |
| Settings search | ✅ | `ModuleManager::search`, matches modules **and** settings |
| Favourite modules | ✅ | `Module::setFavourite`, Favourites page |
| Keybind manager | ✅ | Keybinds page, with toggle / hold / press modes |
| Menu search (Ctrl+K) | ✅ | Opens the menu on its search field, over modules **and** settings |
| Translations | ✅ | English in the source, `assets/lang/<code>.json` for the rest |
| Module permissions | ✅ | `ModulePermissions`, shown before you enable anything |
| Safe mode | ✅ | Two crashes in a row disables everything non-essential |
| Onboarding | ✅ | `dll/ui/Onboarding.*`, five steps, presets per play style |

## Appearance

| Feature | | Where |
| --- | --- | --- |
| Advanced HUD editor | ✅ | `dll/ui/HudEditor.*`, grid, snapping, guides, groups, arrow keys |
| Free placement, rotation, opacity | ✅ | `HudModule`, shared by every element |
| Element groups | ✅ | `group` setting, moved together in the editor |
| Theme creator | ✅ | `dll/ui/Theme.*` and the Themes page, edited live |
| Accessibility mode | ✅ | `accessibility` module: Contrast theme, text scale, thick borders, no motion |
| Screen filters (night, contrast, saturation, colour blindness) | ✅ | `screen_filters`, D2D colour matrices |
| Crosshair designer | ✅ | `crosshair`: six styles, outline, hit flash, dynamic spread |
| Custom hit colour | ✅ | `custom_hit_color`: a tint drawn over the entity you hit, off `ActorHurtEvent`; needs `GameMode::attack` or the entity list |
| Custom damage tint | 🟡 | `damage_tint`: a tint of Velyx's own off `HurtEvent`, which needs `Player::health`. The game's own red flash stays: the function it went through is not one 1.26 still has |
| Fullbright | ✅ | `fullbright`: a lift, a gain and a saturation matrix over the frame. No game signature at all |
| Custom sky | ⬜ | Needs a `SkyRenderer` hook |

## Combat

Everything here reads `sdk::Entities`, a snapshot of the level's actor list taken
once a frame, and projects through `sdk::Camera`. One offset —
`Level::runtimeActorList` — brings the whole category to life.

| Feature | | Where |
| --- | --- | --- |
| Entity snapshot | ✅ | `sdk/Entities.*`: one read a frame, sorted near to far, capped, every pointer guarded |
| World to screen | ✅ | `sdk/Camera.*`: the game's matrix when the pack has it, a derived one when it does not |
| Hitboxes | 🟡 | `hitboxes`: corners, box, 3D outline, filled, feet, plus a health bar. Needs the actor list |
| Nametags | 🟡 | `nametags`: name, health, distance, a Velyx badge, scaled with distance. Same dependency |
| Tracers | 🟡 | `tracers`. Same dependency |
| Target card | 🟡 | `target_hud`: last hit then aim, with a trailing damage bar. Same dependency |
| Radar | 🟡 | `radar`: turns with the camera, rings, field of view cone. Same dependency |
| Hit marker | ✅ | `hit_marker`: four styles, kill mark, streak counter, off `ActorHurtEvent` from the attack hook |
| Reach readout | ✅ | `reach`: measured to the nearest point of the box, as the game does, off `AttackEvent`; the target is read off the address the game names, so it needs no entity list |
| Low health alert | ✅ | `low_health`: pulses the screen edge. Reads nothing but the player's health |

## Performance

| Feature | | Where |
| --- | --- | --- |
| FPS graph (frame times, freezes, 1% lows) | ✅ | `fps_graph` and `FrameStats` |
| Session stats | ✅ | `session_stats` and the `SessionStats` service |
| Performance mode | ✅ | `performance_mode`: thresholds both ways, drops blur, shadows and motion |
| Battery mode | ✅ | `battery_mode`: mains detection, frame limiter, effects off |
| Benchmark | ✅ | `benchmark`: timed run, verdict, suggested settings |
| Playtime tracker | ✅ | `Playtime` service, HUD element and a 14 day chart |
| Server performance monitor | 🟡 | Ping works. Estimated TPS and packet loss need the network hook (`PacketEvent` is defined) |
| Frame limiter | ✅ | `frame_limiter`: paced against the clock, separate caps unfocused and on a menu screen, 1 ms timer |
| Process tuning | ✅ | `process_tuner`: priority, performance cores via `EfficiencyClass`, timer resolution, working set trim |
| System monitor | ✅ | `system_monitor`: processor, memory, video memory via `IDXGIAdapter3`, threads, adapter name |
| Overlay cost | ✅ | `overlay_cost`: microseconds between the first and last thing Velyx draws, plus its draw calls |

## Capture and replay

| Feature | | Where |
| --- | --- | --- |
| Screenshot mode | ✅ | `screenshot_mode`: hides marked elements, captures, notifies |
| Screenshot manager | ✅ | PNG encoding, `<server>/<date>/` layout, thumbnail gallery on the Captures page |
| Clip markers | ✅ | `clip_markers` and the `Clips` service, listed on the Captures page |
| Match history | ✅ | History page: server, duration, K/D, average FPS, blocks |
| Replay system | ⬜ | The big one: encoded frame capture on a dedicated thread |
| Instant replay (30/60 s) | ⬜ | Ring buffer over the back buffer; the keybind is already reserved |

## World and navigation

| Feature | | Where |
| --- | --- | --- |
| Advanced waypoints | ✅ | `waypoints`: dropped on a key, kept per world, pinned to the screen edge when behind you |
| World notes | ⬜ | `Paths::notes()` reserved |
| Friend notes | ⬜ | Same, keyed by player name |
| Favourite servers, quick join | ⬜ | Needs the multiplayer screen hook |
| Resource pack manager | ⬜ | Needs `ResourcePackRepository` signatures |
| Shader presets | ⬜ | Depends on the shader loader |

## Chat and sound

| Feature | | Where |
| --- | --- | --- |
| Chat hook | ✅ | `hook/hooks/ChatHook.*` detours `GuiData::addMessage` (1.26.50 on) or `GuiData::displayChatMessage` (older) and emits `ChatReceiveEvent`, cancellable, with the sender and the line both rewritable. Checked live on 1.26.51.1: the streamer mode filter rewrites a line in place |
| Velyx users | 🟡 | `feature/Presence.*` and the `velyx_users` module: a handshake in the chat, the launcher's own instances through a file, a badge in front of the name and a chip on the nametag |
| Chat tabs | ⬜ | The event arrives now; client side chat *rendering* is what is missing |
| Chat search | ⬜ | Same groundwork |
| Chat mentions | ⬜ | Detection on `ChatReceiveEvent` plus `Notifications::push`, and now the shortest thing left in this table |
| Chat macros | 🟡 | `chat_macros`: four key-to-message binds, queued onto the render thread. Needs `LocalPlayer::sendChatMessage` |
| Chat translator | ⬜ | Needs network access, declared in the module permissions |
| Sound visualiser | ⬜ | `SoundEvent` defined; needs the `SoundEngine::play` signature |
| Sound mixer | ⬜ | Same hook, rewriting the volume |

## Privacy

| Feature | | Where |
| --- | --- | --- |
| Privacy mode | ✅ | `privacy_mode`: hides server, name and coordinates |
| Streamer mode | ✅ | `streamer_mode`: full privacy plus chat filtering |
| Coordinate tools | ✅ | `coord_tools`: clipboard or chat, four layouts |

## Lifecycle

| Feature | | Where |
| --- | --- | --- |
| Notification centre | ✅ | `dll/ui/Notifications.*`, with history |
| Crash reporter | ✅ | Exception filter, timestamped report naming the suspect module, panel in Diagnostics |
| Update manager (stable, beta, nightly) | ✅ | `feature/Updates.*` reads the GitHub releases feed, channel picker in Diagnostics |
| Changelog | 🟡 | Release notes are fetched and the page links out; an in client reader is still missing |

## Extensibility

| Feature | | Where |
| --- | --- | --- |
| Plugin API | ⬜ | `ModuleCategory::Script` and the permission model are in place |
| Lua scripting | ⬜ | The safe surface would be `Settings` plus `EventBus` |
| Marketplace | ⬜ | Needs server infrastructure, last in line |

## What actually runs, module by module

Audited on 2026-08-22 against a machine with no signature pack, by reading every
module's subscriptions and its use of the SDK. Three verdicts, and only one of
them is a bug:

| | Meaning |
| --- | --- |
| ✅ | Works with no signature pack at all |
| 🔑 | Written and correct; reads the game, so it needs the pack |
| 🪝 | Written and correct; listens for an event **nothing emits yet** |

**Never emitted by anything**, as of 2026-09-12: `PacketEvent`, `Render3DEvent`,
`SoundEvent`. Nothing listens for them either. The other nine that used to be on this
list have producers now — `FovEvent` and `PerspectiveEvent` from the game hooks,
`AttackEvent` from the attack hook through `sdk::Signals`, `HurtEvent`,
`ActorHurtEvent`, `DeathEvent`, `RespawnEvent`, `ScreenChangeEvent` and `TickEvent`
read off the snapshot by the same — and every 🪝 below is waiting on a pack entry
rather than on code. See "The game's own functions" in ARCHITECTURE.md.

`ChatReceiveEvent` is emitted for every line the game displays, which is what moved
`streamer_mode` out of the list below and what `velyx_users` is built on.

`TurnDeltaEvent` reaches the game: both input doors and the game's own hand their
movement to `velyx::turn`, which is what the sensitivity multiplier, the cinematic
camera, SnapLook and FreeLook stand on.

### ✅ Works today (31)

`accessibility`, `afk_timer`, `battery_mode`, `benchmark`, `clickgui`,
`clip_markers`, `clock`, `cps`, `crosshair`¹, `fps`, `fps_graph`,
`frame_limiter`, `fullbright`, `hud_editor`, `keystrokes`, `memory`,
`menu_hint`, `notifications`, `null_movement`, `onboarding`, `overlay_cost`,
`performance_mode`, `playtime`, `privacy_mode`, `process_tuner`,
`screen_filters`, `screenshot_mode`, `session_stats`², `stopwatch`,
`system_monitor`, `toggle_sneak`³

¹ draws; the hit flash and the dynamic gap need the game.
² the clock and the counters run; blocks travelled needs the game.
³ holds the key, but cannot tell it is in a menu without the pack.

### 🔑 Needs the signature pack (19)

`armour`, `chat_macros`, `coord_tools`, `coordinates`, `direction`, `hitboxes`,
`ip_display`, `low_health`, `nametags`, `ping`, `radar`, `server_monitor`,
`speed`, `streamer_mode`⁴, `target_hud`, `toggle_sprint`, `tracers`,
`velyx_users`⁵, `waypoints`

Seventeen of them stop at the same place: `ClientInstance::instance` is the one
**required** signature, and without it `game().player().valid` is false forever.
The two that read the chat need `GuiData::displayChatMessage` as well.

⁴ the privacy half works with no pack at all; the chat filter needs the chat hook.
⁵ finds nobody without the chat hook, and cannot publish itself to the other
instances on this machine until it knows the player's own name.

### 🪝 Needs a pack entry for its hook (12)

The code is there for all twelve; what each waits on is one entry in the pack, and
the menu names it on the card.

- `cinematic_camera`, `sens_multiplier`, `snap_look`, `free_look`: the mouse, through
  whichever door the machine uses — raw input under WineGDK, GameInput on Windows,
  `LocalPlayer::applyTurnDelta` where the pack has it. **Live on every machine tested;
  on 1.26.51.1 through `applyTurnDelta`, which the pack carries.**
- `zoom`, `fov_changer`, `java_dynamic_fov`: the two `tanf` calls the camera takes of
  the field of view (`LevelRendererPlayer::fovTangent` and `::drawFovTangent`), or
  `setupCamera` / `getFov` on the builds that have nothing better. **In the 1.26 pack.**
- `hit_marker`, `reach`, `custom_hit_color`, `target_hud`'s last-hit pick:
  `GameMode::attack`. **In the 1.26 pack for every build; slot 14 up to 1.26.45, the
  function behind slot 15 on 1.26.51.1.** The reach needs the target's position, which
  the pack only has for players on 1.26.51.1.
- `damage_tint`: the player's health, so `Player::health` and a road to the player.
- `auto_perspective`: the perspective hook, and the player's state flags, which no
  pack carries yet.

### Bugs the audit found, and fixed

| What | Why it was dead |
| --- | --- |
| `fullbright`, `screen_filters` | `Renderer::colorMatrix` was gated on `effectsEnabled_`, which follows the theme's *blur* flag — so the Contrast theme, and performance mode dropping below its threshold, silently killed all colour grading. The preference (`effectsEnabled_`) and the capability (`effectsSupported_`) are now two flags, and grading depends only on the second |
| `fullbright`, `screen_filters` | Both graded at `EventPriority::Low`, i.e. *after* the HUD elements, so they filtered the client's own readouts along with the game. Moved to `First` |
| `playtime`, the 14-day chart, match history | `Playtime::add()` was only ever called from `SessionStats::flush()`, which fires on `WorldLeaveEvent` — a 🔑 event. The counter never moved, on any machine without a pack, and a session ending any other way was never counted either. `Playtime` now counts the frame it is on, commits once a minute and on the window closing |

## The module catalogue

62 modules are written, 31 of them run on a machine with no signature pack. Most
of what is left to *write* falls into three buckets:

- **HUD readouts**, which are twenty line `TextHud` subclasses. That is filling
  in, not design work;
- **render modules** (View Model, Motion Blur, Fog Colour, Time Changer, Weather
  Changer, Block Outline, Chunk Border), each needing its own signature or hook.
  They are blocked on the signature pack, not on the client;
- **server modules** (Hive Stats, Hive Utils, Zeqa Utils, Auto GG), which need
  chat and scoreboard parsing.

## What comes next

1. **A signature pack for the target Bedrock build.** Nothing else moves the
   number above: thirty-one of the sixty-two modules are written, correct and idle,
   and no amount of module code changes that. `ClientInstance::instance` alone
   revives seventeen of them; the thirteen listening for an event need the hooks
   that pack also makes possible. Three entries carry most of the weight:
   `Level::runtimeActorList` (the whole Combat category),
   `ClientInstance::viewMatrix` (exact projection instead of a derived one) and
   `Actor::entityTypeId` (telling a player from a cow).
2. **The remaining HUD readouts**, in one pass, on `TextHud`.
3. **Client side chat rendering**, which unlocks tabs, search, mentions and the
   translator in one go. The half that reads the chat is written; the half that
   draws it is not.
4. **Render modules**, as signatures arrive.
5. **Replay and instant replay**, handled on their own.
