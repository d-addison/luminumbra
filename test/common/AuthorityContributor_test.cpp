#include "luminumbra_common/persistence/AuthorityContributor.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace P = Luminumbra::Persistence;

class Contributor final : public P::IAuthorityStateContributor {
public:
    Contributor(std::string id, std::string state)
        : m_id(std::move(id))
        , bytes(std::move(state)) {}

    std::string_view contributor_id() const noexcept override {
        return m_id;
    }
    std::string_view canonical_state() const noexcept override {
        ++reads;
        return bytes;
    }

private:
    std::string m_id;

public:
    std::string bytes;
    mutable std::size_t reads = 0;
};

std::string WorldHash(const P::AuthorityContributorRegistry& registry) {
    return P::ComposeWorldHash(
        "chunk", "wind", "weather", "aether", "scent", "ecology", "plant", registry);
}

constexpr char kCanonicalPair[] = "|authorityContributors:v1:"
                                  "\x02\0\0\0\0\0\0\0"
                                  "\x05\0\0\0\0\0\0\0alpha"
                                  "\x01\0\0\0\0\0\0\0A"
                                  "\x04\0\0\0\0\0\0\0zeta"
                                  "\x01\0\0\0\0\0\0\0Z";

TEST(AuthorityContributor, EmptyRegistryPreservesLegacyGoldenHashes) {
    const P::AuthorityContributorRegistry registry;
    EXPECT_TRUE(registry.SerializeCanonicalState().empty());
    EXPECT_EQ(
        WorldHash(registry),
        P::ComposeWorldHash("chunk", "wind", "weather", "aether", "scent", "ecology", "plant"));
    EXPECT_EQ(WorldHash(registry), "76950c71eb8bf95e");
    EXPECT_EQ(P::ComposeWorldHash("", "", "", "", "", "", "", registry), "9deb9f80f9012248");
}

TEST(AuthorityContributor, OnlyEmptyStatesPreserveLegacyGoldenHashes) {
    Contributor alpha("alpha", "");
    Contributor zeta("zeta", "");
    P::AuthorityContributorRegistry registry;
    ASSERT_TRUE(registry.Register(zeta));
    ASSERT_TRUE(registry.Register(alpha));
    EXPECT_TRUE(registry.SerializeCanonicalState().empty());
    EXPECT_TRUE(alpha.ComputeHash().empty());
    EXPECT_EQ(WorldHash(registry), "76950c71eb8bf95e");
    EXPECT_EQ(P::ComposeWorldHash("", "", "", "", "", "", "", registry), "9deb9f80f9012248");
}

TEST(AuthorityContributor, CanonicalBytesAndHashIgnoreEveryRegistrationPermutation) {
    Contributor alpha("alpha", "A");
    Contributor empty("middle", "");
    Contributor zeta("zeta", "Z");
    std::array<const Contributor*, 3> contributors{&alpha, &empty, &zeta};
    std::array<std::size_t, 3> order{0, 1, 2};
    do {
        P::AuthorityContributorRegistry registry;
        for (const auto index : order) {
            ASSERT_TRUE(registry.Register(*contributors[index]));
        }
        EXPECT_EQ(registry.SerializeCanonicalState(),
                  std::string(kCanonicalPair, sizeof(kCanonicalPair) - 1));
        EXPECT_EQ(WorldHash(registry), "50ef200c7aa942c8");
    } while (std::next_permutation(order.begin(), order.end()));
    EXPECT_EQ(alpha.ComputeHash(), "af63fc4c860222ec");
}

TEST(AuthorityContributor, StateAndIdBoundariesAndEmbeddedZeroBytesAffectHash) {
    Contributor first("a", "bc");
    Contributor second("ab", "c");
    P::AuthorityContributorRegistry left;
    P::AuthorityContributorRegistry right;
    ASSERT_TRUE(left.Register(first));
    ASSERT_TRUE(right.Register(second));
    EXPECT_NE(left.SerializeCanonicalState(), right.SerializeCanonicalState());
    EXPECT_NE(WorldHash(left), WorldHash(right));
    second.bytes = first.bytes;
    EXPECT_NE(WorldHash(left), WorldHash(right));
    first.bytes = std::string("x\0y", 3);
    const auto before = WorldHash(left);
    const auto state_before = first.ComputeHash();
    first.bytes.back() = 'z';
    EXPECT_NE(WorldHash(left), before);
    EXPECT_NE(first.ComputeHash(), state_before);
    EXPECT_NE(WorldHash(left), "76950c71eb8bf95e");
    first.bytes.clear();
    EXPECT_EQ(WorldHash(left), "76950c71eb8bf95e");
}

