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
| Custom sky | ⬜ | Needs a `SkyRenderer` hook |

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
| Advanced waypoints | ⬜ | `Paths::waypoints()` reserved; needs world to screen projection |
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
| Chat translator | ⬜ | Needs network access, declared in the module permissions |
| Sound visualiser | ⬜ | `SoundEvent` defined; needs the `SoundEngine::play` signature |
| Sound mixer | ⬜ | Same hook, rewriting the volume |

## Privacy

| Feature | | Where |
| --- | --- | --- |
| Privacy mode | ✅ | `privacy_mode`: hides server, name and coordinates |
| Streamer mode | ✅ | `streamer_mode`: full privacy plus chat filtering |

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

45 modules are written. Most of what is left falls into three buckets:

- **HUD readouts**, which are twenty line `TextHud` subclasses. That is filling
  in, not design work;
- **render modules** (Fullbright, Nametag, View Model, Motion Blur, Fog Colour,
  Time Changer, Weather Changer, Block Outline, Chunk Border), each needing its
  own signature or hook. They are blocked on the signature pack, not on the
  client;
- **server modules** (Hive Stats, Hive Utils, Zeqa Utils, Auto GG), which need
  chat and scoreboard parsing.

## What comes next

1. **A signature pack for the target Bedrock build.** It unlocks more per hour
   spent than anything else; without it a third of the catalogue stays inert no
   matter how much code gets written.
2. **The remaining HUD readouts**, in one pass, on `TextHud`.
3. **Client side chat**, which unlocks tabs, search, mentions and the translator
   in one go.
4. **Render modules**, as signatures arrive.
5. **Replay and instant replay**, handled on their own.
