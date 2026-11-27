#include "../support/RegionalFieldEvidence.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string FrameSummary(const nlohmann::json& packet) {
    std::ostringstream summary;
    // Capture bounds this failure-only pass to 640 frames and 256 owners per frame.
    for (const auto& frame : packet.at("frames")) {
        std::size_t active = 0, reduced = 0, frozen = 0;
        std::uint64_t completed = 0;
        for (const auto& owner : frame.at("owners")) {
            active += owner.at("state") == 0 ? 1u : 0u;
            reduced += owner.at("state") == 1 ? 1u : 0u;
            frozen += owner.at("state") == 2 ? 1u : 0u;
            completed += owner.at("completed_work").get<std::uint64_t>();
        }
        summary << "\ntick=" << frame.at("tick") << " x=" << frame.at("anchor").at(0)
                << " owners(active/reduced/frozen)=" << active << '/' << reduced << '/' << frozen
                << " completed=" << completed << " pressure=" << frame.at("schedule_work");
    }
    return summary.str();
}

} // namespace

TEST(RegionalFieldEvidence, PacketOptionsRequireOneProfileAndOutput) {
    using Luminumbra::TestSupport::ParseRegionalFieldEvidenceOptions;
    for (const auto* mode : {"--diagnostic-pressure", "--shipped-settings"}) {
        for (const bool output_first : {false, true}) {
            const std::vector<const char*> arguments =
                output_first ? std::vector<const char*>{"capture", "--out", "packet.json", mode}
                             : std::vector<const char*>{"capture", mode, "--out", "packet.json"};
            const auto options = ParseRegionalFieldEvidenceOptions(4, arguments.data());
            EXPECT_EQ(options.pressure, std::string_view(mode) == "--diagnostic-pressure");
            EXPECT_EQ(options.output, std::filesystem::path("packet.json"));
        }
    }
    const std::vector<std::vector<const char*>> invalid = {
        {"capture"},
        {"capture", "--diagnostic-pressure"},
        {"capture", "--out", "packet.json"},
        {"capture", "--diagnostic-pressure", "--out"},
        {"capture", "--diagnostic-pressure", "--out", ""},
        {"capture", "--diagnostic-pressure", "--out", "--out"},
        {"capture", "--out", "packet.json", "--out"},
        {"capture", "--diagnostic-pressure", "--shipped-settings", "--out"},
        {"capture", "--out", "--diagnostic-pressure", "packet.json"},
        {"capture", "--unknown", "--out", "packet.json"},
        {"capture", "--diagnostic-pressure", "--out", "packet.json", "--out", "other.json"},
        {"capture", "--diagnostic-pressure", "--diagnostic-pressure", "--out", "packet.json"},
        {"capture", "--diagnostic-pressure", "--out", "packet.json", "--unknown"}};
    for (const auto& arguments : invalid) {
        EXPECT_THROW(
            ParseRegionalFieldEvidenceOptions(static_cast<int>(arguments.size()), arguments.data()),
            std::invalid_argument);
    }
}

TEST(RegionalFieldEvidence, PacketWritePublishesJsonAndPreservesExistingFilesOnFailure) {
    using Luminumbra::Persistence::WorldSaveService;
    using Luminumbra::TestSupport::WriteRegionalFieldEvidence;
    const Luminumbra::TestSupport::ScopedTempFile file("field_evidence_", ".json");
    const nlohmann::json packet = {{"packet", 1}};
    WriteRegionalFieldEvidence(file.Path(), packet);
    EXPECT_EQ(nlohmann::json::parse(Luminumbra::TestSupport::ReadClockTestFile(file.Path())),
              packet);
    const auto old_bytes = Luminumbra::TestSupport::ReadClockTestFile(file.Path());
    WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    EXPECT_THROW(WriteRegionalFieldEvidence(file.Path(), {{"packet", 2}}), std::runtime_error);
    const auto absent = file.Path().parent_path() / "absent.json";
    WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    EXPECT_THROW(WriteRegionalFieldEvidence(absent, packet), std::runtime_error);
    WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_EQ(Luminumbra::TestSupport::ReadClockTestFile(file.Path()), old_bytes);
    EXPECT_FALSE(std::filesystem::exists(absent));
    const nlohmann::json invalid_packet = {{"packet", std::string(1, static_cast<char>(0xff))}};
    EXPECT_THROW(WriteRegionalFieldEvidence(file.Path(), invalid_packet),
                 nlohmann::json::type_error);
    EXPECT_EQ(Luminumbra::TestSupport::ReadClockTestFile(file.Path()), old_bytes);
    EXPECT_THROW(WriteRegionalFieldEvidence(file.Path() / "child.json", packet),
                 std::runtime_error);
    const auto occupied = file.Path().parent_path() / "directory";
    ASSERT_TRUE(std::filesystem::create_directory(occupied));
    EXPECT_THROW(WriteRegionalFieldEvidence(occupied, packet), std::runtime_error);
    EXPECT_TRUE(std::filesystem::is_directory(occupied));
    EXPECT_TRUE(std::filesystem::is_empty(occupied));
    std::size_t entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(file.Path().parent_path())) {
        EXPECT_TRUE(entry.path() == file.Path() || entry.path() == occupied);
        ++entries;
    }
    EXPECT_EQ(entries, 2u);
    WriteRegionalFieldEvidence(file.Path(), {{"packet", 2}});
    EXPECT_EQ(nlohmann::json::parse(Luminumbra::TestSupport::ReadClockTestFile(file.Path())),
              (nlohmann::json{{"packet", 2}}));
}

