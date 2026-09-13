#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include <string>

namespace Luminumbra::Rendering {
struct FoliageRebuildEvidence {
    std::uint64_t input_hash = 0, first_generation = 0, second_generation = 0;
    std::uint64_t first_frame = 0, second_frame = 0, first_hash = 0, second_hash = 0;
    std::uint64_t first_count = 0, second_count = 0;
    bool output_cleared = false, byte_equal = false, completed = false;
};
struct FoliageVertexEvidence {
    std::uint64_t source_frame = 0, available_frame = 0, generation = 0, instance_hash = 0;
    std::uint32_t phase = 0, first_instance = 0;
    float shader_time = 0;
    std::array<float, 16> view_projection{}; // Column-major matrix actually uploaded to the draw.
    // Interleaved actual vertex outputs: world xyz, heightT, clip xyzw.
    // 64 instances * 12 vertices * 8 floats = 24 KiB per phase, hard bounded.
    std::vector<std::array<float, 8>> vertices;
    std::vector<float> blade_heights;
    std::vector<bool> sways;
};
struct FoliageGpuEvidence {
    std::uint64_t query_id = 0, source_frame = 0, generation = 0, instance_generation = 0;
    std::uint32_t phase = 0;
    std::uint64_t instance_count = 0;
    double milliseconds = -1;
    float shader_time = 0;
};
struct FoliageQualificationEvidence {
    FoliageRebuildEvidence rebuild;
    std::array<FoliageVertexEvidence, 2> vertices;
    std::vector<FoliageGpuEvidence> gpu;
    bool frozen_inputs = false;
    std::string gl_vendor, gl_renderer, gl_version;
};
} // namespace Luminumbra::Rendering
