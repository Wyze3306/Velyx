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
| Custom hit colour | 🟡 | `custom_hit_color` written, inert until the `HurtColor` hook has a signature |
| Custom damage tint | 🟡 | `damage_tint`, same dependency |
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
| Nametags | 🟡 | `nametags`: name, health, distance, scaled with distance. Same dependency |
| Tracers | 🟡 | `tracers`. Same dependency |
| Target card | 🟡 | `target_hud`: last hit then aim, with a trailing damage bar. Same dependency |
| Radar | 🟡 | `radar`: turns with the camera, rings, field of view cone. Same dependency |
| Hit marker | 🟡 | `hit_marker`: four styles, kill mark, streak counter. Needs `ActorHurtEvent` to be emitted |
| Reach readout | 🟡 | `reach`: measured to the nearest point of the box, as the game does. Needs `AttackEvent` |
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
| Chat tabs | ⬜ | `ChatReceiveEvent` is defined and cancellable; client side chat rendering is missing |
| Chat search | ⬜ | Same groundwork |
| Chat mentions | ⬜ | Detection on `ChatReceiveEvent` plus `Notifications::push`, the shortest of the four |
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

## The module catalogue

61 modules are written. Most of what is left falls into three buckets:

- **HUD readouts**, which are twenty line `TextHud` subclasses. That is filling
  in, not design work;
- **render modules** (View Model, Motion Blur, Fog Colour, Time Changer, Weather
  Changer, Block Outline, Chunk Border), each needing its own signature or hook.
  They are blocked on the signature pack, not on the client;
- **server modules** (Hive Stats, Hive Utils, Zeqa Utils, Auto GG), which need
  chat and scoreboard parsing.

## What comes next

1. **A signature pack for the target Bedrock build.** It unlocks more per hour
   spent than anything else; without it a third of the catalogue stays inert no
   matter how much code gets written. Three entries carry most of the weight:
   `Level::runtimeActorList` (the whole Combat category),
   `ClientInstance::viewMatrix` (exact projection instead of a derived one) and
   `Actor::entityTypeId` (telling a player from a cow).
2. **The remaining HUD readouts**, in one pass, on `TextHud`.
3. **Client side chat**, which unlocks tabs, search, mentions and the translator
   in one go.
4. **Render modules**, as signatures arrive.
5. **Replay and instant replay**, handled on their own.