TEST(AuthorityContributor, InvalidAndDuplicateIdsLeaveRegistryUnchanged) {
    Contributor original("valid:state-1.0_x", "state");
    P::AuthorityContributorRegistry registry;
    ASSERT_TRUE(registry.Register(original));
    const auto before = registry.SerializeCanonicalState();
    const std::vector<std::string> invalid{
        "",
        "Uppercase",
        "with space",
        "slash/path",
        "pipe|",
        std::string("a\0b", 3),
        std::string(1, '\xff'),
        std::string(P::AuthorityContributorRegistry::kMaxIdBytes + 1, 'a')};
    for (const auto& id : invalid) {
        Contributor rejected(id, "different");
        EXPECT_FALSE(registry.Register(rejected));
        EXPECT_EQ(registry.SerializeCanonicalState(), before);
    }
    Contributor duplicate("valid:state-1.0_x", "different");
    EXPECT_FALSE(registry.Register(duplicate));
    EXPECT_FALSE(registry.Register(original));
    EXPECT_EQ(registry.SerializeCanonicalState(), before);
    Contributor longest(std::string(P::AuthorityContributorRegistry::kMaxIdBytes, 'z'), "state");
    EXPECT_TRUE(registry.Register(longest));
}

TEST(AuthorityContributor, RegistrationBoundAndSingleReadPerContributor) {
    std::vector<Contributor> contributors;
    contributors.reserve(P::AuthorityContributorRegistry::kMaxContributors + 1);
    for (std::size_t index = 0; index <= P::AuthorityContributorRegistry::kMaxContributors;
         ++index) {
        contributors.emplace_back("id:" + std::to_string(index), "x");
    }
    P::AuthorityContributorRegistry registry;
    for (std::size_t index = 0; index < P::AuthorityContributorRegistry::kMaxContributors;
         ++index) {
        ASSERT_TRUE(registry.Register(contributors[index]));
    }
    EXPECT_FALSE(registry.Register(contributors.back()));
    const auto before = registry.SerializeCanonicalState();
    const std::string prefix = "|authorityContributors:v1:";
    EXPECT_EQ(before.substr(prefix.size(), 8), std::string("\0\x01\0\0\0\0\0\0", 8));
    for (std::size_t index = 0; index < P::AuthorityContributorRegistry::kMaxContributors;
         ++index) {
        EXPECT_EQ(contributors[index].reads, 1u);
    }
    EXPECT_EQ(contributors.back().reads, 0u);
    EXPECT_FALSE(registry.Register(contributors.front()));
    EXPECT_EQ(registry.SerializeCanonicalState(), before);
}

TEST(AuthorityContributor, CanonicalByteLimitIsExactAndOversizeCanRecover) {
    Contributor contributor("a", "");
    P::AuthorityContributorRegistry registry;
    ASSERT_TRUE(registry.Register(contributor));
    // Prefix, count, two lengths and the one-byte ID occupy exactly 51 bytes.
    contributor.bytes.assign(P::AuthorityContributorRegistry::kMaxCanonicalBytes - 51, 'x');
    EXPECT_EQ(registry.SerializeCanonicalState().size(),
              P::AuthorityContributorRegistry::kMaxCanonicalBytes);
    contributor.bytes.push_back('x');
    EXPECT_THROW(registry.SerializeCanonicalState(), std::length_error);
    EXPECT_THROW(WorldHash(registry), std::length_error);
    contributor.bytes.assign(P::AuthorityContributorRegistry::kMaxCanonicalBytes + 1, 'x');
    EXPECT_THROW(contributor.ComputeHash(), std::length_error);
    EXPECT_THROW(registry.SerializeCanonicalState(), std::length_error);
    contributor.bytes.clear();
    EXPECT_EQ(WorldHash(registry), "76950c71eb8bf95e");
}

TEST(AuthorityContributor, AggregateSizeLimitIncludesEveryRecord) {
    Contributor first("a", "x");
    Contributor second("b", "x");
    P::AuthorityContributorRegistry registry;
    ASSERT_TRUE(registry.Register(first));
    ASSERT_TRUE(registry.Register(second));
    first.bytes.assign(P::AuthorityContributorRegistry::kMaxCanonicalBytes / 2, 'x');
    second.bytes.assign(P::AuthorityContributorRegistry::kMaxCanonicalBytes / 2, 'y');
    EXPECT_THROW(registry.SerializeCanonicalState(), std::length_error);
    second.bytes.clear();
    EXPECT_FALSE(registry.SerializeCanonicalState().empty());
    first.bytes.assign(P::AuthorityContributorRegistry::kMaxCanonicalBytes - 51, 'x');
    EXPECT_EQ(registry.SerializeCanonicalState().size(),
              P::AuthorityContributorRegistry::kMaxCanonicalBytes);
    second.bytes = "y";
    EXPECT_THROW(registry.SerializeCanonicalState(), std::length_error);
}
} // namespace
