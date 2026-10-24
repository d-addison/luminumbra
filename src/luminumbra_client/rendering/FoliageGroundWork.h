#pragma once

#include "FoliageGroundMesh.h"
#include "FoliageSurfaceSlots.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Luminumbra::Rendering {

struct FoliageScatterInput {
    glm::ivec2 chunk_xz{0, 0};
    glm::vec3 origin{0.0f};
    float extent_m = 32.0f;
    u8 biome_id = 255;
    float density = 0.0f;
};

struct FoliageGroundSample {
    float height = 0.0f;
    float slope = 0.0f;
    float moisture = 0.0f;
    bool valid = true;
};

inline std::uint64_t foliage_splitmix64(std::uint64_t value) {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

inline std::uint64_t foliage_placement_hash(int x, int z, u8 biome, std::uint32_t candidate) {
    auto hash = foliage_splitmix64(static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) ^
                                   0x51AF7C3D9E0B12A7ull);
    hash = foliage_splitmix64(
        hash ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z)) * 0xD1B54A32D192ED03ull));
    hash = foliage_splitmix64(hash ^ (static_cast<std::uint64_t>(biome) * 0x9E3779B97F4A7C15ull));
    return foliage_splitmix64(hash ^
                              (static_cast<std::uint64_t>(candidate) * 0xA24BAED4963EE407ull));
}

inline float foliage_hash_unit(std::uint64_t hash) {
    return static_cast<float>(hash >> 40) / 16777216.0f;
}

// Copied once at mesh publication. Jobs retain const owners, never Chunk/GL
// storage. A remesh replaces the owner rather than modifying this payload.
struct FoliageMeshPayload {
    glm::vec3 origin;
    std::vector<VoxelVertex> vertices;
    std::vector<u32> indices;

    static std::shared_ptr<const FoliageMeshPayload>
    copy(const glm::vec3& mesh_origin,
         const std::vector<VoxelVertex>& mesh_vertices,
         const std::vector<u32>& mesh_indices) {
        if (mesh_vertices.size() > std::numeric_limits<std::size_t>::max() / sizeof(VoxelVertex) ||
            mesh_indices.size() > std::numeric_limits<std::size_t>::max() / sizeof(u32) ||
            mesh_indices.size() / 3 > std::numeric_limits<u32>::max())
            throw std::length_error("foliage mesh exceeds query index range");
        const auto vertex_bytes = mesh_vertices.size() * sizeof(VoxelVertex);
        const auto index_bytes = mesh_indices.size() * sizeof(u32);
        if (vertex_bytes > std::numeric_limits<std::size_t>::max() - index_bytes)
            throw std::length_error("foliage mesh exceeds payload byte range");
        return std::make_shared<const FoliageMeshPayload>(
            FoliageMeshPayload{mesh_origin, mesh_vertices, mesh_indices});
    }
};

struct FoliageGroundVersion {
    std::uint64_t mesh = 0;    // Column mesh revision (world epoch when absent).
    std::uint64_t support = 0; // Exact scatter footprint signature.
    bool operator==(const FoliageGroundVersion&) const = default;
};

struct FoliageQueryMesh {
    ChunkID id;
    std::shared_ptr<const FoliageMeshPayload> payload;
    std::shared_ptr<const FoliageGroundMesh> query;
};

struct FoliageGroundRequest {
    FoliageSurfaceSlots::Key key;
    FoliageGroundVersion version;
    FoliageScatterInput chunk;
    // Mesh order preserves the synchronous highest-hit tie break.
    std::vector<FoliageQueryMesh> meshes;
};

// Public executor-independent work item: run once on an executor, then consume
// after ready(). The request is immutable; result publication is release/acquire.
class FoliageGroundBuild {
public:
    static constexpr std::size_t kCandidates = 8192;
    explicit FoliageGroundBuild(FoliageGroundRequest request)
        : input(std::move(request)) {}

    const FoliageGroundRequest input;
    std::vector<FoliageQueryMesh> queries;
    std::vector<glm::vec4> samples;
    bool failed = false; // Read only after ready().

