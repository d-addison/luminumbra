#include "core/InspectorRegistry.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>

namespace {
using namespace luminumbra::core;

constexpr std::array kDefinitions{
    InspectorMetricDefinition{"duration", "Duration", "CPU completed work", "ms", 2},
    InspectorMetricDefinition{"memory", "Memory", "Device accounting", "bytes", 0}};

TEST(InspectorRegistry, PendingUnsupportedAndZeroAreDistinct) {
    InspectorRegistry registry;
    const auto handle = registry.Register("panel", "Panel", kDefinitions);
    ASSERT_TRUE(handle);
    if (!handle)
        return;
    const auto initial = registry.Read(*handle, 0);
    ASSERT_TRUE(initial);
    if (!initial)
        return;
    EXPECT_EQ(initial->metrics[0].availability, MetricAvailability::Pending);
    EXPECT_FALSE(initial->metrics[0].value);
    EXPECT_FALSE(initial->metrics[0].age_frames);
    EXPECT_STREQ(initial->metrics[0].source.data(), "CPU completed work");

    const std::array samples{InspectorMetricSample{MetricAvailability::Available, 0.0, 0},
                             InspectorMetricSample{MetricAvailability::Unsupported, {}, {}}};
    ASSERT_TRUE(registry.Publish(*handle, samples));
    const auto snapshot = registry.Read(*handle, 0);
    ASSERT_TRUE(snapshot);
    if (!snapshot)
        return;
    EXPECT_EQ(snapshot->metrics[0].availability, MetricAvailability::Available);
    EXPECT_EQ(snapshot->metrics[0].value, 0.0);
    EXPECT_EQ(snapshot->metrics[0].age_frames, 0u);
    EXPECT_EQ(snapshot->metrics[1].availability, MetricAvailability::Unsupported);
    EXPECT_FALSE(snapshot->metrics[1].value);
    EXPECT_FALSE(snapshot->metrics[1].age_frames);
}

TEST(InspectorRegistry, AgeBoundaryStalenessFutureAndUnknownAreExplicit) {
    InspectorRegistry registry;
    const auto handle = registry.Register("panel", "Panel", kDefinitions);
    ASSERT_TRUE(handle);
    if (!handle)
        return;
    const std::array samples{InspectorMetricSample{MetricAvailability::Available, 1.0, 10},
                             InspectorMetricSample{MetricAvailability::Available, 2.0, {}}};
    ASSERT_TRUE(registry.Publish(*handle, samples));
    const auto boundary = registry.Read(*handle, 12);
    ASSERT_TRUE(boundary);
    if (!boundary)
        return;
    EXPECT_EQ(boundary->metrics[0].availability, MetricAvailability::Available);
    EXPECT_EQ(boundary->metrics[0].age_frames, 2u);
    const auto stale = registry.Read(*handle, 13);
    ASSERT_TRUE(stale);
    if (!stale)
        return;
    EXPECT_EQ(stale->metrics[0].availability, MetricAvailability::Stale);
    EXPECT_EQ(stale->metrics[0].age_frames, 3u);
    EXPECT_FALSE(stale->metrics[0].value);
    EXPECT_EQ(stale->metrics[1].availability, MetricAvailability::Available);
    EXPECT_FALSE(stale->metrics[1].age_frames);
    const auto future = registry.Read(*handle, 9);
    ASSERT_TRUE(future);
    if (!future)
        return;
    EXPECT_EQ(future->metrics[0].availability, MetricAvailability::Unavailable);
    EXPECT_FALSE(future->metrics[0].value);
    EXPECT_FALSE(future->metrics[0].age_frames);
    const auto maximum = registry.Read(*handle, std::numeric_limits<std::uint64_t>::max());
    ASSERT_TRUE(maximum);
    if (!maximum)
        return;
    EXPECT_EQ(maximum->metrics[0].age_frames, std::numeric_limits<std::uint64_t>::max() - 10);
    // Queries derive availability without modifying the stored observation.
    const auto again = registry.Read(*handle, 10);
    ASSERT_TRUE(again);
    if (!again)
        return;
    EXPECT_EQ(again->metrics[0].value, 1.0);
}

TEST(InspectorRegistry, RejectedPublicationLeavesEveryMetricUnchanged) {
    InspectorRegistry registry;
    const auto handle = registry.Register("panel", "Panel", kDefinitions);
    ASSERT_TRUE(handle);
    if (!handle)
        return;
    std::array samples{InspectorMetricSample{MetricAvailability::Available, 1.0, 10},
                       InspectorMetricSample{MetricAvailability::Available, 2.0, 10}};
    ASSERT_TRUE(registry.Publish(*handle, samples));
    EXPECT_FALSE(registry.Publish(*handle, {}));
    EXPECT_FALSE(registry.Publish(*handle, std::span(samples).first(1)));
    const std::array<InspectorMetricSample, 3> oversized{};
    EXPECT_FALSE(registry.Publish(*handle, oversized));
    for (const auto invalid :
         {InspectorMetricSample{MetricAvailability::Available, {}, 10},
          InspectorMetricSample{
              MetricAvailability::Available, std::numeric_limits<double>::infinity(), 10},
          InspectorMetricSample{
              MetricAvailability::Available, std::numeric_limits<double>::quiet_NaN(), 10},
          InspectorMetricSample{MetricAvailability::Pending, 0.0, {}},
          InspectorMetricSample{MetricAvailability::Unsupported, 0.0, {}},
          InspectorMetricSample{MetricAvailability::Unavailable, 0.0, {}},
          InspectorMetricSample{MetricAvailability::Stale, {}, {}},
          InspectorMetricSample{static_cast<MetricAvailability>(99), {}, {}}}) {
        samples[0].value = 99.0;
        samples[1] = invalid;
        EXPECT_FALSE(registry.Publish(*handle, samples));
        const auto snapshot = registry.Read(*handle, 10);
        ASSERT_TRUE(snapshot);
        if (!snapshot)
            return;
        EXPECT_EQ(snapshot->metrics[0].value, 1.0);
        EXPECT_EQ(snapshot->metrics[1].value, 2.0);
    }
}

TEST(InspectorRegistry, UnavailableTransitionsNeverReuseAnEarlierValue) {
    InspectorRegistry registry;
    const auto handle = registry.Register("panel", "Panel", kDefinitions);
    ASSERT_TRUE(handle);
    if (!handle)
        return;
    for (const auto status : {MetricAvailability::Pending,
                              MetricAvailability::Unsupported,
                              MetricAvailability::Unavailable}) {
        std::array samples{InspectorMetricSample{MetricAvailability::Available, 5.0, 1},
                           InspectorMetricSample{MetricAvailability::Available, 6.0, 1}};
        ASSERT_TRUE(registry.Publish(*handle, samples));
        samples[0] = {status, {}, 2};
        ASSERT_TRUE(registry.Publish(*handle, samples));
        const auto snapshot = registry.Read(*handle, 3);
        ASSERT_TRUE(snapshot);
        if (!snapshot)
            return;
        EXPECT_EQ(snapshot->metrics[0].availability, status);
        EXPECT_EQ(snapshot->metrics[0].age_frames, 1u);
        EXPECT_FALSE(snapshot->metrics[0].value);
    }
}

TEST(InspectorRegistry, MetadataIsOwnedAndRegistrationRefusalsAreAtomic) {
    InspectorRegistry registry;
    std::string name = "owned";
    const std::array definitions{InspectorMetricDefinition{name, name, name, name, 0}};
    const auto handle = registry.Register(name, name, definitions);
    ASSERT_TRUE(handle);
    if (!handle)
        return;
    name.assign("edited");
    EXPECT_FALSE(registry.Find("edited"));
    EXPECT_TRUE(registry.Find("owned"));
    EXPECT_FALSE(registry.Register("owned", "Duplicate", kDefinitions));
    EXPECT_FALSE(registry.Register("", "Empty", kDefinitions));
    EXPECT_FALSE(registry.Register("panel", "", kDefinitions));
    EXPECT_FALSE(registry.Register("panel", "Panel", {}));
    const std::string long_text(kInspectorTextLimit + 1, 'x');
    const std::string embedded_null("a\0b", 3);
    for (const std::string_view invalid :
         {std::string_view{}, std::string_view(long_text), std::string_view(embedded_null)}) {
        EXPECT_FALSE(registry.Register(invalid, "Panel", kDefinitions));
        EXPECT_FALSE(registry.Register("panel", invalid, kDefinitions));
        EXPECT_FALSE(registry.Find(invalid));
        for (std::size_t field = 0; field < 4; ++field) {
            auto invalid_definitions = kDefinitions;
            auto& metric = invalid_definitions[0];
            const std::array fields{&metric.id, &metric.label, &metric.source, &metric.unit};
            *fields[field] = invalid;
            EXPECT_FALSE(registry.Register("panel", "Panel", invalid_definitions));
        }
    }
    auto duplicate_metrics = kDefinitions;
    duplicate_metrics[1].id = duplicate_metrics[0].id;
    EXPECT_FALSE(registry.Register("panel", "Panel", duplicate_metrics));
    const auto snapshot = registry.Read(*handle, 0);
    ASSERT_TRUE(snapshot);
    if (!snapshot)
        return;
    EXPECT_STREQ(snapshot->id.data(), "owned");
    EXPECT_STREQ(snapshot->metrics[0].source.data(), "owned");
    EXPECT_TRUE(registry.Register("panel", "Panel", kDefinitions));
}

TEST(InspectorRegistry, CapacityAndTextLimitsAreInclusiveAndReusable) {
    InspectorRegistry registry;
    std::array<InspectorMetricDefinition, kInspectorMetricLimit + 1> definitions{};
    std::array<std::string, kInspectorMetricLimit + 1> names{};
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        names[index] = std::to_string(index);
        definitions[index] = {names[index], "Metric", "CPU", "count", 0};
    }
    EXPECT_FALSE(registry.Register("too_many", "Panel", definitions));
    const std::string maximum_text(kInspectorTextLimit, 'x');
    definitions[0] = {maximum_text, maximum_text, maximum_text, maximum_text, 0};
    const auto handle = registry.Register(
        maximum_text, maximum_text, std::span(definitions).first(kInspectorMetricLimit));
    ASSERT_TRUE(handle);
    if (!handle)
        return;
    const auto snapshot = registry.Read(*handle, 0);
    ASSERT_TRUE(snapshot);
    if (!snapshot)
        return;
    EXPECT_EQ(snapshot->metric_count, kInspectorMetricLimit);
    EXPECT_STREQ(snapshot->metrics[0].source.data(), maximum_text.c_str());
    for (std::size_t index = 1; index < kInspectorPanelLimit; ++index) {
        EXPECT_TRUE(registry.Register(std::to_string(index), "Panel", kDefinitions));
    }
    EXPECT_EQ(registry.List().count, kInspectorPanelLimit);
    EXPECT_FALSE(registry.Register("overflow", "Panel", kDefinitions));
    ASSERT_TRUE(registry.Unregister(*handle));
    EXPECT_TRUE(registry.Register("replacement", "Panel", kDefinitions));
}

