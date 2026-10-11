#include "luminumbra_common/core/Crc32.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/world/FarAuthorityOverlay.h"
#include "luminumbra_common/world/FarWantedSet.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace Luminumbra::World;
namespace P = Luminumbra::Persistence;

constexpr std::size_t kPresentRecordBytes = 16 + 250 + 125;
constexpr std::size_t kHomogeneousRecordBytes = 16;

FarTileKey Key(int tier, std::int64_t tx, std::int64_t tz) {
    return FarTileKey{static_cast<std::uint8_t>(tier), tx, tz};
}

FarOverlayBrick MakeBrick(
    std::int32_t bx, std::int32_t by, std::int32_t bz, FarBrickState state, std::size_t seed) {
    FarOverlayBrick brick;
    brick.bx = bx;
    brick.by = by;
    brick.bz = bz;
    brick.state = state;
    // Homogeneous bricks carry no samples, so their arrays stay zero.
    if (state == FarBrickState::Present) {
        for (std::size_t i = 0; i < kFsv1SamplesPerBrick; ++i) {
            const int sample = static_cast<int>((seed + i * 37) % 2001) - 1000;
            brick.density[i] = static_cast<std::int16_t>(sample);
            brick.material[i] = static_cast<std::uint8_t>((seed + i) % 251);
        }
    }
    return brick;
}

// Valid tile: key (2,-1,3) footprint is bx in [-32,0), bz in [96,128).
FarOverlayTile MakeTile() {
    FarOverlayTile tile;
    tile.key = Key(2, -1, 3);
    tile.generation = 0x0102030405060708ull;
    tile.bricks = {
        MakeBrick(-32, -4, 96, FarBrickState::Present, 11),
        MakeBrick(-32, 5, 96, FarBrickState::HomogeneousAir, 0),
        MakeBrick(-5, 0, 97, FarBrickState::Present, 23),
        MakeBrick(-1, 7, 127, FarBrickState::HomogeneousSolid, 0),
    };
    return tile;
}

// Second overlay in the same save; tier-3 tile (-1,-1) footprint is [-32,0) on both axes.
FarOverlayTile SecondTile() {
    FarOverlayTile tile;
    tile.key = Key(3, -1, -1);
    tile.generation = 9;
    tile.bricks.push_back(MakeBrick(-32, 1, -32, FarBrickState::HomogeneousSolid, 0));
    return tile;
}

void ExpectSameTile(const FarOverlayTile& actual, const FarOverlayTile& expected) {
    EXPECT_EQ(actual.key, expected.key);
    EXPECT_EQ(actual.generation, expected.generation);
    ASSERT_EQ(actual.bricks.size(), expected.bricks.size());
    for (std::size_t i = 0; i < expected.bricks.size(); ++i) {
        const FarOverlayBrick& got = actual.bricks[i];
        const FarOverlayBrick& want = expected.bricks[i];
        EXPECT_EQ(got.bx, want.bx);
        EXPECT_EQ(got.by, want.by);
        EXPECT_EQ(got.bz, want.bz);
        EXPECT_EQ(got.state, want.state);
        EXPECT_EQ(got.density, want.density);
        EXPECT_EQ(got.material, want.material);
    }
}

std::string EncodeOrFail(const FarOverlayTile& tile) {
    std::string bytes;
    EXPECT_EQ(EncodeFarOverlay(tile, bytes), FarOverlayError::None);
    return bytes;
}

void ExpectEncodeRefused(const FarOverlayTile& tile, FarOverlayError expected) {
    std::string out = "stale";
    EXPECT_EQ(EncodeFarOverlay(tile, out), expected);
    EXPECT_TRUE(out.empty());
}

// Decodes and returns the error; a refused decode must never expose a partial tile.
FarOverlayError DecodeError(const std::string& bytes, const FarTileKey& expected = Key(2, -1, 3)) {
    const FarOverlayDecodeResult result = DecodeFarOverlay(bytes, expected);
    EXPECT_TRUE(result.tile.bricks.empty());
    return result.error;
}

