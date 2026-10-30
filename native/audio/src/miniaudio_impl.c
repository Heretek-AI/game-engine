// miniaudio_impl.c — M4-04: the single miniaudio implementation TU.
//
// miniaudio is a single-header library: exactly one TU in the process may
// define MINIAUDIO_IMPLEMENTATION. This TU is that definition site (plain
// C11 compile, matching CMAKE LANGUAGES C). All other engine TUs include
// only the header declarations via engine/audio/miniaudio.hpp.
//
// Third-party code: do not hand-edit (see deps/README.md); miniaudio
// v0.11.25, MIT-0 OR public domain, upstream mackron/miniaudio.
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
