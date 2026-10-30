# Fenrir

Open-source data recovery and secure erasure for Windows, written in C++23 with a native [CelosUI](https://github.com/neear0/celosUI) interface.

Two separate jobs:

- **Recovery**: get back what survives on a disk after deletion, formatting or a lost partition, without ever writing to the source.
- **Erasure**: sanitize media with a method that actually fits the media, and report what was verified and what wasn't.

It doesn't promise 100% recovery, and it doesn't call a file overwrite on an SSD "secure".

## Status: P2

| | |
|---|---|
| Read-only image reader (`storage::FileImageSource`) | done, tested |
| MBR (with extended partitions) and GPT parsing, with the backup GPT, CRCs, bounds and overlap checks | done, tested on synthetic images |
| Filesystem signatures per partition (FAT, exFAT, NTFS, ext, BitLocker, LUKS, swap) | done, a hint only |
| SHA-256 of a source, a hex inspector, a fault-injecting source for tests | done, tested |
| FAT32: live and deleted files and directories, deleted long names, Intact / Partial / Uncertain / Unrecoverable with reasons | done, tested on a synthetic volume |
| Export of recovered files to a separate folder, with SHA-256 | done, tested |
| `fenrir-cli info / partitions / hex / hash / files / recover` | done, tested |
| UI shell: Home / Recover / Images / Erase / Reports / Settings | done |
| Recover page | **mock data**: a virtualized list of 100,000 made up candidates |
| Opening images from the UI | not yet (P4) |
| Carving, NTFS, exFAT | not yet (P3, P5) |
| Erasure | not in this build, no code here can write to a drive |

### The command line

```
fenrir-cli partitions disk.img               the partition table and anything wrong with it
fenrir-cli partitions disk.img --sector-size 4096
fenrir-cli hex disk.img 0x1be 64             bytes at an offset, 512 from 0 by default
fenrir-cli hash disk.img                     sha-256, compare it before and after anything you do
fenrir-cli files disk.img --why              deleted files on the FAT32 partition, and why each is rated as it is
fenrir-cli files disk.img --all --partition 2
fenrir-cli recover disk.img D:\recovered     copy out every deleted file that can be, or --id N for some
```

A damaged table doesn't stop `partitions`: it lists what it could still read and notes what's wrong, e.g. a primary GPT that fails its CRC (the backup is used), a truncated image (partitions past the end are marked) or an MBR whose extended partition chain loops.

### How recoverable a deleted FAT32 file is

Deleting a file on FAT32 frees its clusters and marks its directory entries, but leaves its first cluster and size. The chain that said where the rest of it was is gone. So Fenrir assumes the file was stored in one piece and checks every cluster that assumption needs:

- **intact**: all of them are still free, and no other deleted file claims them. That's still an assumption: a file that was fragmented around clusters that are free now can't be told apart from one that wasn't.
- **partial**: a cluster it needs is in use by something else, so it was fragmented there or overwritten. Only what comes before is exported, as `name.partial`.
- **uncertain**: another deleted file claims the same clusters, so at most one of them still has its data there. Both are exported, check them.
- **unrecoverable**: its first cluster is in use, or it never had one.

A deleted file without a long name loses the first letter of its 8.3 name (`notes.txt` comes back as `_otes.txt`). With a long name, the letter is recovered from the long name's checksum. `--why` lists what each rating is based on.

Exports only ever create new files: a name that's taken gets ` (2)`, and names from the disk are made safe (no `..`, separators, `:` streams or device names like `CON`), so nothing read from an image can write outside the folder. When recovering from a real drive (later), the folder has to be on a different drive.

## Building

You need a current Visual Studio 2022 (C++23: `std::expected` and `<print>`; built here with 17.14) with **Desktop development with C++**, the Windows SDK (its `fxc` compiles the shaders) and CMake 3.21 or newer (the one bundled with Visual Studio works). FreeType and doctest are downloaded and hash-checked while configuring.

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
| `make_fixtures` | `tests/.../fenrir-make-fixtures.exe` | writes the test disk images, run by CTest |

CTest also runs `no_write_apis`, which fails if the engine or the CLI mentions an API that can write to a file or a device anywhere but the exporter, and if the exporter could replace a file. `cli_recover_matches_originals` recovers the deleted files from the FAT32 test image and checks each against its original with CMake's own SHA-256.

## Layout

```
include/storage/   the engine's public headers
src/core/          errors, results, hashes, the hex dump
src/io/            block sources: image files, fault injection; read-only devices later
src/partition/     MBR and GPT
src/fs/fat32/      the FAT32 scanner
src/export/        the exporter, the only code in the engine that writes
src/cli/           fenrir-cli
tests/             unit tests, cli tests and the write-API check
tools/             the test disk image generator
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
- The test disk images are built by Fenrir's own generator (`tools/make_fixtures`) from the MBR convention and the UEFI spec, independently of the parser's code, but they haven't been cross-checked against images made by other tools (diskpart, sgdisk, fdisk) yet.
- Hostile tables are tested with random damage in the unit tests, not with a real fuzzer (libFuzzer needs clang-cl, not set up yet).
- C++23 on Linux needs GCC 14 or Clang 18 with libc++ / libstdc++ 14 (`<print>`); that's untested.
- FAT32 gaps: 8.3 names with characters past ASCII (an OEM code page the volume doesn't name) show U+FFFD; only the first cluster of a deleted directory is read; exported files don't get their original timestamps back; on volumes with more than 65,535 clusters a deleted file whose first cluster's high half is 0 is rated uncertain, since Windows clears it on delete. The test volume has fewer clusters than Microsoft's FAT32 minimum (65,525), which Fenrir notes and reads anyway.
