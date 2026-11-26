#include "../../tools/active_regions_dump.h"
#include "../support/UniqueTempPath.h"

#include "luminumbra_common/world/ActiveRegionLedger.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {
using namespace Luminumbra::world;
using Luminumbra::Tools::ActiveRegionDumpStatus;
using Luminumbra::Tools::ActiveRegionsDumpCommand;
using Luminumbra::Tools::DumpActiveRegions;
using nlohmann::json;
namespace fs = std::filesystem;

constexpr RegionKey kFrozen{10, -3};
constexpr std::uint64_t kFrozenDigest = 0xfedcba9876543210ull;

void Write(const fs::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output.is_open());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    ASSERT_TRUE(output);
}

std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input.is_open());
    const std::string result{std::istreambuf_iterator<char>(input), {}};
    EXPECT_FALSE(input.bad());
    return result;
}

void Seal(std::string& bytes) {
    // Independent FNV-1a checksum for historical envelope fixtures.
    std::uint64_t checksum = 14695981039346656037ull;
    for (std::size_t index = 0; index < bytes.size() - 8; ++index) {
        checksum ^= static_cast<unsigned char>(bytes[index]);
        checksum *= 1099511628211ull;
    }
    for (unsigned index = 0; index < 8; ++index) {
        bytes[bytes.size() - 8 + index] = static_cast<char>((checksum >> (8 * index)) & 255);
    }
}

ActiveRegionLedger FrozenFixture() {
    ActiveRegionLedger ledger({0, 1, 1});
    ledger.mark_edited(kFrozen, WorldClock{});
    ledger.schedule(WorldClock(1));
    const std::array work{RegionWork{kFrozen, 1, 0, 0, 0}};
    for (std::uint64_t tick = 2; tick <= 3; ++tick) {
        ledger.schedule(WorldClock(tick), {}, work, [](RegionKey) { return kFrozenDigest; });
    }
    return ledger;
}

std::map<std::string, std::string> TreeBytes(const fs::path& root) {
    std::map<std::string, std::string> result;
    // Test-owned tree has three directories and three short regular files.
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            result.emplace(entry.path().lexically_relative(root).generic_string(),
                           Read(entry.path()));
        }
    }
    return result;
}