std::uint32_t ReadLe32(const std::string& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + i]))
                 << (8 * i);
    }
    return value;
}

void PatchLe(std::string& bytes, std::size_t offset, std::uint64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        bytes[offset + i] = static_cast<char>((value >> (8 * i)) & 0xffu);
    }
}

void RecomputeCrc(std::string& bytes) {
    const std::size_t body_end = bytes.size() - kFsv1TrailerBytes;
    Luminumbra::Core::Crc32Accumulator crc;
    crc.Update(bytes.data(), body_end);
    PatchLe(bytes, body_end, crc.Value(), kFsv1TrailerBytes);
}

// Returns a copy with `size` little-endian bytes at `offset` replaced by `value`.
std::string Patched(std::string bytes,
                    std::size_t offset,
                    std::uint64_t value,
                    std::size_t size,
                    bool recompute_crc = true) {
    PatchLe(bytes, offset, value, size);
    if (recompute_crc) {
        RecomputeCrc(bytes);
    }
    return bytes;
}

std::string ReadBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    return bytes.str();
}

void WriteBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary);
    output << bytes;
}

void WriteValidTree(const std::filesystem::path& dir) {
    ASSERT_TRUE(WriteFarOverlay(dir, MakeTile()));
    ASSERT_TRUE(WriteFarOverlay(dir, SecondTile()));
}

class TempDir {
public:
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("luminumbra_far_overlay_test_" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// Clears the process-global interrupt seam even when an assertion ends the test early.
class InterruptSeamGuard {
public:
    ~InterruptSeamGuard() {
        P::WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    }
};

constexpr std::optional<std::uint64_t> Gen(std::uint64_t value) {
    return value;
}

std::array<std::int16_t, kFsv1SamplesPerBrick> Uniform(std::int16_t value) {
    std::array<std::int16_t, kFsv1SamplesPerBrick> density{};
    density.fill(value);
    return density;
}

} // namespace

TEST(FarOverlayCodec, RoundTripAllThreeStates) {
    const FarOverlayTile tile = MakeTile();
    const std::string bytes = EncodeOrFail(tile);
    constexpr std::size_t kBody = 2 * kPresentRecordBytes + 2 * kHomogeneousRecordBytes;
    ASSERT_EQ(bytes.size(), kFsv1HeaderBytes + kBody + kFsv1TrailerBytes);
    EXPECT_EQ(bytes.substr(0, 4), "FSV1");
    EXPECT_EQ(ReadLe32(bytes, 36), 4u);
    EXPECT_EQ(ReadLe32(bytes, 40), kBody);

    const FarOverlayDecodeResult decoded = DecodeFarOverlay(bytes, Key(2, -1, 3));
    ASSERT_EQ(decoded.error, FarOverlayError::None);
    ExpectSameTile(decoded.tile, tile);
    for (const FarOverlayBrick& brick : decoded.tile.bricks) {
        if (brick.state != FarBrickState::Present) {
            EXPECT_TRUE(std::all_of(
                brick.density.begin(), brick.density.end(), [](std::int16_t v) { return v == 0; }));
            EXPECT_TRUE(std::all_of(brick.material.begin(),
                                    brick.material.end(),
                                    [](std::uint8_t v) { return v == 0; }));
        }
    }
}

TEST(FarOverlayCodec, GoldenBytes) {
    constexpr unsigned char kGolden[] = {
        0x46, 0x53, 0x56, 0x31, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x02, 0x00, 0x00, 0x00, 0x20, 0x00,
        0x00, 0x00, 0xe0, 0xff, 0xff, 0xff, 0x05, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0xe1, 0xff, 0xff, 0xff, 0xfe, 0xff, 0xff, 0xff, 0x60, 0x00,
        0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x9b, 0x7a, 0x24, 0xcd,
    };
    static_assert(std::size(kGolden) == 80);
    const std::string expected(reinterpret_cast<const char*>(kGolden), std::size(kGolden));

    FarOverlayTile tile;
    tile.key = Key(2, -1, 3);
    tile.generation = 0x0102030405060708ull;
    tile.bricks.push_back(FarOverlayBrick{-32, 5, 96, FarBrickState::HomogeneousAir});
    tile.bricks.push_back(FarOverlayBrick{-31, -2, 96, FarBrickState::HomogeneousSolid});

    std::string bytes;
    ASSERT_EQ(EncodeFarOverlay(tile, bytes), FarOverlayError::None);
    EXPECT_EQ(bytes, expected);

    const FarOverlayDecodeResult decoded = DecodeFarOverlay(expected, Key(2, -1, 3));
    ASSERT_EQ(decoded.error, FarOverlayError::None);
    ExpectSameTile(decoded.tile, tile);
}

TEST(FarOverlayCodec, EmptyTileRoundTrips) {
    FarOverlayTile tile;
    tile.key = Key(1, 0, 0);
    tile.generation = 7;
    const std::string bytes = EncodeOrFail(tile);
    EXPECT_EQ(bytes.size(), kFsv1HeaderBytes + kFsv1TrailerBytes);

    const FarOverlayDecodeResult decoded = DecodeFarOverlay(bytes, Key(1, 0, 0));
    ASSERT_EQ(decoded.error, FarOverlayError::None);
    EXPECT_EQ(decoded.tile.generation, 7u);
    EXPECT_TRUE(decoded.tile.bricks.empty());
}

TEST(FarOverlayCodec, TruncationAtEveryPrefixIsRefused) {
    const std::string bytes = EncodeOrFail(MakeTile());
    ASSERT_FALSE(bytes.empty());
    const std::string_view view(bytes);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        const FarOverlayDecodeResult decoded =
            DecodeFarOverlay(view.substr(0, length), Key(2, -1, 3));
        EXPECT_EQ(decoded.error, FarOverlayError::Truncated) << "prefix length " << length;
        EXPECT_TRUE(decoded.tile.bricks.empty()) << "prefix length " << length;
    }
}

