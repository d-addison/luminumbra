#include "../support/RegionalFieldEvidence.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

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

TEST(RegionalFieldEvidence, ShortRouteRecordsActivationFreezeAndReopen) {
    Luminumbra::TestSupport::RegionalFieldEvidence driver(true);
    EXPECT_THROW(driver.Capture(0, 512), std::invalid_argument);
    EXPECT_THROW(driver.Capture(20001, 512), std::invalid_argument);
    EXPECT_THROW(driver.Capture(2048, 127), std::invalid_argument);
    EXPECT_THROW(driver.Capture(2048, 513), std::invalid_argument);
    const auto packet = driver.Capture(2048, 512);
    EXPECT_EQ(packet.at("profile"), "diagnostic-pressure");
    EXPECT_EQ(packet.at("frames").size(), 20u);
    // The 512 m disc at (256, 256) intersects nine 512 m region squares.
    EXPECT_EQ(packet.at("frames").front().at("owners").size(), 9u) << FrameSummary(packet);
    std::size_t activations = 0, frozen = 0, completed = 0;
    for (const auto& frame : packet.at("frames")) {
        std::size_t frame_completed = 0;
        EXPECT_FALSE(frame.at("schedule_digest").get<std::string>().empty());
        EXPECT_FALSE(frame.at("masks").empty());
        for (const auto& owner : frame.at("owners")) {
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
