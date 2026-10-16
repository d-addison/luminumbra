#pragma once

#include "luminumbra_common/core/FramePacing.h"

#include <chrono>
#include <cstdint>
#include <ratio>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Luminumbra::Client {

// A coarse sleep can round up by a full timer tick. Reserve that tick plus
// scheduling slack; even a 1 ms timer needs at least a 2 ms margin.
[[nodiscard]] constexpr std::chrono::steady_clock::duration
CoarseFrameSleepMargin(std::chrono::nanoseconds resolution) {
    const auto margin = resolution + std::chrono::milliseconds(1);
    return std::chrono::ceil<std::chrono::steady_clock::duration>(
        margin < std::chrono::milliseconds(2) ? std::chrono::milliseconds(2) : margin);
}

class SteadyFrameTiming {
public:
#if defined(_WIN32)
    SteadyFrameTiming() {
        DWORD adjustment = 0;
        DWORD increment = 0;
        BOOL adjustment_disabled = FALSE;
        if (::GetSystemTimeAdjustment(&adjustment, &increment, &adjustment_disabled)) {
            // The system clock increment is a conservative bound on coarse timer
            // resolution, without changing the process or system timer period.
            m_coarse_margin = CoarseFrameSleepMargin(
                std::chrono::nanoseconds(static_cast<std::int64_t>(increment) * 100));
        }
        // Unsupported systems reject the high-resolution flag; use coarse sleeps
        // with the queried margin instead. The handle is reused for every frame.
        m_timer = ::CreateWaitableTimerExW(nullptr,
                                           nullptr,
                                           CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_MODIFY_STATE | SYNCHRONIZE);
    }

    ~SteadyFrameTiming() {
        if (m_timer != nullptr)
            ::CloseHandle(m_timer);
    }

    SteadyFrameTiming(const SteadyFrameTiming&) = delete;
    SteadyFrameTiming& operator=(const SteadyFrameTiming&) = delete;
#endif

    [[nodiscard]] std::chrono::steady_clock::time_point now() const {
        return std::chrono::steady_clock::now();
    }

    [[nodiscard]] std::chrono::steady_clock::duration sleep_margin() const {
#if defined(_WIN32)
        if (m_timer == nullptr)
            return m_coarse_margin;
#endif
        return kFineMargin;
    }

    void sleep_until(std::chrono::steady_clock::time_point deadline) {
#if defined(_WIN32)
        if (m_timer != nullptr) {
            const auto remaining = deadline - now();
            if (remaining <= std::chrono::steady_clock::duration::zero())
                return;
            using TimerTick = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;
            LARGE_INTEGER due{};
            due.QuadPart = -std::chrono::ceil<TimerTick>(remaining).count();
            if (::SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE) &&
                ::WaitForSingleObject(m_timer, INFINITE) == WAIT_OBJECT_0)
                return;

            ::CloseHandle(m_timer);
            m_timer = nullptr;
            // This call reserved only the fine margin. Shorten this sleep too,
            // so a timer failure cannot fall through to a late coarse wake-up.
            deadline -= m_coarse_margin - kFineMargin;
        }
#endif
        std::this_thread::sleep_until(deadline);
    }

    void spin() const {
#if defined(_WIN32)
        YieldProcessor();
#endif
    }

private:
    static constexpr auto kFineMargin = std::chrono::microseconds(200);
#if defined(_WIN32)
    HANDLE m_timer = nullptr;
    // If querying fails, cover the usual 15.625 ms Windows timer tick.
    std::chrono::steady_clock::duration m_coarse_margin =
        CoarseFrameSleepMargin(std::chrono::nanoseconds(15625000));
#endif
};

// Call before starting any measured frame buckets. Timing supplies a monotonic
// now(), sleep_margin(), sleep_until() and spin(); injection keeps pacing
// deterministic with a virtual clock, including quantized sleeps. No allocation
// or type-erased call occurs per frame.
class FrameLimiter {
public:
    template<typename Timing>
    void pace(int cap, Timing& timing) {
        if (!luminumbra::core::IsValidFrameCap(cap) || cap == 0) {
            m_cap = 0;
            return;
        }

        auto now = timing.now();
        if (m_cap == cap) {
            const auto period = std::chrono::ceil<Clock::duration>(
                std::chrono::duration<double>(1.0 / static_cast<double>(cap)));
            const auto deadline = m_frame_start + period;
            const auto spin_window = timing.sleep_margin();
            if (now < deadline - spin_window) {
                timing.sleep_until(deadline - spin_window);
                now = timing.now();
            }
            while (now < deadline) {
                timing.spin();
                now = timing.now();
            }
        }
        // Rebase after late/slow frames or a setting change. Never accumulate debt
        // and issue bursts of catch-up frames after a stall or an oversleep.
        m_cap = cap;
        m_frame_start = now;
    }

private:
    using Clock = std::chrono::steady_clock;
    int m_cap = 0;
    Clock::time_point m_frame_start{};
};

} // namespace Luminumbra::Client
