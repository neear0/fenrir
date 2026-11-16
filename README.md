# Fenrir

Open-source data recovery and secure erasure for Windows, written in C++23 with a native [CelosUI](https://github.com/neear0/celosUI) interface.

Two separate jobs:

- **Recovery**: get back what survives on a disk after deletion, formatting or a lost partition, without ever writing to the source.
- **Erasure**: sanitize media with a method that actually fits the media, and report what was verified and what wasn't.

It doesn't promise 100% recovery, and it doesn't call a file overwrite on an SSD "secure".

## Status: P8 in progress, erasing a USB drive

| | |
|---|---|
| Read-only image reader (`storage::FileImageSource`) | done, tested |
| MBR (with extended partitions) and GPT parsing, with the backup GPT, CRCs, bounds and overlap checks | done, tested on synthetic images |
| Filesystem signatures per partition (FAT, exFAT, NTFS, ext, BitLocker, LUKS, swap) | done, a hint only |
| SHA-256 of a source, a hex inspector, a fault-injecting source for tests | done, tested |
| FAT32: live and deleted files and directories, deleted long names, Intact / Partial / Uncertain / Unrecoverable with reasons | done, tested on a synthetic volume |
| exFAT: live and deleted files and directories, deleted files in pieces followed through the FAT, the valid data length, entry set checksums, the backup boot region | done, tested on a synthetic volume |
| Carving: JPEG, PNG, PDF and ZIP (and DOCX, XLSX, PPTX, ODT, EPUB, JAR, APK by what's inside) found by their content, each followed through its own structure | done, tested on a synthetic disk |
| Export of recovered files to a separate folder, with SHA-256 | done, tested |
| `fenrir-cli info / partitions / hex / hash / files / carve / recover` | done, tested |
| UI shell: Home / Recover / Images / Erase / Reports / Settings | done |
| Recovery in the window: open an image, scan it on a worker thread (FAT32, exFAT and NTFS, then the deep scan) with progress, stop and go on, filter, see why each file has its status and its first bytes, export with a report | done, the engine side unit tested, the window tried by hand on the test disk |
| 100,000 made up files, to try the list at a size scans rarely reach | done, one click away on Recover |
| NTFS: every MFT record (update sequences checked), resident and non-resident data, files in pieces, sparse files, alternate data streams, extension records, the tree rebuilt from parent references with $OrphanFiles for what's lost its directory, $MFTMirr and the backup boot sector | done, tested on a synthetic volume |
| NTFS compressed and EFS-encrypted files | listed, said to be unsupported, not exported |
| Picture previews | not yet |
| Imaging: `fenrir-cli image` copies a disk image into a new one with a GNU ddrescue mapfile. It copies what reads easily first, then trims and scrapes around what didn't, then retries; ctrl+c stops it with the map saved, and it goes on from its own map or one ddrescue made | done, tested with injected read failures, flaky sectors and a crash at every map save |
| Images with a map next to them are read through it: what wasn't copied fails as unreadable instead of reading as the zeros the image has there, and found files that run into it are cut short | done, tested |
| Imaging in the window: **Images → Make an image** copies a disk image on a worker thread, with its map drawn as a bar, **Stop** and **Go on** | done, the session unit tested, the page looked at by hand |
| Drives: `fenrir-cli drives` lists them without administrator rights (model, serial, bus, volumes, which one Windows is on); any command reads `\\.\PhysicalDriveN` read-only as administrator, the window lists them on **Images** to copy one, and offers to restart as administrator | done; reading tested on a 29 GiB USB stick (an image of its first 256 MiB hashes the same as the drive, imaging onto the stick itself is refused) |
| Real-hardware failures (a drive with bad sectors, unplugged mid-copy) | only simulated so far, with the fault-injecting source |
| Erasure policy: what a method may be called on a medium (clear, purge, limited clear, none), a dry-run plan with what it can't promise, refusals with every reason (unsupported method with nothing else tried, system or mounted drive, ATA frozen or locked), approval by typed confirmation with the target checked again before anything runs | done, tested |
| Erasure on a simulated drive (HDD, SATA SSD, NVMe SSD, USB stick) with faults to order: failing partway, power loss (a firmware sanitize finishes at the next power on), firmware that lies, unreadable afterwards, swapped between approval and running | done, tested: every fault ends in the status it is, never in success |
| Erasure of disposable test image files: overwritten, flushed, read back, with a JSON report | done, tested (changed since approval, open in another program, stopped partway) |
| Erase in the window and `fenrir-cli erase plan / run` | done, the window tried by hand on a simulated drive |
| Erasing a USB drive (P8): an overwrite of every sector, flushed, read back. USB drives only; never the drive Windows is on, nor one holding fenrir, its temp folder, the current folder or the report's folder; its volumes taken offline first; checked again right before writing and once more on the open handle | written, in review. Compiled in only with `-DFENRIR_DRIVE_ERASE=ON` (off by default, and in CI): without it no code in the build can write to a drive. Not yet run on a real drive |
| ATA secure erase / sanitize, NVMe sanitize on real drives | not planned for now: there's no spare SATA or NVMe drive to test on, so these stay on the simulated drive |

### The window

**Recover → Open an image...** (or `fenrir.exe disk.img`, or *Open with*) opens a raw disk image read-only and scans it: the partition table, every FAT32, exFAT and NTFS volume's records, then, with **Deep scan** on, the whole image for files by their content. Files show up while it runs, **Stop** stops it, and a stopped deep scan can **go on** from where it was. Pick a file to see where it was found, why it has its status, and its first bytes. **Choose where to export**, then export what the filters show or the picked file: files a filesystem still lists keep their names under a folder per partition, carved ones go under `carved/`, and `fenrir report.txt` lists every file with its status, SHA-256 and reasons. **Images** makes images (a copy of a disk image into a new file with its map, drawn as a bar while it runs: **Stop** saves the map and **Go on** carries on from it) and shows the open image's partition table, its map when it has one, and everything the scan noticed. **Erase** plans erasure for a simulated drive (faults included) or a test image file, shows what it may be called and what it can't promise, and runs it once the right confirmation is picked out of four look-alikes (there's no text field in CelosUI yet to type it, as `fenrir-cli erase` asks), then shows the report and saves it as JSON. **Reports** shows the last export.

Everything that reads the image or writes a file runs in `storage::RecoverySession`, on its own threads: the window only draws snapshots of it and asks it to start and stop things.

### The command line

```
fenrir-cli partitions disk.img               the partition table and anything wrong with it
fenrir-cli partitions disk.img --sector-size 4096
fenrir-cli hex disk.img 0x1be 64             bytes at an offset, 512 from 0 by default
fenrir-cli hash disk.img                     sha-256, compare it before and after anything you do
fenrir-cli files disk.img --why              deleted files on the first FAT32, exFAT or NTFS partition, and why each is rated as it is
fenrir-cli files disk.img --all --partition 2  every file, live ones too, on partition 2
fenrir-cli recover disk.img D:\recovered     copy out every deleted file that can be, or --id N for some
fenrir-cli carve disk.img --why              files found by their content alone, and what each rating is based on
fenrir-cli carve disk.img --partition 2 --every-byte
fenrir-cli recover disk.img D:\recovered --carved
fenrir-cli image disk.img copy.img           copy it into copy.img, with copy.map saying what was copied and what couldn't be read
fenrir-cli image disk.img copy.img --retries 3   the same, going on from copy.map, then 3 more tries at the bad sectors
fenrir-cli map copy.map                      what a map says, and where the bad areas are
fenrir-cli drives                            the physical drives, their volumes, and which one Windows is on
fenrir-cli image \\.\PhysicalDrive2 D:\usb.img   image a drive (as administrator: reading a drive needs it)
fenrir-cli files \\.\PhysicalDrive2          any command reads a drive the same way, read-only
fenrir-cli hash \\.\PhysicalDrive2 --size 268435456   --size: only the first N bytes of an image or drive
fenrir-cli erase plan sim:nvme-ssd --method nvme-sanitize-crypto   what erasing would do and may be called; nothing is done
fenrir-cli erase run sim:hdd --confirm "ERASE 000001" --report out   run it on a simulated drive, with a JSON report
fenrir-cli erase run sim:nvme-ssd --method nvme-sanitize-block --fault lies --confirm "ERASE 000003"   reading back catches it
fenrir-cli erase run test.img --confirm "ERASE test.img"   overwrite a disposable test image, then read it back
fenrir-cli erase plan \\.\PhysicalDrive2        what erasing a drive would do, or every reason it's refused (any build)
fenrir-cli erase run \\.\PhysicalDrive2 --confirm "ERASE 055030" --report D:\out   as administrator, FENRIR_DRIVE_ERASE build only
```

A drive is opened for reading only and read as it is, while Windows keeps using it: a drive that's in use, or failing, is better imaged first and the image scanned. `image` and `recover` refuse a destination on the drive being read, and one whose drive can't be told (a network share counts as no drive).

A damaged table doesn't stop `partitions`: it lists what it could still read and notes what's wrong, e.g. a primary GPT that fails its CRC (the backup is used), a truncated image (partitions past the end are marked) or an MBR whose extended partition chain loops.

`image` never writes over a file: the image has to be new, or the one its map was made with (checked against samples of the source). The map is replaced whole each time it's saved, and the image is flushed first, so after a crash the map can be behind the image but never ahead of it. What couldn't be read stays zeros in the image and is marked in the map, so keep the two together: every other command, and the window, reads `disk.img` through `disk.map` (or `disk.img.map`) when it's there.

### How recoverable a deleted FAT32 file is

Deleting a file on FAT32 frees its clusters and marks its directory entries, but leaves its first cluster and size. The chain that said where the rest of it was is gone. So Fenrir assumes the file was stored in one piece and checks every cluster that assumption needs:

- **intact**: all of them are still free, and no other deleted file claims them. That's still an assumption: a file that was fragmented around clusters that are free now can't be told apart from one that wasn't.
- **partial**: a cluster it needs is in use by something else, so it was fragmented there or overwritten. Only what comes before is exported, as `name.partial`.
- **uncertain**: another deleted file claims the same clusters, so at most one of them still has its data there. Both are exported, check them.
- **unrecoverable**: its first cluster is in use, or it never had one.

A deleted file without a long name loses the first letter of its 8.3 name (`notes.txt` comes back as `_otes.txt`). With a long name, the letter is recovered from the long name's checksum. `--why` lists what each rating is based on.

### How recoverable a deleted exFAT file is

exFAT keeps more of a deleted file than FAT32 does. Deleting clears an in-use bit on each of the file's directory entries and frees its clusters in the allocation bitmap, but the entries keep the whole name, the size, the first cluster and whether the file was stored in one piece, and the FAT usually keeps its chain. So:

- a file stored in one piece (its entry says so) is **intact** when all its clusters are still free;
- a file in pieces is followed through its FAT chain, and is **intact** when the chain is still there and every cluster in it is free. If the chain was cleared, one piece is assumed, as on FAT32;
- the first cluster that's in use again ends it: **partial** up to there, **unrecoverable** if it's the first;
- two deleted files claiming the same cluster are both **uncertain**;
- only the bytes up to a file's *valid data length* were ever written, the rest of it reads as zeros and is exported as zeros, whatever its clusters hold now.

Each file's entries carry a checksum. A deleted file whose entries no longer match it had some of them written over by a newer file, so it's left out (and counted under the volume's notes) rather than listed with a name or size that may be someone else's.

### How recoverable a deleted NTFS file is

NTFS keeps the most. Every file has a record in the master file table (MFT) holding its names, times, parent directory and the *data runs* that say which clusters each piece of it is in. Deleting a file clears the record's in-use flag and frees its clusters in `$Bitmap`, and the record stays as it was until a new file takes it. So for a deleted file whose record is still there, where its data was is known exactly, however many pieces it was in, and what decides how much of it is left is whether those clusters are still free:

- **intact**: every cluster its runs name is free. Small files whose data fits in the record itself (*resident* data) are intact as long as the record is;
- **partial**: a cluster in its runs is in use again, it's exported up to there;
- **unrecoverable**: its first cluster is in use again. Compressed files (NTFS compression) and EFS-encrypted ones are listed as unrecoverable too, saying why: fenrir doesn't decompress them yet, and nothing can read EFS data without its user's key;
- **uncertain**: another deleted file claims the same clusters.

Sparse parts of a file and the bytes past its *initialized size* are exported as zeros, which is what they read as. Alternate data streams (`download.exe:Zone.Identifier`) are listed as their own files next to theirs. The directory tree is rebuilt from each record's parent reference, not from the directories' indexes, which drop deleted files: a deleted file in a deleted directory still has its path. When the directory's own record has been reused for something else, there's nothing left that says where the file was, so it's listed under `$OrphanFiles` (as The Sleuth Kit does). Records whose update sequence doesn't check out were torn mid-write or are damaged, they're left out and counted. NTFS keeps times in UTC, FAT and exFAT in the local time of whoever wrote them.

### How a carved file is rated

When nothing on the disk records a file any more (its directory entry is overwritten, the volume was formatted, the partition is gone), it can still be found by its content. `carve` looks for each format's signature where files start (every 512th byte, or every byte with `--every-byte`) and then follows the format's own structure from there, which decides whether it's a file at all, where it ends and whether it's in one piece:

| | what's checked |
|---|---|
| JPEG | every marker, and for baseline JPEGs (what cameras write) every block of the image data decoded, which has to come out at exactly the number the image needs. Bytes from another file stop decoding within a few blocks |
| PNG | every chunk's CRC, from the header to the end chunk |
| ZIP | every entry has to start where the one before ends, stored entries have to match their CRC, and the central directory and end record have to agree with the entries. What's inside tells DOCX, XLSX, PPTX, ODT, ODS, ODP, EPUB, JAR and APK apart |
| PDF | every object, cross-reference table and trailer, startxref has to point at a cross-reference section and every table entry at the object it names, through incremental updates too |

- **intact**: the structure checks out from start to end.
- **partial**: it stops checking out partway, because the file was fragmented or overwritten there (or can't be read, or the image ends). What comes before is exported as `name.partial`. For JPEG the cut is at the sector before decoding failed, so its last few hundred bytes may be someone else's.
- **uncertain**: the parts that can be checked check out, but the parts that would show damage can't be: progressive JPEGs (only their markers are followed), PDFs whose cross-reference entries don't match their objects.

A signature with no file behind it isn't listed, only counted. Once a file is found the search goes on after it, so a thumbnail inside a photo or a picture inside a DOCX isn't found again on its own. Files the FAT32 scan of the same area finds are left out, since `files` has them with their names. Carved files have no names: each is called by where it starts (`00003ff400.jpg` starts at byte 0x3ff400).

Exports only ever create new files: a name that's taken gets ` (2)`, and names from the disk are made safe (no `..`, separators, `:` streams or device names like `CON`), so nothing read from an image can write outside the folder. When recovering from a drive, the folder has to be on a different one: the export refuses it otherwise.

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

**The drive eraser** (P8) is left out unless it's asked for by name: `cmake --preset vs2022 -DFENRIR_DRIVE_ERASE=ON`. A build without it can plan the erasure of a drive and show every reason one is refused, but has no code that could write to a drive. CI builds without it.

What it builds, in `build/vs2022/<Debug|Release>/`:

| target | output | what it is |
|---|---|---|
| `celos_ui` | `fenrir.exe` | the app (CelosUI's target, kept by name) |
| `storage_cli` | `fenrir-cli.exe` | debug and test commands, imaging and erasure |
| `storage_core` | `storage_core.lib` | the engine, no UI or graphics code in it |
| `storage_tests` | `tests/.../storage_tests.exe` | doctest unit tests |
| `make_fixtures` | `tests/.../fenrir-make-fixtures.exe` | writes the test disk images, run by CTest |

CTest also runs `no_write_apis`, which fails if the engine or the CLI mentions an API that can write to a file or a device anywhere but the exporter and the imager, if the exporter could replace a file, and if the imager could create a file that's already there or cut one short. `cli_image` images a test disk and checks the copy, going on from its map, refusing a name that's taken, and imaging an image whose own map has a bad sector. `cli_recover_matches_originals`, `cli_recover_exfat_matches_originals` and `cli_recover_ntfs_matches_originals` recover the deleted files from the FAT32, exFAT and NTFS test images and check each against its original with CMake's own SHA-256, and `cli_recover_carved_matches_originals` does the same for the carving test disk. The session's unit tests run its real threads on an image file: a scan, an export and its report, a preview, a deep scan stopped and gone on with.

## Layout

```
include/storage/   the engine's public headers
src/core/          errors, results, hashes, the hex dump
src/io/            block sources: image files, fault injection
src/partition/     MBR and GPT
src/fs/            what the filesystem scanners share, and scan_volume(), which picks one
src/fs/fat32/      the FAT32 scanner
src/fs/exfat/      the exFAT scanner
src/fs/ntfs/       the NTFS scanner
src/carving/       the signature scan and a validator per format
src/export/        the exporter and the export report
src/imaging/       the imager, its ddrescue map and images read through their maps. with the exporter, the only code that writes
src/sanitation/    erasure: the policy, plans, approval, the simulated drive, test image overwrite, JSON reports
src/platform/      physical drives: listing them, reading one, which drive a path is on (windows; elsewhere not yet)
src/session/       a recovery session: scans and exports on worker threads, read as snapshots
src/cli/           fenrir-cli
tests/             unit tests, cli tests and the write-API check
tools/             the test disk image generator
ui/                CelosUI: core, widgets, styles, platforms
ui/render/screens/ fenrir's pages
cmake/             fenrir's targets
```

## Rules this code keeps

- Recovery never writes to the source. Image files are opened read-only, and on Windows nothing else can open them for writing while they're open.
- Two places in the engine write, and `tests/check_no_write_apis.cmake` keeps it that way: the exporter (new files only) and the imager (a new image, or the one its map was made with, and the map itself). Neither ever writes over a file it didn't make.
- What wasn't read is never taken for zeros: an image's map goes with it everywhere it's read.
- Drives are read through their own source (`storage::DiskSource`), opened for reading only; the image reader refuses device paths. The only things ever sent to a drive are three queries that change nothing (its description, its size, which drives a volume is on), and `no_write_apis` fails on any other.
- Erasure reaches simulated drives, test image files (overwritten through the imager's file layer, so `src/sanitation/` has no write api of its own) and, only in a `FENRIR_DRIVE_ERASE` build, a USB drive. The one file that can write to a drive is `src/platform/windows/drive_erase.cpp`: `no_write_apis` lets it open a drive and its volumes for writing (twice), write (once) and lock and dismount volumes, and nothing else anywhere else. Its rules are checked again right before writing. It's an overwrite only: no ATA or NVMe command is ever sent to a drive. A method a target doesn't support is refused, never swapped for another; verified (it did what it should) is never called validated (it's enough for the data), which fenrir never decides.
- No telemetry and no uploads.

## Known gaps

- Only the Windows build (Win32 + D3D11) has been built and tested here. The POSIX image reader and the GLFW + Vulkan build haven't been compiled in this repo yet.
- Keyboard navigation works, but there's no screen reader / UI Automation support, so the window isn't usable without sight yet. `fenrir-cli` does everything the window does, as text.
- The window is always on top of other windows, as CelosUI's demo was, which gets in the way of looking at exported files next to it.
- The window opens images through the Windows file dialog. The GLFW build has none yet (CelosUI has no text field to type a path into), it can only use `fenrir.exe disk.img`.
- There's no search in the results list (it needs that text field too), only the status and found-by filters. The preview is the first 256 bytes in hex: showing pictures means decoding untrusted data, which should happen in a sandboxed process, not in the window.
- The UI keeps the DPI scale of the monitor it opened on (a CelosUI limitation).
- The results list places rows at float positions, so past roughly 600,000 rows they start to land between pixels.
- Fenrir's own license hasn't been chosen yet. `LICENSE.txt` is the MIT text inherited from the CelosUI fork (Dear ImGui's). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- The test disk images are built by Fenrir's own generator (`tools/make_fixtures`) from the MBR convention and the UEFI spec, independently of the parser's code, but they haven't been cross-checked against images made by other tools (diskpart, sgdisk, fdisk) yet. Likewise the FAT32, exFAT and NTFS volumes are built from Microsoft's specifications (and for NTFS, the linux-ntfs documentation), and haven't been compared with what Windows' own format writes, or read with TestDisk or The Sleuth Kit. That comparison is the plan's last condition for P5, and it's still open. The JPEGs, PNGs and ZIPs it writes for carving were checked once by hand with Windows' own decoders (GDI+ and .NET's ZipFile), the PDFs weren't.
- Hostile tables are tested with random damage in the unit tests, not with a real fuzzer (libFuzzer needs clang-cl, not set up yet).
- C++23 on Linux needs GCC 14 or Clang 18 with libc++ / libstdc++ 14 (`<print>`); that's untested.
- FAT32 gaps: 8.3 names with characters past ASCII (an OEM code page the volume doesn't name) show U+FFFD; only the first cluster of a deleted directory is read; exported files don't get their original timestamps back; on volumes with more than 65,535 clusters a deleted file whose first cluster's high half is 0 is rated uncertain, since Windows clears it on delete. The test volume has fewer clusters than Microsoft's FAT32 minimum (65,525), which Fenrir notes and reads anyway.
- exFAT gaps: names aren't checked against their name hash (that needs the volume's up-case table), times are the local time they were written in (their UTC offset isn't applied), and on a TexFAT volume only the FAT and bitmap in use are read. A deleted directory is read as far as its clusters are free.
- NTFS gaps: compressed files aren't decompressed and EFS files can't be read (both listed, not exported). $LogFile and $UsnJrnl aren't read, nor are the leftovers of deleted entries in directory indexes, so a file whose MFT record was reused is gone from the listing (carving can still find its content). A file with several hard links is listed once, under its first name. Only the first 4 million MFT records are read. Paths come from the directories as they are now: if a directory was moved after a file in it was deleted, the file shows up at the directory's new place.
- Carving gaps: a fragmented file only comes back up to its first gap, the rest isn't searched for. Nothing is decompressed, so compressed ZIP entries, PDF cross-reference streams and progressive JPEGs' image data aren't checked inside. Files inside other files (a JPEG in a ZIP) aren't found on their own, and by default nor is anything that doesn't start at a sector. Only four formats so far; video, audio, RAW photos and Office's older formats aren't carved.
