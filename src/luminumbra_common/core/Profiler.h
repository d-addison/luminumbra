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
// and release build), every macro below expands to `((void)0)` / nothing -- zero
// instructions emitted, no Tracy dependency fetched or linked -- so the compiled
// binaries and `world_hash` are byte-identical to a Tracy-free tree. NEVER define
// LUMINUMBRA_ENABLE_TRACY in a determinism-gate or release configuration; it is a
// dedicated dev/profiling build flag (`-DLUMINUMBRA_ENABLE_TRACY=ON`).

#if defined(LUMINUMBRA_ENABLE_TRACY)

#include <tracy/Tracy.hpp>

// Frame/tick boundary. In the fixed-tick sim one tick == one frame.
#define LUMIN_PROFILE_FRAME() FrameMark
#define LUMIN_PROFILE_FRAME_N(name)    FrameMarkNamed(name)
// Scoped CPU zone (RAII -- lives to the end of the enclosing block).
#define LUMIN_PROFILE_ZONE() ZoneScoped
#define LUMIN_PROFILE_ZONE_N(name)     ZoneScopedN(name)
// Named scalar plot (e.g. a per-tick latency in ms).
#define LUMIN_PROFILE_PLOT(name, val)  TracyPlot(name, val)
// One-off timeline message.
#define LUMIN_PROFILE_MSG(txt, size)   TracyMessage(txt, size)
// Name the current thread in the profiler.
#define LUMIN_PROFILE_THREAD(name)     ::tracy::SetThreadName(name)

#else

#define LUMIN_PROFILE_FRAME() ((void)0)
#define LUMIN_PROFILE_FRAME_N(name)    ((void)0)
#define LUMIN_PROFILE_ZONE() ((void)0)
#define LUMIN_PROFILE_ZONE_N(name)     ((void)0)
#define LUMIN_PROFILE_PLOT(name, val)  ((void)0)
#define LUMIN_PROFILE_MSG(txt, size)   ((void)0)
#define LUMIN_PROFILE_THREAD(name)     ((void)0)

#endif  // LUMINUMBRA_ENABLE_TRACY
