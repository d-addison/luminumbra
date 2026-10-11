#pragma once

#include <FastNoise/FastNoise.h>

namespace Luminumbra::Systems {

// Match terrain's x86 ceiling: AVX-512 changes ambient float bits and hence
// wind/weather/aether world hashes. FastNoise still selects a supported lower
// level on older CPUs. Leave other architectures on their native dispatch.
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
inline constexpr FastSIMD::eLevel kAmbientNoiseMaxSIMDLevel = FastSIMD::Level_AVX2;
#else
inline constexpr FastSIMD::eLevel kAmbientNoiseMaxSIMDLevel = FastSIMD::Level_Null;
#endif

template<typename T>
FastNoise::SmartNode<T> NewAmbientNoise() {
    // Every node in a graph, including Simplex sources, needs the same ceiling.
    return FastNoise::New<T>(kAmbientNoiseMaxSIMDLevel);
}

} // namespace Luminumbra::Systems
