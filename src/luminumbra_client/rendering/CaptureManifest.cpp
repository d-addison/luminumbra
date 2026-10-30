#include "CaptureManifest.h"

#include "FarWorldReplay.h"
#include "PixelIo.h"
#include "luminumbra_common/persistence/SavedInputLimits.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <system_error>
#include <type_traits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Luminumbra::Rendering {
namespace {

// Bounds cover all live settings; do not allocate/serialize arbitrary metadata.
constexpr std::size_t kMaxText = 256;
constexpr std::size_t kMaxSettings = 128;

bool ValidIdentity(const CaptureIdentity& identity) {
    return identity.value.size() <= kMaxText && identity.unavailable.size() <= kMaxText &&
           (!identity.value.empty() || !identity.unavailable.empty());
}

template<std::size_t Size>
bool Finite(const std::array<float, Size>& values) {
    return std::all_of(
        values.begin(), values.end(), [](float value) { return std::isfinite(value); });
}

bool ValidInput(const CaptureManifestInput& input) {
    std::size_t bytes = 0;
    if (!ValidIdentity(input.build_id) || !ValidIdentity(input.asset_hash) ||
        !ValidIdentity(input.shader_hash) || !ValidIdentity(input.world_hash) ||
        input.world_seed.size() > Persistence::SavedInputLimits::kWorldSeedBytes ||
        input.asset_hash_scope.size() > kMaxText ||
        !CapturePixelBytes(input.width, input.height, 4, bytes) || !Finite(input.view) ||
        !Finite(input.projection) || !Finite(input.position) || !Finite(input.forward) ||
        !std::isfinite(input.vertical_fov_degrees) || input.vertical_fov_degrees <= 0.0f ||
        input.vertical_fov_degrees >= 180.0f || !std::isfinite(input.near_plane) ||
        !std::isfinite(input.far_plane) || input.near_plane <= 0.0f ||
        input.far_plane <= input.near_plane || input.render_settings.size() > kMaxSettings)
        return false;
    for (const auto& [key, setting] : input.render_settings) {
        if (key.empty() || key.size() > 64)
            return false;
        const bool valid = std::visit(
            [](const auto& value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, double>)
                    return std::isfinite(value);
                else if constexpr (std::is_same_v<Value, std::string>)
                    return value.size() <= kMaxText;
                else
                    return true;
            },
            setting);
        if (!valid)
            return false;
    }
    return true;
}

std::string UtcText(CaptureTime time) {
    using namespace std::chrono;
    // Subtract in milliseconds: flooring a minimum nanosecond time_point to
    // days and converting back to nanoseconds could overflow before subtraction.
    const auto epoch_millis = floor<milliseconds>(time.time_since_epoch());
    const auto day = floor<days>(epoch_millis);
    const year_month_day date{sys_days(day)};
    const hh_mm_ss tod(epoch_millis - day);
    const int year_number = static_cast<int>(date.year());
    if (year_number < 0 || year_number > 9999)
        return {};
    // UTC text is exactly 24 bytes plus NUL; the buffer is bounded to 32 bytes.
    std::array<char, 32> buffer{};
    const int count = std::snprintf(buffer.data(),
                                    buffer.size(),
                                    "%04d-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ",
                                    year_number,
                                    static_cast<unsigned>(date.month()),
                                    static_cast<unsigned>(date.day()),
                                    static_cast<long long>(tod.hours().count()),
                                    static_cast<long long>(tod.minutes().count()),
                                    static_cast<long long>(tod.seconds().count()),
                                    static_cast<long long>(tod.subseconds().count()));
    return count > 0 && static_cast<std::size_t>(count) < buffer.size()
               ? std::string(buffer.data(), static_cast<std::size_t>(count))
               : std::string{};
}

void Identity(nlohmann::json& document, const char* key, const CaptureIdentity& identity) {
    document[key] =
        identity.value.empty() ? nlohmann::json(nullptr) : nlohmann::json(identity.value);
    if (identity.value.empty())
        document[std::string(key) + "_unavailable"] = identity.unavailable;
}

bool AtomicManifest(const std::filesystem::path& path, const std::string& text) {
    if (text.empty())
        return false;
    std::error_code error;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;
    auto temporary = path;
    temporary += ".tmp";
    // Exclusive creation refuses stale/partial files and links without a
    // check-then-open window. Never remove a path on failure: retain evidence.
    std::FILE* output = nullptr;
#if defined(_WIN32)
    if (_wfopen_s(&output, temporary.c_str(), L"wbx") != 0)
        return false;
#else
    output = std::fopen(temporary.c_str(), "wbx");
#endif
    if (!output)
        return false;
    const bool written = std::fwrite(text.data(), 1, text.size(), output) == text.size();
    const bool flushed = std::fflush(output) == 0;
    const bool closed = std::fclose(output) == 0;
    if (!written || !flushed || !closed)
        return false;
#if defined(_WIN32)
    // std::filesystem::rename cannot replace an existing destination on Windows.
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    std::filesystem::rename(temporary, path, error);
    return !error;
#endif
}

} // namespace

