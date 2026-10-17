#include "core/Profiler.h"

#if defined(LUMINUMBRA_ENABLE_TRACY) || defined(TRACY_ENABLE)
#error This compile-only target must remain independent of the profiler interface.
#endif

#define PROFILE_STRING_IMPL(...) #__VA_ARGS__
#define PROFILE_STRING(...) PROFILE_STRING_IMPL(__VA_ARGS__)

// Undeclared arguments also prove that disabled events never evaluate inputs.
static_assert(sizeof(PROFILE_STRING(LUMIN_PROFILE_FRAME())) == 1);
static_assert(sizeof(PROFILE_STRING(LUMIN_PROFILE_FRAME_N(unknown_name))) == 1);
static_assert(sizeof(PROFILE_STRING(LUMIN_PROFILE_ZONE())) == 1);
static_assert(sizeof(PROFILE_STRING(LUMIN_PROFILE_ZONE_N(unknown_name))) == 1);
static_assert(sizeof(PROFILE_STRING(LUMIN_PROFILE_PLOT(unknown_name, unknown_value))) == 1);
static_assert(sizeof(PROFILE_STRING(LUMIN_PROFILE_MSG(unknown_text, unknown_size))) == 1);
static_assert(sizeof(PROFILE_STRING(LUMIN_PROFILE_THREAD(unknown_name))) == 1);

namespace {
class CompileLock {
    int& m_acquisitions;

public:
    constexpr explicit CompileLock(int& count)
        : m_acquisitions(count) {
        ++m_acquisitions;
    }
    constexpr ~CompileLock() {
        --m_acquisitions;
    }
};

static_assert([] {
    int held = 0;
    int evaluations = 0;
    {
        LUMIN_PROFILE_LOCK(const CompileLock, lock, held, unknown_name);
        if (held != 1 || LUMIN_PROFILE_WAIT(unknown_name, ++evaluations) != 1)
            return false;
    }
    return held == 0 && evaluations == 1;
}());
} // namespace
