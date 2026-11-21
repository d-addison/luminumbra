// LREC1 session replay stream implementation. See ReplayStream.h for
// the format rationale and binding invariants. All multi-byte integers are
// written little-endian with explicit per-byte shifts (no memcpy of native
// integers, no struct padding) so the on-wire bytes are identical on every
// platform/compiler. Strings are length-prefixed (u32 LE length + raw bytes).

#include "replay/ReplayStream.h"
#include "replay/RegionScheduleTrace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/EngineVersion.h"
#include "core/FilesystemPath.h"
#include "core/JobSystem.h"
#include "core/Log.h"
#include "persistence/ReplacementOutputFile.h"
#include "persistence/SaveTransaction.h"
#include "persistence/WorldSaveService.h"
#include "systems/WeatherSystem.h"
#include "world/GameSession.h"
#include "world/SurfaceWorkBudget.h"

namespace Luminumbra::Replay {
world::AuthorityBootDescriptor CaptureReplayBoot(const world::GameSession& session,
                                                 std::uint32_t seed,
                                                 const world::WorldCalendar& calendar,
                                                 world::AuthorityBootDescriptor boot) {
    const auto values = session.EffectiveIdentityValues();
    boot.authority_rules_revision =
        luminumbra::core::SimulationIdentityValues::kAuthorityRulesRevision;
    boot.resolved_world_seed = seed;
    boot.system_config_identity = session.SystemConfigIdentity(calendar);
    static_assert(boot.system_gates.size() == values.enabled.size());
    static_assert(boot.scalar_bits.size() == values.scalars.size());
    // Fixed registry slots: at most 25 gates and 46 scalar/vector settings.
    for (std::size_t i = 0; i < boot.system_gates.size(); ++i) {
        boot.system_gates[i] = static_cast<std::uint8_t>(values.enabled[i]);
    }
    for (std::size_t i = 0; i < boot.scalar_bits.size(); ++i) {
        boot.scalar_bits[i] = std::bit_cast<std::uint64_t>(values.scalars[i]);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            boot.vector_bits[i][axis] =
                std::bit_cast<std::uint32_t>(values.vectors[i][static_cast<int>(axis)]);
        }
    }
    boot.finite_hydrology = values.finite_hydrology_enabled;
    boot.weather_rain_scale_mm = values.weather_rain_scale_mm;
    boot.region_work_limit = values.region_work_limit;
    boot.region_hold_ticks = values.region_hold_ticks;
    boot.region_reduced_shift = values.region_reduced_shift;
    boot.day_length_ticks = calendar.dayLengthTicks;
    boot.days_per_year = calendar.daysPerYear;
    boot.daylight_numerator = calendar.daylightNumerator;
    boot.daylight_denominator = calendar.daylightDenominator;
    boot.lunar_cycle_days = calendar.lunarCycleDays;
    return boot;
}

ReplayHeader CaptureReplayHeader(const world::GameSession& session,
                                 std::uint32_t surface_radius,
                                 std::uint32_t collision_radius,
                                 world::AuthorityBootDescriptor workload) {
    if (session.GetSimulationTickCount() != 0 ||
        !World::SurfaceRadiiSupported(surface_radius, collision_radius) ||
        session.PresetContentHash().empty()) {
        throw std::invalid_argument("Replay requires a fresh boot at tick zero");
    }
    ReplayHeader header;
    header.version = kLrec1BootVersion;
    header.seed_string = session.GetMetadata().seed;
    header.seed = kResolvedSeedTag | session.GetMetadata().resolvedSeed;
    header.preset = session.GetMetadata().worldType;
    header.preset_hash = std::strtoull(Fnv1a64Hex(header.preset).c_str(), nullptr, 16);
    header.surface_radius = surface_radius;
    header.collision_radius = collision_radius;
    header.engine_version = std::string(luminumbra::core::GetEngineVersionString());
    header.system_config_identity = session.SystemConfigIdentity();
    header.preset_content_hash = session.PresetContentHash();
    if (const auto* schedule = session.GetWeatherSchedule()) {
        header.version = kLrec1WeatherVersion;
        header.weather_schedule = schedule->canonical_bytes();
    } else {
        header.authority_boot = CaptureReplayBoot(session,
                                                  session.GetMetadata().resolvedSeed,
                                                  session.GetRenderCalendar(),
                                                  std::move(workload));
    }
    return header;
}

struct ReplayFinalization::State {
    std::string destination;
    Persistence::WorldSaveService::ArtifactIdentity marker_identity{};
    bool prepared = false;
    bool owns_marker = false;
};

ReplayFinalization::ReplayFinalization(const std::string& destination, const std::string& message)
    : m_state(std::make_unique<State>()) {
    m_state->destination = destination;
    try {
        const auto parent = std::filesystem::path(destination).parent_path();
        std::error_code directory_error;
        if (!parent.empty())
            std::filesystem::create_directories(parent, directory_error);
        if (directory_error)
            return;
        const Persistence::ReplayPairReadGuard ownership(destination);
        if (!ownership.is_open())
            return;
        Persistence::WorldSaveService::ArtifactIdentity identity{};
        bool missing = false;
        if (!Persistence::WorldSaveService::artifact_identity(destination, identity, missing))
            return;
        try {
            Persistence::ReplacementOutputFile reason(destination + ".incomplete");
            if (reason.write(message) && reason.commit()) {
                bool marker_missing = false;
                m_state->owns_marker = Persistence::WorldSaveService::artifact_identity(
                    destination + ".incomplete", m_state->marker_identity, marker_missing);
                m_state->prepared = m_state->owns_marker && !marker_missing;
                return;
            }
        } catch (const std::exception&) {
            // Publication failure must not make the retained generation admissible.
        }
        std::error_code error;
        const auto marker = std::filesystem::symlink_status(destination + ".incomplete", error);
        if ((!error && marker.type() != std::filesystem::file_type::not_found) ||
            (error && error != std::errc::no_such_file_or_directory)) {
            return; // Refuse without clearing a companion this attempt does not own.
        }
        m_state->prepared = missing || Persistence::WorldSaveService::remove_owned_entry(
                                           destination, identity, nullptr);
    } catch (const std::exception&) {
        // No payload publication is allowed unless refusal was established first.
    }
}

ReplayFinalization::~ReplayFinalization() = default;

bool ReplayFinalization::ready() const {
    return m_state->prepared;
}

bool ReplayFinalization::Complete() {
    if (!ready())
        return false;
    const auto marker = m_state->destination + ".incomplete";
    try {
        const Persistence::ReplayPairReadGuard ownership(m_state->destination);
        if (!ownership.is_open())
            return false;
        if (m_state->owns_marker) {
            // Recheck identity at removal; never clear another attempt's marker.
            if (Persistence::WorldSaveService::remove_owned_entry(
                    marker, m_state->marker_identity, nullptr))
                return true;
        }
    } catch (const std::exception&) {
        // The removal seam can also throw after the entry was removed.
    }
    // Removal can succeed before a directory flush reports failure. At this
    // point the payload is complete; absence is a successful commit point.
    std::error_code error;
    const auto status = std::filesystem::symlink_status(marker, error);
    return error == std::errc::no_such_file_or_directory ||
           (!error && status.type() == std::filesystem::file_type::not_found);
}