TEST(InspectorRegistry, TeardownInvalidatesHandlesAndPreservesCopiedSnapshots) {
    InspectorRegistry registry;
    InspectorRegistry other;
    const auto handle = registry.Register("panel", "Panel", kDefinitions);
    ASSERT_TRUE(handle);
    if (!handle)
        return;
    const auto snapshot = registry.Read(*handle, 0);
    ASSERT_TRUE(snapshot);
    if (!snapshot)
        return;
    EXPECT_FALSE(other.Read(*handle, 0));
    EXPECT_FALSE(other.Publish(*handle, {}));
    EXPECT_FALSE(other.Unregister(*handle));
    EXPECT_FALSE(registry.Read({}, 0));
    EXPECT_FALSE(registry.Unregister({}));
    EXPECT_FALSE(registry.Publish({}, {}));
    const InspectorPanelHandle invalid_slot{&registry, kInspectorPanelLimit, handle->generation};
    EXPECT_FALSE(registry.Read(invalid_slot, 0));
    EXPECT_FALSE(registry.Publish(invalid_slot, {}));
    EXPECT_FALSE(registry.Unregister(invalid_slot));
    const InspectorPanelHandle invalid_generation{&registry, handle->slot, handle->generation + 1};
    EXPECT_FALSE(registry.Read(invalid_generation, 0));
    ASSERT_TRUE(registry.Unregister(*handle));
    EXPECT_FALSE(registry.Read(*handle, 0));
    EXPECT_FALSE(registry.Unregister(*handle));
    const auto replacement = registry.Register("panel", "Panel", kDefinitions);
    ASSERT_TRUE(replacement);
    if (!replacement)
        return;
    EXPECT_NE(replacement->generation, handle->generation);
    EXPECT_FALSE(registry.Publish(*handle, {}));
    registry.Shutdown();
    registry.Shutdown();
    EXPECT_FALSE(registry.Find("panel"));
    EXPECT_FALSE(registry.Read(*replacement, 0));
    EXPECT_STREQ(snapshot->metrics[0].source.data(), "CPU completed work");
    const auto restarted = registry.Register("panel", "Panel", kDefinitions);
    ASSERT_TRUE(restarted);
    if (!restarted)
        return;
    EXPECT_NE(restarted->generation, replacement->generation);
}

