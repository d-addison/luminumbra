#include <gtest/gtest.h>

#include "luminumbra_common/simulation/SimBudgetProfile.h"
#include "luminumbra_common/world/ActiveRegionLedger.h"

#include <algorithm>
#include <array>
#include <optional>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace luminumbra::simulation;
using namespace Luminumbra::world;

constexpr std::string_view kSchema = R"("schema":"luminumbra.sim_budget_profile.v1")";

std::string Profile(std::string_view body) {
    return "{" + std::string(kSchema) + (body.empty() ? "" : ",") + std::string(body) + "}";
}

void ExpectRefused(std::string_view text, ProfileError expected, std::string_view name) {
    const auto result = ParseSimBudgetProfile(text);
    EXPECT_EQ(result.error, expected) << result.detail << " for " << text;
    EXPECT_EQ(ProfileErrorName(result.error), name);
    EXPECT_FALSE(result.detail.empty());
}

// Copied from ActiveRegionLedger_test.cpp so the scheduler reduction runs identically.
const RegionKey kFar{10, -3};

ActiveRegionLedger Distant(RegionSchedulerConfig config = {10, 2, 2}) {
    ActiveRegionLedger ledger(config);
    ledger.mark_edited(kFar, WorldClock{});
    ledger.schedule(WorldClock(1)); // consume edit wake
    return ledger;
}
RegionSchedule Step(ActiveRegionLedger& ledger, std::uint64_t tick, std::uint64_t units) {
    const std::array work{RegionWork{kFar, units, 0, 0, 0}};
    return ledger.schedule(
        WorldClock(tick), {}, work, [](RegionKey) { return 0x123456789abcdef0ull; });
}

TEST(SimBudgetProfileTest, FullProfileRoundTripsIntoStruct) {
    const auto result = ParseSimBudgetProfile(
        Profile(R"("region_scheduler":{"work_limit":10,"hold_ticks":2,"reduced_shift":2},)"
                R"("stages":{"instinct":5,"wind.gust-1":7},"residency":{"terrain_cache":1024})"));
    ASSERT_EQ(result.error, ProfileError::None) << result.detail;
    EXPECT_EQ(result.profile.region_scheduler,
              std::optional<RegionSchedulerConfig>(RegionSchedulerConfig{10, 2, 2}));
    EXPECT_EQ(result.profile.stage_max_work.size(), 2u);
    EXPECT_EQ(result.profile.stage_max_work.at("instinct"), 5u);
    EXPECT_EQ(result.profile.stage_max_work.at("wind.gust-1"), 7u);
    EXPECT_EQ(result.profile.residency_max_bytes.size(), 1u);
    EXPECT_EQ(result.profile.residency_max_bytes.at("terrain_cache"), 1024u);
}

TEST(SimBudgetProfileTest, SchemaOnlyProfileParsesEmpty) {
    const auto result = ParseSimBudgetProfile(Profile(""));
    ASSERT_EQ(result.error, ProfileError::None) << result.detail;
    EXPECT_FALSE(result.profile.region_scheduler.has_value());
    EXPECT_TRUE(result.profile.stage_max_work.empty());
    EXPECT_TRUE(result.profile.residency_max_bytes.empty());
}