TEST(FarOverlayCodec, RefusalTable) {
    const std::string valid = EncodeOrFail(MakeTile());
    // First record starts right after the 44-byte header.
    constexpr std::size_t kFirst = kFsv1HeaderBytes;

    EXPECT_EQ(DecodeError(Patched(valid, 0, 'X', 1, false)), FarOverlayError::BadMagic);
    {
        std::string flipped = valid;
        flipped[100] = static_cast<char>(flipped[100] ^ 0x40);
        EXPECT_EQ(DecodeError(flipped), FarOverlayError::CrcMismatch);
    }
    EXPECT_EQ(DecodeError(Patched(valid, 4, 0, 2)), FarOverlayError::CorruptVersion);
    EXPECT_EQ(DecodeError(Patched(valid, 4, 2, 2)), FarOverlayError::FutureVersion);
    EXPECT_EQ(DecodeError(Patched(valid, 6, 1, 2)), FarOverlayError::ReservedBits);
    EXPECT_EQ(DecodeError(Patched(valid, 9, 1, 1)), FarOverlayError::ReservedBits);
    EXPECT_EQ(DecodeError(Patched(valid, 8, 0, 1)), FarOverlayError::InvalidTier);
    EXPECT_EQ(DecodeError(Patched(valid, 8, 6, 1)), FarOverlayError::InvalidTier);
    EXPECT_EQ(DecodeError(Patched(valid, 12, 5, 8)), FarOverlayError::KeyMismatch);
    EXPECT_EQ(DecodeError(valid, Key(2, -1, 4)), FarOverlayError::KeyMismatch);
    EXPECT_EQ(DecodeError(Patched(valid, 36, kFsv1MaxBricks + 1, 4)),
              FarOverlayError::LimitExceeded);
    EXPECT_EQ(DecodeError(Patched(valid, 40, 70000000, 4)), FarOverlayError::LimitExceeded);
    EXPECT_EQ(DecodeError(valid + "x"), FarOverlayError::TrailingBytes);
    EXPECT_EQ(DecodeError(Patched(valid, 36, 5, 4)), FarOverlayError::SizeMismatch);
    EXPECT_EQ(DecodeError(Patched(valid, kFirst + 12, 3, 1)), FarOverlayError::InvalidBrickState);
    EXPECT_EQ(DecodeError(Patched(valid, kFirst + 13, 1, 1)), FarOverlayError::ReservedBits);
    EXPECT_EQ(DecodeError(Patched(valid, kFirst, 0, 4)), FarOverlayError::BrickOutOfFootprint);
    EXPECT_EQ(DecodeError(Patched(valid, kFirst + 8, 128, 4)),
              FarOverlayError::BrickOutOfFootprint);
}