TEST(InspectorRegistry, DiscoveryContainsOnlyLivePanelsAndNeverRevalidatesOldHandles) {
    InspectorRegistry registry;
    EXPECT_EQ(registry.List().count, 0u);
    EXPECT_FALSE(registry.Find("absent"));
    const auto first = registry.Register("first", "First", kDefinitions);
    const auto second = registry.Register("second", "Second", kDefinitions);
    ASSERT_TRUE(first);
    if (!first)
        return;
    ASSERT_TRUE(second);
    if (!second)
        return;
    const auto original = registry.List();
    ASSERT_EQ(original.count, 2u);
    const auto first_snapshot = registry.Read(original.panels[0], 0);
    const auto second_snapshot = registry.Read(original.panels[1], 0);
    ASSERT_TRUE(first_snapshot);
    if (!first_snapshot)
        return;
    ASSERT_TRUE(second_snapshot);
    if (!second_snapshot)
        return;
    EXPECT_STREQ(first_snapshot->id.data(), "first");
    EXPECT_STREQ(second_snapshot->id.data(), "second");
    ASSERT_TRUE(registry.Unregister(*first));
    EXPECT_FALSE(registry.Find("first"));
    ASSERT_TRUE(registry.Register("replacement", "Replacement", kDefinitions));
    EXPECT_FALSE(registry.Read(original.panels[0], 0));
    const auto current = registry.List();
    ASSERT_EQ(current.count, 2u);
    const auto replacement = registry.Read(current.panels[0], 0);
    ASSERT_TRUE(replacement);
    if (!replacement)
        return;
    EXPECT_STREQ(replacement->id.data(), "replacement");
    registry.Shutdown();
    EXPECT_EQ(registry.List().count, 0u);
}

TEST(InspectorRegistry, AvailabilityLabelsCoverEveryStateAndUnknownMetadata) {
    EXPECT_STREQ(MetricAvailabilityName(MetricAvailability::Available), "available");
    EXPECT_STREQ(MetricAvailabilityName(MetricAvailability::Pending), "pending");
    EXPECT_STREQ(MetricAvailabilityName(MetricAvailability::Unsupported), "unsupported");
    EXPECT_STREQ(MetricAvailabilityName(MetricAvailability::Unavailable), "unavailable");
    EXPECT_STREQ(MetricAvailabilityName(MetricAvailability::Stale), "stale");
    EXPECT_STREQ(MetricAvailabilityName(static_cast<MetricAvailability>(99)), "unavailable");
}
} // namespace
