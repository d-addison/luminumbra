#include "core/Profiler.h"

#include <mutex>
#include <shared_mutex>
#include <type_traits>

#if !defined(LUMINUMBRA_ENABLE_TRACY) || !defined(TRACY_ENABLE) || !defined(TRACY_ON_DEMAND)
#error This compile-only target requires the real on-demand profiler interface.
#endif

// Compile every public macro against the pinned dependency, including forms
// currently unused by production. The application targets compile actual call
// sites and inline rendering/world headers separately; this is not a substitute.
void CompileProfilerForms(std::mutex& mutex, std::shared_mutex& shared_mutex) {
    LUMIN_PROFILE_THREAD("Compile worker");
    LUMIN_PROFILE_FRAME();
    LUMIN_PROFILE_FRAME_N("Compile frame");
    LUMIN_PROFILE_ZONE();
    {
        LUMIN_PROFILE_ZONE_N("Compile nested zone");
        LUMIN_PROFILE_PLOT("Compile plot", 1.0);
        constexpr char message[] = "compile";
        LUMIN_PROFILE_MSG(message, sizeof(message) - 1);
    }
    {
        LUMIN_PROFILE_LOCK(std::lock_guard<std::mutex>, lock, mutex, "Compile lock");
        static_assert(std::is_same_v<decltype(lock), std::lock_guard<std::mutex>>);
    }
    {
        LUMIN_PROFILE_LOCK(std::unique_lock, lock, mutex, "Compile deduced lock");
        static_assert(std::is_same_v<decltype(lock), std::unique_lock<std::mutex>>);
        LUMIN_PROFILE_WAIT("Compile void wait", lock.unlock());
        const bool owns = LUMIN_PROFILE_WAIT("Compile value wait", lock.owns_lock());
        (void)owns;
    }
    {
        LUMIN_PROFILE_LOCK(
            std::shared_lock<std::shared_mutex>, lock, shared_mutex, "Compile shared lock");
        static_assert(std::is_same_v<decltype(lock), std::shared_lock<std::shared_mutex>>);
    }
    const int value = LUMIN_PROFILE_WAIT("Compile predicate wait", [&mutex, &shared_mutex] {
        (void)mutex;
        (void)shared_mutex;
        return 1;
    }());
    (void)value;
}
