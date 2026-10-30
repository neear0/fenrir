# Third-party notices

What Fenrir compiles in, and where it came from. This is a record, not a legal review. Every item marked *open* has to be settled before a release.

## CelosUI

- Source: https://github.com/neear0/celosUI (a fork of https://github.com/mistline/celosUI)
- Pinned at commit `d297d470b371312c8e49e3972439a19b0a88162e` (2026-10-22), copied into this repo with `cUI.sln` / `cUI.vcxproj` left out (CMake builds it here)
- Changed here: `main.cpp`, `CMakeLists.txt`, `ui/ui.h` (`<functional>`, `platform::minimize()` / `close()`), `ui/win32/platform_win32.cpp` and `ui/glfw/platform_glfw.cpp` (those two), `ui/widgets/widgets.h` (`virtual_list`), `ui/render/main_window.cpp`. Added: `ui/widgets/virtual_list.cpp`, `ui/render/screens/`
- `ui/win32/d3d_create.cpp`, `ui_win32.h`, `window_effects.cpp` and `window_functions.cpp` differ from upstream only in line endings: upstream stored them with CRLF, its own `.gitattributes` (`* text=auto`) has git store LF
- License: *open*. The fork's root `LICENSE.txt` is MIT text naming Omar Cornut (Dear ImGui). The license for CelosUI's own code hasn't been confirmed separately.
- The upstream README is kept as [docs/celosui-README.md](docs/celosui-README.md)

## Dear ImGui (in `external/imgui`, through CelosUI)

- License: MIT, Copyright (c) 2014-2024 Omar Cornut (`LICENSE.txt`)

## FreeType (downloaded while configuring)

- Version 2.14.3, https://freetype.org, SHA-256 checked in `CMakeLists.txt`
- License: the FreeType License, which asks for a credit in the documentation:
  "Portions of this software are copyright (c) 2026 The FreeType Project (https://freetype.org). All rights reserved."

## Poppins (fonts in `assets/`, compiled into the executable)

- License: SIL Open Font License 1.1. Its license text has to ship with the software.
- *open*: the OFL text isn't in this repository yet. It has to be added before any binary is distributed.

## stb_image (in `external/stb`)

- License: public domain or MIT, at the user's choice

## doctest (tests only, downloaded while configuring)

- Version 2.5.3, https://github.com/doctest/doctest, SHA-256 checked in `tests/CMakeLists.txt`
- License: MIT. Only linked into `storage_tests`, never shipped.
