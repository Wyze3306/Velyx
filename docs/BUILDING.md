# Building

Velyx is a Windows x64 binary. It builds from Linux with mingw-w64 or from
Windows with MSVC, and CI covers both.

## From Linux

```bash
sudo apt install mingw-w64 cmake ninja-build
./build.sh
```

`./build.sh debug` produces a debug build with a console attached inside the
game process. `./build.sh clean` wipes the build directory first.

## From Windows

```bat
build.bat
```

Visual Studio 2022 or newer, with the C++ desktop workload.

## Output

| File | Role |
| --- | --- |
| `Velyx.dll` | the client, injected into `Minecraft.Windows.exe` |
| `Velyx.exe` | the launcher |
| `assets/` | fonts, themes, signature packs |

There is nothing to download. MinHook and nlohmann/json live in `external/`.

`assets/fonts` carries Space Grotesk and JetBrains Mono (SIL Open Font License).
Both binaries load them from `assets/fonts` next to the executable, so the folder
has to travel with a build; without it the interface falls back to Segoe UI.

## Options

| Option | Default | Effect |
| --- | --- | --- |
| `VELYX_BUILD_DLL` | `ON` | build the client |
| `VELYX_BUILD_LAUNCHER` | `ON` | build the launcher |
| `VELYX_CONSOLE` | `OFF` | allocate a debug console in the game process |
| `VELYX_WERROR` | `OFF` | treat warnings as errors, as CI does |
| `VELYX_CHANNEL` | `nightly` | release channel stamped into the binary |
