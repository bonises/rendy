// Single TU for the audio codec implementations.

#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-value"
#endif
#include <stb_vorbis.c>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
