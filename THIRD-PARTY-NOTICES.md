# Third-Party Notices

OpenCaliper's own source code is licensed under the MIT license (see
[`LICENSE`](LICENSE)). This application is built on the open-source software listed below. All of it
is **linked dynamically** (shared libraries / DLLs) and **not modified**. No
third-party source code is copied into this project's own source files: the
sketch constraint solver, the icons and the file-format glue are original work.

Full license texts are in the [`LICENSES/`](LICENSES/) folder.

---

## Open CASCADE Technology (OCCT)

**This software makes use of, and is based on facilities provided by, the
Open CASCADE Technology software.** (Notice required by the Open CASCADE
exception, see below.)

| | |
|---|---|
| Used for | Geometry kernel (B-Rep solids, booleans, fillets), 3D view, STEP/IGES/STL I/O |
| Version | 7.9 (tested with 7.9.3) |
| License | `LGPL-2.1-only WITH OCCT-exception-1.0` |
| Texts | [LGPL-2.1-only](LICENSES/LGPL-2.1-only.txt), [OCCT-exception-1.0](LICENSES/OCCT-exception-1.0.txt) |
| Source | https://github.com/Open-Cascade-SAS/OCCT (tag `V7_9_3`) |
| Copyright | Open CASCADE SAS |

Libraries used: TKernel, TKMath, TKG2d, TKG3d, TKGeomBase, TKBRep, TKGeomAlgo,
TKTopAlgo, TKPrim, TKBO, TKBool, TKShHealing, TKFillet, TKMesh, TKService,
TKV3d, TKOpenGl, TKDE, TKXSBase, TKDESTEP, TKDEIGES, TKDESTL (and the OCCT
libraries these depend on). "Open CASCADE" is a trademark of Open CASCADE SAS.

## Qt 6

| | |
|---|---|
| Used for | Windows, menus, dialogs, OpenGL widget, JSON project files |
| Modules | Qt Core, Qt Gui, Qt Widgets, Qt OpenGL, Qt OpenGL Widgets |
| Version | 6.x (tested with 6.11) |
| License | Used under `LGPL-3.0-only` (Qt is also offered under GPL and commercial terms) |
| Texts | [LGPL-3.0-only](LICENSES/LGPL-3.0-only.txt), [GPL-3.0-only](LICENSES/GPL-3.0-only.txt) (LGPL-3.0 builds on GPL-3.0) |
| Source | https://download.qt.io/official_releases/qt/ |
| Copyright | The Qt Company Ltd. and other contributors |

Qt is linked dynamically and can be replaced by the user with a compatible
build of Qt. "Qt" is a trademark of The Qt Company Ltd.

## FreeType

Portions of this software are copyright © The FreeType Project
(https://freetype.org). All rights reserved.

| | |
|---|---|
| Used for | Text rendering (used by OCCT and Qt) |
| License | `FTL OR GPL-2.0-or-later`, used under the FreeType License (FTL) |
| Text | [FTL](LICENSES/FTL.txt) |

---

## Libraries that ship inside packaged builds

Packaged builds (AppImage, Windows zip, Flatpak runtime) also contain
libraries that Qt, OCCT and the C++ runtime depend on. They are used unmodified
under the licenses below; their license texts travel with the respective
package (Flatpak runtime, Qt distribution, AppImage `usr/share/doc`).

| Library | License (SPDX) |
|---|---|
| HarfBuzz | MIT-Modern-Variant |
| ICU | Unicode-DFS-2016 (and BSD-2-Clause, BSD-3-Clause, NAIST-2003) |
| PCRE2 | BSD-3-Clause |
| zlib / zlib-ng | Zlib |
| libpng | Zlib |
| double-conversion | BSD-3-Clause |
| md4c | MIT |
| libb2 | CC0-1.0 OR Apache-2.0 |
| Brotli | MIT |
| Zstandard | BSD-3-Clause OR GPL-2.0-only |
| xz (liblzma) | 0BSD |
| bzip2 | bzip2-1.0.6 / BSD-style |
| Fontconfig | HPND (and Unicode-DFS-2016) |
| libxkbcommon | MIT |
| X11 client libraries (libX11, libxcb, libXext, libXau) | MIT / X11 |
| GLib | LGPL-2.1-or-later |
| graphite2 | LGPL-2.1-or-later OR MPL-2.0 OR GPL-2.0-or-later |
| libxml2 | MIT |
| oneTBB (only if OCCT was built with TBB) | Apache-2.0 |
| GCC runtime (libstdc++, libgcc) | GPL-3.0-or-later WITH GCC-exception-3.1 |
| Microsoft Visual C++ runtime (Windows build only) | Microsoft redistributable license |

OpenGL drivers (Mesa, NVIDIA, AMD, Intel, libglvnd) come from the user's
system and are not distributed with this software.

## Build tools (not distributed)

CMake (BSD-3-Clause), Ninja (Apache-2.0), vcpkg (MIT), linuxdeploy (MIT),
flatpak-builder (LGPL-2.1-or-later) and GitHub Actions such as
install-qt-action (MIT) are only used to build the program and are not part
of the distributed software.

---

## Your rights under the LGPL

OCCT and Qt are licensed under the GNU LGPL. You may replace the shipped
OCCT and Qt libraries with your own modified, compatible builds. The exact
versions used and the scripts that build them are in this repository
(`packaging/`, `vcpkg.json`, `.github/workflows/`). The corresponding source
code is available at the URLs above; on request we will also provide it
directly.
