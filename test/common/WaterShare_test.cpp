#include <gtest/gtest.h>

#include "luminumbra_common/world/WaterShare.h"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace W = Luminumbra::world::water_share;
using Luminumbra::world::RegionKey;
using Luminumbra::world::RegionState;

// Independent arbitrary-precision decimal oracle: production uses binary limbs.
// Each multiplication factor is <=2^32; decimal carries and division remainders
// fit u64 while the complete numerator can be arbitrarily wider than u64.
std::string MultiplyDecimal(std::string digits, std::uint64_t factor) {
    std::uint64_t carry = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        const auto product = static_cast<std::uint64_t>(*it - '0') * factor + carry;
        *it = static_cast<char>('0' + product % 10);
        carry = product / 10;
    }
    while (carry != 0) {
        digits.insert(digits.begin(), static_cast<char>('0' + carry % 10));
        carry /= 10;
    }
    const auto first = digits.find_first_not_of('0');
    return first == std::string::npos ? "0" : digits.substr(first);
}
std::pair<std::uint64_t, std::uint64_t> DivideDecimal(const std::string& digits,
                                                      std::uint64_t divisor) {
    std::uint64_t quotient = 0, remainder = 0;
    for (const char digit : digits) {
        remainder = remainder * 10 + static_cast<unsigned>(digit - '0');
        quotient = quotient * 10 + remainder / divisor;
        remainder %= divisor;
    }
    return {quotient, remainder};
}

bool Oracle(std::uint64_t budget,
            const std::vector<W::Claim>& claims,
            std::map<RegionKey, std::uint64_t>& debts,
            std::vector<W::Grant>& grants) {
    auto next = debts;
    std::vector<W::Grant> result;
    if (budget == 0 || claims.empty()) {
        grants = result;
        return true;
    }
    std::uint64_t total = 0;
    for (const auto& claim : claims)
        total += claim.awake;
    struct Credit {
        RegionKey key;
        std::uint64_t awake, share, remainder;
    };
    std::vector<Credit> credit;
    std::uint64_t sum = 0;
    for (const auto& claim : claims) {
        auto fraction =
            total == 0
                ? std::pair<std::uint64_t, std::uint64_t>{0, 0}
                : DivideDecimal(MultiplyDecimal(
                                    MultiplyDecimal(std::to_string(claim.awake), budget), W::kQ32),
                                total);
        credit.push_back({claim.key, claim.awake, fraction.first, fraction.second});
        sum += fraction.first;
    }
    if (total != 0) {
        std::sort(credit.begin(), credit.end(), [](const auto& a, const auto& b) {
            return a.remainder > b.remainder || (a.remainder == b.remainder && a.key < b.key);
        });
        for (std::uint64_t i = 0; i < budget * W::kQ32 - sum; ++i)
            ++credit[static_cast<std::size_t>(i)].share;
    }
    for (const auto& entry : credit) {
        const auto old = next.contains(entry.key) ? next.at(entry.key) : 0;
        if ((total != 0 && entry.awake != 0 && entry.share == 0) || entry.share > W::kMaxDebt - old)
            return false;
        if (entry.share != 0)
            next[entry.key] = old + entry.share;
    }
    // Independent selection: scan the remaining candidates for the largest
    // original post-credit debt. Removing a recipient never changes the ranking.
    auto remaining = budget;
    while (!credit.empty() && remaining != 0) {
        const auto best =
            std::max_element(credit.begin(), credit.end(), [&next](const auto& a, const auto& b) {
                const auto av = next.contains(a.key) ? next.at(a.key) : 0;
                const auto bv = next.contains(b.key) ? next.at(b.key) : 0;
                return av < bv || (av == bv && b.key < a.key);
            });
        const auto key = best->key;
        credit.erase(best);
        if (!next.contains(key))
            continue;
        const auto windows = std::min(next.at(key) / W::kQ32, remaining);
        if (windows == 0)
            continue;
        result.push_back({key, windows});
        next.at(key) -= windows * W::kQ32;
        remaining -= windows;
        if (next.at(key) == 0)
            next.erase(key);
    }
    debts = next;
    grants = result;
    return true;
}
std::uint64_t DebtTotal(const std::map<RegionKey, std::uint64_t>& debts) {
    std::uint64_t total = 0;
    for (const auto& [key, debt] : debts) {
        (void)key;
        total += debt;
    }
    return total;
}
std::uint64_t GrantTotal(const std::vector<W::Grant>& grants) {
    std::uint64_t total = 0;
    for (const auto& grant : grants)
        total += grant.windows;
    return total;
}
} // namespace

