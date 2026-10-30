# Fenrir

Open-source data recovery and secure erasure for Windows, written in C++20 with a native [CelosUI](https://github.com/neear0/celosUI) interface.

Two separate jobs:

- **Recovery**: get back what survives on a disk after deletion, formatting or a lost partition, without ever writing to the source.
- **Erasure**: sanitize media with a method that actually fits the media, and report what was verified and what wasn't.

It doesn't promise 100% recovery, and it doesn't call a file overwrite on an SSD "secure".

## Status: P0

| | |
|---|---|
| Read-only image reader (`storage::FileImageSource`) | done, tested |
| `fenrir-cli info <image>` | done, tested |
| UI shell: Home / Recover / Images / Erase / Reports / Settings | done |
| Recover page | **mock data**: a virtualized list of 100,000 made up candidates |
| Partition tables, filesystems, carving | not yet (P1–P5) |
| Erasure | not in this build, no code here can write to a drive |

## Building

You need Visual Studio 2022 with **Desktop development with C++**, the Windows SDK (its `fxc` compiles the shaders) and CMake 3.21 or newer (the one bundled with Visual Studio works). FreeType and doctest are downloaded and hash-checked while configuring.

**Visual Studio:** generate the solution, then open `build/vs2022/fenrir.sln`. F5 runs the app.
```
cmake --preset vs2022
```
Or use *File > Open > Folder* on the repo and pick the *Visual Studio 2022* preset.

**Command line:**
```
cmake --preset vs2022
cmake --build --preset release
ctest --preset release
```

What it builds, in `build/vs2022/<Debug|Release>/`:

| target | output | what it is |
|---|---|---|
| `celos_ui` | `fenrir.exe` | the app (CelosUI's target, kept by name) |
| `storage_cli` | `fenrir-cli.exe` | read-only debug and test commands |
| `storage_core` | `storage_core.lib` | the engine, no UI or graphics code in it |
| `storage_tests` | `tests/.../storage_tests.exe` | doctest unit tests |

CTest also runs `no_write_apis`, which fails the build's tests if the engine or the CLI ever mentions an API that can write to a file or a device.

## Layout

```
include/storage/   the engine's public headers
src/core/          errors and results
src/io/            block sources: image files now, read-only devices later
src/cli/           fenrir-cli
tests/             unit tests and the write-API check
ui/                CelosUI: core, widgets, styles, platforms
ui/render/screens/ fenrir's pages
cmake/             fenrir's targets
```

## Rules this code keeps

- Recovery never writes to the source. Image files are opened read-only, and on Windows nothing else can open them for writing while they're open.
- Device paths (`\\.\PhysicalDrive0`, `\\?\Volume{...}`) are refused by the image reader. Reading drives directly gets its own reviewed source later.
- No erasure code exists until the simulator (P7). Real drives come last (P8), after an independent review.
- No telemetry and no uploads.

## Known gaps

- Only the Windows build (Win32 + D3D11) has been built and tested here. The POSIX image reader and the GLFW + Vulkan build haven't been compiled in this repo yet.
- Keyboard navigation works, but there's no screen reader / UI Automation support.
- The UI keeps the DPI scale of the monitor it opened on (a CelosUI limitation).
- The results list places rows at float positions, so past roughly 600,000 rows they start to land between pixels.
- Fenrir's own license hasn't been chosen yet. `LICENSE.txt` is the MIT text inherited from the CelosUI fork (Dear ImGui's). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