TEST(SimBudgetProfileTest, OmittedWorkLimitIsUnlimitedAndAbsentSchedulerIsDefault) {
    const auto partial = ParseSimBudgetProfile(Profile(R"("region_scheduler":{"hold_ticks":4})"));
    ASSERT_EQ(partial.error, ProfileError::None) << partial.detail;
    const auto config = ToRegionSchedulerConfig(partial.profile);
    EXPECT_EQ(config.work_limit, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(config.hold_ticks, 4u);
    EXPECT_EQ(config.reduced_shift, RegionSchedulerConfig{}.reduced_shift);

    const auto absent = ParseSimBudgetProfile(Profile(""));
    EXPECT_EQ(ToRegionSchedulerConfig(absent.profile), RegionSchedulerConfig{});
}

TEST(SimBudgetProfileTest, RefusesMalformedJson) {
    ExpectRefused("", ProfileError::Malformed, "malformed");
    ExpectRefused("{", ProfileError::Malformed, "malformed");
    ExpectRefused("{\"schema\":", ProfileError::Malformed, "malformed");
}

TEST(SimBudgetProfileTest, RefusesNonObjectTopLevel) {
    ExpectRefused("7", ProfileError::Malformed, "malformed");
    ExpectRefused("\"luminumbra.sim_budget_profile.v1\"", ProfileError::Malformed, "malformed");
}

TEST(SimBudgetProfileTest, RefusesMissingOrWrongSchema) {
    ExpectRefused("{}", ProfileError::Malformed, "malformed");
    ExpectRefused(R"({"stages":{}})", ProfileError::Malformed, "malformed");
    ExpectRefused(R"({"schema":"other.profile.v1"})", ProfileError::Malformed, "malformed");
    ExpectRefused(R"({"schema":3})", ProfileError::Malformed, "malformed");
}

TEST(SimBudgetProfileTest, RefusesFutureVersion) {
    ExpectRefused(R"({"schema":"luminumbra.sim_budget_profile.v2"})",
                  ProfileError::FutureVersion,
                  "future_version");
}

TEST(SimBudgetProfileTest, RefusesUnknownKeys) {
    ExpectRefused(Profile(R"("extra":1)"), ProfileError::UnknownKey, "unknown_key");
    ExpectRefused(
        Profile(R"("region_scheduler":{"speed":1})"), ProfileError::UnknownKey, "unknown_key");
}

TEST(SimBudgetProfileTest, RefusesDuplicateKeys) {
    ExpectRefused(R"({"schema":"luminumbra.sim_budget_profile.v1",)"
                  R"("schema":"luminumbra.sim_budget_profile.v1"})",
                  ProfileError::Duplicate,
                  "duplicate");
    ExpectRefused(Profile(R"("stages":{"a":1,"a":2})"), ProfileError::Duplicate, "duplicate");
}

TEST(SimBudgetProfileTest, RefusesFloats) {
    ExpectRefused(Profile(R"("stages":{"a":1.5})"), ProfileError::Malformed, "malformed");
    ExpectRefused(Profile(R"("stages":{"a":1e3})"), ProfileError::Malformed, "malformed");
}

TEST(SimBudgetProfileTest, RefusesNegativeIntegers) {
    ExpectRefused(Profile(R"("stages":{"a":-1})"), ProfileError::OutOfRange, "out_of_range");
}

TEST(SimBudgetProfileTest, RefusesIntegerAboveUint64) {
    const auto result = ParseSimBudgetProfile(Profile(R"("stages":{"a":18446744073709551616})"));
    EXPECT_NE(result.error, ProfileError::None);
}

TEST(SimBudgetProfileTest, RefusesArrays) {
    ExpectRefused(Profile(R"("stages":{"a":[1]})"), ProfileError::Malformed, "malformed");
}

TEST(SimBudgetProfileTest, RefusesOutOfRangeSchedulerValues) {
    ExpectRefused(Profile(R"("region_scheduler":{"hold_ticks":0})"),
                  ProfileError::OutOfRange,
                  "out_of_range");
    ExpectRefused(Profile(R"("region_scheduler":{"reduced_shift":0})"),
                  ProfileError::OutOfRange,
                  "out_of_range");
    ExpectRefused(Profile(R"("region_scheduler":{"reduced_shift":17})"),
                  ProfileError::OutOfRange,
                  "out_of_range");
}

TEST(SimBudgetProfileTest, RefusesBadNames) {
    ExpectRefused(Profile(R"("stages":{"Upper":1})"), ProfileError::OutOfRange, "out_of_range");
    ExpectRefused(Profile(R"("stages":{"":1})"), ProfileError::OutOfRange, "out_of_range");
    const std::string longName(65, 'a');
    ExpectRefused(Profile(R"("residency":{")" + longName + R"(":1})"),
                  ProfileError::OutOfRange,
                  "out_of_range");
}

TEST(SimBudgetProfileTest, RefusesTooManyStages) {
    std::string body = R"("stages":{)";
    for (int i = 0; i < 65; ++i) {
        if (i) {
            body += ",";
        }
        body += "\"s" + std::to_string(i) + "\":1";
    }
    body += "}";
    ExpectRefused(Profile(body), ProfileError::TooLarge, "too_large");
}

TEST(SimBudgetProfileTest, RefusesTooManyResidencyClasses) {
    std::string body = R"("residency":{)";
    for (int i = 0; i < 33; ++i) {
        if (i) {
            body += ",";
        }
        body += "\"c" + std::to_string(i) + "\":1";
    }
    body += "}";
    ExpectRefused(Profile(body), ProfileError::TooLarge, "too_large");
}

TEST(SimBudgetProfileTest, RefusesOversizeText) {
    const std::string text(kMaxProfileBytes + 1, ' ');
    ExpectRefused(text, ProfileError::TooLarge, "too_large");
}

TEST(SimBudgetProfileTest, GarbageNeverThrows) {
    const std::vector<std::string> garbage = {
        "", "{", "\xff\xfe", "[[[[[[[[[[[[", std::string(100000, '{')};
    for (const auto& text : garbage) {
        EXPECT_NO_THROW({
            const auto result = ParseSimBudgetProfile(text);
            EXPECT_NE(result.error, ProfileError::None);
        });
    }
}

TEST(SimBudgetProfileTest, ViolationsSortedByKindNameTickEvenWhenShuffled) {
    SimBudgetProfile profile;
    profile.stage_max_work = {{"b", 5}, {"a", 5}};
    profile.residency_max_bytes = {{"z", 10}, {"c", 10}};
    std::vector<StageTick> ticks = {{"b", 3, 9}, {"a", 2, 9}, {"a", 1, 9}};
    std::vector<ResidencySample> samples = {{"z", 20}, {"c", 11}};
    std::reverse(ticks.begin(), ticks.end());
    std::reverse(samples.begin(), samples.end());

    const auto verdict = EvaluateSimBudget(profile, ticks, samples);
    ASSERT_EQ(verdict.violations.size(), 5u);
    const auto& v = verdict.violations;
    EXPECT_EQ(v[0].kind, BudgetViolation::Kind::Stage);
    EXPECT_EQ(v[0].name, "a");
    EXPECT_EQ(v[0].tick, 1u);
    EXPECT_EQ(v[1].name, "a");
    EXPECT_EQ(v[1].tick, 2u);
    EXPECT_EQ(v[2].name, "b");
    EXPECT_EQ(v[2].tick, 3u);
    EXPECT_EQ(v[3].kind, BudgetViolation::Kind::Residency);
    EXPECT_EQ(v[3].name, "c");
    EXPECT_EQ(v[3].tick, 0u);
    EXPECT_EQ(v[4].name, "z");
    EXPECT_EQ(v[4].observed, 20u);
    EXPECT_EQ(v[4].limit, 10u);
    EXPECT_FALSE(verdict.no_violation);
}

TEST(SimBudgetProfileTest, CapEqualToObservedIsNotViolation) {
    SimBudgetProfile profile;
    profile.stage_max_work = {{"a", 5}};
    profile.residency_max_bytes = {{"c", 10}};
    const std::vector<StageTick> ticks = {{"a", 1, 5}};
    const std::vector<ResidencySample> samples = {{"c", 10}};
    const auto verdict = EvaluateSimBudget(profile, ticks, samples);
    EXPECT_TRUE(verdict.violations.empty());
    EXPECT_TRUE(verdict.no_violation);
    EXPECT_TRUE(verdict.coverage_complete);
    EXPECT_TRUE(verdict.unevaluated.empty());
}

TEST(SimBudgetProfileTest, MissingStageAndClassAreUnevaluatedWithoutViolation) {
    SimBudgetProfile profile;
    profile.stage_max_work = {{"a", 5}, {"b", 5}};
    profile.residency_max_bytes = {{"c", 1}};
    const std::vector<StageTick> ticks = {{"a", 1, 1}};
    const auto verdict = EvaluateSimBudget(profile, ticks, {});
    EXPECT_TRUE(verdict.no_violation);
    EXPECT_FALSE(verdict.coverage_complete);
    EXPECT_EQ(verdict.unevaluated, (std::vector<std::string>{"residency:c", "stage:b"}));
}

TEST(SimBudgetProfileTest, UnknownObservationNamesAreIgnored) {
    SimBudgetProfile profile;
    profile.stage_max_work = {{"a", 5}};
    const std::vector<StageTick> ticks = {{"zzz", 1, 999}, {"a", 2, 1}};
    const std::vector<ResidencySample> samples = {{"nope", 999}};
    const auto verdict = EvaluateSimBudget(profile, ticks, samples);
    EXPECT_TRUE(verdict.violations.empty());
    EXPECT_TRUE(verdict.no_violation);
    EXPECT_TRUE(verdict.coverage_complete);
}

TEST(SimBudgetProfileTest, EveryResidencySampleOfAClassIsChecked) {
    SimBudgetProfile profile;
    profile.residency_max_bytes = {{"c", 10}};
    const std::vector<ResidencySample> samples = {{"c", 11}, {"c", 12}};
    const auto verdict = EvaluateSimBudget(profile, {}, samples);
    EXPECT_EQ(verdict.violations.size(), 2u);
    EXPECT_FALSE(verdict.no_violation);
}

TEST(SimBudgetProfileTest, SchedulerReductionFollowsConfiguredProfile) {
    const auto parsed = ParseSimBudgetProfile(
        Profile(R"("region_scheduler":{"work_limit":10,"hold_ticks":2,"reduced_shift":2})"));
    ASSERT_EQ(parsed.error, ProfileError::None) << parsed.detail;
    auto ledger = Distant(ToRegionSchedulerConfig(parsed.profile));
    for (std::uint64_t tick = 2; tick <= 5; ++tick) {
        SCOPED_TRACE(tick);
        Step(ledger, tick, tick <= 6 ? 11u : 10u);
        if (tick == 3) {
            EXPECT_EQ(ledger.records().at(kFar).state, RegionState::Reduced);
        }
        if (tick == 5) {
            EXPECT_EQ(ledger.records().at(kFar).state, RegionState::Frozen);
        }
    }
}

TEST(SimBudgetProfileTest, UnlimitedProfileNeverReducesSameTicks) {
    const auto parsed =
        ParseSimBudgetProfile(Profile(R"("region_scheduler":{"hold_ticks":2,"reduced_shift":2})"));
    ASSERT_EQ(parsed.error, ProfileError::None) << parsed.detail;
    auto ledger = Distant(ToRegionSchedulerConfig(parsed.profile));
    for (std::uint64_t tick = 2; tick <= 5; ++tick) {
        SCOPED_TRACE(tick);
        Step(ledger, tick, 11u);
        EXPECT_EQ(ledger.records().at(kFar).state, RegionState::Active);
    }
}

} // namespace
