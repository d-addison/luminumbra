#pragma once

#include <cstddef>
#include <cstdint>

namespace Luminumbra::Rendering {

struct FoliageCountObservation {
    std::size_t instances = 0;
    std::uint64_t candidate_slots = 0;
    std::uint64_t candidates = 0;
    bool gpu = false;
};

// Window-boundary observation shared by CPU and GPU scatter. A GPU capacity
// marker is never an observed instance count; read the indirect draw count.
template<class ReadGpuCount>
FoliageCountObservation observe_foliage_counts(bool enabled,
                                               bool built,
                                               bool gpu,
                                               std::size_t frame_count,
                                               std::uint64_t slots,
                                               std::uint64_t candidates,
                                               ReadGpuCount read_gpu_count) {
    if (!enabled || !built)
        return {};
    const auto instances = gpu && frame_count != 0 ? read_gpu_count() : frame_count;
    return {instances, slots, candidates, gpu};
}

} // namespace Luminumbra::Rendering
