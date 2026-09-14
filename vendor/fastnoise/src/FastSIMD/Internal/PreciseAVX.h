#pragma once

#include "FastSIMD/FastSIMD_Config.h"
#include "FastSIMD/FunctionList.h"
#include <immintrin.h>

namespace FastSIMD
{
    // Kept in a separately compiled precise-math translation unit. Inlining
    // these expressions into FastNoise's fast-math code can reintroduce the
    // CPU-dependent reciprocal estimates that change cave occupancy.
    FASTSIMD_API __m256 FS_VECTORCALL PreciseInvSqrtAVX( __m256 value );
    FASTSIMD_API __m256 FS_VECTORCALL PreciseReciprocalAVX( __m256 value );
}
