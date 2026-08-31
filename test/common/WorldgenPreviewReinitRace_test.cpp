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

    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::atomic<int> samplers_started{0};
    std::vector<std::thread> samplers;
    constexpr int kSamplerThreads = 4;
    samplers.reserve(kSamplerThreads);
    for (int t = 0; t < kSamplerThreads; ++t) {
        samplers.emplace_back([&world, &stop, &failed, &samplers_started, t]() {
            // Far-LOD-style sampling: coarse strides over a wide area, each
            // burst bracketed by the epoch gate exactly as the real tile-build
            // job is.
            samplers_started.fetch_add(1, std::memory_order_release);
            const float base = 1000.0f * static_cast<float>(t + 1);
            while (!stop.load(std::memory_order_acquire)) {
                const auto scope = world.acquire_worldgen_sample_scope();
                for (int i = 0; i < 64; ++i) {
                    const float x = base + static_cast<float>(i) * 37.0f;
                    const float z = base - static_cast<float>(i) * 53.0f;
                    const float h = world.GetTerrainHeightAt(x, z);
                    if (!std::isfinite(h)) {
                        failed.store(true, std::memory_order_release);
                        return;
                    }
                }
            }
        });
    }

    // Guarantee genuine overlap: wait for every sampler to be live, then
    // hammer the preview's knob path for a fixed contention window. Every
    // set_params rebuilds the generator set under the exclusive side of the
    // gate; toggling shaping_enabled swings the shaping generators between
    // built and NULLED — the old crash window.
    while (samplers_started.load(std::memory_order_acquire) < kSamplerThreads) {
        std::this_thread::yield();
    }
    TerrainGenParams knob = params;
    const auto soak_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    int iteration = 0;
    while (std::chrono::steady_clock::now() < soak_end && !failed.load(std::memory_order_acquire)) {
        knob.shaping_enabled = (iteration % 2) == 1;
        knob.height_offset = static_cast<float>(iteration % 7);
        world.set_params(knob);
        ++iteration;
    }
    EXPECT_GT(iteration, 10) << "the soak barely iterated — contention window too small";

    stop.store(true, std::memory_order_release);
    for (std::thread& thread : samplers) {
        thread.join();
    }
    EXPECT_FALSE(failed.load()) << "a sampler observed a non-finite height mid-reinit";

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
