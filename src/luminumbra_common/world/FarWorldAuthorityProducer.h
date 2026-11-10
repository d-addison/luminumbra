#pragma once

#include "FarWorldAuthorityChanges.h"
#include "persistence/WorldSaveService.h"
#include <bitset>
#include <span>
#include <vector>

namespace Luminumbra::World {

// Inactive owner-thread source service. Copies require the world's publication
// boundary; no worker receives the index, mutable chunks, or the saved reader.
class FarWorldAuthorityProducer final {
public:
    static constexpr std::size_t kSlots = 512;
    struct Limits {
        std::size_t index_records = 0;
        std::uint64_t copy_bytes = 0;
        std::uint64_t query_columns = 0;
    };
    enum class Error {
        None,
        InvalidLimits,
        InvalidWindow,
        IndexCapacity,
        CopyLimit,
        AuthorityUnavailable,
        WaterUnavailable,
        AllocationFailure
    };
    struct Image {
        IVec3 coords{};
        bool parked = false, terrain_authority = false, water_authority = false;
        int water_resolution = 0;
        std::vector<float> sdf;
        std::vector<std::uint8_t> material;
        std::vector<std::int32_t> bed, depth;
    };
    struct DirtyTiles {
        std::bitset<kSlots> tiles;
        std::uint64_t visited = 0, terrain = 0, water = 0;
        bool overflow = false;
    };
    struct Counters {
        std::uint64_t tick_items = 0, query_items = 0;
        std::uint64_t copied_records = 0, copied_bytes = 0, dirty_overflows = 0;
    };
    explicit FarWorldAuthorityProducer(const Limits& limits);
    ~FarWorldAuthorityProducer();
    FarWorldAuthorityProducer(const FarWorldAuthorityProducer&) = delete;
    FarWorldAuthorityProducer& operator=(const FarWorldAuthorityProducer&) = delete;
    std::shared_ptr<FarWorldAuthorityChanges> changes() const;
    // Windows include the complete sampling/normal halo. Replacement is
    // transactional; at most 512 descriptors and dirty bits are retained.
    bool set_wanted(std::span<const Persistence::WorldSaveService::AuthorityWindow> wanted);
    DirtyTiles tick();
    Counters counters() const;
    // Refusal preserves output. Only published owned terrain and integer water
    // are copied; resident generation and staged arrays never become authority.
    Error copy_published(const Persistence::WorldSaveService::AuthorityWindow& window,
                         std::vector<Image>& output);
    bool admit_saved_source(const std::filesystem::path& root,
                            const Persistence::WorldSaveService::AuthorityReadLimits& limits,
                            std::string& error);
    Persistence::WorldSaveService::AuthorityWindowResult
    read_committed(const Persistence::WorldSaveService::AuthorityWindow& window,
                   const Persistence::WorldSaveService::AuthorityReadLimits& limits) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace Luminumbra::World
