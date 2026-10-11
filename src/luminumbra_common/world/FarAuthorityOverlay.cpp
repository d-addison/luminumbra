#include "FarAuthorityOverlay.h"

#include "core/Crc32.h"
#include "persistence/WorldSaveService.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace Luminumbra::World {
namespace {

constexpr std::size_t kFsv1BodyHeaderBytes = 16;
constexpr std::size_t kFsv1DensityBytes = kFsv1SamplesPerBrick * 2;
constexpr std::size_t kFsv1MaterialBytes = kFsv1SamplesPerBrick;
constexpr std::size_t kFsv1PresentPayloadBytes = kFsv1DensityBytes + kFsv1MaterialBytes;
constexpr std::size_t kFsv1ReservedBodyBytes = 3;
constexpr std::uint8_t kFsv1MaxStateValue = 2;
constexpr std::int64_t kFsv1TileBricksSpan = kFsv1BricksPerTileEdge;

// Brick ordering key: bz first, then bx, then by.
using BrickOrderKey = std::tuple<std::int32_t, std::int32_t, std::int32_t>;

BrickOrderKey OrderKey(std::int32_t bx, std::int32_t by, std::int32_t bz) {
    return BrickOrderKey{bz, bx, by};
}

void AppendU8(std::string& out, std::uint8_t value) {
    out.push_back(static_cast<char>(value));
}

void AppendU16(std::string& out, std::uint16_t value) {
    for (unsigned shift = 0; shift < 16; shift += 8) {
        AppendU8(out, static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
}

void AppendU32(std::string& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        AppendU8(out, static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
}

void AppendU64(std::string& out, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        AppendU8(out, static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
}

void AppendI16(std::string& out, std::int16_t value) {
    AppendU16(out, static_cast<std::uint16_t>(value));
}

void AppendI32(std::string& out, std::int32_t value) {
    AppendU32(out, static_cast<std::uint32_t>(value));
}

void AppendI64(std::string& out, std::int64_t value) {
    AppendU64(out, static_cast<std::uint64_t>(value));
}

// Readers assume the caller has verified that `offset + size` lies inside `bytes`.
std::uint8_t ReadU8(std::string_view bytes, std::size_t offset) {
    return static_cast<std::uint8_t>(bytes[offset]);
}

std::uint16_t ReadU16(std::string_view bytes, std::size_t offset) {
    std::uint16_t value = 0;
    for (std::size_t i = 0; i < 2; ++i) {
        value |= static_cast<std::uint16_t>(static_cast<std::uint16_t>(ReadU8(bytes, offset + i))
                                            << (8u * i));
    }
    return value;
}

std::uint32_t ReadU32(std::string_view bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(ReadU8(bytes, offset + i)) << (8u * i);
    }
    return value;
}

std::uint64_t ReadU64(std::string_view bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(ReadU8(bytes, offset + i)) << (8u * i);
    }
    return value;
}

std::int16_t ReadI16(std::string_view bytes, std::size_t offset) {
    return static_cast<std::int16_t>(ReadU16(bytes, offset));
}

std::int32_t ReadI32(std::string_view bytes, std::size_t offset) {
    return static_cast<std::int32_t>(ReadU32(bytes, offset));
}

std::int64_t ReadI64(std::string_view bytes, std::size_t offset) {
    return static_cast<std::int64_t>(ReadU64(bytes, offset));
}

bool IsValidTier(std::uint64_t tier) {
    return tier >= 1 && tier <= static_cast<std::uint64_t>(kFarTierCount);
}

bool IsValidTileIndex(std::int64_t index) {
    return index >= -kFsv1MaxAbsTileIndex && index <= kFsv1MaxAbsTileIndex;
}

bool IsValidKey(const FarTileKey& key) {
    return IsValidTier(key.tier) && IsValidTileIndex(key.tx) && IsValidTileIndex(key.tz);
}

bool IsInFootprint(const FarTileKey& key, std::int32_t bx, std::int32_t by, std::int32_t bz) {
    const std::int64_t min_x = key.tx * kFsv1BricksPerTileEdge;
    const std::int64_t min_z = key.tz * kFsv1BricksPerTileEdge;
    const std::int64_t brick_x = bx;
    const std::int64_t brick_z = bz;
    const std::int64_t brick_y = by;
    return brick_x >= min_x && brick_x < min_x + kFsv1TileBricksSpan && brick_z >= min_z &&
           brick_z < min_z + kFsv1TileBricksSpan &&
           (brick_y < 0 ? -brick_y : brick_y) <= static_cast<std::int64_t>(kFsv1MaxAbsBrickY);
}

bool HasForbiddenDensity(const std::array<std::int16_t, kFsv1SamplesPerBrick>& density) {
    return std::any_of(density.begin(), density.end(), [](std::int16_t value) {
        return value == kFsv1ForbiddenDensity;
    });
}

void Report(std::vector<std::string>* errors, std::string message) {
    if (errors != nullptr) {
        errors->push_back(std::move(message));
    }
}

// Parses a canonical decimal integer: optional single '-', no leading zeros, no "-0".
std::optional<std::int64_t> ParseCanonicalInt64(std::string_view text) {
    if (text.empty() || text == "-0") {
        return std::nullopt;
    }
    const std::size_t digit_start = text.front() == '-' ? 1 : 0;
    const std::string_view digits = text.substr(digit_start);
    if (digits.empty() || (digits.size() > 1 && digits.front() == '0')) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    return value;
}

// Footprint, state, ordering and density checks shared by encode and decode for one brick.
// `previous` is the key of the preceding brick, when there is one.
FarOverlayError CheckBrickRecord(const FarTileKey& key,
                                 const std::optional<BrickOrderKey>& previous,
                                 std::int32_t bx,
                                 std::int32_t by,
                                 std::int32_t bz,
                                 FarBrickState state,
                                 const std::array<std::int16_t, kFsv1SamplesPerBrick>& density) {
    if (!IsInFootprint(key, bx, by, bz)) {
        return FarOverlayError::BrickOutOfFootprint;
    }
    if (static_cast<std::uint8_t>(state) > kFsv1MaxStateValue) {
        return FarOverlayError::InvalidBrickState;
    }
    const BrickOrderKey current = OrderKey(bx, by, bz);
    if (previous.has_value() && !(*previous < current)) {
        return FarOverlayError::BricksNotSorted;
    }
    if (state == FarBrickState::Present && HasForbiddenDensity(density)) {
        return FarOverlayError::ForbiddenDensity;
    }
    return FarOverlayError::None;
}

} // namespace

const char* FarOverlayErrorName(FarOverlayError error) {
    switch (error) {
        case FarOverlayError::None:
            return "None";
        case FarOverlayError::NotFound:
            return "NotFound";
        case FarOverlayError::IoError:
            return "IoError";
        case FarOverlayError::BadMagic:
            return "BadMagic";
        case FarOverlayError::Truncated:
            return "Truncated";
        case FarOverlayError::CrcMismatch:
            return "CrcMismatch";
        case FarOverlayError::FutureVersion:
            return "FutureVersion";
        case FarOverlayError::CorruptVersion:
            return "CorruptVersion";
        case FarOverlayError::ReservedBits:
            return "ReservedBits";
        case FarOverlayError::InvalidTier:
            return "InvalidTier";
        case FarOverlayError::InvalidKey:
            return "InvalidKey";
        case FarOverlayError::KeyMismatch:
            return "KeyMismatch";
        case FarOverlayError::BrickOutOfFootprint:
            return "BrickOutOfFootprint";
        case FarOverlayError::BricksNotSorted:
            return "BricksNotSorted";
        case FarOverlayError::InvalidBrickState:
            return "InvalidBrickState";
        case FarOverlayError::ForbiddenDensity:
            return "ForbiddenDensity";
        case FarOverlayError::LimitExceeded:
            return "LimitExceeded";
        case FarOverlayError::SizeMismatch:
            return "SizeMismatch";
        case FarOverlayError::TrailingBytes:
            return "TrailingBytes";
    }
    return "Unknown";
}

FarOverlayError EncodeFarOverlay(const FarOverlayTile& tile, std::string& out) {
    out.clear();
    if (!IsValidKey(tile.key)) {
        return FarOverlayError::InvalidKey;
    }
    if (static_cast<std::uint64_t>(tile.bricks.size()) > kFsv1MaxBricks) {
        return FarOverlayError::LimitExceeded;
    }

    std::uint64_t body_bytes = 0;
    std::optional<BrickOrderKey> previous;
    for (const FarOverlayBrick& brick : tile.bricks) {
        const FarOverlayError brick_error = CheckBrickRecord(
            tile.key, previous, brick.bx, brick.by, brick.bz, brick.state, brick.density);
        if (brick_error != FarOverlayError::None) {
            return brick_error;
        }
        previous = OrderKey(brick.bx, brick.by, brick.bz);
        body_bytes += kFsv1BodyHeaderBytes;
        if (brick.state == FarBrickState::Present) {
            body_bytes += kFsv1PresentPayloadBytes;
        }
    }

    const std::uint64_t total_bytes = kFsv1HeaderBytes + body_bytes + kFsv1TrailerBytes;
    if (total_bytes > kFsv1MaxFileBytes) {
        return FarOverlayError::LimitExceeded;
    }

    std::string bytes;
    bytes.reserve(static_cast<std::size_t>(total_bytes));

    bytes.append("FSV1");
    AppendU16(bytes, kFsv1Version);
    AppendU16(bytes, 0);
    AppendU8(bytes, static_cast<std::uint8_t>(tile.key.tier));
    AppendU8(bytes, 0);
    AppendU8(bytes, 0);
    AppendU8(bytes, 0);
    AppendI64(bytes, tile.key.tx);
    AppendI64(bytes, tile.key.tz);
    AppendU64(bytes, tile.generation);
    AppendU32(bytes, static_cast<std::uint32_t>(tile.bricks.size()));
    AppendU32(bytes, static_cast<std::uint32_t>(body_bytes));

    for (const FarOverlayBrick& brick : tile.bricks) {
        AppendI32(bytes, brick.bx);
        AppendI32(bytes, brick.by);
        AppendI32(bytes, brick.bz);
        AppendU8(bytes, static_cast<std::uint8_t>(brick.state));
        AppendU8(bytes, 0);
        AppendU8(bytes, 0);
        AppendU8(bytes, 0);
        if (brick.state == FarBrickState::Present) {
            for (const std::int16_t sample : brick.density) {
                AppendI16(bytes, sample);
            }
            for (const std::uint8_t material : brick.material) {
                AppendU8(bytes, material);
            }
        }
    }

    Core::Crc32Accumulator crc;
    crc.Update(bytes.data(), bytes.size());
    AppendU32(bytes, crc.Value());

    out = std::move(bytes);
    return FarOverlayError::None;
}

FarOverlayDecodeResult DecodeFarOverlay(std::string_view bytes, const FarTileKey& expected_key) {
    FarOverlayDecodeResult result;
    const auto fail = [&result](FarOverlayError error) {
        result.error = error;
        result.tile = FarOverlayTile{};
        return result;
    };

    if (!IsValidKey(expected_key)) {
        return fail(FarOverlayError::InvalidKey);
    }
    if (bytes.size() < 4) {
        return fail(FarOverlayError::Truncated);
    }
    if (bytes.substr(0, 4) != std::string_view("FSV1")) {
        return fail(FarOverlayError::BadMagic);
    }
    if (bytes.size() < kFsv1HeaderBytes + kFsv1TrailerBytes) {
        return fail(FarOverlayError::Truncated);
    }

    const std::uint16_t version = ReadU16(bytes, 4);
    if (version == 0) {
        return fail(FarOverlayError::CorruptVersion);
    }
    if (version > kFsv1Version) {
        return fail(FarOverlayError::FutureVersion);
    }

    const std::uint16_t flags = ReadU16(bytes, 6);
    if (flags != 0 || ReadU8(bytes, 9) != 0 || ReadU8(bytes, 10) != 0 || ReadU8(bytes, 11) != 0) {
        return fail(FarOverlayError::ReservedBits);
    }

    const std::uint8_t tier = ReadU8(bytes, 8);
    if (!IsValidTier(tier)) {
        return fail(FarOverlayError::InvalidTier);
    }

    const std::int64_t tx = ReadI64(bytes, 12);
    const std::int64_t tz = ReadI64(bytes, 20);
    if (tier != expected_key.tier || tx != expected_key.tx || tz != expected_key.tz) {
        return fail(FarOverlayError::KeyMismatch);
    }

    const std::uint64_t generation = ReadU64(bytes, 28);
    const std::uint32_t brick_count = ReadU32(bytes, 36);
    const std::uint32_t body_size = ReadU32(bytes, 40);
    const std::uint64_t total_bytes =
        kFsv1HeaderBytes + static_cast<std::uint64_t>(body_size) + kFsv1TrailerBytes;
    if (brick_count > kFsv1MaxBricks || total_bytes > kFsv1MaxFileBytes) {
        return fail(FarOverlayError::LimitExceeded);
    }

    const auto actual_bytes = static_cast<std::uint64_t>(bytes.size());
    if (actual_bytes < total_bytes) {
        return fail(FarOverlayError::Truncated);
    }
    if (actual_bytes > total_bytes) {
        return fail(FarOverlayError::TrailingBytes);
    }

    const std::size_t body_end = kFsv1HeaderBytes + static_cast<std::size_t>(body_size);
    Core::Crc32Accumulator crc;
    crc.Update(bytes.data(), body_end);
    if (ReadU32(bytes, body_end) != crc.Value()) {
        return fail(FarOverlayError::CrcMismatch);
    }

    FarOverlayTile tile;
    tile.key = expected_key;
    tile.generation = generation;
    tile.bricks.reserve(brick_count);

    std::size_t cursor = kFsv1HeaderBytes;
    std::optional<BrickOrderKey> previous;
    for (std::uint32_t index = 0; index < brick_count; ++index) {
        if (cursor + kFsv1BodyHeaderBytes > body_end) {
            return fail(FarOverlayError::SizeMismatch);
        }
        const std::int32_t bx = ReadI32(bytes, cursor);
        const std::int32_t by = ReadI32(bytes, cursor + 4);
        const std::int32_t bz = ReadI32(bytes, cursor + 8);
        const std::uint8_t state_value = ReadU8(bytes, cursor + 12);
        if (state_value > kFsv1MaxStateValue) {
            return fail(FarOverlayError::InvalidBrickState);
        }
        const auto state = static_cast<FarBrickState>(state_value);
        const bool present = state == FarBrickState::Present;
        if (present && cursor + kFsv1BodyHeaderBytes + kFsv1PresentPayloadBytes > body_end) {
            return fail(FarOverlayError::SizeMismatch);
        }
        for (std::size_t reserved = 0; reserved < kFsv1ReservedBodyBytes; ++reserved) {
            if (ReadU8(bytes, cursor + 13 + reserved) != 0) {
                return fail(FarOverlayError::ReservedBits);
            }
        }

        FarOverlayBrick brick;
        brick.bx = bx;
        brick.by = by;
        brick.bz = bz;
        brick.state = state;
        if (present) {
            const std::size_t payload = cursor + kFsv1BodyHeaderBytes;
            for (std::size_t sample = 0; sample < kFsv1SamplesPerBrick; ++sample) {
                brick.density[sample] = ReadI16(bytes, payload + sample * 2);
            }
            for (std::size_t sample = 0; sample < kFsv1SamplesPerBrick; ++sample) {
                brick.material[sample] = ReadU8(bytes, payload + kFsv1DensityBytes + sample);
            }
        }

        const FarOverlayError brick_error =
            CheckBrickRecord(expected_key, previous, bx, by, bz, state, brick.density);
        if (brick_error != FarOverlayError::None) {
            return fail(brick_error);
        }
        previous = OrderKey(bx, by, bz);
        tile.bricks.push_back(brick);

        cursor += kFsv1BodyHeaderBytes + (present ? kFsv1PresentPayloadBytes : 0);
    }

    if (cursor != body_end) {
        return fail(FarOverlayError::SizeMismatch);
    }

    result.error = FarOverlayError::None;
    result.tile = std::move(tile);
    return result;
}

std::filesystem::path FarOverlayPath(const std::filesystem::path& save_dir, const FarTileKey& key) {
    const std::string file_name = "v" + std::to_string(static_cast<int>(key.tier)) + "." +
                                  std::to_string(key.tx) + "." + std::to_string(key.tz) + ".lmr";
    return save_dir / "far" / file_name;
}

std::optional<FarTileKey> ParseFarOverlayFileName(std::string_view file_name) {
    constexpr std::string_view kSuffix = ".lmr";
    if (file_name.size() < 1 + kSuffix.size() || file_name.front() != 'v' ||
        file_name.substr(file_name.size() - kSuffix.size()) != kSuffix) {
        return std::nullopt;
    }
    const std::string_view middle = file_name.substr(1, file_name.size() - 1 - kSuffix.size());

    std::string_view fields[3];
    std::size_t field_count = 0;
    std::size_t field_start = 0;
    while (true) {
        const std::size_t dot = middle.find('.', field_start);
        const std::size_t field_end = dot == std::string_view::npos ? middle.size() : dot;
        if (field_count == 3) {
            return std::nullopt;
        }
        fields[field_count++] = middle.substr(field_start, field_end - field_start);
        if (dot == std::string_view::npos) {
            break;
        }
        field_start = dot + 1;
    }
    if (field_count != 3) {
        return std::nullopt;
    }

    const auto tier = ParseCanonicalInt64(fields[0]);
    const auto tx = ParseCanonicalInt64(fields[1]);
    const auto tz = ParseCanonicalInt64(fields[2]);
    if (!tier.has_value() || !tx.has_value() || !tz.has_value()) {
        return std::nullopt;
    }
    if (*tier < 1 || *tier > static_cast<std::int64_t>(kFarTierCount)) {
        return std::nullopt;
    }

    FarTileKey key;
    key.tier = static_cast<std::uint8_t>(*tier);
    key.tx = *tx;
    key.tz = *tz;
    return key;
}

bool WriteFarOverlay(const std::filesystem::path& save_dir,
                     const FarOverlayTile& tile,
                     std::vector<std::string>* errors) {
    std::string bytes;
    const FarOverlayError error = EncodeFarOverlay(tile, bytes);
    if (error != FarOverlayError::None) {
        Report(errors, std::string("far overlay encode failed: ") + FarOverlayErrorName(error));
        return false;
    }
    return Persistence::WorldSaveService::write_file_durably(
        FarOverlayPath(save_dir, tile.key), bytes, errors);
}

FarOverlayDecodeResult ReadFarOverlay(const std::filesystem::path& save_dir,
                                      const FarTileKey& key) {
    FarOverlayDecodeResult result;
    const std::filesystem::path path = FarOverlayPath(save_dir, key);

    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        result.error = FarOverlayError::IoError;
        return result;
    }
    if (!exists) {
        result.error = FarOverlayError::NotFound;
        return result;
    }
    const bool regular = std::filesystem::is_regular_file(path, ec);
    if (ec || !regular) {
        result.error = FarOverlayError::IoError;
        return result;
    }
    const std::uintmax_t file_size = std::filesystem::file_size(path, ec);
    if (ec) {
        result.error = FarOverlayError::IoError;
        return result;
    }
    if (file_size > kFsv1MaxFileBytes) {
        result.error = FarOverlayError::LimitExceeded;
        return result;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.error = FarOverlayError::IoError;
        return result;
    }
    std::string bytes(static_cast<std::size_t>(file_size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        result.error = FarOverlayError::IoError;
        return result;
    }

    return DecodeFarOverlay(bytes, key);
}

bool ValidateFarOverlayDirectory(const std::filesystem::path& save_dir,
                                 std::vector<std::string>* errors) {
    const std::filesystem::path dir = save_dir / "far";
    std::error_code ec;
    const bool exists = std::filesystem::exists(dir, ec);
    if (ec) {
        Report(errors, "far overlay directory: IoError");
        return false;
    }
    if (!exists) {
        return true;
    }
    const bool is_directory = std::filesystem::is_directory(dir, ec);
    if (ec) {
        Report(errors, "far overlay directory: IoError");
        return false;
    }
    if (!is_directory) {
        Report(errors, "far overlay directory: not a directory");
        return false;
    }

    struct Candidate {
        std::string file_name;
        FarTileKey key;
    };
    std::vector<Candidate> candidates;
    bool failed = false;

    std::filesystem::directory_iterator iterator(dir, ec);
    if (ec) {
        Report(errors, "far overlay directory: IoError");
        return false;
    }
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        const std::filesystem::directory_entry& entry = *iterator;
        std::error_code entry_ec;
        const bool regular = entry.is_regular_file(entry_ec);
        if (entry_ec) {
            Report(errors, "far overlay directory: IoError");
            return false;
        }
        const std::string file_name = entry.path().filename().string();
        if (const auto key = ParseFarOverlayFileName(file_name)) {
            if (!regular) {
                Report(errors, "far overlay " + file_name + ": not a regular file");
                failed = true;
            } else {
                candidates.push_back(Candidate{file_name, *key});
            }
        }
        iterator.increment(ec);
        if (ec) {
            Report(errors, "far overlay directory: IoError");
            return false;
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.file_name < b.file_name;
    });

    for (const Candidate& candidate : candidates) {
        const FarOverlayDecodeResult decoded = ReadFarOverlay(save_dir, candidate.key);
        if (decoded.error != FarOverlayError::None) {
            Report(errors,
                   "far overlay " + candidate.file_name + ": " +
                       FarOverlayErrorName(decoded.error));
            failed = true;
        }
    }
    return !failed;
}

std::int16_t QuantizeFarDensity(float signed_distance_meters) {
    if (std::isnan(signed_distance_meters)) {
        return 0;
    }
    constexpr float kLimit = 32767.0f;
    const float scaled = std::round(signed_distance_meters * kFsv1DensityScale);
    const float clamped = std::clamp(scaled, -kLimit, kLimit);
    const auto result = static_cast<std::int16_t>(clamped);
    if (signed_distance_meters != 0.0f && result == 0) {
        return signed_distance_meters > 0.0f ? static_cast<std::int16_t>(1)
                                             : static_cast<std::int16_t>(-1);
    }
    return result;
}

FarBrickState ClassifyFarBrick(const std::array<std::int16_t, kFsv1SamplesPerBrick>& density) {
    if (std::all_of(density.begin(), density.end(), [](std::int16_t v) { return v < 0; })) {
        return FarBrickState::HomogeneousSolid;
    }
    if (std::all_of(density.begin(), density.end(), [](std::int16_t v) { return v > 0; })) {
        return FarBrickState::HomogeneousAir;
    }
    return FarBrickState::Present;
}

} // namespace Luminumbra::World
