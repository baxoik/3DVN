# Third-party notices

This fork's own changes are offered as-is (upstream [baxoik/3DVN](https://github.com/baxoik/3DVN)
has no license). The builds and tools use the following third-party code, under their own
licenses. No game content is included anywhere in this repository or its releases.

## In the engine (all builds)

- **stb_image** and **stb_vorbis** (`source/stb_image.h`, `source/stb_vorbis.h`) by Sean Barrett
  and contributors: public domain or MIT, at your choice (see the end of each file).

## Nintendo 3DS build

- **libctru**, **citro3d**, **citro2d** (devkitPro, linked into `3dvn.3dsx`): zlib license.

## Wii U build

- **wut** (devkitPro, linked into the `.rpx` / `.wuhb`): zlib license.
- **SDL2** and **SDL2_ttf** (Wii U ports, devkitPro portlibs): zlib license.
  Copyright (C) 1997-2025 Sam Lantinga.
- **FreeType** (through SDL2_ttf): see the FreeType notice below.

## PC build

- **SDL2** and **SDL2_ttf**, from your system: zlib license.

## Android app

- **SDL2** 2.32.10 and **SDL2_ttf** 2.24.0 (built into the APK): zlib license.
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>.
- **FreeType** (vendored by SDL2_ttf, built into the APK): FreeType License (FTL).
  Portions of this software are copyright © 2024 The FreeType Project (www.freetype.org).
  All rights reserved.

## Tools

- `ever17-tools/`: GPLv2, ported from Ever17Converter (see that folder).
- The converters (`tools/*.py`) use Python's standard library, Pillow (HPND license) and
  call FFmpeg (LGPL/GPL) as separate programs; neither is included.

## Sync server (`sync-server/`)

- **pyftpdlib** 2.0.1 (MIT), installed by its Dockerfile when you build the image, on the
  official Python image (PSF license). Neither is included in this repository.

## zlib license (SDL2, SDL2_ttf, libctru, citro2d, citro3d, wut)

    This software is provided 'as-is', without any express or implied warranty. In no event
    will the authors be held liable for any damages arising from the use of this software.

    Permission is granted to anyone to use this software for any purpose, including
    commercial applications, and to alter it and redistribute it freely, subject to the
    following restrictions:

    1. The origin of this software must not be misrepresented; you must not claim that you
       wrote the original software. If you use this software in a product, an acknowledgment
       in the product documentation would be appreciated but is not required.
    2. Altered source versions must be plainly marked as such, and must not be
       misrepresented as being the original software.
    3. This notice may not be removed or altered from any source distribution.
