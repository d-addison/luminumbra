#include "WaterShare.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>

namespace Luminumbra::world::water_share {
namespace {
struct Wide {
    std::uint64_t high;
    std::uint64_t low;
};
bool ValidKey(RegionKey key) {
    return key.x >= -32768 && key.x <= 32767 && key.z >= -32768 && key.z <= 32767;
}
// Long division of portable limbs. The checked domain guarantees divisor<=2^50,
// so doubling the partial remainder never overflows. Quotients fit W*Q<=2^52.
bool Divide(Wide numerator,
            std::uint64_t divisor,
            std::uint64_t& quotient,
            std::uint64_t& remainder) {
    quotient = 0;
    remainder = 0;
    for (int bit = 127; bit >= 0; --bit) {
        const auto digit =
            bit >= 64 ? (numerator.high >> (bit - 64)) & 1u : (numerator.low >> bit) & 1u;
        remainder = remainder * 2 + digit;
        if (remainder < divisor)
            continue;
        remainder -= divisor;
        if (bit >= 64)
            return false;
        quotient |= std::uint64_t{1} << bit;
    }
    return true;
}
struct Credit {
    RegionKey key;
    std::uint64_t awake;
    std::uint64_t share;
    std::uint64_t remainder;
};
} // namespace

bool allocate(std::uint64_t window_budget,
              std::span<const Claim> claims,
              std::map<RegionKey, std::uint64_t>& debts,
              std::vector<Grant>& grants) {
    if (window_budget > kMaxWindowBudget || claims.size() > ActiveRegionLedger::kMaxRegions ||
        debts.size() > ActiveRegionLedger::kMaxRegions)
        return false;
    for (const auto& [key, debt] : debts) {
        if (!ValidKey(key) || debt == 0 || debt > kMaxDebt)
            return false;
    }
    std::uint64_t total_awake = 0;
    std::optional<RegionKey> previous;
    for (const auto& claim : claims) {
        if (!ValidKey(claim.key) || claim.awake > kMaxAwake ||
            (previous && claim.key <= previous.value()) ||
            claim.awake > kMaxTotalAwake - total_awake)
            return false;
        total_awake += claim.awake;
        previous = claim.key;
    }
    auto next = debts;
    std::vector<Grant> result;
    if (window_budget == 0 || claims.empty()) {
        grants.swap(result);
        return true;
    }
    std::vector<Credit> credits;
    credits.reserve(claims.size());
    std::uint64_t credited = 0;
    for (const auto& claim : claims) {
        std::uint64_t share = 0, remainder = 0;
        if (total_awake != 0) {
            const auto product = window_budget * claim.awake;
            if (!Divide({product >> 32, product << 32}, total_awake, share, remainder))
                return false;
        }
        credits.push_back({claim.key, claim.awake, share, remainder});
        credited += share;
    }
    if (total_awake != 0) {
        const auto missing = window_budget * kQ32 - credited;
        if (missing >= credits.size())
            return false;
        std::sort(credits.begin(), credits.end(), [](const Credit& a, const Credit& b) {
            return a.remainder != b.remainder ? a.remainder > b.remainder : a.key < b.key;
        });
        for (std::uint64_t index = 0; index < missing; ++index)
            ++credits[static_cast<std::size_t>(index)].share;
    }
    for (const auto& credit : credits) {
        if (total_awake != 0 && credit.awake != 0 && credit.share == 0)
            return false;
        const auto found = next.find(credit.key);
        const auto old = found == next.end() ? 0 : found->second;
        if (credit.share > kMaxDebt - old)
            return false;
        if (credit.share != 0)
            next.insert_or_assign(credit.key, old + credit.share);
    }
    if (next.size() > ActiveRegionLedger::kMaxRegions)
        return false;
    // Remainder allocation is complete; this single ranking remains fixed while
    // whole windows are deducted. Do not repeatedly reprioritize grant recipients.
    std::sort(credits.begin(), credits.end(), [&next](const Credit& a, const Credit& b) {
        const auto ad = next.find(a.key), bd = next.find(b.key);
        const auto av = ad == next.end() ? 0 : ad->second;
        const auto bv = bd == next.end() ? 0 : bd->second;
        return av != bv ? av > bv : a.key < b.key;
    });
    auto remaining = window_budget;
    for (const auto& credit : credits) {
        if (remaining == 0)
            break;
        const auto debt = next.find(credit.key);
        if (debt == next.end())
            continue;
        const auto windows = std::min(debt->second / kQ32, remaining);
        if (windows == 0)
            continue;
        debt->second -= windows * kQ32;
        result.push_back({credit.key, windows});
        remaining -= windows;
        if (debt->second == 0)
            next.erase(debt);
    }
    debts.swap(next);
    grants.swap(result);
    return true;
}

bool service_due_tick_bound(std::uint64_t fixed_total_bound,
                            std::uint64_t completed_debt,
                            std::uint64_t credited_share,
                            std::uint64_t& out) {
    if (credited_share == 0 || completed_debt > fixed_total_bound)
        return false;
    const auto ticks = (fixed_total_bound - completed_debt) / credited_share;
    if (ticks == std::numeric_limits<std::uint64_t>::max())
        return false;
    out = ticks + 1;
    return true;
}

BorderMode border_mode(RegionState a, RegionState b) {
    return a == RegionState::Frozen || b == RegionState::Frozen ? BorderMode::Sealed
                                                                : BorderMode::Open;
}
bool reserve_border(std::map<RegionKey, std::uint64_t>& remaining,
                    RegionKey a,
                    RegionState state_a,
                    RegionKey b,
                    RegionState state_b) {
    if (a == b)
        throw std::invalid_argument("Water border requires two distinct owners.");
    if (border_mode(state_a, state_b) == BorderMode::Sealed)
        return false;
    const auto first = remaining.find(a), second = remaining.find(b);
    if (first == remaining.end() || second == remaining.end() || first->second == 0 ||
        second->second == 0)
        return false;
    --first->second;
    --second->second;
    return true;
}
} // namespace Luminumbra::world::water_share
