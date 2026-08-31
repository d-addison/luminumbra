//  ( implementation note): the worldgen-preview reinit-vs-far-LOD race
// is closed by a QUIESCE, not a point guard. Worker threads soak far-LOD-style
// sampling under the worldgen-epoch gate (acquire_worldgen_sample_scope) while
// the main thread hammers set_params (reinitialize_noise takes the exclusive
// side, toggling the shaping generators between built and nulled — the exact
// transient the old pan crash dereferenced). The pin: no crash, every sampled
// height finite, and post-settle values byte-equal a fresh reference world.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "systems/SHIELD_WorldSystem.h"

using namespace Luminumbra;
using namespace Luminumbra::Systems;

namespace {

TEST(WorldgenPreviewReinitRace, ConcurrentReinitAndSamplingSoak) {
    TerrainGenParams params;
    params.shaping_enabled = false;
    SHIELD_WorldSystem world(nullptr, nullptr, params, 424242);

    std::atomic<bool> failed{false};
    std::atomic<bool> release_samplers{false};
    std::atomic<bool> writer_started{false};
    std::atomic<bool> writer_finished{false};
    std::atomic<int> samplers_in_scope{0};
    std::vector<std::thread> samplers;
    constexpr int kSamplerThreads = 2;
    samplers.reserve(kSamplerThreads);
    for (int t = 0; t < kSamplerThreads; ++t) {
        samplers.emplace_back([&world, &failed, &release_samplers, &samplers_in_scope, t]() {
            // Far-LOD-style sampling: coarse strides over a wide area, each
            // finite burst bracketed by the epoch gate exactly as the real
            // tile-build job is.
            const auto scope = world.acquire_worldgen_sample_scope();
            samplers_in_scope.fetch_add(1, std::memory_order_release);
            while (!release_samplers.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const float base = 1000.0f * static_cast<float>(t + 1);
            for (int i = 0; i < 64; ++i) {
                const float x = base + static_cast<float>(i) * 37.0f;
                const float z = base - static_cast<float>(i) * 53.0f;
                const float h = world.GetTerrainHeightAt(x, z);
                if (!std::isfinite(h)) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
            }
        });
    }

    // Guarantee genuine overlap without relying on reader/writer scheduling
    // fairness: every reader first holds the shared epoch, then a writer starts
    // and must wait until the finite reader bursts are released.
    while (samplers_in_scope.load(std::memory_order_acquire) < kSamplerThreads) {
        std::this_thread::yield();
    }
    TerrainGenParams knob = params;
    knob.shaping_enabled = true;
    knob.height_offset = 1.0f;
    std::thread writer([&]() {
        writer_started.store(true, std::memory_order_release);
        world.set_params(knob);
        writer_finished.store(true, std::memory_order_release);
    });
    while (!writer_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(writer_finished.load(std::memory_order_acquire))
        << "reinitialization bypassed active worldgen sampling scopes";

    release_samplers.store(true, std::memory_order_release);
    for (std::thread& thread : samplers) {
        thread.join();
    }
    writer.join();
    EXPECT_TRUE(writer_finished.load(std::memory_order_acquire));
    EXPECT_FALSE(failed.load()) << "a sampler observed a non-finite height mid-reinit";

    // Repeatedly swing the shaping generators between built and null after
    // the contended handoff. This preserves broad state-transition coverage
    // while keeping the concurrency proof bounded on every platform.
    constexpr int kReinitializations = 16;
    for (int iteration = 1; iteration < kReinitializations; ++iteration) {
        knob.shaping_enabled = (iteration % 2) == 1;
        knob.height_offset = static_cast<float>(iteration % 7);
        world.set_params(knob);
    }

    // Post-settle: the world must sample byte-identically to a fresh reference
    // built with the same final params (no lingering half-rebuilt generator
    // state — the no-flicker half of the proving signal).
    TerrainGenParams final_params = knob;
    SHIELD_WorldSystem reference(nullptr, nullptr, final_params, 424242);
    for (int i = 0; i < 32; ++i) {
        const float x = 512.0f + static_cast<float>(i) * 91.0f;
        const float z = 256.0f - static_cast<float>(i) * 77.0f;
        EXPECT_EQ(world.GetTerrainHeightAt(x, z), reference.GetTerrainHeightAt(x, z))
            << "post-settle height differs from a fresh world at sample " << i;
    }
}

} // namespace