TEST(ActiveRegionDump, OrderedOutputPreservesFrozenAndExtensionRegisters) {
    Luminumbra::TestSupport::ScopedTempFile source("ledger-dump-", ".arl");
    auto ledger = FrozenFixture();
    // Insert out of order, including a tie on x, to exercise the total key order.
    ledger.pin({-2, 9}, true, WorldClock(3));
    ledger.pin({-2, -9}, true, WorldClock(3));
    ledger.mark_edited({-2, 9}, WorldClock(3));
    ledger.set_populated({-2, -9}, true);
    ledger.set_water_cursor(kFrozen, std::numeric_limits<std::uint64_t>::max());
    ledger.set_local_anchor(Luminumbra::Vec3(1.25f, -2.5f, 3.75f));
    ledger.acknowledge_world_clock(WorldClock(7));
    ledger.mark_saved(WorldClock(7));
    ASSERT_TRUE(ledger.reserve_persistent_ids(ActiveRegionLedger::kPersistentIdLimit));
    ASSERT_TRUE(ledger.set_energy_ages({{{-2, -9}, 0}, {kFrozen, WorldClock::kTickLimit - 1}}));
    ASSERT_TRUE(ledger.set_water_debts({{kFrozen, ActiveRegionLedger::kMaxWaterDebt}}));
    const std::array completed{
        RegionWork{kFrozen, std::numeric_limits<std::uint64_t>::max(), 0, 0, 0}};
    ledger.publish_completed_work(completed);
    auto bytes = ledger.encode();
    // Exercise true visited and both nonzero hold counters without adding activation
    // regions to this tiny persisted fixture. The codec validates the sealed result.
    bytes[ActiveRegionLedger::kHeaderBytes + 8] = 13; // visited, pinned, populated
    bytes[ActiveRegionLedger::kHeaderBytes + 24] = 3; // last_proximity
    bytes[ActiveRegionLedger::kHeaderBytes + 84] = 1; // under_hold
    bytes[ActiveRegionLedger::kHeaderBytes + ActiveRegionLedger::kRecordBytes + 80] = 1;
    Seal(bytes);
    Write(source.Path(), bytes);

    std::ostringstream output, errors;
    EXPECT_EQ(ActiveRegionsDumpCommand(source.Path(), output, errors), 0);
    EXPECT_TRUE(errors.str().empty());
    const auto document = json::parse(output.str());
    EXPECT_EQ(document.at("schema"), "luminumbra.active_regions_dump.v1");
    EXPECT_EQ(document.at("source_version"), 3);
    EXPECT_EQ(document.at("source_bytes"), bytes.size());
    EXPECT_EQ(document.at("tick"), 3);
    EXPECT_EQ(document.at("header_tick"), 7);
    EXPECT_EQ(document.at("local_anchor"), json::array({1.25, -2.5, 3.75}));
    EXPECT_EQ(document.at("config"),
              (json{{"work_limit", 0}, {"hold_ticks", 1}, {"reduced_shift", 1}}));
    EXPECT_EQ(document.at("next_persistent_id"), ActiveRegionLedger::kPersistentIdLimit);
    const auto& records = document.at("records");
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[0].at("x"), -2);
    EXPECT_EQ(records[0].at("z"), -9);
    EXPECT_EQ(records[1].at("x"), -2);
    EXPECT_EQ(records[1].at("z"), 9);
    EXPECT_EQ(records[0].at("visited"), true);
    EXPECT_EQ(records[0].at("pinned"), true);
    EXPECT_EQ(records[0].at("populated"), true);
    EXPECT_EQ(records[0].at("wake_pending"), true);
    EXPECT_EQ(records[0].at("state"), 0);
    EXPECT_EQ(records[0].at("cadence_shift"), 0);
    EXPECT_EQ(records[0].at("first_active"), 3);
    EXPECT_EQ(records[0].at("last_proximity"), 3);
    EXPECT_EQ(records[1].at("last_edit"), 3);
    EXPECT_EQ(records[0].at("under_hold"), 1);
    EXPECT_EQ(records[1].at("over_hold"), 1);
    EXPECT_EQ(records[0].at("energy_age"), 0);
    EXPECT_TRUE(records[0].at("water_debt").is_null());
    EXPECT_TRUE(records[0].at("completed_work").is_null());
    EXPECT_TRUE(records[1].at("energy_age").is_null());
    const json expected_frozen = {{"x", 10},
                                  {"z", -3},
                                  {"visited", false},
                                  {"edited", true},
                                  {"pinned", false},
                                  {"populated", false},
                                  {"wake_pending", false},
                                  {"state", 2},
                                  {"cadence_shift", 1},
                                  {"cadence_phase", ActiveRegionLedger::phase(kFrozen, 1)},
                                  {"first_active", 0},
                                  {"last_proximity", 0},
                                  {"last_edit", 0},
                                  {"last_ticked", 1},
                                  {"frozen_at", 3},
                                  {"last_save", 7},
                                  {"last_decision", 3},
                                  {"pressure", 1},
                                  {"over_hold", 0},
                                  {"under_hold", 0},
                                  {"water_cursor", std::numeric_limits<std::uint64_t>::max()},
                                  {"frozen_digest", kFrozenDigest},
                                  {"energy_age", WorldClock::kTickLimit - 1},
                                  {"water_debt", ActiveRegionLedger::kMaxWaterDebt},
                                  {"completed_work", std::numeric_limits<std::uint64_t>::max()}};
    EXPECT_EQ(records[2], expected_frozen);
    // Verify the exact source extension envelope, including projection 2, survives.
    const auto extension_begin =
        ActiveRegionLedger::kHeaderBytes + 3 * ActiveRegionLedger::kRecordBytes;
    const auto hex = document.at("extension_envelope_hex").get<std::string>();
    ASSERT_EQ(hex.size(), (bytes.size() - extension_begin - 8) * 2);
    for (std::size_t index = 0; index < hex.size() / 2; ++index) {
        const auto value = std::stoul(hex.substr(index * 2, 2), nullptr, 16);
        EXPECT_EQ(value, static_cast<unsigned char>(bytes[extension_begin + index]));
    }
    EXPECT_EQ(DumpActiveRegions(source.Path()).document, output.str());
}

