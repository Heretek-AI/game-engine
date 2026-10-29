// miniaudio.hpp — M4-04: single include point for the vendored miniaudio
// header (v0.11.25, MIT-0 OR public domain; see deps/README.md).
//
// Usage: `#include "engine/audio/miniaudio.hpp"` — never include
// <miniaudio.h> directly. The header-only declarations come from
// deps/miniaudio/miniaudio.h; the implementation lives in exactly one TU
// (native/audio/src/miniaudio_impl.c, MINIAUDIO_IMPLEMENTATION).
//
// The include path for <miniaudio.h> is set on the engine_miniaudio target
// (PUBLIC, SYSTEM so its warnings never leak into engine builds).
#pragma once

// The vendored header must be included exactly once per TU without
// MINIAUDIO_IMPLEMENTATION (the .c TU above owns the implementation).
#include <miniaudio.h>