namespace {

namespace fs = std::filesystem;

// --- Bounded text fragments for the recorder's Open refusal. -----------------
// Only the tail of a path or OS message enters the fixed-size refusal; the full
// strings go to the logger. The three helpers feed one "%s%.*s" conversion.
constexpr std::size_t kRefusalFragmentBytes = 80;

// The longest refusal (reserve-directory) with every field at its bound must fit
// the fixed buffer, so the error category and code are never truncated away.
constexpr std::size_t kUnsigned64Digits = 20;
constexpr std::size_t kSignedIntChars = 11;
constexpr std::size_t kCategoryBytes = 16;
constexpr std::size_t kLongestRefusalBytes =
    sizeof("reserve-directory: attempt  candidate 'lockstep-recording-' beside '': (  )") +
    2 * kUnsigned64Digits + 2 * (kRefusalFragmentBytes + sizeof("...") - 1) + kCategoryBytes +
    kSignedIntChars;
static_assert(kLongestRefusalBytes < BoundedReplayRecorder::kOpenRefusalBytes,
              "the longest Open refusal must fit the fixed refusal buffer");

const char* PathTailPrefix(const std::string& text) {
    return text.size() > kRefusalFragmentBytes ? "..." : "";
}

int PathTailLength(const std::string& text) {
    return static_cast<int>(std::min(text.size(), kRefusalFragmentBytes));
}

const char* PathTail(const std::string& text) {
    return text.c_str() + (text.size() - static_cast<std::size_t>(PathTailLength(text)));
}

// --- Little-endian fixed-width append helpers (the only on-wire encoders). ---
void PutU8(std::vector<std::uint8_t>& out, std::uint8_t v) {
    out.push_back(v);
}

void PutU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (std::size_t i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

void PutU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (std::size_t i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

// Length-prefixed string: u32 LE byte count + raw bytes.
void PutString(std::vector<std::uint8_t>& out, const std::string& s) {
    PutU32(out, static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

// Length-prefixed byte blob.
void PutBlob(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& b) {
    PutU32(out, static_cast<std::uint32_t>(b.size()));
    out.insert(out.end(), b.begin(), b.end());
}

// --- Sequential cursor reader over an in-memory byte buffer. -----------------
// Every Get* returns false (and sets the cursor past the end) on a short read,
// so a truncated tail is detected rather than reading garbage.
struct Cursor {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;
    bool ok = true;

    bool Remaining(std::size_t n) const {
        return ok && pos <= size && n <= size - pos;
    }

    bool GetU8(std::uint8_t& v) {
        if (!Remaining(1)) {
            ok = false;
            return false;
        }
        v = data[pos++];
        return true;
    }
    bool GetU16(std::uint16_t& v) {
        if (!Remaining(2)) {
            ok = false;
            return false;
        }
        v = static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[pos]) |
                                       (static_cast<std::uint16_t>(data[pos + 1]) << 8));
        pos += 2;
        return true;
    }
    bool GetU32(std::uint32_t& v) {
        if (!Remaining(4)) {
            ok = false;
            return false;
        }
        v = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(data[pos + i]) << (8 * i);
        }
        pos += 4;
        return true;
    }
    bool GetU64(std::uint64_t& v) {
        if (!Remaining(8)) {
            ok = false;
            return false;
        }
        v = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(data[pos + i]) << (8 * i);
        }
        pos += 8;
        return true;
    }
    bool GetBytes(void* destination, std::size_t bytes) {
        if (!Remaining(bytes)) {
            ok = false;
            return false;
        }
        if (bytes != 0) {
            std::memcpy(destination, data + pos, bytes);
        }
        pos += bytes;
        return true;
    }
};

using BootDescriptor = world::AuthorityBootDescriptor;
constexpr std::array<std::uint8_t, 4> kBootMagic{'A', 'U', 'B', 'D'};

bool ValidBootDescriptor(const BootDescriptor& boot, std::string& error) {
    if (boot.system_config_identity.empty() ||
        boot.system_config_identity.size() > BootDescriptor::kIdentityBytes) {
        error = "authority boot identity length is outside [1,256]";
        return false;
    }
    if (boot.authority_rules_revision == 0 || boot.seed_conversion_version > 1 ||
        boot.region_hold_ticks == 0 || boot.region_reduced_shift == 0 ||
        boot.region_reduced_shift > 16 || boot.avatar_count > BootDescriptor::kMaxAvatars) {
        error = "invalid authority boot revision, seed conversion, scheduler or avatar count";
        return false;
    }
    if (boot.day_length_ticks == 0 || boot.day_length_ticks >= (std::uint32_t{1} << 31) ||
        boot.days_per_year == 0 || boot.days_per_year > 366 || boot.daylight_numerator == 0 ||
        boot.daylight_numerator >= boot.daylight_denominator || boot.daylight_denominator > 65535 ||
        std::gcd(boot.daylight_numerator, boot.daylight_denominator) != 1 ||
        boot.lunar_cycle_days == 0 || boot.lunar_cycle_days > 366) {
        error = "invalid authority boot calendar";
        return false;
    }
    for (const auto gate : boot.system_gates) {
        if (gate > 1) {
            error = "invalid authority boot system gate";
            return false;
        }
    }
    for (std::size_t index = 0; index < BootDescriptor::kSystemParamCount; ++index) {
        if ((boot.scalar_bits[index] & UINT64_C(0x7ff0000000000000)) ==
            UINT64_C(0x7ff0000000000000)) {
            error = "non-finite authority boot scalar";
            return false;
        }
        for (const auto component : boot.vector_bits[index]) {
            if ((component & 0x7f800000u) == 0x7f800000u) {
                error = "non-finite authority boot vector";
                return false;
            }
        }
    }
    return true;
}

bool EncodeBootDescriptor(const BootDescriptor& boot,
                          std::vector<std::uint8_t>& output,
                          std::string& error) {
    error.clear();
    if (!ValidBootDescriptor(boot, error)) {
        return false;
    }
    // Fixed slot counts and the validated identity cap bound this allocation.
    std::vector<std::uint8_t> bytes;
    bytes.reserve(BootDescriptor::kMaxEncodedBytes);
    bytes.insert(bytes.end(), kBootMagic.begin(), kBootMagic.end());
    PutU16(bytes, BootDescriptor::kVersion);
    PutU32(bytes, boot.authority_rules_revision);
    PutU32(bytes, boot.resolved_world_seed);
    PutU32(bytes, boot.seed_conversion_version);
    PutString(bytes, boot.system_config_identity);
    PutU16(bytes, BootDescriptor::kSystemGateCount);
    bytes.insert(bytes.end(), boot.system_gates.begin(), boot.system_gates.end());
    PutU16(bytes, BootDescriptor::kSystemParamCount);
    for (const auto bits : boot.scalar_bits) {
        PutU64(bytes, bits);
    }
    for (const auto& vector : boot.vector_bits) {
        for (const auto bits : vector) {
            PutU32(bytes, bits);
        }
    }
    PutU8(bytes, static_cast<std::uint8_t>(boot.finite_hydrology));
    PutU32(bytes, std::bit_cast<std::uint32_t>(boot.weather_rain_scale_mm));
    PutU64(bytes, boot.region_work_limit);
    PutU32(bytes, boot.region_hold_ticks);
    PutU8(bytes, boot.region_reduced_shift);
    PutU32(bytes, boot.day_length_ticks);
    PutU32(bytes, boot.days_per_year);
    PutU32(bytes, boot.daylight_numerator);
    PutU32(bytes, boot.daylight_denominator);
    PutU32(bytes, boot.lunar_cycle_days);
    PutU64(bytes, boot.parked_bytes_limit);
    PutU32(bytes, boot.avatar_count);
    PutU8(bytes, static_cast<std::uint8_t>(boot.ecology_roster));
    PutU8(bytes, static_cast<std::uint8_t>(boot.planted_roster));
    PutU8(bytes, static_cast<std::uint8_t>(boot.moving_anchor));
    PutU8(bytes, static_cast<std::uint8_t>(boot.water_smoke));
    output.swap(bytes);
    return true;
}

bool DecodeBootDescriptor(std::span<const std::uint8_t> bytes,
                          BootDescriptor& output,
                          std::string& error) {
    error = "truncated authority boot descriptor";
    if (bytes.size() > BootDescriptor::kMaxEncodedBytes) {
        error = "oversized authority boot descriptor";
        return false;
    }
    Cursor cursor{bytes.data(), bytes.size(), 0, true};
    std::array<std::uint8_t, kBootMagic.size()> magic{};
    std::uint16_t version = 0;
    if (!cursor.GetBytes(magic.data(), magic.size()) || !cursor.GetU16(version)) {
        return false;
    }
    if (magic != kBootMagic || version != BootDescriptor::kVersion) {
        error = "bad authority boot magic or unsupported descriptor version";
        return false;
    }
    BootDescriptor boot;
    std::uint32_t identity_bytes = 0;
    if (!cursor.GetU32(boot.authority_rules_revision) || !cursor.GetU32(boot.resolved_world_seed) ||
        !cursor.GetU32(boot.seed_conversion_version) || !cursor.GetU32(identity_bytes)) {
        return false;
    }
    if (identity_bytes > BootDescriptor::kIdentityBytes) {
        error = "oversized authority boot identity";
        return false;
    }
    if (!cursor.Remaining(identity_bytes)) {
        return false;
    }
    boot.system_config_identity.resize(identity_bytes);
    if (!cursor.GetBytes(boot.system_config_identity.data(), identity_bytes)) {
        return false;
    }
    std::uint16_t count = 0;
    if (!cursor.GetU16(count)) {
        return false;
    }
    if (count != BootDescriptor::kSystemGateCount) {
        error = "unsupported authority boot gate count";
        return false;
    }
    for (auto& gate : boot.system_gates) {
        if (!cursor.GetU8(gate)) {
            return false;
        }
    }
    if (!cursor.GetU16(count)) {
        return false;
    }
    if (count != BootDescriptor::kSystemParamCount) {
        error = "unsupported authority boot parameter count";
        return false;
    }
    for (auto& bits : boot.scalar_bits) {
        if (!cursor.GetU64(bits)) {
            return false;
        }
    }
    for (auto& vector : boot.vector_bits) {
        for (auto& bits : vector) {
            if (!cursor.GetU32(bits)) {
                return false;
            }
        }
    }
    const auto boolean = [&cursor, &error](bool& value) {
        std::uint8_t byte = 0;
        if (!cursor.GetU8(byte)) {
            return false;
        }
        if (byte > 1) {
            error = "invalid authority boot boolean";
            return false;
        }
        value = byte != 0;
        return true;
    };
    std::uint32_t rain_bits = 0;
    if (!boolean(boot.finite_hydrology) || !cursor.GetU32(rain_bits) ||
        !cursor.GetU64(boot.region_work_limit) || !cursor.GetU32(boot.region_hold_ticks) ||
        !cursor.GetU8(boot.region_reduced_shift) || !cursor.GetU32(boot.day_length_ticks) ||
        !cursor.GetU32(boot.days_per_year) || !cursor.GetU32(boot.daylight_numerator) ||
        !cursor.GetU32(boot.daylight_denominator) || !cursor.GetU32(boot.lunar_cycle_days) ||
        !cursor.GetU64(boot.parked_bytes_limit) || !cursor.GetU32(boot.avatar_count) ||
        !boolean(boot.ecology_roster) || !boolean(boot.planted_roster) ||
        !boolean(boot.moving_anchor) || !boolean(boot.water_smoke)) {
        return false;
    }
    boot.weather_rain_scale_mm = std::bit_cast<std::int32_t>(rain_bits);
    if (cursor.pos != cursor.size) {
        error = "trailing authority boot descriptor bytes";
        return false;
    }
    if (!ValidBootDescriptor(boot, error)) {
        return false;
    }
    output = std::move(boot);
    error.clear();
    return true;
}

bool HasBootDescriptor(std::uint16_t version) {
    return version == kLrec1BootVersion || version == kLrec1BootElidedVersion;
}

bool ValidBootHeader(const ReplayHeader& header) {
    if (!HasBootDescriptor(header.version)) {
        return !header.authority_boot.has_value();
    }
    std::string error;
    return header.authority_boot.has_value() &&
           header.authority_boot->system_config_identity == header.system_config_identity &&
           ValidBootDescriptor(*header.authority_boot, error);
}

bool HeaderEncodingValid(const ReplayHeader& header) {
    const bool scheduled = WeatherVersion(header.version);
    // Readers admit versions before this check. Preserve unscheduled writers'
    // legacy version stamping while refusing schedule payload downgrades.
    if (!ValidBootHeader(header) || scheduled != !header.weather_schedule.empty() ||
        scheduled !=
            header.system_config_identity.starts_with(Systems::WeatherSchedule::kIdentityPrefix)) {
        return false;
    }
    if (!scheduled) {
        return true;
    }
    Systems::WeatherSchedule decoded;
    return Systems::WeatherSchedule::Decode(header.weather_schedule, decoded);
}

std::size_t HeaderStringBudget(const ReplayHeader& header) {
    // At most 64 MiB per encoded header, independently of record storage.
    std::size_t remaining = BoundedReplayRecorder::kSegmentBytes - 66;
    if (HasBootDescriptor(header.version)) {
        remaining -= 4 + BootDescriptor::kMaxEncodedBytes;
    }
    if (WeatherVersion(header.version)) {
        remaining -= 4;
    }
    return remaining;
}

bool HeaderWithinBudget(const ReplayHeader& header) {
    auto remaining = HeaderStringBudget(header);
    for (const auto* part : {&header.seed_string,
                             &header.preset,
                             &header.start_world_hash,
                             &header.engine_version,
                             &header.system_config_identity,
                             &header.preset_content_hash,
                             &header.weather_schedule}) {
        if (part->size() > remaining) {
            return false;
        }
        remaining -= part->size();
    }
    return true;
}

// Header layout. The tick_count field is written as a backpatch slot and finalized
// in-place by Finalize once the run length is known (its byte offset is fixed
// because everything before it is fixed-width).
//
//   [0..4]   magic "LREC1"            (5 bytes)
//   [5]      reserved u8 (0)
//   [6..7]   version u16
//   [8..9]   tick_rate_hz u16
//   [10..17] tick_count u64           (PATCHED by Finalize)  <- offset 10
//   [18..25] seed u64
//   [26..33] preset_hash u64
//   [34..37] surface_radius u32
//   [38..41] collision_radius u32
//   then length-prefixed strings: seed_string, preset, start_world_hash,
//   engine_version, system_config_identity, preset_content_hash (v3).
//   Versions 5/6 append u32 descriptor byte length + AUBD descriptor bytes.
//   Versions 7/8 instead append u32 schedule byte length + canonical WSC1 bytes.
constexpr std::size_t kTickCountOffset = 10;

std::unique_ptr<const Persistence::ReplayPairReadGuard>
AcquireReplayPublication(const fs::path& destination) {
    // The directory must exist before the shared publication guard opens it.
    if (destination.has_parent_path()) {
        std::error_code error;
        fs::create_directories(destination.parent_path(), error);
        if (error) {
            return {};
        }
    }
    auto ownership = std::make_unique<const Persistence::ReplayPairReadGuard>(destination);
    if (!ownership->is_open()) {
        return {};
    }
    return ownership;
}

void AppendHeader(std::vector<std::uint8_t>& out, const ReplayHeader& h) {
    out.insert(out.end(), kLrec1Magic, kLrec1Magic + sizeof(kLrec1Magic));
    PutU8(out, 0); // reserved
    PutU16(out, h.version);
    PutU16(out, h.tick_rate_hz);
    PutU64(out, 0); // tick_count backpatch slot -- finalized in Finalize
    PutU64(out, h.seed);
    PutU64(out, h.preset_hash);
    PutU32(out, h.surface_radius);
    PutU32(out, h.collision_radius);
    PutString(out, h.seed_string);
    PutString(out, h.preset);
    PutString(out, h.start_world_hash);
    PutString(out, h.engine_version);
    PutString(out, h.system_config_identity);
    PutString(out, h.preset_content_hash);
    if (WeatherVersion(h.version)) {
        PutString(out, h.weather_schedule);
    }
    if (HasBootDescriptor(h.version) && h.authority_boot.has_value()) {
        std::vector<std::uint8_t> descriptor;
        std::string error;
        if (!EncodeBootDescriptor(*h.authority_boot, descriptor, error)) {
            throw std::invalid_argument(error);
        }
        PutBlob(out, descriptor);
    }
}

template<typename Reader>
bool ParseHeader(Reader& c, ReplayHeader& h, std::uint64_t& header_tick_count, std::string& error) {
    error = "invalid or truncated LREC1 header";
    char magic[sizeof(kLrec1Magic)] = {0};
    if (!c.GetBytes(magic, sizeof(magic)))
        return false;
    if (std::memcmp(magic, kLrec1Magic, sizeof(kLrec1Magic)) != 0)
        return false;
    std::uint8_t reserved = 0;
    if (!c.GetU8(reserved))
        return false;
    if (!c.GetU16(h.version))
        return false;
    // Refuse a version mismatch LOUDLY (the header's binding contract): a future/unknown LREC1
    // version would have a different record layout, so parsing it under today's assumptions
    // would silently mis-decode into garbage. Reject instead (Factorio breaks silently; we don't).
    if (!SupportedVersion(h.version)) {
        error = "unsupported LREC1 version";
        return false;
    }
    if ((HasBootDescriptor(h.version) || WeatherVersion(h.version)) && reserved != 0) {
        error = "nonzero LREC1 reserved byte";
        return false;
    }
    if (!c.GetU16(h.tick_rate_hz))
        return false;
    if (!c.GetU64(header_tick_count))
        return false;
    if (!c.GetU64(h.seed))
        return false;
    if (!c.GetU64(h.preset_hash))
        return false;
    if (!c.GetU32(h.surface_radius))
        return false;
    if (!c.GetU32(h.collision_radius))
        return false;
    if (!World::SurfaceRadiiSupported(h.surface_radius, h.collision_radius))
        return false;
    if (!c.GetString(h.seed_string))
        return false;
    if (!c.GetString(h.preset))
        return false;
    if (!c.GetString(h.start_world_hash))
        return false;
    if (!c.GetString(h.engine_version))
        return false;
    if (!c.GetString(h.system_config_identity))
        return false;
    if (!c.GetString(h.preset_content_hash))
        return false;
    if (WeatherVersion(h.version) &&
        !c.GetField(h.weather_schedule, Systems::WeatherSchedule::kMaxEncodedBytes)) {
        return false;
    }
    if (HasBootDescriptor(h.version)) {
        std::uint32_t length = 0;
        error = "truncated or oversized authority boot envelope";
        // Fixed stack scratch; check the encoded length before any allocation.
        std::array<std::uint8_t, BootDescriptor::kMaxEncodedBytes> bytes{};
        if (!c.GetU32(length) || length > bytes.size() || !c.budget.Field(length) ||
            !c.GetBytes(bytes.data(), length)) {
            return false;
        }
        BootDescriptor boot;
        if (!DecodeBootDescriptor({bytes.data(), length}, boot, error)) {
            return false;
        }
        if (boot.system_config_identity != h.system_config_identity) {
            error = "authority boot identity disagrees with replay header";
            return false;
        }
        h.authority_boot = std::move(boot);
    }
    return c.ok && HeaderEncodingValid(h);
}

// One record frame: u8 type + u64 tick + u32 payload-length + payload. The
// explicit payload length lets a reader skip an unknown/partial record and lets
// truncation be detected (a frame claiming N payload bytes that are not present
// is a truncated tail).
void AppendRecordFrame(std::vector<std::uint8_t>& out,
                       RecordType type,
                       std::uint64_t tick,
                       const std::vector<std::uint8_t>& payload) {
    PutU8(out, static_cast<std::uint8_t>(type));
    PutU64(out, tick);
    PutU32(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

} // namespace

std::string Fnv1a64Hex(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL; // FNV offset basis
    for (const char character : text) {
        const auto ch = static_cast<unsigned char>(character);
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL; // FNV prime
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

// --- ReplayWriter ------------------------------------------------------------

ReplayWriter::ReplayWriter() = default;
ReplayWriter::~ReplayWriter() = default;

bool ReplayWriter::Open(const std::string& path, const ReplayHeader& header) {
    if (!World::SurfaceRadiiSupported(header.surface_radius, header.collision_radius))
        return false;
    if (!HeaderEncodingValid(header) || !HeaderWithinBudget(header)) {
        return false;
    }
    m_replacement_guard.reset();
    m_path = path;
    m_buffer.clear();
    m_input_count = 0;
    m_checkpoint_count = 0;
    AppendHeader(m_buffer, header);
    m_open = true;
    return true;
}

std::optional<ReplayContents> ReplayWriter::OpenForReplacement(const std::string& path) {
    m_open = false;
    m_buffer.clear();
    m_replacement_guard.reset();
    try {
        auto ownership = AcquireReplayPublication(path);
        if (!ownership) {
            return std::nullopt;
        }
        auto contents = ReadReplay(path);
        if (!contents.has_value() || !Open(path, contents->header)) {
            return std::nullopt;
        }
        m_replacement_guard = std::move(ownership);
        return contents;
    } catch (const std::exception&) {
        m_open = false;
        return std::nullopt;
    }
}

void ReplayWriter::RecordInput(std::uint64_t tick, const std::vector<std::uint8_t>& inputs) {
    if (!m_open)
        return;
    // Payload: just the opaque input blob (length-prefixed). An empty input set
    // is 4 bytes (a zero length) -- compact, as the contract requires.
    std::vector<std::uint8_t> payload;
    PutBlob(payload, inputs);
    AppendRecordFrame(m_buffer, RecordType::Input, tick, payload);
    ++m_input_count;
}

void ReplayWriter::RecordCheckpoint(const CheckpointRecord& checkpoint) {
    if (!m_open)
        return;
    std::vector<std::uint8_t> payload;
    PutString(payload, checkpoint.world_hash);
    PutString(payload, checkpoint.terrain);
    PutString(payload, checkpoint.water);
    PutString(payload, checkpoint.entities);
    AppendRecordFrame(m_buffer, RecordType::Checkpoint, checkpoint.tick, payload);
    ++m_checkpoint_count;
}

bool ReplayWriter::Finalize(std::uint64_t tick_count) {
    return Finish(tick_count, nullptr, false);
}

bool ReplayWriter::Finish(std::uint64_t tick_count, const std::string* trace_bytes, bool paired) {
    if (!m_open)
        return false;
    m_open = false;
    auto buffer = std::move(m_buffer);
    auto ownership = std::move(m_replacement_guard);
    try {
        // Paired Finalize refuses replacement mode and commit owns its guard
        // internally. Standalone replacement retains ownership from its read.
        if (!paired) {
            if (!ownership) {
                ownership = AcquireReplayPublication(m_path);
            }
            if (!ownership) {
                return false;
            }
        }
        // Patch the fixed header slot and append exactly one trailer per attempt.
        for (std::size_t i = 0; i < 8; ++i) {
            buffer[kTickCountOffset + i] =
                static_cast<std::uint8_t>((tick_count >> (8 * i)) & 0xFF);
        }
        buffer.insert(buffer.end(), kLrec1Trailer, kLrec1Trailer + sizeof(kLrec1Trailer));
        PutU64(buffer, tick_count);
        PutU64(buffer, m_input_count);
        PutU64(buffer, m_checkpoint_count);
        Persistence::ReplacementOutputFile output(m_path);
        if (!output.write({reinterpret_cast<const char*>(buffer.data()), buffer.size()}))
            return false;
        return paired ? output.commit_with_companion(trace_bytes) : output.commit();
    } catch (const std::exception&) {
        return false;
    }
}

// --- BoundedReplayRecorder ---------------------------------------------------

BoundedReplayRecorder::~BoundedReplayRecorder() {
    Close();
}

bool BoundedReplayRecorder::Configure(std::size_t max_tick_bytes, std::uint64_t unconfirmed_ticks) {
    if (!m_directory.empty())
        return false;
    const auto size_max = (std::numeric_limits<std::size_t>::max)();
    const auto segment_bytes = std::max(kSegmentBytes, max_tick_bytes);
    if (max_tick_bytes < 17 || segment_bytes > size_max / 3 ||
        unconfirmed_ticks > (size_max - 3 * segment_bytes) / max_tick_bytes)
        return false;
    m_max_tick_bytes = max_tick_bytes;
    m_byte_limit = static_cast<std::size_t>(unconfirmed_ticks) * max_tick_bytes + 3 * segment_bytes;
    return true;
}

bool BoundedReplayRecorder::Refused(int formatted) {
    // snprintf reports the untruncated length; the buffer always holds a
    // terminated prefix of it. A formatting error leaves the text empty.
    m_open_refusal_length =
        formatted < 0 ? 0 : std::min(static_cast<std::size_t>(formatted), kOpenRefusalBytes - 1);
    m_open_refusal[m_open_refusal_length] = '\0';
    return false;
}

bool BoundedReplayRecorder::Open(const std::string& destination,
                                 const ReplayHeader& header,
                                 bool regions) {
    // Every refusal below names its step in the fixed-size refusal text; the
    // full paths go to the logger only.
    if (m_byte_limit == 0)
        return Refused(std::snprintf(m_open_refusal.data(),
                                     kOpenRefusalBytes,
                                     "unconfigured: Configure must succeed before Open"));
    if (!m_directory.empty())
        return Refused(std::snprintf(m_open_refusal.data(),
                                     kOpenRefusalBytes,
                                     "already-open: working directory '%s%.*s' is reserved",
                                     PathTailPrefix(m_directory),
                                     PathTailLength(m_directory),
                                     PathTail(m_directory)));
    if (!HeaderEncodingValid(header)) {
        const char* refusal = ValidBootHeader(header)
                                  ? "unsupported: invalid version or weather schedule"
                                  : "authority-boot: missing, invalid or mismatched descriptor";
        return Refused(std::snprintf(m_open_refusal.data(), kOpenRefusalBytes, "%s", refusal));
    }
    // Header allocation is bounded independently of the tick/segment budget.
    std::size_t remaining = HeaderStringBudget(header);
    std::size_t part_index = 0;
    for (const auto* part : {&header.seed_string,
                             &header.preset,
                             &header.start_world_hash,
                             &header.engine_version,
                             &header.system_config_identity,
                             &header.preset_content_hash,
                             &header.weather_schedule}) {
        if (part->size() > remaining)
            return Refused(std::snprintf(m_open_refusal.data(),
                                         kOpenRefusalBytes,
                                         "header-budget: part %llu holds %llu bytes, %llu remain",
                                         static_cast<unsigned long long>(part_index),
                                         static_cast<unsigned long long>(part->size()),
                                         static_cast<unsigned long long>(remaining)));
        remaining -= part->size();
        ++part_index;
    }
    std::error_code ec;
    const fs::path out_path(destination);
    if (out_path.has_parent_path()) {
        fs::create_directories(out_path.parent_path(), ec);
        if (ec) {
            const std::string parent = out_path.parent_path().string();
            const std::string message = ec.message();
            LUMINUMBRA_CORE_ERROR(
                "Replay recorder: cannot create parent '{}': {} ({})", parent, message, ec.value());
            return Refused(std::snprintf(m_open_refusal.data(),
                                         kOpenRefusalBytes,
                                         "create-parent: '%s%.*s': %.*s (%.16s %d)",
                                         PathTailPrefix(parent),
                                         PathTailLength(parent),
                                         PathTail(parent),
                                         PathTailLength(message),
                                         message.c_str(),
                                         ec.category().name(),
                                         ec.value()));
        }
    }
    // Exclusive directory creation avoids collisions between concurrent sessions,
    // including processes using the same default dump destination.
    static std::atomic<std::uint64_t> next_directory{0};
    // Concurrent opens interleave on the counter, so the refusal names only the
    // first and last candidates this call itself reserved, never a continuous range.
    std::uint64_t first_ordinal = 0;
    std::uint64_t ordinal = 0;
    for (std::size_t attempt = 0; attempt < kSegmentTicks; ++attempt) {
        ordinal = next_directory.fetch_add(1);
        if (attempt == 0)
            first_ordinal = ordinal;
        const auto candidate =
            (out_path.parent_path() / ("lockstep-recording-" + std::to_string(ordinal))).string();
        if (fs::create_directory(candidate, ec)) {
            m_directory = candidate;
            m_header = header;
            m_regions = regions;
            m_digest = 1469598103934665603ULL;
            m_elided_tick = 0;
            m_elided_digest = m_digest;
            m_last_tick = 0;
            m_output.clear();
            m_open_refusal_length = 0;
            m_open_refusal[0] = '\0';
            return true;
        }
        if (ec) {
            const std::string message = ec.message();
            LUMINUMBRA_CORE_ERROR("Replay recorder: cannot reserve directory '{}': {} ({})",
                                  candidate,
                                  message,
                                  ec.value());
            return Refused(
                std::snprintf(m_open_refusal.data(),
                              kOpenRefusalBytes,
                              "reserve-directory: attempt %llu candidate "
                              "'lockstep-recording-%llu' beside '%s%.*s': %.*s (%.16s %d)",
                              static_cast<unsigned long long>(attempt),
                              static_cast<unsigned long long>(ordinal),
                              PathTailPrefix(destination),
                              PathTailLength(destination),
                              PathTail(destination),
                              PathTailLength(message),
                              message.c_str(),
                              ec.category().name(),
                              ec.value()));
        }
    }
    LUMINUMBRA_CORE_ERROR("Replay recorder: exhausted {} directory candidates beside '{}'",
                          kSegmentTicks,
                          destination);
    return Refused(std::snprintf(m_open_refusal.data(),
                                 kOpenRefusalBytes,
                                 "exhausted: %llu candidates tried (first lockstep-recording-%llu, "
                                 "last lockstep-recording-%llu) beside '%s%.*s' already exist",
                                 static_cast<unsigned long long>(kSegmentTicks),
                                 static_cast<unsigned long long>(first_ordinal),
                                 static_cast<unsigned long long>(ordinal),
                                 PathTailPrefix(destination),
                                 PathTailLength(destination),
                                 PathTail(destination)));
}

bool BoundedReplayRecorder::Prune(std::uint64_t tick, std::uint64_t confirmed_tick) {
    const auto oldest = tick > kSegmentTicks ? tick - kSegmentTicks : 0;
    // Each segment is examined/removed once. Never elide an unconfirmed tick:
    // a delayed mismatching checkpoint must still have its inputs in the dump.
    while (m_segments.size() > 1) {
        const auto& segment = m_segments.front();
        if (segment.last_tick > confirmed_tick ||
            (segment.last_tick > oldest && m_bytes - segment.bytes < kSegmentBytes))
            break;
        std::error_code ec;
        if (!fs::remove(segment.path, ec) || ec)
            return false;
        m_elided_tick = segment.last_tick;
        m_elided_digest = segment.digest;
        m_bytes -= segment.bytes;
        m_segments.pop_front();
    }
    return true;
}

bool BoundedReplayRecorder::RecordTick(std::uint64_t tick,
                                       const std::vector<std::uint8_t>& inputs,
                                       const std::array<std::string_view, 4>* hashes,
                                       std::uint64_t confirmed_tick,
                                       std::string_view schedule_digest) {
    if (m_directory.empty() || tick <= m_last_tick || m_max_tick_bytes < 17 ||
        inputs.size() > m_max_tick_bytes - 17 ||
        inputs.size() > (std::numeric_limits<std::uint32_t>::max)() - 4)
        return false;
    std::size_t bytes = 17 + inputs.size();
    if (hashes) {
        if (m_max_tick_bytes - bytes < 29)
            return false;
        bytes += 29; // checkpoint frame + four u32 lengths
        for (const auto part : *hashes) {
            if (part.size() > m_max_tick_bytes - bytes ||
                part.size() > (std::numeric_limits<std::uint32_t>::max)() - 16)
                return false;
            bytes += part.size();
        }
        if (bytes - 17 - inputs.size() - 13 > (std::numeric_limits<std::uint32_t>::max)())
            return false;
    }
    // The private trace frame is stripped during finalization; LREC1 is unchanged.
    constexpr std::size_t trace_bytes = 13 + 16;
    if (m_regions
            ? (tick != m_last_tick + 1 || schedule_digest.size() != 16 ||
               schedule_digest.find_first_not_of("0123456789abcdef") != std::string_view::npos ||
               trace_bytes > m_max_tick_bytes - bytes)
            : !schedule_digest.empty())
        return false;
    const auto replay_bytes = bytes;
    if (m_regions)
        bytes += trace_bytes;
    if (!Prune(tick, confirmed_tick) || bytes > m_byte_limit - m_bytes)
        return false;

    // One encoding buffer, bounded by the validated maximum merged input plus
    // one checkpoint (three negotiated transport frames). No history copies.
    std::vector<std::uint8_t> encoded;
    encoded.reserve(bytes);
    PutU8(encoded, static_cast<std::uint8_t>(RecordType::Input));
    PutU64(encoded, tick);
    PutU32(encoded, static_cast<std::uint32_t>(4 + inputs.size()));
    PutBlob(encoded, inputs);
    if (hashes) {
        PutU8(encoded, static_cast<std::uint8_t>(RecordType::Checkpoint));
        PutU64(encoded, tick);
        PutU32(encoded, static_cast<std::uint32_t>(replay_bytes - 17 - inputs.size() - 13));
        for (const auto part : *hashes) {
            PutU32(encoded, static_cast<std::uint32_t>(part.size()));
            encoded.insert(encoded.end(), part.begin(), part.end());
        }
    }
    if (m_regions) {
        // 0xff is private spool metadata, never a published replay record type.
        PutU8(encoded, 0xff);
        PutU64(encoded, tick);
        PutU32(encoded, 16);
        encoded.insert(encoded.end(), schedule_digest.begin(), schedule_digest.end());
    }
    const bool rotate = !m_segments.empty() &&
                        (m_segments.back().ticks == kSegmentTicks ||
                         bytes > kSegmentBytes - std::min(kSegmentBytes, m_segments.back().bytes));
    if (rotate) {
        m_output.close();
        if (m_output.fail())
            return false;
    }
    if (m_segments.empty() || rotate) {
        const auto path = (fs::path(m_directory) / std::to_string(tick)).string();
        m_output.open(path, std::ios::binary | std::ios::trunc);
        if (!m_output.is_open())
            return false;
        m_segments.push_back({.path = path, .last_tick = 0, .digest = 0, .bytes = 0, .ticks = 0});
    }
    m_output.write(reinterpret_cast<const char*>(encoded.data()),
                   static_cast<std::streamsize>(encoded.size()));
    if (!m_output.good())
        return false;
    for (std::size_t index = 0; index < replay_bytes; ++index) {
        m_digest ^= encoded[index];
        m_digest *= 1099511628211ULL;
    }
    auto& segment = m_segments.back();
    segment.last_tick = tick;
    segment.digest = m_digest;
    segment.bytes += bytes;
    ++segment.ticks;
    m_bytes += bytes;
    m_last_tick = tick;
    return Prune(tick, confirmed_tick);
}

bool BoundedReplayRecorder::Finalize(const std::string& destination,
                                     std::uint64_t tick_count,
                                     std::uint64_t divergence_tick) {
    struct CloseOnExit {
        BoundedReplayRecorder& recorder;
        ~CloseOnExit() {
            recorder.Close();
        }
    };
    const CloseOnExit close{*this};
    try {
        // Trace admission matches the existing encoder/reader: at most one
        // million ticks and 96 MiB encoded. Elided dumps are diagnostic only.
        if (m_directory.empty() || divergence_tick <= m_elided_tick ||
            (m_regions && m_elided_tick == 0 && tick_count > 1000000))
            return false;
        if (m_output.is_open()) {
            m_output.close();
            if (m_output.fail())
                return false;
        }
        Persistence::report_snapshot_step("replay-segment-close", destination);
        std::vector<std::uint8_t> header;
        if (m_elided_tick != 0) {
            m_header.version = WeatherVersion(m_header.version)      ? kLrec1WeatherElidedVersion
                               : HasBootDescriptor(m_header.version) ? kLrec1BootElidedVersion
                                                                     : kLrec1ElidedVersion;
        }
        AppendHeader(header, m_header);
        for (std::size_t i = 0; i < 8; ++i)
            header[kTickCountOffset + i] =
                static_cast<std::uint8_t>((tick_count >> (8 * i)) & 0xFF);
        if (m_elided_tick != 0) {
            PutU8(header, static_cast<std::uint8_t>(RecordType::ElidedPrefix));
            PutU64(header, m_elided_tick);
            PutU32(header, 8);
            PutU64(header, m_elided_digest);
        }
        Persistence::ReplacementOutputFile out(destination);
        if (!out.is_open())
            return false;
        if (!out.write({reinterpret_cast<const char*>(header.data()), header.size()}))
            return false;
        RegionScheduleTrace trace;
        std::uint64_t input_count = 0;
        std::uint64_t checkpoint_count = 0;
        std::uint64_t trace_tick = m_elided_tick;
        // Finalization streams through a fixed 64 KiB scratch buffer; the replay
        // artifact is never assembled in memory, regardless of the negotiated bound.
        std::array<char, std::size_t{64} * 1024> scratch{};
        for (const auto& segment : m_segments) {
            Persistence::report_snapshot_step("replay-segment", segment.path);
            Filesystem::ReplacementSafeInputFile in;
            in.open(segment.path);
            if (!in.is_open())
                return false;
            std::size_t remaining = segment.bytes;
            while (remaining != 0) {
                std::array<std::uint8_t, 13> frame{};
                if (remaining < frame.size() ||
                    !in.read(reinterpret_cast<char*>(frame.data()), frame.size()))
                    return false;
                Persistence::report_snapshot_step("replay-read", segment.path);
                Cursor cursor{frame.data(), frame.size(), 0, true};
                std::uint8_t type = 0;
                std::uint64_t tick = 0;
                std::uint32_t payload = 0;
                if (!cursor.GetU8(type) || !cursor.GetU64(tick) || !cursor.GetU32(payload) ||
                    payload > remaining - frame.size())
                    return false;
                if (type == 0xff) {
                    std::array<char, 16> digest{};
                    if (!m_regions || payload != digest.size() ||
                        !in.read(digest.data(), digest.size()))
                        return false;
                    const std::string_view digest_view(digest.data(), digest.size());
                    if (trace_tick == (std::numeric_limits<std::uint64_t>::max)() ||
                        tick != trace_tick + 1 ||
                        digest_view.find_first_not_of("0123456789abcdef") != std::string_view::npos)
                        return false;
                    trace_tick = tick;
                    if (m_elided_tick == 0) {
                        if (trace.size() >= tick_count || tick != trace.size() + 1)
                            return false;
                        trace.emplace_back(tick, std::string(digest.data(), digest.size()));
                    }
                    remaining -= frame.size() + payload;
                    continue;
                }
                const bool keep = type != static_cast<std::uint8_t>(RecordType::Checkpoint) ||
                                  tick <= divergence_tick;
                if (type == static_cast<std::uint8_t>(RecordType::Input))
                    ++input_count;
                else if (type == static_cast<std::uint8_t>(RecordType::Checkpoint))
                    checkpoint_count += keep;
                else
                    return false;
                if (keep && !out.write({reinterpret_cast<const char*>(frame.data()), frame.size()}))
                    return false;
                remaining -= frame.size() + payload;
                std::size_t pending = payload;
                while (pending != 0) {
                    const auto count = std::min(pending, scratch.size());
                    if (!in.read(scratch.data(), count))
                        return false;
                    if (keep && !out.write({scratch.data(), count}))
                        return false;
                    pending -= count;
                }
            }
        }
        std::vector<std::uint8_t> trailer;
        trailer.insert(trailer.end(), kLrec1Trailer, kLrec1Trailer + sizeof(kLrec1Trailer));
        PutU64(trailer, tick_count);
        PutU64(trailer, input_count);
        PutU64(trailer, checkpoint_count);
        std::string trace_bytes;
        const bool publish_trace = m_regions && m_elided_tick == 0;
        if (m_regions && trace_tick != tick_count)
            return false;
        if (publish_trace &&
            (trace.size() != tick_count || !EncodeRegionScheduleTrace(trace, trace_bytes)))
            return false;
        return out.write({reinterpret_cast<const char*>(trailer.data()), trailer.size()}) &&
               out.commit_with_companion(publish_trace ? &trace_bytes : nullptr);
    } catch (const std::exception&) {
        return false;
    }
}

void BoundedReplayRecorder::Close() {
    // Windows forbids deleting open files; close before removing the segments.
    if (m_output.is_open())
        m_output.close();
    if (!m_directory.empty()) {
        std::error_code ec;
        fs::remove_all(m_directory, ec);
        m_directory.clear();
    }
    m_segments.clear();
    m_bytes = 0;
}

// --- QueuedReplayRecorder ----------------------------------------------------

struct QueuedReplayRecorder::State {
    explicit State(std::unique_ptr<ReplayTickSink> destination)
        : sink(std::move(destination)) {}

    bool
    Configure(std::size_t tick_byte_limit, std::uint64_t unconfirmed_ticks, QueueLimits limits);
    bool Open(const std::string& destination, const ReplayHeader& header, bool regions_enabled);
    bool RecordTick(std::uint64_t tick,
                    std::shared_ptr<const std::vector<std::uint8_t>> inputs,
                    std::shared_ptr<const CheckpointRecord> checkpoint,
                    std::uint64_t confirmed_tick,
                    std::string_view schedule_digest);
    bool Flush();
    bool Finalize(const std::string& destination,
                  std::uint64_t tick_count,
                  std::uint64_t divergence_tick);
    void Close();
    [[nodiscard]] const char* FailureReason() const;
    [[nodiscard]] std::string_view OpenRefusal() const;
    [[nodiscard]] std::size_t RetainedBytes() const;
    [[nodiscard]] std::size_t ByteLimit() const;
    [[nodiscard]] std::size_t SegmentCount() const;
    [[nodiscard]] std::size_t PendingTicks() const;
    [[nodiscard]] std::size_t PendingBytes() const;

private:
    enum class Failure : std::uint8_t {
        None,
        QueueFull,
        InvalidTick,
        Sink,
        Finalization
    };
    struct Tick {
        std::uint64_t tick = 0;
        std::shared_ptr<const std::vector<std::uint8_t>> inputs;
        std::shared_ptr<const CheckpointRecord> checkpoint;
        std::uint64_t confirmed = 0;
        std::size_t bytes = 0;
        std::array<char, 16> schedule_digest{};
    };

    // Requires mutex. Preserve the first failure even while draining accepted work.
    void Fail(Failure reason, std::uint64_t tick) {
        if (failure == Failure::None) {
            failure = reason;
            failure_tick = tick;
        }
    }

    void Run() {
        for (;;) {
            Tick next;
            {
                std::unique_lock lock(mutex);
                waiting = true;
                ready.wait(lock, [this] { return stopping || queued_ticks != 0; });
                waiting = false;
                if (queued_ticks == 0)
                    return;
                next = std::move(queue[queue_head]);
                queue[queue_head] = Tick{};
                queue_head = (queue_head + 1) % queue.size();
                --queued_ticks;
            }
            // The queue mutex is never held across sink I/O, encoding or digesting.
            // Virtual dispatch is once per whole tick, exclusively on this worker.
            bool recorded = false;
            try {
                std::array<std::string_view, 4> hashes{};
                if (next.checkpoint) {
                    const auto& hash = *next.checkpoint;
                    hashes = {hash.world_hash, hash.terrain, hash.water, hash.entities};
                }
                recorded = sink->RecordTick(next.tick,
                                            *next.inputs,
                                            next.checkpoint ? &hashes : nullptr,
                                            next.confirmed,
                                            regions ? std::string_view(next.schedule_digest.data(),
                                                                       next.schedule_digest.size())
                                                    : std::string_view{});
            } catch (...) {
                // JobSystem catches exceptions too, but the lifecycle waiters must
                // always be notified and this artifact must be marked incomplete.
                recorded = false;
            }
            retained_bytes.store(sink->RetainedBytes(), std::memory_order_relaxed);
            segments.store(sink->SegmentCount(), std::memory_order_relaxed);
            const auto completed_bytes = next.bytes;
            const auto completed_tick = next.tick;
            // Release payload ownership before reporting the capacity available.
            next = Tick{};
            {
                const std::lock_guard lock(mutex);
                if (!recorded)
                    Fail(Failure::Sink, completed_tick);
                pending_bytes -= completed_bytes;
                --pending_ticks;
                if (pending_ticks == 0)
                    drained.notify_all();
            }
        }
    }

    void Stop() {
        {
            const std::lock_guard lock(mutex);
            stopping = true;
        }
        ready.notify_one();
        // JobSystem drains its accepted job and joins; Run drains its FIFO first.
        jobs.shutdown();
    }

    std::unique_ptr<ReplayTickSink> sink;
    JobSystem jobs;
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable drained;
    // Preallocated at setup: at most 4096 fixed-size slots. Enqueue never allocates.
    std::vector<Tick> queue;
    std::size_t queue_head = 0;
    std::size_t queued_ticks = 0;
    std::size_t max_tick_bytes = 0;
    std::size_t queue_byte_limit = 0;
    std::size_t queue_tick_limit = 0;
    std::size_t pending_bytes = 0;
    std::size_t pending_ticks = 0;
    std::size_t byte_limit = 0;
    std::uint64_t last_tick = 0;
    std::uint64_t failure_tick = 0;
    std::atomic<std::size_t> retained_bytes{0};
    std::atomic<std::size_t> segments{0};
    Failure failure = Failure::None;
    // Static text for this recorder's own Open preconditions; nullptr defers to
    // the sink. Written and read only on the thread that calls Open.
    const char* open_refusal = nullptr;
    bool regions = false;
    bool open = false;
    bool stopping = false;
    bool waiting = false;
};

QueuedReplayRecorder::QueuedReplayRecorder()
    : QueuedReplayRecorder(std::make_unique<BoundedReplayRecorder>()) {}

QueuedReplayRecorder::QueuedReplayRecorder(std::unique_ptr<ReplayTickSink> sink)
    : m_state(std::make_unique<State>(std::move(sink))) {}

QueuedReplayRecorder::~QueuedReplayRecorder() {
    Close();
}

bool QueuedReplayRecorder::Configure(std::size_t max_tick_bytes, std::uint64_t unconfirmed_ticks) {
    return Configure(max_tick_bytes, unconfirmed_ticks, QueueLimits{});
}

bool QueuedReplayRecorder::State::Configure(std::size_t tick_byte_limit,
                                            std::uint64_t unconfirmed_ticks,
                                            QueueLimits limits) {
    auto& state = *this;
    if (state.open || !state.sink || limits.ticks == 0 || limits.ticks > kQueueTicks ||
        !state.sink->Configure(tick_byte_limit, unconfirmed_ticks))
        return false;
    state.max_tick_bytes = tick_byte_limit;
    state.queue_byte_limit =
        limits.bytes == 0 ? std::max(kQueueBytes, tick_byte_limit) : limits.bytes;
    state.queue_tick_limit = limits.ticks;
    state.queue.resize(limits.ticks);
    state.queue_head = 0;
    state.queued_ticks = 0;
    state.byte_limit = state.sink->ByteLimit();
    return true;
}

bool QueuedReplayRecorder::State::Open(const std::string& destination,
                                       const ReplayHeader& header,
                                       bool regions_enabled) {
    auto& state = *this;
    if (state.open) {
        state.open_refusal = "already-open: recorder is open";
        return false;
    }
    if (state.max_tick_bytes == 0) {
        state.open_refusal = "unconfigured: Configure must succeed before Open";
        return false;
    }
    // From here the sink's own refusal text (or its emptiness) is authoritative.
    state.open_refusal = nullptr;
    if (!state.sink->Open(destination, header, regions_enabled))
        return false;
    state.regions = regions_enabled;
    state.stopping = false;
    state.failure = State::Failure::None;
    state.failure_tick = 0;
    state.last_tick = 0;
    state.jobs.startup(1);
    state.open = true;
    // One persistent consumer uses the existing engine worker infrastructure.
    // No std::function construction/dispatch occurs per tick.
    state.jobs.dispatch([&state] { state.Run(); });
    return true;
}

bool QueuedReplayRecorder::State::RecordTick(
    std::uint64_t tick,
    std::shared_ptr<const std::vector<std::uint8_t>> inputs,
    std::shared_ptr<const CheckpointRecord> checkpoint,
    std::uint64_t confirmed_tick,
    std::string_view schedule_digest) {
    auto& state = *this;
    // Validate exact wire size before retaining anything. Checkpoint validation is
    // four constant-time size checks, with no scans/copies of the encoded payload.
    bool valid = inputs && state.max_tick_bytes >= 17 &&
                 inputs->size() <= state.max_tick_bytes - 17 &&
                 inputs->size() <= (std::numeric_limits<std::uint32_t>::max)() - 4;
    std::size_t bytes = valid ? 17 + inputs->size() : 0;
    std::size_t storage = valid ? inputs->capacity() : 0;
    if (valid && checkpoint) {
        valid = checkpoint->tick == tick && state.max_tick_bytes - bytes >= 29;
        if (valid) {
            bytes += 29;
            std::size_t payload = 16;
            for (const auto* part : {&checkpoint->world_hash,
                                     &checkpoint->terrain,
                                     &checkpoint->water,
                                     &checkpoint->entities}) {
                if (part->size() > state.max_tick_bytes - bytes ||
                    part->size() > (std::numeric_limits<std::uint32_t>::max)() - payload ||
                    part->capacity() > (std::numeric_limits<std::size_t>::max)() - storage) {
                    valid = false;
                    break;
                }
                bytes += part->size();
                payload += part->size();
                storage += part->capacity();
            }
        }
    }
    if (valid && state.regions) {
        valid = schedule_digest.size() == 16 &&
                schedule_digest.find_first_not_of("0123456789abcdef") == std::string_view::npos &&
                state.max_tick_bytes - bytes >= 29;
        if (valid)
            bytes += 29;
    } else if (!schedule_digest.empty()) {
        valid = false;
    }
    // Charge capacity as well as wire size: a small view of a large allocation
    // must not evade the queue's byte budget. Fixed metadata is count-bounded.
    const auto charged = std::max(bytes, storage);
    const std::lock_guard lock(state.mutex);
    if (!state.open || state.stopping || state.failure != State::Failure::None)
        return false;
    if (!valid || tick <= state.last_tick || (state.regions && tick != state.last_tick + 1)) {
        state.Fail(State::Failure::InvalidTick, tick);
        return false;
    }
    if (state.pending_ticks == state.queue_tick_limit ||
        charged > state.queue_byte_limit - state.pending_bytes) {
        state.Fail(State::Failure::QueueFull, tick);
        return false;
    }
    const bool wake = state.waiting && state.queued_ticks == 0;
    const auto slot = (state.queue_head + state.queued_ticks) % state.queue.size();
    state.queue[slot] = {.tick = tick,
                         .inputs = std::move(inputs),
                         .checkpoint = std::move(checkpoint),
                         .confirmed = confirmed_tick,
                         .bytes = charged,
                         .schedule_digest = {}};
    if (state.regions)
        std::copy(schedule_digest.begin(),
                  schedule_digest.end(),
                  state.queue[slot].schedule_digest.begin());
    ++state.queued_ticks;
    state.pending_bytes += charged;
    ++state.pending_ticks;
    state.last_tick = tick;
    if (wake)
        state.ready.notify_one();
    return true;
}

bool QueuedReplayRecorder::State::Flush() {
    auto& state = *this;
    std::unique_lock lock(state.mutex);
    state.drained.wait(lock, [&state] { return state.pending_ticks == 0; });
    return state.failure == State::Failure::None;
}

bool QueuedReplayRecorder::State::Finalize(const std::string& destination,
                                           std::uint64_t tick_count,
                                           std::uint64_t divergence_tick) {
    auto& state = *this;
    if (!state.open)
        return false;
    state.Stop();
    bool finalized = false;
    try {
        const auto failed_tick =
            state.failure == State::Failure::None ? divergence_tick : state.failure_tick;
        const auto* cause = state.failure == State::Failure::None ? "recorder finalization failed"
                                                                  : FailureReason();
        const auto message =
            "Incomplete replay at tick " + std::to_string(failed_tick) + ": " + cause + '\n';
        ReplayFinalization publication(destination, message);
        finalized = publication.ready() && state.failure == State::Failure::None &&
                    state.sink->Finalize(destination, tick_count, divergence_tick) &&
                    publication.Complete();
    } catch (const std::exception&) {
        finalized = false;
    }
    if (finalized) {
        state.Close();
        return true;
    }
    {
        const std::lock_guard lock(state.mutex);
        state.Fail(State::Failure::Finalization, divergence_tick);
    }
    state.Close();
    LUMINUMBRA_CORE_ERROR("Replay recording incomplete at tick {}: {} ({})",
                          state.failure_tick,
                          FailureReason(),
                          destination);
    return false;
}

void QueuedReplayRecorder::State::Close() {
    auto& state = *this;
    if (!state.open)
        return;
    state.Stop();
    if (state.failure != State::Failure::None)
        LUMINUMBRA_CORE_ERROR(
            "Replay recording incomplete at tick {}: {}", state.failure_tick, FailureReason());
    state.sink->Close();
    state.retained_bytes.store(0, std::memory_order_relaxed);
    state.segments.store(0, std::memory_order_relaxed);
    state.open = false;
}

const char* QueuedReplayRecorder::State::FailureReason() const {
    const std::lock_guard lock(mutex);
    switch (failure) {
        case State::Failure::None:
            return "";
        case State::Failure::QueueFull:
            return "recorder queue capacity exceeded";
        case State::Failure::InvalidTick:
            return "invalid or oversized recorder tick";
        case State::Failure::Sink:
            return "recorder sink write failed";
        case State::Failure::Finalization:
            return "recorder finalization failed";
    }
    return "recorder failure";
}

std::string_view QueuedReplayRecorder::State::OpenRefusal() const {
    if (open_refusal != nullptr)
        return open_refusal;
    return sink ? sink->OpenRefusal() : std::string_view{};
}

std::size_t QueuedReplayRecorder::State::RetainedBytes() const {
    return retained_bytes.load(std::memory_order_relaxed);
}

std::size_t QueuedReplayRecorder::State::ByteLimit() const {
    return byte_limit;
}

std::size_t QueuedReplayRecorder::State::SegmentCount() const {
    return segments.load(std::memory_order_relaxed);
}

std::size_t QueuedReplayRecorder::State::PendingTicks() const {
    const std::lock_guard lock(mutex);
    return pending_ticks;
}

std::size_t QueuedReplayRecorder::State::PendingBytes() const {
    const std::lock_guard lock(mutex);
    return pending_bytes;
}

bool QueuedReplayRecorder::Configure(std::size_t max_tick_bytes,
                                     std::uint64_t unconfirmed_ticks,
                                     QueueLimits limits) {
    return m_state->Configure(max_tick_bytes, unconfirmed_ticks, limits);
}

bool QueuedReplayRecorder::Open(const std::string& destination,
                                const ReplayHeader& header,
                                bool regions) {
    return m_state->Open(destination, header, regions);
}

bool QueuedReplayRecorder::RecordTick(std::uint64_t tick,
                                      std::shared_ptr<const std::vector<std::uint8_t>> inputs,
                                      std::shared_ptr<const CheckpointRecord> checkpoint,
                                      std::uint64_t confirmed_tick,
                                      std::string_view schedule_digest) {
    return m_state->RecordTick(
        tick, std::move(inputs), std::move(checkpoint), confirmed_tick, schedule_digest);
}

bool QueuedReplayRecorder::Flush() {
    return m_state->Flush();
}

bool QueuedReplayRecorder::Finalize(const std::string& destination,
                                    std::uint64_t tick_count,
                                    std::uint64_t divergence_tick) {
    return m_state->Finalize(destination, tick_count, divergence_tick);
}

void QueuedReplayRecorder::Close() {
    m_state->Close();
}

const char* QueuedReplayRecorder::FailureReason() const {
    return m_state->FailureReason();
}

std::string_view QueuedReplayRecorder::OpenRefusal() const {
    return m_state->OpenRefusal();
}

std::size_t QueuedReplayRecorder::RetainedBytes() const {
    return m_state->RetainedBytes();
}

std::size_t QueuedReplayRecorder::ByteLimit() const {
    return m_state->ByteLimit();
}

std::size_t QueuedReplayRecorder::SegmentCount() const {
    return m_state->SegmentCount();
}

std::size_t QueuedReplayRecorder::PendingTicks() const {
    return m_state->PendingTicks();
}

std::size_t QueuedReplayRecorder::PendingBytes() const {
    return m_state->PendingBytes();
}

// --- Reader ------------------------------------------------------------------

namespace {

struct StreamReplaySource {
    std::istream& input;

    bool seek(std::uint64_t offset) {
        return offset <= static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) &&
               static_cast<bool>(input.seekg(static_cast<std::streamoff>(offset)));
    }
    bool read(char* destination, std::size_t bytes) {
        // A transfer is at most 64 KiB, including on providers with narrow streamsize.
        constexpr std::size_t kTransferBytes = 64 * 1024;
        while (bytes != 0) {
            const auto count = std::min(bytes, kTransferBytes);
            if (!input.read(destination, static_cast<std::streamsize>(count))) {
                return false;
            }
            destination += count;
            bytes -= count;
        }
        return true;
    }
};

struct ReadBudget {
    ReplayReadLimits limits;
    std::size_t records = 0;
    bool failed = false;

    bool Field(std::uint32_t bytes) {
        if (bytes > limits.field_bytes) {
            failed = true;
            return false;
        }
        limits.field_bytes -= bytes;
        return true;
    }
};

// Reads directly into admitted fields. Scalar scratch is at most eight bytes;
// there is no allocation proportional to the file or an unvalidated frame.
// Each cursor's end is checked before subtraction or allocation. A payload
// cursor shares the same source and budget but cannot read outside its frame.
template<typename Source>
struct ReplayCursor {
    Source& source;
    ReadBudget& budget;
    std::uint64_t size;
    std::uint64_t pos = 0;
    bool ok = true;

    bool Remaining(std::uint64_t bytes) const {
        return ok && pos <= size && bytes <= size - pos;
    }
    bool GetBytes(void* destination, std::size_t bytes) {
        if (!Remaining(bytes)) {
            ok = false;
            return false;
        }
        if (bytes != 0 && !source.read(static_cast<char*>(destination), bytes)) {
            budget.failed = true;
            ok = false;
            return false;
        }
        pos += bytes;
        return true;
    }
    template<typename Integer>
    bool GetInteger(Integer& value) {
        std::array<std::uint8_t, sizeof(Integer)> bytes{};
        if (!GetBytes(bytes.data(), bytes.size())) {
            return false;
        }
        std::uint64_t decoded = 0;
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            decoded |= static_cast<std::uint64_t>(bytes[index]) << (8 * index);
        }
        value = static_cast<Integer>(decoded);
        return true;
    }
    bool GetU8(std::uint8_t& value) {
        return GetInteger(value);
    }
    bool GetU16(std::uint16_t& value) {
        return GetInteger(value);
    }
    bool GetU32(std::uint32_t& value) {
        return GetInteger(value);
    }
    bool GetU64(std::uint64_t& value) {
        return GetInteger(value);
    }

    template<typename Field>
    bool GetField(Field& field,
                  std::size_t max_bytes = (std::numeric_limits<std::uint32_t>::max)()) {
        std::uint32_t length = 0;
        if (!GetU32(length) || length > max_bytes || !Remaining(length) || !budget.Field(length)) {
            ok = false;
            return false;
        }
        field.resize(length);
        return GetBytes(field.data(), field.size());
    }
    bool GetString(std::string& text) {
        return GetField(text);
    }
    bool GetBlob(std::vector<std::uint8_t>& bytes) {
        return GetField(bytes);
    }

    bool SkipTo(std::uint64_t end) {
        if (end < pos || !Remaining(end - pos)) {
            ok = false;
            return false;
        }
        if (end != pos && !source.seek(end)) {
            budget.failed = true;
            ok = false;
            return false;
        }
        pos = end;
        return true;
    }
};

template<typename Source>
std::optional<ReplayContents>
ParseReplay(Source& source, std::uint64_t size, ReplayReadLimits limits, std::string& error) {
    error = "cannot seek replay input";
    if (!source.seek(0)) {
        return std::nullopt;
    }
    ReadBudget budget{limits, 0, false};
    ReplayCursor<Source> c{source, budget, size, 0, true};
    ReplayContents contents;
    std::uint64_t header_tick_count = 0;
    if (!ParseHeader(c, contents.header, header_tick_count, error)) {
        return std::nullopt;
    }
    error = "malformed or truncated replay records/trailer";
    const bool strict = HasBootDescriptor(contents.header.version);

    // Work is bounded by the admitted field bytes plus constant framing per
    // admitted record. Opaque extension bytes are skipped, never buffered.
    while (c.ok && c.pos < c.size) {
        std::uint8_t type_byte = 0;
        if (!c.GetU8(type_byte)) {
            break;
        }
        if (type_byte == static_cast<std::uint8_t>(kLrec1Trailer[0])) {
            std::array<char, sizeof(kLrec1Trailer) - 1> suffix{};
            std::uint64_t trailer_ticks = 0;
            std::uint64_t trailer_inputs = 0;
            std::uint64_t trailer_checkpoints = 0;
            if (c.GetBytes(suffix.data(), suffix.size()) &&
                std::memcmp(suffix.data(), kLrec1Trailer + 1, suffix.size()) == 0 &&
                c.GetU64(trailer_ticks) && c.GetU64(trailer_inputs) &&
                c.GetU64(trailer_checkpoints)) {
                contents.tick_count = trailer_ticks;
                contents.trailer_present = true;
                contents.truncated = trailer_inputs != contents.inputs.size() ||
                                     trailer_checkpoints != contents.checkpoints.size() ||
                                     c.pos != c.size;
            }
            break;
        }

        std::uint64_t tick = 0;
        std::uint32_t payload_len = 0;
        if (!c.GetU64(tick) || !c.GetU32(payload_len) || !c.Remaining(payload_len)) {
            break;
        }
        if (budget.records == limits.records) {
            error = "replay record count exceeds admission limit";
            return std::nullopt;
        }
        ++budget.records;
        const auto payload_end = c.pos + payload_len; // Remaining checked the sum.
        ReplayCursor<Source> payload{source, budget, payload_end, c.pos, true};
        const auto type = static_cast<RecordType>(type_byte);
        if (type == RecordType::Input) {
            InputRecord rec;
            rec.tick = tick;
            if (!payload.GetBlob(rec.inputs)) {
                break;
            }
            contents.inputs.push_back(std::move(rec));
        } else if (type == RecordType::Checkpoint) {
            CheckpointRecord rec;
            rec.tick = tick;
            if (!payload.GetString(rec.world_hash) || !payload.GetString(rec.terrain) ||
                !payload.GetString(rec.water) || !payload.GetString(rec.entities)) {
                break;
            }
            contents.checkpoints.push_back(std::move(rec));
        } else if (type == RecordType::ElidedPrefix && ElidedVersion(contents.header.version) &&
                   contents.inputs.empty() && contents.checkpoints.empty() &&
                   contents.elided_through_tick == 0 && tick != 0 && payload_len == 8) {
            if (!payload.GetU64(contents.elided_prefix_digest)) {
                break;
            }
            contents.elided_through_tick = tick;
        } else {
            break;
        }
        c.pos = payload.pos;
        if (strict && c.pos != payload_end) {
            return std::nullopt;
        }
        if (!c.SkipTo(payload_end)) {
            break;
        }
    }
    if (budget.failed) {
        error = "replay field budget or input read failed";
        return std::nullopt;
    }
    if (!contents.trailer_present) {
        contents.truncated = true;
    }
    if (strict && (contents.truncated || contents.tick_count != header_tick_count)) {
        return std::nullopt;
    }
    error.clear();
    return contents;
}

} // namespace

std::optional<ReplayContents> ReadReplay(const std::string& path) {
    return ReadReplay(path, ReplayReadLimits{});
}

std::optional<ReplayContents> ReadReplay(const std::string& path, ReplayReadLimits limits) {
    std::string error;
    return ReadReplay(path, limits, error);
}

std::optional<ReplayContents>
ReadReplay(const std::string& path, ReplayReadLimits limits, std::string& error) {
    try {
        error = "cannot open replay input";
        Filesystem::ReplacementSafeInputFile source;
        if (!source.open(fs::path(path))) {
            return std::nullopt;
        }
        std::error_code size_error;
        const auto size = source.size(size_error);
        if (size_error) {
            error = "cannot measure replay input";
            return std::nullopt;
        }
        return ParseReplay(source, size, limits, error);
    } catch (const std::exception&) {
        // Keep the staged diagnostic without allocating during allocation failure.
        return std::nullopt;
    }
}

std::optional<ReplayContents> ReadReplay(std::istream& input, ReplayReadLimits limits) {
    std::string error;
    return ReadReplay(input, limits, error);
}

std::optional<ReplayContents>
ReadReplay(std::istream& input, ReplayReadLimits limits, std::string& error) {
    try {
        error = "cannot seek or measure replay input";
        if (!input.seekg(0, std::ios::end)) {
            return std::nullopt;
        }
        const auto end = input.tellg();
        if (end < std::streampos{0}) {
            return std::nullopt;
        }
        StreamReplaySource source{input};
        return ParseReplay(source, static_cast<std::uint64_t>(end), limits, error);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

const InputRecord* FindInput(const ReplayContents& contents, std::uint64_t tick) {
    for (const InputRecord& rec : contents.inputs) {
        if (rec.tick == tick)
            return &rec;
    }
    return nullptr;
}

const CheckpointRecord* FindCheckpoint(const ReplayContents& contents, std::uint64_t tick) {
    for (const CheckpointRecord& rec : contents.checkpoints) {
        if (rec.tick == tick)
            return &rec;
    }
    return nullptr;
}

} // namespace Luminumbra::Replay

namespace Luminumbra::world {

// Share the explicit LE primitives with the replay envelope; neither codec
// depends on native struct layout or publishes a partially decoded descriptor.
bool EncodeAuthorityBootDescriptor(const AuthorityBootDescriptor& descriptor,
                                   std::vector<std::uint8_t>& output,
                                   std::string& error) {
    return Replay::EncodeBootDescriptor(descriptor, output, error);
}

bool DecodeAuthorityBootDescriptor(std::span<const std::uint8_t> bytes,
                                   AuthorityBootDescriptor& output,
                                   std::string& error) {
    return Replay::DecodeBootDescriptor(bytes, output, error);
}

} // namespace Luminumbra::world
