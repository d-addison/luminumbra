#include "luminumbra_client/rendering/FoliageGroundWork.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::Rendering;

FoliageGroundRequest Request(std::uint64_t key = 1) {
    const float extent = static_cast<float>(CHUNK_SIZE_X);
    const std::vector<VoxelVertex> vertices{{{0, 4, 0}, {0, 1, 0}, 1},
                                            {{extent, 6, 0}, {0, 1, 0}, 1},
                                            {{0, 4, extent}, {0, 1, 0}, 1},
                                            {{extent, 6, extent}, {0, 1, 0}, 1}};
    auto payload = FoliageMeshPayload::copy(
        {-extent, static_cast<float>(key), extent}, vertices, {0, 2, 3, 0, 3, 1});
    return {{key, 7},
            {11, 7},
            {{-1, 1}, {-extent, 0, extent}, extent, 2, 0.5f},
            {{1, std::move(payload), {}}}};
}

std::vector<glm::vec4> Synchronous(const FoliageGroundRequest& request) {
    std::vector<FoliageGroundMesh> queries;
    for (const auto& mesh : request.meshes)
        queries.emplace_back(mesh.payload->origin, mesh.payload->vertices, mesh.payload->indices);
    std::vector<glm::vec4> samples;
    for (std::uint32_t candidate = 0; candidate < FoliageGroundBuild::kCandidates; ++candidate) {
        const auto& chunk = request.chunk;
        const auto h0 =
            foliage_placement_hash(chunk.chunk_xz.x, chunk.chunk_xz.y, chunk.biome_id, candidate);
        const auto h1 = foliage_splitmix64(h0 ^ 0x2545F4914F6CDD1Dull);
        const float x = chunk.origin.x + foliage_hash_unit(h0) * chunk.extent_m;
        const float z = chunk.origin.z + foliage_hash_unit(h1) * chunk.extent_m;
        FoliageGroundSample sample{};
        sample.valid = false;
        for (const auto& query : queries) {
            const auto hit = query.sample(x, z);
            if (hit.valid && (!sample.valid || hit.height > sample.height)) {
                sample.height = hit.height;
                sample.slope = std::clamp(hit.slope, 0.0f, 1.0f);
                sample.moisture = 1.0f - sample.slope;
                sample.valid = true;
            }
        }
        samples.emplace_back(
            sample.height, sample.moisture, sample.slope, sample.valid ? 1.0f : 0.0f);
    }
    return samples;
}

TEST(FoliageGroundWork, WorkerSamplesMatchSynchronousBytesAndReuseImmutableQueries) {
    auto request = Request();
    // Multiple vertical meshes, invalid triangles and a footprint crossing the
    // column edge exercise hits and exact misses.
    const float extent = static_cast<float>(CHUNK_SIZE_X);
    std::vector<VoxelVertex> roof{
        {{0, 8, 0}, {0, 1, 0}, 1}, {{extent, 9, 0}, {0, 1, 0}, 1}, {{0, 8, extent}, {0, 1, 0}, 1}};
    request.meshes.push_back(
        {2, FoliageMeshPayload::copy({-extent, 0, extent}, roof, {0, 2, 1, 0, 999, 1}), {}});
    request.chunk.extent_m += 2;
    const auto reference = Synchronous(request);
    // Mutating producer storage cannot affect either worker or sync snapshots.
    roof.clear();
    FoliageGroundBuild build(request);
    EXPECT_FALSE(build.ready());
    build.run();
    ASSERT_TRUE(build.ready());
    ASSERT_FALSE(build.failed);
    ASSERT_EQ(build.samples.size(), reference.size());
    EXPECT_EQ(
        std::memcmp(build.samples.data(), reference.data(), reference.size() * sizeof(glm::vec4)),
        0);
    EXPECT_TRUE(std::any_of(
        reference.begin(), reference.end(), [](const auto& sample) { return sample.w == 0; }));
    request.meshes = build.queries;
    FoliageGroundBuild reused(std::move(request));
    reused.run();
    ASSERT_FALSE(reused.failed);
    EXPECT_EQ(reused.queries.front().query, build.queries.front().query);
    EXPECT_EQ(
        std::memcmp(reused.samples.data(), reference.data(), reference.size() * sizeof(glm::vec4)),
        0);
}

TEST(FoliageGroundWork, QueueBoundsIncludeRunningAndUnconsumedCompletions) {
    FoliageGroundQueue queue;
    queue.begin_frame();
    std::vector<std::shared_ptr<FoliageGroundBuild>> jobs;
    for (std::size_t index = 0; index < FoliageGroundQueue::kPerFrame; ++index) {
        auto job = queue.enqueue(Request(index));
        ASSERT_TRUE(job);
        jobs.push_back(std::move(job));
    }
    EXPECT_FALSE(queue.enqueue(Request(100)));
    EXPECT_EQ(queue.stats().dispatched, FoliageGroundQueue::kPerFrame);
    queue.begin_frame();
    EXPECT_FALSE(queue.enqueue(Request(0))); // No duplicate pending footprint.
    for (std::size_t index = jobs.size(); index < FoliageGroundQueue::kInFlight; ++index) {
        auto job = queue.enqueue(Request(index));
        ASSERT_TRUE(job);
        jobs.push_back(std::move(job));
    }
    for (auto& job : jobs)
        job->run();
    queue.begin_frame();
    EXPECT_FALSE(queue.enqueue(Request(100))); // Ready results still own slots.
    std::size_t consumed = 0;
    queue.poll([](const auto&) { return true; }, [&](auto&) { ++consumed; });
    EXPECT_EQ(consumed, FoliageGroundQueue::kInFlight);
    EXPECT_EQ(queue.stats().completed, FoliageGroundQueue::kInFlight);
    EXPECT_EQ(queue.stats().max_age_frames, 2u);
    EXPECT_TRUE(queue.enqueue(Request(100)));
}

