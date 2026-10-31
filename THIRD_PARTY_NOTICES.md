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
