// stb_image_write_impl.c — P1-04 PNG writer implementation TU.
//
// The single STB_IMAGE_WRITE_IMPLEMENTATION site (spec rule: exactly one).
// Third-party C code gets base flags (-w), not the engine hot flags, so
// upstream warnings never trip -Werror.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
