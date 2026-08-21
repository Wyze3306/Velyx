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

One file: `build/bin/Velyx.exe`.

The launcher links the client DLL, the interface fonts and the signature
templates in as resources and unpacks them under `%APPDATA%\Velyx` the first
time it runs, so there is nothing to copy next to the executable and nothing to
download. MinHook and nlohmann/json live in `external/`.

| Unpacked to | Contents |
| --- | --- |
| `%APPDATA%\Velyx\bin\Velyx.dll` | the client, injected into `Minecraft.Windows.exe` |
| `%APPDATA%\Velyx\assets\fonts` | Space Grotesk and JetBrains Mono (SIL Open Font License) |
| `%APPDATA%\Velyx\assets\signatures` | the signature pack template and its notes |

A file already there with the same size is left alone, so replacing the
executable refreshes the payload without touching anything you edited.

## Options

| Option | Default | Effect |
| --- | --- | --- |
| `VELYX_BUILD_DLL` | `ON` | build the client |
| `VELYX_BUILD_LAUNCHER` | `ON` | build the launcher |
| `VELYX_CONSOLE` | `OFF` | allocate a debug console in the game process |
| `VELYX_WERROR` | `OFF` | treat warnings as errors, as CI does |
| `VELYX_CHANNEL` | `nightly` | release channel stamped into the binary |