TEST(WaterShare, ThreeEqualOneWindowCreditsAllQ32UnitsAndServicesEveryOwner) {
    const std::vector<W::Claim> claims{{{0, 0}, 1}, {{1, 0}, 1}, {{2, 0}, 1}};
    std::map<RegionKey, std::uint64_t> debts;
    std::vector<W::Grant> grants;
    ASSERT_TRUE(W::allocate(1, claims, debts, grants));
    EXPECT_EQ(debts.at({0, 0}), 1431655766u);
    EXPECT_EQ(debts.at({1, 0}), 1431655765u);
    EXPECT_EQ(debts.at({2, 0}), 1431655765u);
    EXPECT_EQ(DebtTotal(debts), W::kQ32);
    EXPECT_TRUE(grants.empty());
    ASSERT_TRUE(W::allocate(1, claims, debts, grants));
    EXPECT_TRUE(grants.empty());
    for (int tick = 3; tick <= 5; ++tick) {
        ASSERT_TRUE(W::allocate(1, claims, debts, grants));
        ASSERT_EQ(grants.size(), 1u);
        EXPECT_EQ(grants.front().key, (RegionKey{tick - 3, 0}));
        EXPECT_EQ(grants.front().windows, 1u);
    }
    debts.clear();
    ASSERT_TRUE(W::allocate(3, claims, debts, grants));
    EXPECT_EQ(grants, (std::vector<W::Grant>{{{0, 0}, 1}, {{1, 0}, 1}, {{2, 0}, 1}}));
    EXPECT_TRUE(debts.empty());
}

TEST(WaterShare, PortableWideNumeratorMatchesIndependentBigIntegerOracle) {
    std::uint64_t rng = 0x2635623493245u;
    const auto random = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };
    for (int trial = 0; trial < 120; ++trial) {
        const std::uint64_t budget = trial == 0 ? W::kMaxWindowBudget : 1 + random() % 1000;
        std::vector<W::Claim> claims;
        for (int owner = 0; owner < 1 + trial % 31; ++owner)
            claims.push_back({{owner, -owner}, 1 + random() % W::kMaxAwake});
        std::map<RegionKey, std::uint64_t> production, reference;
        std::vector<W::Grant> actual, expected;
        for (int tick = 0; tick < 24; ++tick) {
            const auto before = DebtTotal(production);
            const bool ok = W::allocate(budget, claims, production, actual);
            const bool oracle_ok = Oracle(budget, claims, reference, expected);
            ASSERT_EQ(ok, oracle_ok);
            EXPECT_EQ(production, reference);
            EXPECT_EQ(actual, expected);
            if (ok) {
                EXPECT_LE(GrantTotal(actual), budget);
                EXPECT_EQ(before + budget * W::kQ32,
                          DebtTotal(production) + GrantTotal(actual) * W::kQ32);
            }
        }
    }
}

TEST(WaterShare, OverflowCapAndZeroPositiveCreditRefuseBothOutputsAtomically) {
    const std::vector<W::Claim> one{{{0, 0}, 1}};
    std::map<RegionKey, std::uint64_t> debts{{{0, 0}, W::kMaxDebt}};
    std::vector<W::Grant> grants{{{9, 9}, 7}};
    const auto old_debts = debts;
    const auto old_grants = grants;
    EXPECT_FALSE(W::allocate(1, one, debts, grants));
    EXPECT_EQ(debts, old_debts);
    EXPECT_EQ(grants, old_grants);
    debts.clear();
    std::vector<W::Claim> skew{{{0, 0}, 1}};
    for (int owner = 1; owner <= 16; ++owner)
        skew.push_back({{owner, 0}, W::kMaxAwake});
    EXPECT_FALSE(W::allocate(1, skew, debts, grants));
    EXPECT_TRUE(debts.empty());
    EXPECT_EQ(grants, old_grants);
    const std::vector<W::Claim> unordered{{{1, 0}, 1}, {{0, 0}, 1}};
    EXPECT_FALSE(W::allocate(1, unordered, debts, grants));
    EXPECT_FALSE(W::allocate(W::kMaxWindowBudget + 1, one, debts, grants));
    EXPECT_EQ(grants, old_grants);
}