TEST(ActiveRegionDump, ExistingVersionsKeepHistoricalFrozenDigests) {
    Luminumbra::TestSupport::ScopedTempFile source("ledger-dump-", ".arl");
    auto ledger = FrozenFixture();
    for (const unsigned version : {1u, 2u, 3u}) {
        SCOPED_TRACE(version);
        if (version == 3) {
            ASSERT_TRUE(ledger.set_energy_age(kFrozen, 0));
        }
        auto bytes = ledger.encode();
        if (version == 1) {
            bytes.resize(ActiveRegionLedger::kHeaderBytes + ActiveRegionLedger::kRecordBytes + 8);
            bytes[4] = 1;
            Seal(bytes);
        }
        Write(source.Path(), bytes);
        const auto result = DumpActiveRegions(source.Path());
        ASSERT_EQ(result.status, ActiveRegionDumpStatus::Ok) << result.error;
        const auto document = json::parse(result.document);
        EXPECT_EQ(document.at("source_version"), version);
        EXPECT_EQ(document.at("records")[0].at("frozen_digest"), kFrozenDigest);
        EXPECT_EQ(Read(source.Path()), bytes);
        if (version == 1) {
            EXPECT_EQ(document.at("extension_envelope_hex"), "");
            ActiveRegionLedger legacy;
            std::string error;
            ASSERT_TRUE(ActiveRegionLedger::decode(bytes, legacy, error)) << error;
            ASSERT_TRUE(legacy.reserve_persistent_ids(2));
            Write(source.Path(), legacy.encode());
            const auto extended_legacy = DumpActiveRegions(source.Path());
            ASSERT_EQ(extended_legacy.status, ActiveRegionDumpStatus::Ok) << extended_legacy.error;
            const auto retained = json::parse(extended_legacy.document);
            EXPECT_EQ(retained.at("source_version"), 3);
            EXPECT_EQ(retained.at("records")[0].at("frozen_digest"), kFrozenDigest);
            // Allocator TLV only; legacy frozen history has no projection declaration.
            EXPECT_EQ(retained.at("extension_envelope_hex"),
                      "0e0000000100080000000200000000000000");
        }
        if (version == 3) {
            // Historical extended v2 is also accepted by the production codec.
            bytes[4] = 2;
            Seal(bytes);
            Write(source.Path(), bytes);
            const auto historical = DumpActiveRegions(source.Path());
            ASSERT_EQ(historical.status, ActiveRegionDumpStatus::Ok) << historical.error;
            const auto previous = json::parse(historical.document);
            EXPECT_EQ(previous.at("source_version"), 2);
            EXPECT_EQ(previous.at("records"), document.at("records"));
        }
    }
    ledger.schedule(WorldClock(4));
    Write(source.Path(), ledger.encode());
    const auto reduced = DumpActiveRegions(source.Path());
    ASSERT_EQ(reduced.status, ActiveRegionDumpStatus::Ok) << reduced.error;
    EXPECT_EQ(json::parse(reduced.document).at("records")[0].at("state"), 1);
}

