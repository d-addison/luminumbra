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
} // namespace Luminumbra::Rendering
