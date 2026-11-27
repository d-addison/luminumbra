#pragma once

#include "FarWorldRenderer.h"

namespace Luminumbra::Rendering {
// Injected qualification envelope only. Zero refuses; there are no shipping
// values. Rendering pools include serving/target, CPU/GPU and queue overlap.
struct FarWorldSessionQualificationLimits {
    FarWorldExperimentalLimits rendering;
    // Leave memory empty here; SourceConfig supplies the admitted owning pool.
    World::FarWorldAuthorityProducer::Limits source;
    World::FarWorldAuthorityProducer::CaptureLimits capture;
    World::FarFrozenCaptureLimits sampling;
    World::FarFrozenCaptureWork::Counts transaction_samples;
    std::size_t tiles = 0;
    std::uint64_t planning_operations = 0, admission_operations = 0;
    std::uint64_t operations_per_pump = 0, refresh_ticks = 0, ticks_per_second = 0;
    std::uint32_t refresh_frames = 0;
    bool Complete() const noexcept;
};
// Diagnostics and measurement only, enabled by --far-ladder-diagnostics.
// These generous finite ceilings are not production values. Production approval
// follows native measurement. The caller retains the monotone clock context.
inline FarWorldSessionQualificationLimits
FarWorldDiagnosticsSessionLimits(std::uint64_t (*now)(const void*) noexcept,
                                 const void* context = nullptr) noexcept {
    FarWorldSessionQualificationLimits limits;
    limits.rendering = FarWorldDevelopmentLimits();
    limits.source = {512, 4u * 1024u * 1024u, 300'000, {}};
    constexpr std::uint64_t aggregate_bytes = 8ull * 1024ull * 1024ull * 1024ull;
    limits.capture = {
        {512, 512, 1024, 8192, 4194304, aggregate_bytes, 4194304, aggregate_bytes, 131072},
        32'768,
        {now, context, 3'600'000}};
    limits.sampling = {1, 400'000, 20'000'000, 400'000, 64u * 1024u * 1024u};
    limits.transaction_samples = {64'000'000, 1'200'000'000, 80'000'000};
    limits.tiles = 512;
    limits.planning_operations = 32'000'000;
    limits.admission_operations = 1'000'000'000;
    limits.operations_per_pump = 32'768;
    limits.ticks_per_second = 1000;
    limits.refresh_ticks = 3'600'000;
    limits.refresh_frames = 65'536;
    return limits;
}
} // namespace Luminumbra::Rendering
