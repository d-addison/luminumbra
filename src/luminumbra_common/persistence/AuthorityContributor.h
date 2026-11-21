#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Luminumbra::Persistence {

// In-process authority extension; this C++ contract is not a dynamic-library ABI.
// State is canonical binary data: explicit byte order, stable ordering, no padding
// or addresses. An empty view means no authority state and contributes no bytes.
class IAuthorityStateContributor {
public:
    virtual ~IAuthorityStateContributor() = default;
    [[nodiscard]] virtual std::string_view contributor_id() const noexcept = 0;
    [[nodiscard]] virtual std::string_view canonical_state() const noexcept = 0;
    // The same stable checksum used by world hashes; empty state returns empty.
    [[nodiscard]] std::string ComputeHash() const;
};

// Owner-thread registry. Contributors outlive it; IDs remain stable. Neither
// registry nor contributor state may mutate during serialization/hashing, and
// returned state views remain valid until the owner next mutates that state.
class AuthorityContributorRegistry {
public:
    // Bound registration work, snapshot storage and canonical output allocation.
    static constexpr std::size_t kMaxContributors = 256;
    static constexpr std::size_t kMaxIdBytes = 256;
    static constexpr std::size_t kMaxCanonicalBytes = 16 * 1024 * 1024;

    // IDs use [a-z0-9_.:-], with 1..kMaxIdBytes bytes. Duplicate/invalid IDs or
    // a full registry return false without mutation. IDs are copied at admission.
    [[nodiscard]] bool Register(const IAuthorityStateContributor& contributor);
    // Empty states are omitted. Nonempty output is |authorityContributors:v1:,
    // uint64 LE count, then sorted (uint64 LE ID size, ID, uint64 LE state size,
    // state) records. Throws length_error before allocation on oversized output.
    [[nodiscard]] std::string SerializeCanonicalState() const;

private:
    struct Entry {
        std::string id;
        std::reference_wrapper<const IAuthorityStateContributor> contributor;
    };
    std::vector<Entry> m_entries;
};

// Explicit opt-in overload; existing seven-argument callers retain their bytes.
// Defined alongside the legacy hash composition in WorldPersistenceRoundtrip.cpp.
std::string ComposeWorldHash(const std::string& chunk_hash,
                             const std::string& wind_hash,
                             const std::string& weather_hash,
                             const std::string& aether_hash,
                             const std::string& scent_hash,
                             const std::string& ecology_hash,
                             const std::string& plant_hash,
                             const AuthorityContributorRegistry& contributors);

} // namespace Luminumbra::Persistence