TEST(RegionalFieldEvidence, ShortRouteRecordsActivationFreezeAndReopen) {
    Luminumbra::TestSupport::RegionalFieldEvidence driver(true);
    EXPECT_THROW(driver.Capture(0, 512), std::invalid_argument);
    EXPECT_THROW(driver.Capture(20001, 512), std::invalid_argument);
    EXPECT_THROW(driver.Capture(2048, 127), std::invalid_argument);
    EXPECT_THROW(driver.Capture(2048, 513), std::invalid_argument);
    const auto packet = driver.Capture(2048, 512);
    const Luminumbra::TestSupport::ScopedTempFile file("field_evidence_route_", ".json");
    Luminumbra::TestSupport::WriteRegionalFieldEvidence(file.Path(), packet);
    EXPECT_EQ(nlohmann::json::parse(Luminumbra::TestSupport::ReadClockTestFile(file.Path())),
              packet);
    EXPECT_EQ(packet.at("profile"), "diagnostic-pressure");
    EXPECT_EQ(packet.at("frames").size(), 20u);
    // The 512 m disc at (256, 256) intersects nine 512 m region squares.
    EXPECT_EQ(packet.at("frames").front().at("owners").size(), 9u) << FrameSummary(packet);
    std::size_t activations = 0, frozen = 0, completed = 0;
    std::uint64_t pressure_run = 0;
    std::map<std::pair<int, int>, std::pair<unsigned, std::uint64_t>> prior_states;
    for (const auto& frame : packet.at("frames")) {
        const auto tick = frame.at("tick").get<std::uint64_t>();
        pressure_run = frame.at("schedule_work").get<std::uint64_t>() > 10 ? pressure_run + 1 : 0;
        std::size_t frame_completed = 0;
        EXPECT_FALSE(frame.at("schedule_digest").get<std::string>().empty());
        EXPECT_FALSE(frame.at("masks").empty());
        for (const auto& owner : frame.at("owners")) {
            const auto key =
                std::pair{owner.at("key").at(0).get<int>(), owner.at("key").at(1).get<int>()};
            const auto state = owner.at("state").get<unsigned>();
            auto& [prior_state, reduced_at] = prior_states[key];
            if (state == 1 && prior_state != 1) {
                reduced_at = tick;
            }
            if (state == 2 && prior_state != 2) {
                EXPECT_EQ(prior_state, 1u) << FrameSummary(packet);
                EXPECT_GE(pressure_run, 2u) << FrameSummary(packet);
                EXPECT_GE(tick - reduced_at, 2u) << FrameSummary(packet);
            }
            prior_state = state;
            activations += owner.at("activated").get<bool>() ? 1u : 0u;
            frozen += owner.at("state") == 2 ? 1u : 0u;
            frame_completed += owner.at("completed_work").get<std::size_t>();
        }
        completed += frame_completed;
        EXPECT_GT(frame_completed, 0u) << FrameSummary(packet);
        if (frame.at("tick") != 1) {
            // Prior-tick ambient cell updates alone keep every later tick over limit.
            EXPECT_GT(frame.at("schedule_work").get<std::uint64_t>(), 10u) << FrameSummary(packet);
        }
    }
    EXPECT_GT(activations, 9u) << FrameSummary(packet);
    EXPECT_GT(frozen, 0u) << FrameSummary(packet);
    EXPECT_GT(completed, 0u) << FrameSummary(packet);
    EXPECT_EQ(packet.at("reopen").at("exact_equal"), true);
    EXPECT_EQ(packet.at("reopen").at("continuation_ticks"), 2);
    EXPECT_EQ(packet.at("reopen").at("ledger_digest"),
              packet.at("frames").back().at("ledger_digest"));
    EXPECT_EQ(packet.at("reopen").at("fields_digest"),
              packet.at("frames").back().at("fields_digest"));
}
