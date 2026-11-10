#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <unordered_set>

#include "luminumbra_common/fields/FieldPageState.h"
#include "luminumbra_common/systems/WeatherSystem.h"

namespace Luminumbra::Rendering {

// Render-only consumption history; never feeds back into simulation state.
class LiveStrikeTracker {
public:
    static constexpr std::uint64_t kGraceTicks = 2;
    static constexpr std::size_t kWindowTicks = kGraceTicks + 1;
    // Each tick bucket holds at most the global regional strike limit, which also covers
    // the smaller legacy schedule. Only consumed strikes allocate nodes; there are three buckets.
    static constexpr std::size_t kStrikesPerTick = world::FieldPageState::kMaxStrikes;
    static constexpr std::size_t kCapacity = kWindowTicks * kStrikesPerTick;

    // Call once per schedule poll, including empty schedules, to reclaim expired identities.
    void AdvanceTo(std::uint64_t tick) {
        current_tick_ = tick;
        for (auto& bucket : buckets_) {
            if (!bucket.identities.empty() &&
                (bucket.tick > tick || tick - bucket.tick > kGraceTicks)) {
                bucket.identities.clear();
            }
        }
    }

    [[nodiscard]] bool TryConsume(const Systems::StrikeEvent& strike) {
        if (strike.strike_tick > current_tick_ ||
            current_tick_ - strike.strike_tick > kGraceTicks) {
            return false;
        }
        auto& bucket = buckets_[strike.strike_tick % kWindowTicks];
        if (bucket.identities.size() == kStrikesPerTick) {
            return false;
        }
        const Identity identity{strike.storm_salt,
                                std::bit_cast<std::uint32_t>(strike.world_x),
                                std::bit_cast<std::uint32_t>(strike.world_z)};
        bucket.tick = strike.strike_tick;
        return bucket.identities.insert(identity).second;
    }

    void Clear() {
        for (auto& bucket : buckets_) {
            bucket.identities.clear();
        }
        current_tick_ = 0;
    }

    // Number of retained consumed identities in the active presentation window.
    [[nodiscard]] std::size_t size() const {
        return buckets_[0].identities.size() + buckets_[1].identities.size() +
               buckets_[2].identities.size();
    }

private:
    struct Identity {
        std::uint32_t storm_salt = 0;
        std::uint32_t world_x = 0;
        std::uint32_t world_z = 0;

        bool operator==(const Identity&) const = default;
    };

    struct IdentityHash {
        std::size_t operator()(const Identity& identity) const {
            std::uint64_t hash = 14695981039346656037ull;
            hash = (hash ^ identity.storm_salt) * 1099511628211ull;
            hash = (hash ^ identity.world_x) * 1099511628211ull;
            hash = (hash ^ identity.world_z) * 1099511628211ull;
            return static_cast<std::size_t>(hash);
        }
    };

    struct Bucket {
        std::uint64_t tick = 0;
        std::unordered_set<Identity, IdentityHash> identities;
    };

    std::array<Bucket, kWindowTicks> buckets_{};
    std::uint64_t current_tick_ = 0;
};

} // namespace Luminumbra::Rendering