TEST(ActiveRegionDump, AbsentCorruptAndFutureInputsRemainDistinct) {
    Luminumbra::TestSupport::ScopedTempFile source("ledger-dump-", ".arl");
    const auto expect_refusal = [&](ActiveRegionDumpStatus status) {
        const auto result = DumpActiveRegions(source.Path());
        EXPECT_EQ(result.status, status) << result.error;
        EXPECT_TRUE(result.document.empty());
        EXPECT_FALSE(result.error.empty());
        std::ostringstream output, errors;
        EXPECT_EQ(ActiveRegionsDumpCommand(source.Path(), output, errors),
                  static_cast<int>(status));
        EXPECT_TRUE(output.str().empty());
        EXPECT_FALSE(errors.str().empty());
    };
    expect_refusal(ActiveRegionDumpStatus::Absent);
    EXPECT_EQ(DumpActiveRegions(source.Path().parent_path() / "missing" / "ledger.arl").status,
              ActiveRegionDumpStatus::Absent);
    auto bytes = FrozenFixture().encode();
    const std::vector<std::string> invalid = {
        {}, "ARL1", "other bytes", bytes.substr(0, bytes.size() - 1), bytes + "trailing"};
    for (const auto& candidate : invalid) {
        Write(source.Path(), candidate);
        expect_refusal(ActiveRegionDumpStatus::Corrupt);
    }
    auto bad_checksum = bytes;
    bad_checksum.back() ^= 1;
    Write(source.Path(), bad_checksum);
    expect_refusal(ActiveRegionDumpStatus::Corrupt);
    // Even a checksum-valid envelope cannot export an incomplete record set.
    auto bad_record = bytes;
    bad_record[ActiveRegionLedger::kHeaderBytes + 9] = 3;
    Seal(bad_record);
    Write(source.Path(), bad_record);
    expect_refusal(ActiveRegionDumpStatus::Corrupt);
    auto multiple = FrozenFixture();
    multiple.pin({-2, -9}, true, WorldClock(3));
    auto invalid_tail = multiple.encode();
    invalid_tail[ActiveRegionLedger::kHeaderBytes + ActiveRegionLedger::kRecordBytes + 9] = 3;
    Seal(invalid_tail);
    Write(source.Path(), invalid_tail);
    expect_refusal(ActiveRegionDumpStatus::Corrupt);
    auto unknown_projection = bytes;
    unknown_projection[unknown_projection.size() - 9] = 3;
    Seal(unknown_projection);
    Write(source.Path(), unknown_projection);
    expect_refusal(ActiveRegionDumpStatus::Corrupt);
    EXPECT_EQ(DumpActiveRegions(source.Path()).error, ActiveRegionLedger::kProjectionMessage);
    bytes[4] = 4;
    Write(source.Path(), bytes);
    expect_refusal(ActiveRegionDumpStatus::FutureVersion);
    Write(source.Path(), std::string_view(bytes).substr(0, 6));
    expect_refusal(ActiveRegionDumpStatus::FutureVersion);
    // Bad magic remains corrupt even if the version bytes look futuristic.
    bytes[0] = 'X';
    Write(source.Path(), bytes);
    expect_refusal(ActiveRegionDumpStatus::Corrupt);
    EXPECT_EQ(DumpActiveRegions(source.Path() / "child.arl").status,
              ActiveRegionDumpStatus::IoError);
    EXPECT_EQ(DumpActiveRegions({}).status, ActiveRegionDumpStatus::IoError);
    EXPECT_EQ(DumpActiveRegions(source.Path().parent_path() / std::string(32768, 'x')).status,
              ActiveRegionDumpStatus::IoError);
#ifdef _WIN32
    EXPECT_EQ(DumpActiveRegions(fs::path(L"NUL")).status, ActiveRegionDumpStatus::IoError);
#endif
    EXPECT_TRUE(fs::remove(source.Path()));
    EXPECT_TRUE(fs::create_directory(source.Path()));
    expect_refusal(ActiveRegionDumpStatus::IoError);
}

TEST(ActiveRegionDump, ReadingLeavesSourceTreeByteIdentical) {
    Luminumbra::TestSupport::ScopedTempFile source("ledger-dump-", ".arl");
    const auto root = source.Path().parent_path();
    const auto region = root / "chunks" / "region";
    ASSERT_TRUE(fs::create_directories(region));
    const auto ledger_path = region / "active-regions.arl";
    Write(ledger_path, FrozenFixture().encode());
    Write(root / "metadata.json", "{\"sentinel\": true}\r\n");
    Write(region / "unrelated.bin", std::string_view("\0\xff\n\r", 4));
    const auto before = TreeBytes(root);
    const auto stamp = fs::last_write_time(ledger_path);
    const auto permissions = fs::status(ledger_path).permissions();
    ASSERT_EQ(before.size(), 3u);
    for (int repeat = 0; repeat < 2; ++repeat) {
        ASSERT_EQ(DumpActiveRegions(ledger_path).status, ActiveRegionDumpStatus::Ok);
        EXPECT_EQ(DumpActiveRegions(root / "missing.arl").status, ActiveRegionDumpStatus::Absent);
        EXPECT_EQ(TreeBytes(root), before);
        EXPECT_EQ(fs::last_write_time(ledger_path), stamp);
        EXPECT_EQ(fs::status(ledger_path).permissions(), permissions);
    }
    // Refusal must also leave corrupt/future source bytes and neighbours intact.
    for (const auto& invalid : {std::string("ARL1"), std::string("ARL1\x04\x00", 6)}) {
        Write(ledger_path, invalid);
        const auto invalid_tree = TreeBytes(root);
        EXPECT_TRUE(DumpActiveRegions(ledger_path).document.empty());
        EXPECT_EQ(TreeBytes(root), invalid_tree);
    }
}