TEST(FarOverlayCodec, EncodeRefusalTable) {
    {
        FarOverlayTile tile = MakeTile();
        tile.bricks = {MakeBrick(-5, 0, 97, FarBrickState::HomogeneousAir, 0),
                       MakeBrick(-32, -4, 96, FarBrickState::HomogeneousAir, 0)};
        ExpectEncodeRefused(tile, FarOverlayError::BricksNotSorted);
    }
    {
        FarOverlayTile tile = MakeTile();
        tile.bricks = {MakeBrick(-32, 5, 96, FarBrickState::HomogeneousAir, 0),
                       MakeBrick(-32, 5, 96, FarBrickState::HomogeneousAir, 0)};
        ExpectEncodeRefused(tile, FarOverlayError::BricksNotSorted);
    }
    {
        FarOverlayTile tile = MakeTile();
        tile.bricks = {MakeBrick(-32, -4, 96, FarBrickState::Present, 11)};
        tile.bricks[0].density[3] = kFsv1ForbiddenDensity;
        ExpectEncodeRefused(tile, FarOverlayError::ForbiddenDensity);
    }
    {
        FarOverlayTile tile = MakeTile();
        tile.bricks = {MakeBrick(-32, (1 << 20) + 1, 96, FarBrickState::HomogeneousAir, 0)};
        ExpectEncodeRefused(tile, FarOverlayError::BrickOutOfFootprint);
    }
    {
        FarOverlayTile tile = MakeTile();
        tile.key = Key(0, -1, 3);
        ExpectEncodeRefused(tile, FarOverlayError::InvalidKey);
    }
    {
        FarOverlayTile tile = MakeTile();
        tile.key = Key(6, -1, 3);
        ExpectEncodeRefused(tile, FarOverlayError::InvalidKey);
    }
    {
        FarOverlayTile tile = MakeTile();
        tile.key = Key(2, kFsv1MaxAbsTileIndex + 1, 3);
        ExpectEncodeRefused(tile, FarOverlayError::InvalidKey);
    }
}

TEST(FarOverlayFiles, PathAndNameRoundTrip) {
    const std::filesystem::path dir = "saves";
    const std::filesystem::path path = FarOverlayPath(dir, Key(2, -1, 3));
    EXPECT_EQ(path.filename().string(), "v2.-1.3.lmr");
    EXPECT_EQ(path.parent_path(), dir / "far");

    const auto minimal = ParseFarOverlayFileName("v1.0.0.lmr");
    ASSERT_TRUE(minimal.has_value());
    EXPECT_EQ(*minimal, Key(1, 0, 0));
    const auto negative = ParseFarOverlayFileName("v5.-12.34.lmr");
    ASSERT_TRUE(negative.has_value());
    EXPECT_EQ(*negative, Key(5, -12, 34));

    const char* const kRejected[] = {
        "v0.1.1.lmr",
        "v6.1.1.lmr",
        "v1.01.1.lmr",
        "v1.-0.1.lmr",
        "v1.+1.1.lmr",
        "v1.1.1.txt",
        "v1.1.lmr",
        "x1.1.1.lmr",
        "v1.99999999999999999999.1.lmr",
        "",
    };
    for (const char* const name : kRejected) {
        EXPECT_FALSE(ParseFarOverlayFileName(name).has_value()) << name;
    }
}