TEST(WaterShare, EmptyZeroBudgetZeroAwakeAndAbsentDebtHaveExplicitBehavior) {
    std::map<RegionKey, std::uint64_t> debts{{{0, 0}, 2 * W::kQ32}, {{8, 8}, W::kQ32}};
    std::vector<W::Grant> grants{{{9, 9}, 7}};
    const auto original = debts;
    EXPECT_TRUE(W::allocate(1, {}, debts, grants));
    EXPECT_EQ(debts, original);
    EXPECT_TRUE(grants.empty());
    const std::vector<W::Claim> zero{{{0, 0}, 0}};
    EXPECT_TRUE(W::allocate(0, zero, debts, grants));
    EXPECT_EQ(debts, original);
    EXPECT_TRUE(W::allocate(1, zero, debts, grants));
    EXPECT_EQ(grants, (std::vector<W::Grant>{{{0, 0}, 1}}));
    EXPECT_EQ(debts.at({8, 8}), W::kQ32);
}

TEST(WaterShare, FixedTotalBoundAndPerOwnerServiceBoundHoldUnderFixedShares) {
    const std::vector<W::Claim> claims{{{0, 0}, 1}, {{1, 0}, 1}, {{2, 0}, 1}};
    std::map<RegionKey, std::uint64_t> debts;
    std::vector<W::Grant> grants;
    const auto fixed_bound = 3 * W::kQ32 - 1;
    std::map<RegionKey, std::uint64_t> deadlines;
    for (std::uint64_t tick = 1; tick <= 600; ++tick) {
        ASSERT_TRUE(W::allocate(1, claims, debts, grants));
        EXPECT_LE(DebtTotal(debts), fixed_bound);
        for (const auto& grant : grants)
            deadlines.erase(grant.key);
        for (const auto& claim : claims) {
            if (deadlines.contains(claim.key)) {
                EXPECT_GT(deadlines.at(claim.key), tick);
                continue;
            }
            std::uint64_t bound = 0;
            const auto debt = debts.contains(claim.key) ? debts.at(claim.key) : 0;
            ASSERT_TRUE(W::service_due_tick_bound(
                fixed_bound, debt, claim.key.x == 0 ? 1431655766u : 1431655765u, bound));
            deadlines[claim.key] = tick + bound;
        }
    }
    std::uint64_t untouched = 77;
    EXPECT_FALSE(W::service_due_tick_bound(9, 10, 1, untouched));
    EXPECT_FALSE(W::service_due_tick_bound(9, 0, 0, untouched));
    EXPECT_FALSE(
        W::service_due_tick_bound(std::numeric_limits<std::uint64_t>::max(), 0, 1, untouched));
    EXPECT_EQ(untouched, 77u);
}

TEST(WaterShare, BorderReservationChargesBothOrNeither) {
    std::map<RegionKey, std::uint64_t> remaining{{{0, 0}, 1}, {{1, 0}, 2}};
    const auto before = remaining;
    EXPECT_FALSE(
        W::reserve_border(remaining, {0, 0}, RegionState::Active, {1, 0}, RegionState::Frozen));
    EXPECT_EQ(remaining, before);
    EXPECT_FALSE(
        W::reserve_border(remaining, {0, 0}, RegionState::Active, {2, 0}, RegionState::Active));
    EXPECT_EQ(remaining, before);
    EXPECT_TRUE(
        W::reserve_border(remaining, {0, 0}, RegionState::Active, {1, 0}, RegionState::Reduced));
    EXPECT_EQ(remaining.at({0, 0}), 0u);
    EXPECT_EQ(remaining.at({1, 0}), 1u);
    EXPECT_THROW(
        W::reserve_border(remaining, {0, 0}, RegionState::Active, {0, 0}, RegionState::Active),
        std::invalid_argument);
}