bool SetCaptureManifestCamera(CaptureManifestInput& input, const FarReplayCamera& camera) {
    if (input.width <= 0 || input.height <= 0 ||
        camera.width != static_cast<std::uint32_t>(input.width) ||
        camera.height != static_cast<std::uint32_t>(input.height) || !Finite(camera.view) ||
        !Finite(camera.projection) || !std::isfinite(camera.zoom) || camera.zoom <= 0.0f ||
        camera.zoom >= 180.0f || !std::isfinite(camera.near_plane) || camera.near_plane <= 0.0f ||
        !std::isfinite(camera.far_plane) || camera.far_plane <= camera.near_plane)
        return false;
    const auto& view = camera.view;
    // Camera::GetViewMatrix is rigid: inverse translation = -transpose(R) * t.
    // Double intermediates prevent overflow before the float-range checks below.
    std::array<float, 3> position{};
    for (std::size_t axis = 0; axis < position.size(); ++axis) {
        const auto offset = axis * 4;
        const double coordinate =
            -(double(view[offset]) * view[12] + double(view[offset + 1]) * view[13] +
              double(view[offset + 2]) * view[14]);
        if (!std::isfinite(coordinate) || std::abs(coordinate) > std::numeric_limits<float>::max())
            return false;
        position[axis] = static_cast<float>(coordinate);
    }
    input.view = camera.view;
    input.projection = camera.projection;
    input.position = position;
    input.forward = {-view[2], -view[6], -view[10]};
    input.vertical_fov_degrees = camera.zoom;
    input.near_plane = camera.near_plane;
    input.far_plane = camera.far_plane;
    return true;
}

CaptureTime CaptureSystemUtc() {
    return std::chrono::system_clock::now();
}

std::string BuildCaptureManifest(const CaptureManifestInput& input,
                                 CaptureTime started,
                                 CaptureTime finished,
                                 CaptureManifestStatus status,
                                 const std::string& reason) try {
    if (!ValidInput(input) || reason.empty() || reason.size() > kMaxText)
        return {};
    const auto started_text = UtcText(started);
    const auto finished_text = UtcText(finished);
    if (started_text.empty() || finished_text.empty())
        return {};
    const char* status_text = nullptr;
    switch (status) {
        case CaptureManifestStatus::Incomplete:
            status_text = "incomplete";
            break;
        case CaptureManifestStatus::Complete:
            status_text = "complete";
            break;
        case CaptureManifestStatus::Failed:
            status_text = "failed";
            break;
        default:
            return {};
    }
    nlohmann::json document;
    document["schema"] = 1;
    Identity(document, "build_id", input.build_id);
    Identity(document, "asset_hash", input.asset_hash);
    Identity(document, "shader_hash", input.shader_hash);
    Identity(document, "world_hash", input.world_hash);
    document["asset_hash_scope"] = input.asset_hash_scope;
    document["simulation_tick"] = input.simulation_tick;
    document["world_seed"] = input.world_seed;
    document["camera"] = {
        {"matrix_order", "column_major"},
        {"view", input.view},
        {"projection", input.projection},
        {"position", input.position},
        {"forward", input.forward},
        {"vertical_fov_degrees", input.vertical_fov_degrees},
        {"near_plane", input.near_plane},
        {"far_plane", input.far_plane},
        {"projection_convention", "right_handed_reversed_z_zero_to_one_unjittered"}};
    document["resolution"] = {{"width", input.width}, {"height", input.height}};
    auto settings = nlohmann::json::object();
    for (const auto& entry : input.render_settings)
        settings[entry.first] =
            std::visit([](const auto& value) -> nlohmann::json { return value; }, entry.second);
    document["render_settings"] = std::move(settings);
    document["started_utc"] = started_text;
    document["finished_utc"] = status == CaptureManifestStatus::Incomplete
                                   ? nlohmann::json(nullptr)
                                   : nlohmann::json(finished_text);
    document["status"] = status_text;
    document["reason"] = reason;
    return document.dump(2) + "\n";
} catch (const nlohmann::json::exception&) {
    // Invalid UTF-8 metadata is a refusal, never a partially published document.
    return {};
}

std::filesystem::path CaptureManifestPath(const std::filesystem::path& capture) {
    if (capture.empty() || capture.filename().empty())
        return {};
    auto path = capture;
    path.replace_extension(".manifest.json");
    return path;
}

CaptureManifest::CaptureManifest(const std::filesystem::path& capture,
                                 const CaptureManifestInput& input,
                                 CaptureClock clock)
    : m_path(CaptureManifestPath(capture))
    , m_input(ValidInput(input) ? input : CaptureManifestInput{})
    , m_clock(clock) {}

bool CaptureManifest::Begin() {
    if (m_active || !m_clock || m_path.empty())
        return false;
    m_started = m_clock();
    if (!AtomicManifest(m_path,
                        BuildCaptureManifest(m_input,
                                             m_started,
                                             m_started,
                                             CaptureManifestStatus::Incomplete,
                                             "capture_in_progress")))
        return false;
    m_active = true;
    return true;
}

bool CaptureManifest::Finish(bool capture_succeeded) {
    if (!m_active)
        return false;
    const bool written = AtomicManifest(
        m_path,
        BuildCaptureManifest(m_input,
                             m_started,
                             m_clock(),
                             capture_succeeded ? CaptureManifestStatus::Complete
                                               : CaptureManifestStatus::Failed,
                             capture_succeeded ? "capture_finished" : "capture_failed"));
    m_active = false;
    return written;
}

} // namespace Luminumbra::Rendering
