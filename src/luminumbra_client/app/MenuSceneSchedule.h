#pragma once

#include "luminumbra_client/rendering/GraphicsRuntimeConfig.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace Luminumbra::Client::App {

// Effective graphics settings, including those composed by graphics presets.
// Compare values instead of invalidating for temporary preview overrides.
struct MenuRenderSettings {
    float render_scale = 1;
    float exposure_override = -1;
    int cloud_quality = 0;
    int ssao_quality = 0;
    int volumetric_quality = 0;
    bool taau = false;
    bool sky_lut_gpu = false;
    bool metered_exposure = false;
    Luminumbra::Rendering::GraphicsRuntimeConfig graphics{};
    bool operator==(const MenuRenderSettings&) const = default;
};

// Camera values before and after the elapsed-time scenic pan. Only external
// edits invalidate immediately; the pan itself keeps the fixed 30 Hz cadence.
class MenuCameraRevision {
public:
    using State = std::array<float, 15>; // position, front, up, world-up, yaw, pitch, FOV
    std::uint64_t observe(const State& state) {
        if (state != m_expected) {
            ++m_revision;
            m_expected = state;
        }
        return m_revision;
    }
    void animated(const State& state) {
        m_expected = state;
    }

private:
    State m_expected{};
    std::uint64_t m_revision = 0;
};

struct MenuSceneKey {
    int width = 0;
    int height = 0;
    std::uint64_t document = 0;
    std::uint64_t preview = 0;
    std::uint64_t preview_world = 0;
    std::uint64_t renderer_target = 0;
    std::uint64_t camera = 0;
    MenuRenderSettings settings{};
    bool preview_active = false;
    bool wireframe = false;
};
inline bool operator==(const MenuSceneKey& left, const MenuSceneKey& right) {
    return left.width == right.width && left.height == right.height &&
           left.document == right.document && left.preview == right.preview &&
           left.preview_world == right.preview_world &&
           left.renderer_target == right.renderer_target && left.camera == right.camera &&
           left.settings == right.settings && left.preview_active == right.preview_active &&
           left.wireframe == right.wireframe;
}

// Absolute 30 Hz slots, rather than now + period: long frames skip obsolete
// samples in O(1), without accumulating drift or rendering a catch-up burst.
class MenuSceneSchedule {
public:
    using Time = std::chrono::nanoseconds;
    bool update(Time now, const MenuSceneKey& key, bool interactive = false) {
        if (!m_started) {
            m_started = true;
            m_epoch = now;
            m_last_render = now;
        }
        m_now = now;
        const auto elapsed = (now - m_epoch).count();
        const auto slot = (elapsed / kSecond) * 30 + ((elapsed % kSecond) * 30) / kSecond;
        const bool due = slot != m_slot;
        m_slot = slot;
        if (key != m_key) {
            m_key = key;
            m_valid = false;
        }
        return key.width > 0 && key.height > 0 && (!m_valid || due || interactive);
    }
    void rendered() {
        m_valid = true;
        m_last_render = m_now;
    }
    void invalidate() {
        m_valid = false;
    }
    bool valid() const {
        return m_valid;
    }
    // Wall presentation time, including cache replay and menu absence. This is
    // independent of the bounded render-only simulation step below.
    double elapsed_seconds() const {
        return std::chrono::duration<double>(m_now - m_epoch).count();
    }
    float render_delta_seconds() const {
        // Match the shipping frame loop's 50 ms ceiling for particles and other
        // render-only integration. Never repay skipped slots as simulation debt.
        // The caller uses steady_clock; like the frame loop, only cap the maximum.
        return std::min(std::chrono::duration<float>(m_now - m_last_render).count(), 1.0f / 20.0f);
    }

private:
    static constexpr std::int64_t kSecond = 1'000'000'000;
    Time m_epoch{}, m_now{}, m_last_render{};
    MenuSceneKey m_key{};
    std::int64_t m_slot = -1;
    bool m_started = false;
    bool m_valid = false;
};

// Validate exact RGBA storage before allocating a menu image on either word size.
inline std::optional<std::size_t> menu_image_bytes(int width, int height) {
    if (width <= 0 || height <= 0)
        return std::nullopt;
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    if (w > std::numeric_limits<std::size_t>::max() / 4 / h)
        return std::nullopt;
    return w * h * 4;
}

// One deadline per requested image, shared by every menu capture mode. Scene,
// readiness and target changes may restart settling, but cannot extend this wait.
// The caller supplies successful-render settling; cached replay is not a sample.
class MenuCaptureGate {
public:
    using Time = std::chrono::steady_clock::time_point;
    enum class Result {
        Waiting,
        Ready,
        TimedOut
    };

    bool arm(Time now, std::size_t index, bool thumbnail) {
        if (m_armed && index == m_index && thumbnail == m_thumbnail)
            return false;
        m_armed = true;
        m_index = index;
        m_thumbnail = thumbnail;
        m_start = now;
        return true;
    }
    Result decide(Time now, bool settled, bool drawable, bool content_ready) const {
        if (!m_armed)
            return Result::Waiting;
        if (now - m_start >= std::chrono::seconds{45})
            return Result::TimedOut;
        if (settled && drawable && content_ready)
            return Result::Ready;
        return Result::Waiting;
    }
    void reset() {
        m_armed = false;
    }

private:
    Time m_start{};
    std::size_t m_index = 0;
    bool m_thumbnail = false;
    bool m_armed = false;
};
} // namespace Luminumbra::Client::App
