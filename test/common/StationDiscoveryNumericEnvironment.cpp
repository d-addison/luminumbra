// Exercise the executable entry path in a child process with a changed FP state.
#include "../../tools/StationDiscoveryArguments.h"

#include <cfenv>

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

// The object target compiles the shipping CLI with only its entry symbol renamed.
#ifdef _WIN32
int StationDiscoveryEntry(int argc, wchar_t** argv);
#else
int StationDiscoveryEntry(int argc, char** argv);
#endif

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    if (argc < 2) {
        return 2;
    }
    const auto mode = Luminumbra::StationDiscoveryCli::ArgumentText(argv[1]);
    // Each invocation is a separate process. Establish a known supported state
    // before changing exactly one control; the process owns its environment.
    if (std::fesetround(FE_TONEAREST) != 0) {
        return 3;
    }
#if defined(__x86_64__) || defined(_M_X64)
    const unsigned int baseline = _mm_getcsr() & ~((1u << 15) | (1u << 6) | (3u << 13));
    _mm_setcsr(baseline);
#endif
    int rounding = FE_TONEAREST;
    if (mode == "downward") {
        rounding = FE_DOWNWARD;
    } else if (mode == "upward") {
        rounding = FE_UPWARD;
    } else if (mode == "toward-zero") {
        rounding = FE_TOWARDZERO;
    } else if (mode != "supported") {
#if defined(__x86_64__) || defined(_M_X64)
        if (mode == "ftz") {
            _mm_setcsr(baseline | (1u << 15));
        } else if (mode == "daz") {
            _mm_setcsr(baseline | (1u << 6));
        } else if (mode == "simd-downward") {
            _mm_setcsr(baseline | (1u << 13));
        } else if (mode == "simd-upward") {
            _mm_setcsr(baseline | (2u << 13));
        } else if (mode == "simd-toward-zero") {
            _mm_setcsr(baseline | (3u << 13));
        } else {
            return 2;
        }
#else
        return 2;
#endif
    }
    if (rounding != FE_TONEAREST && std::fesetround(rounding) != 0) {
        return 3;
    }
    return StationDiscoveryEntry(argc - 1, argv + 1);
}
