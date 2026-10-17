#include "core/Profiler.h"

#if defined(LUMINUMBRA_ENABLE_TRACY) || defined(TRACY_ENABLE)
#error This compile-only target must remain independent of the profiler interface.
#endif

// Fixed parameters keep empty expansion stringification independent of variadic
// argument forwarding in traditional and conforming preprocessors.
#define PROFILE_STRING_IMPL(value) #value
#define PROFILE_STRING(value) PROFILE_STRING_IMPL(value)

static_assert(sizeof(PROFILE_STRING(expansion_control)) == sizeof("expansion_control"));

// Undeclared arguments also prove that disabled events never evaluate inputs.
static_assert(sizeof(PROFILE_STRING(LUMIN_PROFILE_FRAME())) == 1);
static_assert(sizeof(PROFILE_STRING(LUMIN_PROFILE_FRAME_N(unknown_name))) == 1);
static_assert(sizeof(PROFILE_STRING(LUMIN_PROFILE_ZONE())) == 1);
static_assert(sizeof(PROFILE_STRING(LUMIN_PROFILE_ZONE_N(unknown_name))) == 1);
static_assert(sizeof(PROFILE_STRING(LUMIN_PROFILE_PLOT(unknown_name, unknown_value))) == 1);
static_assert(sizeof(PROFILE_STRING(LUMIN_PROFILE_MSG(unknown_text, unknown_size))) == 1);
static_assert(sizeof(PROFILE_STRING(LUMIN_PROFILE_THREAD(unknown_name))) == 1);

namespace {
struct LockState {
    int held = 0;
    int acquisitions = 0;
    int releases = 0;
};

class CompileLock {
    LockState* m_state;

public:
    constexpr explicit CompileLock(LockState& state)
        : m_state(&state) {
        ++m_state->held;
        ++m_state->acquisitions;
    }
    CompileLock(const CompileLock&) = delete;
    CompileLock(CompileLock&&) = delete;
    CompileLock& operator=(const CompileLock&) = delete;
    CompileLock& operator=(CompileLock&&) = delete;
    constexpr ~CompileLock() {
        --m_state->held;
        ++m_state->releases;
    }
};

// Check the probe itself separately from the wrapper. Named constexpr functions
// expose scope exit and each counter independently of an immediate lambda.
constexpr bool CheckDirectLockLifetime() {
    LockState state;
    {
        const CompileLock lock(state);
        if (state.held != 1 || state.acquisitions != 1 || state.releases != 0)
            return false;
    }
    return state.held == 0 && state.acquisitions == 1 && state.releases == 1;
}

template<class Actual, class Expected>
inline constexpr bool SameLockType = false;
template<class Lock>
inline constexpr bool SameLockType<Lock, Lock> = true;

template<class Lock>
constexpr bool CheckLockLifetime() {
    LockState state;
    int mutex_evaluations = 0;
    {
        LUMIN_PROFILE_LOCK(Lock, lock, (++mutex_evaluations, state), unknown_name);
        static_assert(SameLockType<decltype(lock), Lock>);
        if (state.held != 1 || state.acquisitions != 1 || state.releases != 0 ||
            mutex_evaluations != 1)
            return false;
    }
    return state.held == 0 && state.acquisitions == 1 && state.releases == 1 &&
           mutex_evaluations == 1;
}

constexpr bool CheckWaitEvaluation() {
    int evaluations = 0;
    if (LUMIN_PROFILE_WAIT(unknown_name, ++evaluations) != 1)
        return false;
    LUMIN_PROFILE_WAIT(unknown_name, static_cast<void>(++evaluations));
    return evaluations == 2 &&
           LUMIN_PROFILE_WAIT(unknown_name,
                              [&evaluations, increment = 1] {
                                  evaluations += increment;
                                  return evaluations;
                              }()) == 3 &&
           evaluations == 3;
}

constexpr bool CheckDisabledEvents() {
    LUMIN_PROFILE_FRAME();
    LUMIN_PROFILE_FRAME_N(unknown_name);
    LUMIN_PROFILE_ZONE();
    LUMIN_PROFILE_ZONE_N(unknown_name);
    LUMIN_PROFILE_PLOT(unknown_name, unknown_value);
    LUMIN_PROFILE_MSG(unknown_text, unknown_size);
    LUMIN_PROFILE_THREAD(unknown_name);
    return true;
}

static_assert(CheckDirectLockLifetime());
static_assert(CheckLockLifetime<CompileLock>());
static_assert(CheckLockLifetime<const CompileLock>());
static_assert(CheckWaitEvaluation());
static_assert(CheckDisabledEvents());
} // namespace
