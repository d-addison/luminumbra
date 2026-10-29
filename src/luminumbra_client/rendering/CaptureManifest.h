#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <variant>

namespace Luminumbra::Rendering {

struct FarReplayCamera;

// Plain capture-time observations. No GL, filesystem reads or simulation ownership.
// Empty identities serialize as null and require a nonempty unavailable reason.
struct CaptureIdentity {
    std::string value;
    std::string unavailable;
};

using CaptureSetting = std::variant<bool, std::int64_t, std::uint64_t, double, std::string>;

struct CaptureManifestInput {
    CaptureIdentity build_id;
    CaptureIdentity asset_hash;
    CaptureIdentity shader_hash;
    CaptureIdentity world_hash;
    std::string asset_hash_scope;
    std::string world_seed;
    std::uint64_t simulation_tick = 0;
    std::array<float, 16> view{};       // column-major: index = column * 4 + row
    std::array<float, 16> projection{}; // unjittered reversed-Z, GL_ZERO_TO_ONE
    std::array<float, 3> position{};
    std::array<float, 3> forward{};
    float vertical_fov_degrees = 45.0f;
    float near_plane = 0.1f;
    float far_plane = 3200.0f;
    int width = 0;
    int height = 0;
    std::map<std::string, CaptureSetting> render_settings;
};

// Copies the camera actually used to prepare the rendered frame, rather than a
// potentially moved live camera. Eye/forward derive from its rigid view matrix.
// Invalid or mismatched snapshot dimensions leave input unchanged.
bool SetCaptureManifestCamera(CaptureManifestInput& input, const FarReplayCamera& camera);

enum class CaptureManifestStatus {
    Incomplete,
    Complete,
    Failed
};
using CaptureTime = std::chrono::system_clock::time_point;
using CaptureClock = CaptureTime (*)();
CaptureTime CaptureSystemUtc();

// Stable lexicographic object-key order; JSON numbers round-trip float32 values.
// Refuses nonfinite values, invalid extents/planes and oversized metadata. Empty
// result means invalid input; no partial document is returned.
std::string BuildCaptureManifest(const CaptureManifestInput& input,
                                 CaptureTime started,
                                 CaptureTime finished,
                                 CaptureManifestStatus status,
                                 const std::string& reason);
std::filesystem::path CaptureManifestPath(const std::filesystem::path& capture);

// Render-thread owner, used only inside explicit CLI capture branches. Begin must
// succeed before readback/output. Finish publishes the verdict after all payloads
// close. A crash/exception leaves incomplete evidence. Each replacement closes an
// exclusively created sibling .tmp file before atomic rename (also on Windows).
// Concurrent writers of the same capture path are refused while .tmp is occupied.
class CaptureManifest {
public:
    CaptureManifest(const std::filesystem::path& capture,
                    const CaptureManifestInput& input,
                    CaptureClock clock = CaptureSystemUtc);
    CaptureManifest(const CaptureManifest&) = delete;
    CaptureManifest& operator=(const CaptureManifest&) = delete;
    bool Begin();
    bool Finish(bool capture_succeeded);

private:
    std::filesystem::path m_path;
    CaptureManifestInput m_input;
    CaptureClock m_clock;
    CaptureTime m_started{};
    bool m_active = false;
};

template<typename Capture>
bool WithCaptureManifest(const std::filesystem::path& capture,
                         const CaptureManifestInput& input,
                         const Capture& write_capture,
                         CaptureClock clock = CaptureSystemUtc) {
    CaptureManifest manifest(capture, input, clock);
    if (!manifest.Begin())
        return false;
    const bool captured = write_capture();
    const bool finished = manifest.Finish(captured);
    return captured && finished;
}

} // namespace Luminumbra::Rendering