TEST(ActiveRegionDump, EmptyLedgerAndOutputFailure) {
    Luminumbra::TestSupport::ScopedTempFile source("ledger-dump-", ".arl");
    Write(source.Path(), ActiveRegionLedger{}.encode());
    const auto result = DumpActiveRegions(source.Path());
    ASSERT_EQ(result.status, ActiveRegionDumpStatus::Ok) << result.error;
    const auto document = json::parse(result.document);
    EXPECT_EQ(document.at("records"), json::array());
    EXPECT_EQ(document.at("next_persistent_id"), 1);
    EXPECT_EQ(document.at("config").at("work_limit"), std::numeric_limits<std::uint64_t>::max());
    EXPECT_TRUE(document.at("local_anchor").is_null());
    const auto unicode_path = source.Path().parent_path() / fs::path(u8"ledger-\u96ea.arl");
    Write(unicode_path, ActiveRegionLedger{}.encode());
    EXPECT_EQ(DumpActiveRegions(unicode_path).document, result.document);
    ActiveRegionLedger allocator_only;
    ASSERT_TRUE(allocator_only.reserve_persistent_ids(2));
    Write(unicode_path, allocator_only.encode());
    const auto allocator = DumpActiveRegions(unicode_path);
    ASSERT_EQ(allocator.status, ActiveRegionDumpStatus::Ok) << allocator.error;
    const auto allocated = json::parse(allocator.document);
    EXPECT_EQ(allocated.at("source_version"), 3);
    EXPECT_EQ(allocated.at("records"), json::array());
    EXPECT_EQ(allocated.at("next_persistent_id"), 2);
    std::ostringstream output, errors;
    output.setstate(std::ios::badbit);
    EXPECT_EQ(ActiveRegionsDumpCommand(source.Path(), output, errors),
              static_cast<int>(ActiveRegionDumpStatus::IoError));
    EXPECT_FALSE(errors.str().empty());
}

TEST(ActiveRegionDump, OversizedInputsPreserveFutureStatus) {
    Luminumbra::TestSupport::ScopedTempFile source("ledger-dump-", ".arl");
    Write(source.Path(), std::string_view("ARL1\x04\x00", 6));
    fs::resize_file(source.Path(), ActiveRegionLedger::kMaxFileBytes + 1);
    const auto future = DumpActiveRegions(source.Path());
    EXPECT_EQ(future.status, ActiveRegionDumpStatus::FutureVersion);
    EXPECT_TRUE(future.document.empty());
    std::fstream overwrite(source.Path(), std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(overwrite.is_open());
    overwrite.put('X');
    overwrite.close();
    ASSERT_TRUE(overwrite);
    const auto result = DumpActiveRegions(source.Path());
    EXPECT_EQ(result.status, ActiveRegionDumpStatus::Corrupt);
    EXPECT_TRUE(result.document.empty());
    EXPECT_EQ(fs::file_size(source.Path()), ActiveRegionLedger::kMaxFileBytes + 1);
}

#ifndef _WIN32
TEST(ActiveRegionDump, LinksAndSpecialFilesAreIoRefusals) {
    Luminumbra::TestSupport::ScopedTempFile source("ledger-dump-", ".arl");
    const auto target = source.Path().parent_path() / "target.arl";
    Write(target, ActiveRegionLedger{}.encode());
    fs::create_symlink(target, source.Path());
    EXPECT_EQ(DumpActiveRegions(source.Path()).status, ActiveRegionDumpStatus::IoError);
    ASSERT_TRUE(fs::remove(target));
    EXPECT_EQ(DumpActiveRegions(source.Path()).status, ActiveRegionDumpStatus::IoError);
    ASSERT_TRUE(fs::remove(source.Path()));
    ASSERT_EQ(::mkfifo(source.Path().c_str(), 0600), 0);
    EXPECT_EQ(DumpActiveRegions(source.Path()).status, ActiveRegionDumpStatus::IoError);
    EXPECT_EQ(DumpActiveRegions("/dev/null").status, ActiveRegionDumpStatus::IoError);
    const auto parent_link = source.Path().parent_path() / "parent-link";
    fs::create_directory_symlink(source.Path().parent_path(), parent_link);
    EXPECT_EQ(DumpActiveRegions(parent_link / "missing.arl").status,
              ActiveRegionDumpStatus::IoError);
}
#endif
} // namespace