TEST(FoliageGroundWork, MeshSupportAndAbsentSupportEditsRejectAndPermitRerequest) {
    for (const bool mesh_edit : {false, true}) {
        FoliageGroundQueue queue;
        queue.begin_frame();
        auto request = Request();
        const auto job = queue.enqueue(request);
        ASSERT_TRUE(job);
        if (mesh_edit)
            ++request.version.mesh;
        else
            ++request.version.support;
        job->run();
        queue.begin_frame();
        std::size_t consumed = 0;
        queue.poll([&](const auto& input) { return input.version == request.version; },
                   [&](auto&) { ++consumed; });
        EXPECT_EQ(consumed, 0u);
        EXPECT_EQ(queue.stats().completed, 1u);
        EXPECT_EQ(queue.stats().stale_rejected, 1u);
        ASSERT_TRUE(queue.enqueue(request));
    }
    FoliageGroundQueue queue;
    queue.begin_frame();
    auto absent = Request();
    absent.meshes.clear();
    absent.version.mesh = 0;
    const auto job = queue.enqueue(absent);
    ASSERT_TRUE(job);
    queue.invalidate(absent.key.column); // Support appears, then disappears.
    EXPECT_FALSE(queue.enqueue(absent)); // Still charged while obsolete job runs.
    job->run();
    queue.begin_frame();
    queue.poll([](const auto&) { return true; },
               [](auto&) { ADD_FAILURE() << "stale support published"; });
    EXPECT_EQ(queue.stats().stale_rejected, 1u);
    EXPECT_TRUE(queue.enqueue(absent));
}

TEST(FoliageGroundWork, ExecutorRejectionsReleaseSlotsAndKeepAdmissionAttemptsBounded) {
    FoliageGroundQueue queue;
    queue.begin_frame();
    queue.withdraw({});
    EXPECT_EQ(queue.stats().dispatched, 0u);
    for (std::size_t index = 0; index < FoliageGroundQueue::kPerFrame; ++index) {
        auto job = queue.enqueue(Request(index));
        ASSERT_TRUE(job);
        std::weak_ptr<FoliageGroundBuild> retained = job;
        queue.withdraw(job);
        job.reset();
        EXPECT_TRUE(retained.expired());
    }
    EXPECT_EQ(queue.stats().dispatched, 0u);
    EXPECT_FALSE(queue.enqueue(Request(100)));
    queue.begin_frame();
    EXPECT_TRUE(queue.enqueue(Request(100)));
}

TEST(FoliageGroundWork, FinalOrderedBytesMatchFreshBuildForEveryCompletionOrder) {
    std::array<int, 3> order{0, 1, 2};
    std::array<std::vector<glm::vec4>, 3> fresh;
    for (std::size_t index = 0; index < fresh.size(); ++index)
        fresh[index] = Synchronous(Request(index));
    const auto hash = [](const auto& columns) {
        std::uint64_t value = 1469598103934665603ull;
        for (const auto& samples : columns) {
            const auto* bytes = reinterpret_cast<const unsigned char*>(samples.data());
            for (std::size_t index = 0; index < samples.size() * sizeof(glm::vec4); ++index) {
                value ^= bytes[index];
                value *= 1099511628211ull;
            }
        }
        return value;
    };
    const auto fresh_hash = hash(fresh);
    do {
        FoliageGroundQueue queue;
        queue.begin_frame();
        std::array<std::shared_ptr<FoliageGroundBuild>, 3> jobs;
        std::array<std::vector<glm::vec4>, 3> results;
        for (std::size_t index = 0; index < jobs.size(); ++index) {
            jobs[index] = queue.enqueue(Request(index));
            ASSERT_TRUE(jobs[index]);
        }
        for (const auto index : order) {
            jobs[static_cast<std::size_t>(index)]->run();
            queue.begin_frame();
            queue.poll(
                [](const auto&) { return true; },
                [&](auto& build) { results[build.input.key.column] = std::move(build.samples); });
        }
        for (std::size_t index = 0; index < fresh.size(); ++index) {
            ASSERT_EQ(results[index].size(), fresh[index].size());
            EXPECT_EQ(std::memcmp(results[index].data(),
                                  fresh[index].data(),
                                  fresh[index].size() * sizeof(glm::vec4)),
                      0);
        }
        EXPECT_EQ(hash(results), fresh_hash);
    } while (std::next_permutation(order.begin(), order.end()));
}
} // namespace
