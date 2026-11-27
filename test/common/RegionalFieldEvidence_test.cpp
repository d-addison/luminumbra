#include "../support/RegionalFieldEvidence.h"

#include <gtest/gtest.h>

TEST(RegionalFieldEvidence, ShortRouteRecordsActivationFreezeAndReopen) {
    Luminumbra::TestSupport::RegionalFieldEvidence driver(true);
    EXPECT_THROW(driver.Capture(0, 512), std::invalid_argument);
    EXPECT_THROW(driver.Capture(20001, 512), std::invalid_argument);
    EXPECT_THROW(driver.Capture(2048, 127), std::invalid_argument);
    EXPECT_THROW(driver.Capture(2048, 513), std::invalid_argument);
    const auto packet = driver.Capture(2048, 512);
    EXPECT_EQ(packet.at("profile"), "diagnostic-pressure");
    EXPECT_EQ(packet.at("frames").size(), 20u);
    std::size_t activations = 0, frozen = 0, completed = 0;
    for (const auto& frame : packet.at("frames")) {
        EXPECT_FALSE(frame.at("schedule_digest").get<std::string>().empty());
        EXPECT_FALSE(frame.at("masks").empty());
        for (const auto& owner : frame.at("owners")) {
            activations += owner.at("activated").get<bool>() ? 1u : 0u;
            frozen += owner.at("state") == 2 ? 1u : 0u;
            completed += owner.at("completed_work").get<std::size_t>();
        }
    }
    EXPECT_GT(activations, 9u);
    EXPECT_GT(frozen, 0u);
    EXPECT_GT(completed, 0u);
    EXPECT_EQ(packet.at("reopen").at("exact_equal"), true);
    EXPECT_EQ(packet.at("reopen").at("continuation_ticks"), 2);
    EXPECT_EQ(packet.at("reopen").at("ledger_digest"),
              packet.at("frames").back().at("ledger_digest"));
    EXPECT_EQ(packet.at("reopen").at("fields_digest"),
              packet.at("frames").back().at("fields_digest"));
}
