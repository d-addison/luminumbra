// src/luminumbra/core/ExternalImplementations.cpp

// This file is designated to hold the implementations for
// single-header libraries that require it.

// --- MINIAUDIO IMPLEMENTATION ---
// By defining MINIAUDIO_IMPLEMENTATION here, we compile the library's source code
// directly into our project.
#define MINIAUDIO_IMPLEMENTATION
#define MA_ENABLE_VORBIS  // Enables the decoder for .ogg files
#define MA_ENABLE_MP3     // Enables the decoder for .mp3 files
#define MA_ENABLE_FLAC    // Enables the decoder for .flac files
#include <miniaudio.h> 

#define FNL_IMPL
#include "FastNoiseLite.h"