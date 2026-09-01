#pragma once

// stb_vorbis.c doubles as its own header with this define; the
// implementation is compiled in audio_impl.cpp.
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#undef STB_VORBIS_HEADER_ONLY
