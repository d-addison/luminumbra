#pragma once

// Integer, atomic allocation of the enabled per-tick water-window budget.
#include "ActiveRegionLedger.h"

#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace Luminumbra::world::water_share {
inline constexpr std::uint64_t kQ32 = std::uint64_t{1} << 32;
inline constexpr std::uint64_t kMaxWindowBudget = std::uint64_t{1} << 20;
inline constexpr std::uint64_t kMaxAwake = std::uint64_t{1} << 30;
inline constexpr std::uint64_t kMaxTotalAwake = std::uint64_t{1} << 50;
inline constexpr std::uint64_t kMaxDebt = std::uint64_t{1} << 62;

struct Claim {
    RegionKey key;
    std::uint64_t awake = 0;
};
struct Grant {
    RegionKey key;
    std::uint64_t windows = 0;
    bool operator==(const Grant&) const = default;
};

// Strictly ascending unique due claims. For A>0, credit floor(W*awake*Q/A), then
// distribute missing Q32 units by remainder descending, key ascending. A positive
// awake claim receiving zero credit refuses. Checked credit overflow/cap refuses
// atomically; no saturation. Sort ONCE after credit by debt descending/key, granting
// bulk min(debt/Q,remainingW) to current claims only. Absent owners are untouched.
// Empty claims or W=0 replace grants with empty; A=0 credits nothing but can service
// existing due debt. All validation still applies. False changes neither output.
[[nodiscard]] bool allocate(std::uint64_t window_budget,
                            std::span<const Claim> claims,
                            std::map<RegionKey, std::uint64_t>& debts,
                            std::vector<Grant>& grants);

// Under fixed successful positive credited shares and fixed eligibility/W, total
// debt is bounded by B=max(initial total debt,n*Q-1). For completed debt d and share
// s, next service occurs within floor((B-d)/s)+1 due ticks. B is the FIXED bound,
// not the current total. Refuse invalid inputs or a nonrepresentable bound.
[[nodiscard]] bool service_due_tick_bound(std::uint64_t fixed_total_bound,
                                          std::uint64_t completed_debt,
                                          std::uint64_t credited_share,
                                          std::uint64_t& out);

enum class BorderMode : std::uint8_t {
    Open,
    Sealed
};
[[nodiscard]] BorderMode border_mode(RegionState a, RegionState b);
// Open reservations charge both owners or neither. Sealed/missing/empty owners
// return false unchanged; equal owners throw invalid_argument.
[[nodiscard]] bool reserve_border(std::map<RegionKey, std::uint64_t>& remaining,
                                  RegionKey a,
                                  RegionState state_a,
                                  RegionKey b,
                                  RegionState state_b);
} // namespace Luminumbra::world::water_share