TEST(FarOverlayFiles, WriteReadRoundTrip) {
    TempDir temp;
    const FarOverlayTile tile = MakeTile();
    ASSERT_TRUE(WriteFarOverlay(temp.path(), tile));
    EXPECT_TRUE(std::filesystem::is_directory(temp.path() / "far"));

    const FarOverlayDecodeResult read = ReadFarOverlay(temp.path(), Key(2, -1, 3));
    ASSERT_EQ(read.error, FarOverlayError::None);
    ExpectSameTile(read.tile, tile);

    EXPECT_EQ(ReadFarOverlay(temp.path(), Key(3, 0, 0)).error, FarOverlayError::NotFound);
}

TEST(FarOverlayFiles, OverwriteReplacesPreviousGeneration) {
    TempDir temp;
    FarOverlayTile tile = MakeTile();
    tile.generation = 1;
    ASSERT_TRUE(WriteFarOverlay(temp.path(), tile));
    tile.generation = 2;
    ASSERT_TRUE(WriteFarOverlay(temp.path(), tile));

    const FarOverlayDecodeResult read = ReadFarOverlay(temp.path(), Key(2, -1, 3));
    ASSERT_EQ(read.error, FarOverlayError::None);
    EXPECT_EQ(read.tile.generation, 2u);
}

TEST(FarOverlayFiles, InterruptedWriteKeepsPreviousFile) {
    TempDir temp;
    FarOverlayTile tile = MakeTile();
    tile.generation = 1;
    ASSERT_TRUE(WriteFarOverlay(temp.path(), tile));

    tile.generation = 2;
    InterruptSeamGuard guard;
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    std::vector<std::string> errors;
    EXPECT_FALSE(WriteFarOverlay(temp.path(), tile, &errors));
    EXPECT_EQ(errors.size(), 1u);
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(false);

    const FarOverlayDecodeResult read = ReadFarOverlay(temp.path(), Key(2, -1, 3));
    ASSERT_EQ(read.error, FarOverlayError::None);
    EXPECT_EQ(read.tile.generation, 1u);

    std::size_t entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(temp.path() / "far")) {
        ++entries;
        EXPECT_TRUE(entry.is_regular_file());
    }
    EXPECT_EQ(entries, 1u);
}

TEST(FarOverlayFiles, ValidateDirectory) {
    {
        TempDir temp;
        std::vector<std::string> errors;
        EXPECT_TRUE(ValidateFarOverlayDirectory(temp.path(), &errors));
        EXPECT_TRUE(errors.empty());
    }
    {
        TempDir temp;
        std::vector<std::string> errors;
        WriteValidTree(temp.path());
        EXPECT_TRUE(ValidateFarOverlayDirectory(temp.path(), &errors));
        EXPECT_TRUE(errors.empty());

        const std::filesystem::path far = temp.path() / "far";
        WriteBytes(far / "notes.txt", "hello");
        WriteBytes(far / "v2.-1.3.lmr.tmp", "partial");
        EXPECT_TRUE(ValidateFarOverlayDirectory(temp.path(), &errors));
        EXPECT_TRUE(errors.empty());
    }
    {
        TempDir temp;
        WriteValidTree(temp.path());
        const std::filesystem::path file = FarOverlayPath(temp.path(), Key(2, -1, 3));
        std::string bytes = ReadBytes(file);
        bytes[100] = static_cast<char>(bytes[100] ^ 0x40);
        WriteBytes(file, bytes);

        std::vector<std::string> errors;
        EXPECT_FALSE(ValidateFarOverlayDirectory(temp.path(), &errors));
        EXPECT_EQ(errors.size(), 1u);
    }
    {
        TempDir temp;
        WriteValidTree(temp.path());
        WriteBytes(FarOverlayPath(temp.path(), Key(2, 0, 3)),
                   ReadBytes(FarOverlayPath(temp.path(), Key(2, -1, 3))));

        std::vector<std::string> errors;
        EXPECT_FALSE(ValidateFarOverlayDirectory(temp.path(), &errors));
        EXPECT_EQ(errors.size(), 1u);
    }
    {
        TempDir temp;
        WriteValidTree(temp.path());
        const std::filesystem::path file = FarOverlayPath(temp.path(), Key(2, -1, 3));
        WriteBytes(file, Patched(ReadBytes(file), 4, 2, 2, false));

        std::vector<std::string> errors;
        EXPECT_FALSE(ValidateFarOverlayDirectory(temp.path(), &errors));
        EXPECT_EQ(errors.size(), 1u);
    }
    {
        // A directory that carries an overlay name cannot be read as an overlay.
        TempDir temp;
        std::filesystem::create_directories(temp.path() / "far" / "v1.0.0.lmr");

        std::vector<std::string> errors;
        EXPECT_FALSE(ValidateFarOverlayDirectory(temp.path(), &errors));
        EXPECT_EQ(errors.size(), 1u);
    }
    {
        // `far` existing as a regular file is a corrupt save layout.
        TempDir temp;
        WriteBytes(temp.path() / "far", "not a directory");

        std::vector<std::string> errors;
        EXPECT_FALSE(ValidateFarOverlayDirectory(temp.path(), &errors));
        EXPECT_EQ(errors.size(), 1u);
    }
}

