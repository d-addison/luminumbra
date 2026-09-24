#include "Internal/PreciseAVX.h"

namespace FastSIMD
{
    __m256 FS_VECTORCALL PreciseInvSqrtAVX( __m256 value )
    {
        return _mm256_div_ps( _mm256_set1_ps( 1.0f ), _mm256_sqrt_ps( value ) );
    }

    __m256 FS_VECTORCALL PreciseReciprocalAVX( __m256 value )
    {
        return _mm256_div_ps( _mm256_set1_ps( 1.0f ), value );
    }
}
