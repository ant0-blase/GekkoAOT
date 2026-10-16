# Third-party notices

GekkoAOT is GPL-3.0-or-later. The repository also integrates with or carries patches for the following upstream projects. Their original copyrights and license terms remain in effect.

## DolRecomp

Upstream: `https://github.com/ExpansionPak/DolRecomp`

Default pinned revision: `71ce7f97419b1bb1ba9a9596c41507f6629e0fb0`

License: GNU General Public License v3.0. GekkoAOT's `COPYING` contains the full GPLv3 text. Local changes are represented as patches under `patches/dolrecomp/` and are distributed under compatible GPL terms.

## Aurora

Upstream: `https://github.com/encounter/aurora`

Default pinned revision: `9c0bf66f1ed3276b60ad1cd746e2fb48818a6298` (explicit CMake overrides are supported).

License: MIT.

Copyright (c) 2022 Luke Street

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## encounter/nod

Upstream: `https://github.com/encounter/nod`

Pinned release: `v2.0.0-alpha.12`

nod is available under MIT OR Apache-2.0. GekkoAOT release packaging uses the MIT grant for redistribution of the linked prebuilt library.

Copyright 2021 Luke Street.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## Qt

Qt 6 is used by the optional desktop frontend. Portable releases deploy Qt runtime libraries and platform plugins, including the Windows Qt runtime and the Linux AppImage deployment. Qt retains its commercial/open-source licensing terms and upstream notices. Source builds may use a system Qt installation.

## Launcher fonts and icons

Google Sans Flex and Doto are embedded under the SIL Open Font License 1.1.
Their original licenses, the Google Sans Flex trademark notice and source
information are retained in `assets/fonts` and installed with the launcher.
Lucide icons use the ISC license retained in `assets/icons/lucide/LICENSE`.

## Optional Discord Social SDK

Discord Rich Presence can use the official Discord Social SDK 1.10 or later.
The SDK is a separate optional dependency supplied through Discord's Developer
Portal, subject to Discord's SDK terms; it is not relicensed as GekkoAOT source.
SDK-enabled packages retain its upstream `License-Notices.txt` under
`share/licenses/GekkoAOT/discord`. See `docs/discord-rich-presence.md` for build
and platform requirements.

## Game and platform trademarks

Nintendo, GameCube and game titles/trademarks belong to their respective owners. GekkoAOT is an independent project and is not affiliated with or endorsed by Nintendo, Electronic Arts, Warner Bros. or other game/platform rights holders.


## SDL3

Upstream: `https://github.com/libsdl-org/SDL`

Pinned portable fallback: `release-3.2.22`

License: zlib License. Portable builds may statically link SDL3 to provide native keyboard, gamepad and joystick input when no system SDL3 package is available.

Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software. Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the conditions of the SDL zlib license.

## Standalone DSP compatibility code

The optional standalone DSP fallback builds a subset of the open-source
[Dolphin](https://github.com/dolphin-emu/dolphin) DSP implementation under its
GPL terms. GekkoAOT does not use Dolphin's Core/System runtime chassis and does
not distribute Nintendo DSP ROMs. Source staging is managed by the controller and the standalone DSP build
configuration in `runtime/native/CMakeLists.txt`.

## Portable development toolchain

Release packages can include LLVM/Clang, CMake, Ninja and Zig. Their original
license notices are retained by the portable toolchain staging process under
`toolchain/licenses`; these components are not relicensed as GekkoAOT code.
Optional system FFmpeg libraries retain the license terms of the selected host
build.

## Memory card interoperability

The save-serial rebinding routines in `src/MemoryCard.cpp` are adapted from the Dolphin Emulator Project’s GCMemcard implementation, Copyright 2008 Dolphin Emulator Project, licensed GPL-2.0-or-later (used here under GPL-3.0-or-later). The GCS/SAV container layouts and animated-icon interpretation were checked against Dolphin’s GCMemcard and GCMemcardUtils sources, Copyright 2008/2020 Dolphin Emulator Project. See [memory-card references](docs/memory-cards.md). The original project is available at <https://github.com/dolphin-emu/dolphin>.
