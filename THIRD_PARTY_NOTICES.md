# Third-party notices

This file records the upstream licenses for third-party dependencies
fetched at configure time (see `deps/README.md` for pins). Keep the
upstream copyright notice for each entry per its license terms.

## SDL3 — release-3.4.18 (Zlib)

- Upstream: https://github.com/libsdl-org/SDL
- Tarball: https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.18.tar.gz
- SHA256: `c4b08b950bd29d83caae0ab8d884298d27046a9d30700fb42841c50992665c90`
- Used by: P1-02 onward (fetched via FetchContent in
  `native/renderer/CMakeLists.txt`, linked static `SDL3::SDL3-static`).
- License text (`LICENSE.txt` from the release-3.4.18 tarball):

```
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## stb — 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5 (MIT)

- Upstream: https://github.com/nothings/stb
- Tarball: https://github.com/nothings/stb/archive/003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5.tar.gz
- SHA256: `a6c31950b3bdf05eb92258768ba2a11bd44449490574b1aac67028452288c40d`
- Used by: P1-04 `stb_image_write.h` (PNG screenshots, fetched via
  FetchContent as `_engine_stb_image_write` in
  `native/renderer/CMakeLists.txt`; the decode side `stb_image.h` is the
  existing assets fetch of the same tarball).
- License text (`LICENSE` from the tarball; dual MIT/Public-Domain,
  MIT terms kept here):

```
Copyright (c) 2017 Sean Barrett

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
## Box2D v3.2.0 (P6-07)

- Upstream: https://github.com/erincatto/box2d
- Tarball: https://github.com/erincatto/box2d/archive/refs/tags/v3.2.0.tar.gz
- SHA256: `ffec747b27a2267016f8d0240d658c82d187fb2271fa0f69fcbe53fbc067180d`
- License: MIT (see below; build-tree only, never vendored — fetched at
  configure into the build dir via FetchContent,
  `native/physics/CMakeLists.txt`).
- Targets used: `box2d` (samples/benchmarks/docs/unit-tests OFF).
- Full license text: https://github.com/erincatto/box2d/blob/v3.2.0/LICENSE

```
MIT License

Copyright (c) 2022 Erin Catto

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Recast/Detour — v1.6.0 (Zlib)

- Upstream: https://github.com/recastnavigation/recastnavigation
- Tarball: https://github.com/recastnavigation/recastnavigation/archive/refs/tags/v1.6.0.tar.gz
- SHA256: `d48ca0121962fa0639502c0f56c4e3ae72f98e55d88727225444f500775c0074`
- Used by: P6-03 navmesh bake + crowd (fetched at configure via
  FetchContent in `native/navigation/CMakeLists.txt`, targets `Recast`,
  `Detour`, `DetourCrowd`; demo/tests/examples OFF).
- License text (`License.txt` from the v1.6.0 tarball):

```
Copyright (c) 2009-2010 Mikko Mononen memon@inside.org

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

3. This notice may not be removed or altered from any source
   distribution.
```