TEST(FarBakeDue, Matrix) {
    static_assert(!FarBakeDue(std::nullopt, std::nullopt));
    static_assert(!FarBakeDue(Gen(5), std::nullopt));
    static_assert(FarBakeDue(std::nullopt, Gen(0)));
    static_assert(FarBakeDue(std::nullopt, Gen(7)));
    static_assert(!FarBakeDue(Gen(5), Gen(5)));
    static_assert(!FarBakeDue(Gen(5), Gen(4)));
    static_assert(FarBakeDue(Gen(5), Gen(6)));
    SUCCEED();
}

TEST(FarDensity, QuantizeFarDensity) {
    EXPECT_EQ(QuantizeFarDensity(0.0f), 0);
    EXPECT_EQ(QuantizeFarDensity(1.0f), 256);
    EXPECT_EQ(QuantizeFarDensity(-1.0f), -256);
    EXPECT_EQ(QuantizeFarDensity(0.0001f), 1);
    EXPECT_EQ(QuantizeFarDensity(-0.0001f), -1);
    EXPECT_EQ(QuantizeFarDensity(1000.0f), 32767);
    EXPECT_EQ(QuantizeFarDensity(-1000.0f), -32767);
    const float infinity = std::numeric_limits<float>::infinity();
    EXPECT_EQ(QuantizeFarDensity(infinity), 32767);
    EXPECT_EQ(QuantizeFarDensity(-infinity), -32767);
    EXPECT_EQ(QuantizeFarDensity(std::numeric_limits<float>::quiet_NaN()), 0);

    constexpr std::int16_t kMin = std::numeric_limits<std::int16_t>::min();
    for (int step = 0; step <= 1081; ++step) {
        const float value = -200.0f + 0.37f * static_cast<float>(step);
        EXPECT_NE(QuantizeFarDensity(value), kMin) << value;
    }
}

TEST(FarDensity, ClassifyFarBrick) {
    EXPECT_EQ(ClassifyFarBrick(Uniform(-1)), FarBrickState::HomogeneousSolid);
    EXPECT_EQ(ClassifyFarBrick(Uniform(1)), FarBrickState::HomogeneousAir);

    auto one_zero = Uniform(1);
    one_zero[0] = 0;
    EXPECT_EQ(ClassifyFarBrick(one_zero), FarBrickState::Present);

    auto mixed = Uniform(1);
    mixed[7] = -1;
    EXPECT_EQ(ClassifyFarBrick(mixed), FarBrickState::Present);

    EXPECT_EQ(ClassifyFarBrick(Uniform(0)), FarBrickState::Present);
}
