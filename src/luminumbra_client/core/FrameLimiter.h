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

#include <mmsystem.h>
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

// Native timer and sleep behavior is unqualified until measured on Windows;
// VirtualTiming tests qualify only the pacing policy, not the platform APIs.
class SteadyFrameTiming {
public:
#if defined(_WIN32)
    SteadyFrameTiming() {
        DWORD adjustment = 0;
        DWORD increment = 0;
        BOOL adjustment_disabled = FALSE;
        if (::GetSystemTimeAdjustment(&adjustment, &increment, &adjustment_disabled)) {
            // Use the queried tick if the 1 ms resolution request fails.
            const auto tick = std::chrono::nanoseconds(static_cast<std::int64_t>(increment) * 100);
            if (tick >= std::chrono::milliseconds(1))
                m_coarse_quantum = tick;
        }
        // Unsupported systems reject the high-resolution flag; use coarse sleeps
        // instead. The handle is reused for every frame.
        m_timer = ::CreateWaitableTimerExW(nullptr,
                                           nullptr,
                                           CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (m_timer == nullptr)
            request_coarse_resolution();
    }

    ~SteadyFrameTiming() {
        if (m_timer != nullptr)
            ::CloseHandle(m_timer);
        if (m_resolution_requested)
            ::timeEndPeriod(1);
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
            return CoarseFrameSleepMargin(m_coarse_quantum);
#endif
        return kFineMargin;
    }

    // Zero selects the fine sleep margin. A coarse quantum is at least 1 ms;
    // a positive sleep of up to that duration can consume a whole timer tick.
    [[nodiscard]] std::chrono::steady_clock::duration coarse_sleep_quantum() const {
#if defined(_WIN32)
        if (m_timer == nullptr)
            return std::chrono::ceil<std::chrono::steady_clock::duration>(m_coarse_quantum);
#endif
        return std::chrono::steady_clock::duration::zero();
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
            request_coarse_resolution();
            // Return without sleeping: the limiter must recompute its wait
            // using the coarse quantum before trying again in this frame.
            return;
        }
        const auto remaining = deadline - now();
        if (remaining > std::chrono::steady_clock::duration::zero()) {
            // Round upward explicitly: a sub-millisecond request must still
            // sleep for a positive quantum instead of becoming Sleep(0).
            const auto milliseconds =
                std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
            // Sleep(INFINITE) never returns; saturate longer public requests.
            constexpr auto max_sleep = static_cast<std::int64_t>(INFINITE - 1);
            ::Sleep(static_cast<DWORD>(milliseconds > max_sleep ? max_sleep : milliseconds));
        }
#else
        std::this_thread::sleep_until(deadline);
#endif
    }

    void spin() const {
#if defined(_WIN32)
        if (m_timer == nullptr)
            std::this_thread::yield();
        else
            YieldProcessor();
#endif
    }

private:
    static constexpr auto kFineMargin = std::chrono::microseconds(200);
#if defined(_WIN32)
    void request_coarse_resolution() {
        m_resolution_requested = ::timeBeginPeriod(1) == TIMERR_NOERROR;
        if (m_resolution_requested)
            m_coarse_quantum = std::chrono::milliseconds(1);
    }

    HANDLE m_timer = nullptr;
    bool m_resolution_requested = false;
    // If querying fails, cover the usual 15.625 ms Windows timer tick.
    std::chrono::nanoseconds m_coarse_quantum{15625000};
#endif
};

// Call before starting any measured frame buckets. Timing supplies a monotonic
// now(), sleep_margin(), coarse_sleep_quantum(), sleep_until() and spin().
// Injection keeps pacing deterministic with a virtual clock, including quantized
// sleeps. No allocation or type-erased call occurs per frame.
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
            while (now < deadline) {
                const auto quantum = timing.coarse_sleep_quantum();
                const auto remaining = deadline - now;
                if (quantum > Clock::duration::zero() && remaining >= quantum) {
                    // Request whole ticks with a positive final sleep of 1 ms.
                    // Rounding up to the next coarse tick then fits in the
                    // remaining time, leaving less than one tick to yield-spin.
                    const auto ticks = remaining / quantum;
                    timing.sleep_until(now + (ticks - 1) * quantum + std::chrono::milliseconds(1));
                } else if (quantum == Clock::duration::zero() &&
                           remaining > timing.sleep_margin()) {
                    timing.sleep_until(deadline - timing.sleep_margin());
                } else {
                    timing.spin();
                }
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
