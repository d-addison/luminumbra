#pragma once
//  (Path to 600 fps),  — honest measurement substrate.
//
// Optional, guarded NVML sampler for GPU power (W) + core clock (MHz). The
// frame is CPU/present-bound today and the GPU DOWNCLOCKS while it starves, so
// per-pass GPU-ms timers measured at idle clock are misleading — the budget
// gate must report power+clock alongside ms to prove the card is at boost.
//
// nvml.dll ships with the NVIDIA driver; we LOAD it dynamically (no SDK
// link-time dependency) so non-NVIDIA / headless / CI builds keep building and
// running — supported simply returns false and sampling is skipped.

#include <cstdint>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Luminumbra {
namespace Client {

class NvmlSampler {
public:
    // Returns true if NVML loaded + a device handle was acquired. Safe to call
    // once; cheap no-op + false on non-NVIDIA / headless / non-Windows.
    bool init() {
#if defined(_WIN32)
        if (m_ok)
            return true;
        m_lib = ::LoadLibraryA("nvml.dll");
        if (!m_lib) {
            m_lib = ::LoadLibraryA("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
        }
        if (!m_lib)
            return false;
        // Route through void* to satisfy -Werror=cast-function-type (GCC rejects a
        // direct FARPROC -> typed-fn-ptr reinterpret_cast). Function<->object
        // pointer conversion is conditionally-supported and valid on Win64.
        auto load = [&](const char* name) -> void* {
            return reinterpret_cast<void*>(::GetProcAddress(m_lib, name));
        };
        auto p_init = reinterpret_cast<int (*)()>(load("nvmlInit_v2"));
        m_getHandle =
            reinterpret_cast<int (*)(unsigned int, void**)>(load("nvmlDeviceGetHandleByIndex_v2"));
        m_getPower =
            reinterpret_cast<int (*)(void*, unsigned int*)>(load("nvmlDeviceGetPowerUsage"));
        m_getClock =
            reinterpret_cast<int (*)(void*, int, unsigned int*)>(load("nvmlDeviceGetClockInfo"));
        m_shutdown = reinterpret_cast<int (*)()>(load("nvmlShutdown"));
        if (!p_init || !m_getHandle || !m_getPower || !m_getClock)
            return false;
        if (p_init() != 0)
            return false; // NVML_SUCCESS == 0
        if (m_getHandle(0u, &m_dev) != 0)
            return false;
        m_ok = true;
        return true;
#else
        return false;
#endif
    }

    bool supported() const {
        return m_ok;
    }

    // Sample GPU board power (watts) + GRAPHICS clock (MHz). Returns true if at
    // least one value was read; leaves outputs untouched on failure.
    bool sample(double& power_w, double& clock_mhz) {
#if defined(_WIN32)
        if (!m_ok)
            return false;
        unsigned int mw = 0u, mhz = 0u;
        bool got = false;
        if (m_getPower && m_getPower(m_dev, &mw) == 0) {
            power_w = static_cast<double>(mw) / 1000.0;
            got = true;
        }
        if (m_getClock && m_getClock(m_dev, 0 /*NVML_CLOCK_GRAPHICS*/, &mhz) == 0) {
            clock_mhz = static_cast<double>(mhz);
            got = true;
        }
        return got;
#else
        (void)power_w;
        (void)clock_mhz;
        return false;
#endif
    }

    void shutdown() {
#if defined(_WIN32)
        if (m_ok && m_shutdown)
            m_shutdown();
        if (m_lib)
            ::FreeLibrary(m_lib);
        m_lib = nullptr;
        m_ok = false;
#endif
    }

private:
    bool m_ok = false;
#if defined(_WIN32)
    HMODULE m_lib = nullptr;
    void* m_dev = nullptr;
    int (*m_getHandle)(unsigned int, void**) = nullptr;
    int (*m_getPower)(void*, unsigned int*) = nullptr;
    int (*m_getClock)(void*, int, unsigned int*) = nullptr;
    int (*m_shutdown)() = nullptr;
#endif
};

} // namespace Client
} // namespace Luminumbra