    void run() noexcept {
        try {
            if (input.meshes.size() > queries.max_size() ||
                input.meshes.size() >
                    std::numeric_limits<std::size_t>::max() / sizeof(FoliageQueryMesh))
                throw std::length_error("foliage query snapshot exceeds storage range");
            queries.reserve(input.meshes.size());
            for (const auto& mesh : input.meshes) {
                auto query = mesh.query;
                if (!query)
                    query = std::make_shared<const FoliageGroundMesh>(
                        mesh.payload->origin, mesh.payload->vertices, mesh.payload->indices);
                // Results need only queries. The immutable input owns payloads
                // until the in-flight work item is consumed or discarded.
                queries.push_back({mesh.id, {}, std::move(query)});
            }
            // Exactly 8192 vec4s (128 KiB) per work item, independent of density.
            static_assert(kCandidates <=
                          std::numeric_limits<std::size_t>::max() / sizeof(glm::vec4));
            samples.reserve(kCandidates);
            for (std::uint32_t candidate = 0; candidate < kCandidates; ++candidate) {
                const auto& chunk = input.chunk;
                const auto h0 = foliage_placement_hash(
                    chunk.chunk_xz.x, chunk.chunk_xz.y, chunk.biome_id, candidate);
                const auto h1 = foliage_splitmix64(h0 ^ 0x2545F4914F6CDD1Dull);
                const float x = chunk.origin.x + foliage_hash_unit(h0) * chunk.extent_m;
                const float z = chunk.origin.z + foliage_hash_unit(h1) * chunk.extent_m;
                const auto sample = sample_queries(queries, x, z);
                samples.emplace_back(
                    sample.height, sample.moisture, sample.slope, sample.valid ? 1.0f : 0.0f);
            }
        } catch (...) {
            failed = true;
            samples.clear();
            queries.clear();
        }
        m_ready.store(true, std::memory_order_release);
    }
    bool ready() const {
        return m_ready.load(std::memory_order_acquire);
    }
    static void execute(void* payload) {
        static_cast<FoliageGroundBuild*>(payload)->run();
    }
    static FoliageGroundSample
    sample_queries(const std::vector<FoliageQueryMesh>& meshes, float x, float z) {
        FoliageGroundSample sample{};
        sample.valid = false;
        for (const auto& mesh : meshes) {
            const auto hit = mesh.query->sample(x, z);
            if (hit.valid && (!sample.valid || hit.height > sample.height)) {
                sample.valid = true;
                sample.height = hit.height;
                sample.slope = std::clamp(hit.slope, 0.0f, 1.0f);
                sample.moisture = 1.0f - sample.slope;
            }
        }
        return sample;
    }

private:
    std::atomic<bool> m_ready{false};
};

// Owner-thread admission and completion ledger. At most eight outstanding
// items (1 MiB of candidate output), four new items per frame. Payload/query
// owners are shared with resident terrain; no mesh arrays are copied on enqueue.
// Destroying the ledger never waits: executors retain each item's lifetime.
class FoliageGroundQueue {
public:
    static constexpr std::size_t kInFlight = 8;
    static constexpr std::size_t kPerFrame = 4;
    struct Stats {
        std::uint64_t dispatched = 0;
        std::uint64_t completed = 0;
        std::uint64_t stale_rejected = 0;
        std::uint64_t max_age_frames = 0;
    };
    void begin_frame() {
        ++m_frame;
        m_submitted = 0;
        clear_stats();
    }
    void clear_stats() {
        m_stats = {};
    }
    bool can_enqueue(FoliageSurfaceSlots::Key key) const {
        if (m_submitted == kPerFrame)
            return false;
        bool free = false;
        for (const auto& slot : m_slots) {
            if (slot.build && slot.build->input.key == key)
                return false;
            free = free || !slot.build;
        }
        return free;
    }
    std::shared_ptr<FoliageGroundBuild> enqueue(FoliageGroundRequest input) {
        if (!can_enqueue(input.key))
            return {};
        for (auto& slot : m_slots) {
            if (!slot.build) {
                slot.build = std::make_shared<FoliageGroundBuild>(std::move(input));
                slot.frame = m_frame;
                slot.invalidated = false;
                ++m_submitted;
                ++m_stats.dispatched;
                return slot.build;
            }
        }
        return {};
    }
    // Edits invalidate even an absent -> present -> absent transition while a
    // job is running. Retain its slot until completion, so edits cannot flood
    // the executor with replacement jobs or free snapshots still in use.
    void invalidate(std::optional<std::uint64_t> column = std::nullopt) {
        for (auto& slot : m_slots)
            if (slot.build && (!column || slot.build->input.key.column == *column))
                slot.invalidated = true;
    }
    // Only for executor admission rejection: the item has never been run.
    void withdraw(const std::shared_ptr<FoliageGroundBuild>& build) {
        if (!build)
            return;
        for (auto& slot : m_slots) {
            if (slot.build == build) {
                slot.build.reset();
                if (m_stats.dispatched != 0)
                    --m_stats.dispatched;
                return;
            }
        }
    }
    template<typename Validate, typename Consume>
    void poll(Validate validate, Consume consume) {
        for (auto& slot : m_slots) {
            if (!slot.build)
                continue;
            m_stats.max_age_frames = std::max(m_stats.max_age_frames, m_frame - slot.frame);
            if (!slot.build->ready())
                continue;
            ++m_stats.completed;
            if (slot.invalidated || !validate(slot.build->input))
                ++m_stats.stale_rejected;
            else if (!slot.build->failed)
                consume(*slot.build);
            slot.build.reset();
        }
    }
    const Stats& stats() const {
        return m_stats;
    }

private:
    struct Slot {
        std::shared_ptr<FoliageGroundBuild> build;
        std::uint64_t frame = 0;
        bool invalidated = false;
    };
    std::array<Slot, kInFlight> m_slots{};
    std::uint64_t m_frame = 0;
    std::size_t m_submitted = 0;
    Stats m_stats{};
};

} // namespace Luminumbra::Rendering
