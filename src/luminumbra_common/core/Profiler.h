#pragma once

// Luminumbra profiling seam -- Tracy Profiler behind LUMINUMBRA_ENABLE_TRACY.
//
// Tracy's GPU-zone abstraction spans
// OpenGL AND Vulkan/D3D12, so it survives the GL -> Diligent RHI migration where the
// bespoke GL-only GpuTimerPass (glBeginQuery) cannot. It is wired behind an
// engine-owned shim (this header) exactly like the RHI seam sits above Diligent, so
// no Tracy header leaks into general code and the profiler can be swapped or removed
// from one place.
//
// DETERMINISM: off by default. When LUMINUMBRA_ENABLE_TRACY is undefined (every gate
// and release build), the event macros below expand to nothing -- zero
// instructions emitted, no Tracy dependency fetched or linked -- so the compiled
// binaries and `world_hash` are byte-identical to a Tracy-free tree. NEVER define
// LUMINUMBRA_ENABLE_TRACY in a determinism-gate or release configuration; it is a
// dedicated dev/profiling build flag (`-DLUMINUMBRA_ENABLE_TRACY=ON`).

#if defined(LUMINUMBRA_ENABLE_TRACY)

#include <tracy/Tracy.hpp>

// Frame/tick boundary. In the fixed-tick sim one tick == one frame.
#define LUMIN_PROFILE_FRAME() FrameMark
#define LUMIN_PROFILE_FRAME_N(name) FrameMarkNamed(name)
// Scoped CPU zone (RAII -- lives to the end of the enclosing block).
// Unique locals avoid shadowing when a pass contains nested zones.
#define LUMIN_PROFILE_JOIN_IMPL(a, b) a##b
#define LUMIN_PROFILE_JOIN(a, b) LUMIN_PROFILE_JOIN_IMPL(a, b)
#define LUMIN_PROFILE_ZONE_IMPL(variable) ZoneNamed(variable, true)
#define LUMIN_PROFILE_ZONE_N_IMPL(variable, name) ZoneNamedN(variable, name, true)
#define LUMIN_PROFILE_ZONE()                                                                       \
    LUMIN_PROFILE_ZONE_IMPL(LUMIN_PROFILE_JOIN(lumin_profile_zone_, __LINE__))
#define LUMIN_PROFILE_ZONE_N(name)                                                                 \
    LUMIN_PROFILE_ZONE_N_IMPL(LUMIN_PROFILE_JOIN(lumin_profile_zone_, __LINE__), name)
// Named scalar plot (e.g. a per-tick latency in ms).
#define LUMIN_PROFILE_PLOT(name, val) TracyPlot(name, val)
// One-off timeline message.
#define LUMIN_PROFILE_MSG(txt, size) TracyMessage(txt, size)
// Name the current thread in the profiler.
#define LUMIN_PROFILE_THREAD(name) ::tracy::SetThreadName(name)
// Measure acquisition only. Guaranteed copy elision preserves the lock's lifetime
// in the caller; the temporary zone ends as soon as the constructor returns.
#define LUMIN_PROFILE_LOCK(type, variable, mutex, name)                                            \
    auto variable = [&]() {                                                                        \
        LUMIN_PROFILE_ZONE_N(name);                                                                \
        return type(mutex);                                                                        \
    }()
// Expressions (including predicates) are evaluated exactly once.
#define LUMIN_PROFILE_WAIT(name, ...)                                                              \
    [&]() {                                                                                        \
        LUMIN_PROFILE_ZONE_N(name);                                                                \
        return (__VA_ARGS__);                                                                      \
    }()

#else

#define LUMIN_PROFILE_FRAME()
#define LUMIN_PROFILE_FRAME_N(name)
#define LUMIN_PROFILE_ZONE()
#define LUMIN_PROFILE_ZONE_N(name)
#define LUMIN_PROFILE_PLOT(name, val)
#define LUMIN_PROFILE_MSG(txt, size)
#define LUMIN_PROFILE_THREAD(name)
// These two wrappers preserve the original operation, with no profiler code.
#define LUMIN_PROFILE_LOCK(type, variable, mutex, name) type variable(mutex)
#define LUMIN_PROFILE_WAIT(name, ...) (__VA_ARGS__)

#endif // LUMINUMBRA_ENABLE_TRACY
