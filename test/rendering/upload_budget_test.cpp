#include "luminumbra_client/rendering/UploadBudget.h"

#include <gtest/gtest.h>
#include <limits>
#include <vector>

namespace {
using namespace Luminumbra::Rendering;

struct Work {
    std::uint64_t id = 0;
    std::uint64_t priority = 0;
    std::size_t bytes = 0;
    double milliseconds = 0;
    int version = 0;
};

void refresh(UploadQueue<Work>& queue, const std::vector<Work>& items) {
    refresh_uploads(
        queue,
        items,
        [](const Work& item) { return item.id; },
        [](const Work& item) { return item.priority; });
}

TEST(UploadBudget, ValidatesEveryLimitAndRejectsNonfiniteTime) {
    EXPECT_TRUE(kTerrainUploadLimits.valid());
    EXPECT_TRUE(kWaterUploadLimits.valid());
    EXPECT_FALSE((UploadLimits{0, 8, 1, 3}).valid());
    EXPECT_FALSE((UploadLimits{64 * 1024 * 1024 + 1, 8, 1, 3}).valid());
    EXPECT_FALSE((UploadLimits{1, 0, 1, 3}).valid());
    EXPECT_FALSE((UploadLimits{1, 65, 1, 3}).valid());
    EXPECT_FALSE((UploadLimits{1, 8, 0, 3}).valid());
    EXPECT_FALSE((UploadLimits{1, 8, 9, 3}).valid());
    EXPECT_FALSE((UploadLimits{1, 8, 1, 0}).valid());
    EXPECT_FALSE((UploadLimits{1, 8, 1, 17}).valid());
    EXPECT_FALSE((UploadLimits{1, 8, 1, std::numeric_limits<double>::quiet_NaN()}).valid());
    EXPECT_FALSE((UploadLimits{1, 8, 1, std::numeric_limits<double>::infinity()}).valid());
    UploadBudget fallback({0, 0, 0, 0});
    EXPECT_TRUE(fallback.admits(1, 0));
    fallback.charge(1, true);
    EXPECT_TRUE(fallback.admits(1, 0));
}

TEST(UploadBudget, ExactByteBoundaryAndCountCeiling) {
    UploadBudget budget({100, 2, 1, 3});
    ASSERT_TRUE(budget.admits(60, 0));
    budget.charge(60, true);
    EXPECT_FALSE(budget.admits(41, 0));
    ASSERT_TRUE(budget.admits(40, 0));
    budget.charge(40, true);
    EXPECT_FALSE(budget.admits(0, 0));
    const auto stats = budget.finish(1);
    EXPECT_EQ(stats.attempts, 2u);
    EXPECT_EQ(stats.adopted, 2u);
    EXPECT_EQ(stats.bytes, 100u);
    EXPECT_EQ(stats.byte_overshoot, 0u);
    EXPECT_DOUBLE_EQ(stats.overshoot_ms, 0);
}

TEST(UploadBudget, SingleUploadProgressFloorReportsIndivisibleOvershoot) {
    UploadQueue<Work> queue;
    refresh(queue, {{1, 0, 150, 4, 1}, {2, 0, 10, 0, 1}});
    double elapsed = 2; // candidate preparation is already part of the frame.
    std::vector<std::uint64_t> adopted;
    const auto stats = drain_uploads(
        queue,
        {100, 8, 1, 3},
        [](const Work& item) { return item.bytes; },
        [&](const Work& item) {
            elapsed += item.milliseconds; // copy + ground construction + GPU work
            adopted.push_back(item.id);
            return true;
        },
        [&] { return elapsed; });
    EXPECT_EQ(adopted, (std::vector<std::uint64_t>{1}));
    EXPECT_EQ(queue.size(), 1u);
    EXPECT_EQ(stats.byte_overshoot, 50u);
    EXPECT_DOUBLE_EQ(stats.elapsed_ms, 6);
    EXPECT_DOUBLE_EQ(stats.overshoot_ms, 3);
}

TEST(UploadBudget, PreparationCannotRemoveProgressAndNoSecondCopyStartsAfterTimeLimit) {
    UploadQueue<Work> queue;
    refresh(queue, {{1, 0, 10, 2, 1}, {2, 0, 10, 0, 1}});
    double elapsed = 5;
    unsigned copies = 0;
    const auto stats = drain_uploads(
        queue,
        {100, 8, 1, 3},
        [](const Work& item) { return item.bytes; },
        [&](const Work& item) {
            ++copies;
            elapsed += item.milliseconds;
            return true;
        },
        [&] { return elapsed; });
    EXPECT_EQ(copies, 1u);
    EXPECT_EQ(stats.adopted, 1u);
    EXPECT_DOUBLE_EQ(stats.overshoot_ms, 4);
    UploadBudget exact({100, 8, 1, 3});
    exact.charge(1, true);
    EXPECT_FALSE(exact.admits(1, 3));
}

TEST(UploadBudget, ConfiguredProgressFloorStillObeysCountCeiling) {
    UploadBudget budget({1, 2, 2, 1});
    EXPECT_TRUE(budget.admits(10, 5));
    budget.charge(10, true);
    EXPECT_TRUE(budget.admits(10, 6));
    budget.charge(10, true);
    EXPECT_FALSE(budget.admits(10, 7));
    EXPECT_EQ(budget.finish(7).byte_overshoot, 19u);
}

TEST(UploadBudget, EqualPrioritiesUseStableChunkIdOrderAcrossDiscoveryOrders) {
    UploadQueue<Work> first;
    UploadQueue<Work> second;
    refresh(first, {{9, 5, 1, 0, 1}, {2, 5, 1, 0, 1}, {7, 1, 1, 0, 1}});
    refresh(second, {{7, 1, 1, 0, 1}, {2, 5, 1, 0, 1}, {9, 5, 1, 0, 1}});
    for (const auto expected : {7u, 2u, 9u}) {
        EXPECT_EQ(first.front().id, expected);
        EXPECT_EQ(second.front().id, expected);
        first.complete(true);
        second.complete(true);
    }
}

TEST(UploadBudget, RetainedBacklogIsRefreshedWithoutRepeatedPriorityWork) {
    UploadQueue<Work> queue;
    const std::vector<Work> initial{{9, 5, 1, 0, 1}, {2, 5, 1, 0, 1}, {7, 1, 1, 0, 1}};
    refresh(queue, initial);
    unsigned priority_calls = 0;
    const auto priority = [&](const Work& item) {
        ++priority_calls;
        return item.priority;
    };
    // Changed priority and payload refresh the waiting entry without moving it.
    const std::vector<Work> changed{{2, 0, 2, 0, 2}, {9, 1, 3, 0, 3}, {7, 9, 4, 0, 4}};
    refresh_uploads(queue, changed, [](const Work& item) { return item.id; }, priority);
    EXPECT_EQ(priority_calls, 0u);
    EXPECT_EQ(queue.front().id, 7u);
    EXPECT_EQ(queue.front().version, 4);
    queue.complete(false); // A refused upload rotates exactly once, as before.
    refresh_uploads(queue, changed, [](const Work& item) { return item.id; }, priority);
    EXPECT_EQ(priority_calls, 0u);
    for (const auto expected : {2u, 9u, 7u}) {
        EXPECT_EQ(queue.front().id, expected);
        queue.complete(true);
    }
    refresh_uploads(queue, {}, [](const Work& item) { return item.id; }, priority);
    EXPECT_EQ(queue.size(), 0u);
    EXPECT_EQ(priority_calls, 0u);
}

TEST(UploadBudget, OnlyArrivalsAreSortedAndTheyFollowRetainedWork) {
    UploadQueue<Work> queue;
    refresh(queue, {{99, 100, 1, 0, 1}, {98, 101, 1, 0, 1}});
    unsigned priority_calls = 0;
    const auto priority = [&](const Work& item) {
        ++priority_calls;
        return item.priority;
    };
    const std::vector<Work> mixed{{9, 5, 1, 0, 1}, {99, 200, 1, 0, 2}, {2, 5, 1, 0, 1}};
    refresh_uploads(queue, mixed, [](const Work& item) { return item.id; }, priority);
    EXPECT_EQ(priority_calls, 32u); // Two key evaluations, eight passes, two arrivals.
    EXPECT_FALSE(queue.contains(98));
    EXPECT_EQ(queue.front().version, 2);
    for (const auto expected : {99u, 2u, 9u}) {
        EXPECT_EQ(queue.front().id, expected);
        queue.complete(true);
    }
    // A lone arrival needs no sort; an empty refresh prunes it.
    refresh_uploads(queue, {{1, 0, 1, 0, 1}}, [](const Work& item) { return item.id; }, priority);
    EXPECT_EQ(queue.front().id, 1u);
    EXPECT_EQ(priority_calls, 32u);
    refresh_uploads(queue, {}, [](const Work& item) { return item.id; }, priority);
    EXPECT_EQ(queue.size(), 0u);
    EXPECT_FALSE(queue.contains(1));
}

TEST(UploadBudget, WaitingReplacementCannotStarveUnderContinuousNearerArrivals) {
    UploadQueue<Work> queue;
    refresh(queue, {{1, 0, 1, 0, 1}, {99, 100, 1, 0, 1}});
    std::vector<std::uint64_t> adopted;
    for (std::uint64_t frame = 0; frame < 3; ++frame) {
        if (frame)
            refresh(queue, {{frame + 1, 0, 1, 0, 1}, {99, 100, 1, 0, 2}});
        drain_uploads(
            queue,
            {100, 1, 1, 3},
            [](const Work& item) { return item.bytes; },
            [&](const Work& item) {
                adopted.push_back(item.id);
                return true;
            },
            [] { return 0.0; });
    }
    ASSERT_EQ(adopted.size(), 3u);
    EXPECT_EQ(adopted[1], 99u);
}

TEST(UploadBudget, FailedUploadsRotateAndEachIsTriedOnlyOncePerFrame) {
    UploadQueue<Work> queue;
    refresh(queue, {{1, 0, 1, 0, 1}, {2, 0, 1, 0, 1}});
    unsigned attempts = 0;
    const auto stats = drain_uploads(
        queue,
        {100, 8, 1, 3},
        [](const Work& item) { return item.bytes; },
        [&](const Work& item) {
            ++attempts;
            return item.id == 2;
        },
        [] { return 0.0; });
    EXPECT_EQ(attempts, 2u);
    EXPECT_EQ(stats.attempts, 2u);
    EXPECT_EQ(stats.adopted, 1u);
    EXPECT_EQ(queue.size(), 1u);
    EXPECT_EQ(queue.front().id, 1u);
}

TEST(UploadBudget, OldGeometryRetainedUntilAdmittedReplacementSucceeds) {
    UploadQueue<Work> queue;
    refresh(queue, {{1, 0, 1, 0, 1}, {2, 0, 100, 0, 2}});
    int resident_version = 1;
    const auto adopt = [&](const Work& item) {
        if (item.id == 2)
            resident_version = item.version;
        return true;
    };
    const auto first = drain_uploads(
        queue,
        {10, 8, 1, 3},
        [](const Work& item) { return item.bytes; },
        adopt,
        [] { return 0.0; });
    EXPECT_EQ(first.adopted, 1u);
    EXPECT_EQ(resident_version, 1);
    ASSERT_TRUE(queue.contains(2));
    refresh(queue, {{2, 0, 100, 0, 3}}); // newer payload, same waiting position
    const auto second = drain_uploads(
        queue,
        {10, 8, 1, 3},
        [](const Work& item) { return item.bytes; },
        adopt,
        [] { return 0.0; });
    EXPECT_EQ(second.byte_overshoot, 90u);
    EXPECT_EQ(resident_version, 3);
    EXPECT_EQ(queue.size(), 0u);
}

TEST(UploadBudget, FailedPoolGrowthDoesNotReleaseResidentAndSuccessAllocatesFirst) {
    bool resident_live = true;
    const auto failed = replace_upload(
        1,
        -1,
        [&] {
            EXPECT_TRUE(resident_live);
            return -1;
        },
        [&](int) { resident_live = false; });
    EXPECT_EQ(failed, -1);
    EXPECT_TRUE(resident_live);
    const auto succeeded = replace_upload(
        1,
        -1,
        [&] {
            EXPECT_TRUE(resident_live);
            return 2;
        },
        [&](int resident) {
            EXPECT_EQ(resident, 1);
            resident_live = false;
        });
    EXPECT_EQ(succeeded, 2);
    EXPECT_FALSE(resident_live);
}

TEST(UploadBudget, RefreshRemovesDepartedWorkAndClearReleasesMetadata) {
    UploadQueue<Work> queue;
    refresh(queue, {{1, 0, 1, 0, 1}, {2, 0, 1, 0, 1}});
    refresh(queue, {{2, 0, 1, 0, 2}});
    EXPECT_EQ(queue.size(), 1u);
    EXPECT_FALSE(queue.contains(1));
    EXPECT_EQ(queue.front().version, 2);
    queue.clear();
    EXPECT_EQ(queue.size(), 0u);
    EXPECT_FALSE(queue.contains(2));
}

TEST(UploadBudget, EmptySettledFrameReportsPreparationWithoutAdoptingAnything) {
    UploadQueue<Work> queue;
    const auto stats = drain_uploads(
        queue,
        kTerrainUploadLimits,
        [](const Work&) {
            ADD_FAILURE();
            return 0u;
        },
        [](const Work&) {
            ADD_FAILURE();
            return false;
        },
        [] { return 4.0; });
    EXPECT_EQ(stats.attempts, 0u);
    EXPECT_EQ(stats.bytes, 0u);
    EXPECT_DOUBLE_EQ(stats.overshoot_ms, 1);
}

TEST(UploadBudget, QualificationSettlesThroughBoundedFramesWithoutChangingPerFramePolicy) {
    UploadQueue<Work> queue;
    refresh(queue, {{1, 0, 200, 4, 1}, {2, 0, 200, 4, 1}, {3, 0, 200, 4, 1}});
    unsigned frames = 0;
    std::size_t adopted = 0;
    EXPECT_TRUE(settle_uploads([&] { return std::pair{queue.size(), adopted}; },
                               [&] {
                                   ++frames;
                                   double elapsed = 0;
                                   const auto work = drain_uploads(
                                       queue,
                                       {100, 8, 1, 3},
                                       [](const Work& item) { return item.bytes; },
                                       [&](const Work& item) {
                                           elapsed += item.milliseconds;
                                           return true;
                                       },
                                       [&] { return elapsed; });
                                   adopted = work.adopted;
                                   EXPECT_EQ(work.adopted, 1u);
                                   EXPECT_EQ(work.byte_overshoot, 100u);
                                   EXPECT_DOUBLE_EQ(work.overshoot_ms, 1);
                               }));
    EXPECT_EQ(frames, 3u);
    EXPECT_TRUE(settle_uploads([&] { return std::pair{queue.size(), std::size_t{0}}; },
                               [&] { ADD_FAILURE(); }));
    refresh(queue, {{1, 0, 1, 0, 1}, {2, 0, 1, 0, 1}});
    unsigned failed_frames = 0;
    EXPECT_FALSE(settle_uploads([&] { return std::pair{queue.size(), std::size_t{0}}; },
                                [&] { ++failed_frames; }));
    EXPECT_EQ(failed_frames, 2u);
}

TEST(UploadBudget, DrainAcceptsPublicationDuringRenderAndCountsBothLanes) {
    std::size_t pending = 1;
    std::size_t adopted = 0;
    unsigned frames = 0;
    EXPECT_TRUE(settle_uploads([&] { return std::pair{pending, adopted}; },
                               [&] {
                                   ++frames;
                                   adopted = 1;
                                   --pending;
                                   if (frames == 1)
                                       pending += 3;
                               }));
    EXPECT_EQ(frames, 4u);
    EXPECT_EQ(pending, 0u);
}

TEST(UploadBudget, ContinuousPublicationAndPersistentFailuresAreBounded) {
    UploadSettlingWait failed;
    EXPECT_TRUE(failed.waiting(2));
    EXPECT_TRUE(failed.waiting(2));
    EXPECT_FALSE(failed.waiting(2));
    EXPECT_TRUE(failed.failed());
    EXPECT_EQ(failed.frames(), 2u);

    std::size_t adopted = 0;
    unsigned frames = 0;
    EXPECT_FALSE(
        settle_uploads([&] { return std::pair{std::size_t{1}, adopted}; },
                       [&] {
                           ++frames;
                           adopted = 1; // each render publishes one replacement for the one adopted
                       }));
    EXPECT_EQ(frames, 8193u);
}

TEST(UploadBudget, PresentedWaitAcceptsArrivalsEvenWhenBacklogShrinks) {
    UploadSettlingWait wait;
    EXPECT_TRUE(wait.waiting(4));
    EXPECT_TRUE(wait.waiting(3, 4)); // four adopted, three newly published
    EXPECT_TRUE(wait.waiting(2, 1));
    EXPECT_TRUE(wait.waiting(1, 1));
    EXPECT_FALSE(wait.waiting(0, 1));
    EXPECT_FALSE(wait.failed());
    EXPECT_EQ(wait.frames(), 4u);
}

TEST(UploadBudget, ChecksExactMeshSizesAndOverflowBeforeAllocation) {
    std::size_t bytes = 0;
    EXPECT_TRUE(mesh_upload_bytes(10, 32, 12, bytes));
    EXPECT_EQ(bytes, 368u);
    EXPECT_FALSE(mesh_upload_bytes(std::numeric_limits<std::size_t>::max(), 32, 1, bytes));
    EXPECT_FALSE(mesh_upload_bytes(2, std::numeric_limits<std::size_t>::max(), 1, bytes));
    EXPECT_FALSE(mesh_upload_bytes(1, 0, 1, bytes));
    const auto maximum = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
    EXPECT_FALSE(mesh_upload_bytes(1, maximum, 1, bytes));
    EXPECT_TRUE(mesh_upload_bytes(1, maximum - 4, 1, bytes));
    EXPECT_EQ(bytes, maximum);
    UploadBudget budget({1, 2, 2, 3});
    budget.charge(std::numeric_limits<std::size_t>::max(), false);
    budget.charge(2, false);
    EXPECT_EQ(budget.finish(0).bytes, std::numeric_limits<std::size_t>::max());
}
} // namespace
