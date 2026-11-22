#pragma once

#include "../core/ModuleRegistry.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

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

// Adapter for an existing metadata projection. Preparation may stage private
// values but must not change live state; commit cannot fail. No new wire records
// are implied: the owning format supplies its required module identities.
// Metadata is caller-owned staging, admitted to that format's size budget before
// restoration; writers publish it only after every payload has been prepared.
class IAuthorityMetadataContributor : public IAuthorityStateContributor {
public:
    virtual void WriteMetadata(nlohmann::json& metadata) const = 0;
    [[nodiscard]] virtual bool PrepareRestore(const nlohmann::json& metadata,
                                              std::string& error) = 0;
    virtual void CommitRestore() noexcept = 0;
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
    [[nodiscard]] bool RegisterMetadata(IAuthorityMetadataContributor& contributor,
                                        std::string_view module_id);
    // Validate the complete required set and every metadata binding before any
    // callback. At most 256 requirements and bindings, each ID at most 128 bytes.
    [[nodiscard]] bool ValidateRequiredModules(const ModuleRegistry& modules,
                                               std::span<const std::string_view> required) const;
    [[nodiscard]] bool WriteMetadata(const ModuleRegistry& modules,
                                     std::span<const std::string_view> required,
                                     nlohmann::json& metadata) const;
    [[nodiscard]] bool RestoreMetadata(const ModuleRegistry& modules,
                                       std::span<const std::string_view> required,
                                       const nlohmann::json& metadata,
                                       std::string& error) const;
    // Preserve a legacy hash/checkpoint slot without adding the extension suffix.
    // Throws invalid_argument for an unavailable identity or contributor, and
    // length_error if the borrowed state exceeds kMaxCanonicalBytes.
    [[nodiscard]] std::string_view LegacyCanonicalState(const ModuleRegistry& modules,
                                                        std::span<const std::string_view> required,
                                                        std::string_view contributor_id) const;
    // Empty states are omitted. Nonempty output is |authorityContributors:v1:,
    // uint64 LE count, then sorted (uint64 LE ID size, ID, uint64 LE state size,
    // state) records. Throws length_error before allocation on oversized output.
    [[nodiscard]] std::string SerializeCanonicalState() const;

private:
    struct Entry {
        std::string id;
        std::reference_wrapper<const IAuthorityStateContributor> contributor;
        std::string module_id{};
        IAuthorityMetadataContributor* metadata = nullptr;
    };
    std::vector<Entry> m_entries;
};

inline bool
AuthorityContributorRegistry::RegisterMetadata(IAuthorityMetadataContributor& contributor,
                                               std::string_view module_id) {
    if (!IsScheduleIdentifier(module_id)) {
        return false;
    }
    std::string identity(module_id);
    if (!Register(contributor)) {
        return false;
    }
    const auto position =
        std::lower_bound(m_entries.begin(),
                         m_entries.end(),
                         contributor.contributor_id(),
                         [](const Entry& entry, std::string_view key) { return entry.id < key; });
    position->module_id.swap(identity);
    position->metadata = &contributor;
    return true;
}

inline bool AuthorityContributorRegistry::ValidateRequiredModules(
    const ModuleRegistry& modules, std::span<const std::string_view> required) const {
    if (!modules.running() || required.size() > kMaxContributors) {
        return false;
    }
    for (const auto id : required) {
        if (!IsScheduleIdentifier(id) || !modules.contains(id) ||
            std::none_of(m_entries.begin(), m_entries.end(), [&](const Entry& entry) {
                return entry.metadata != nullptr && entry.module_id == id;
            })) {
            return false;
        }
    }
    return std::all_of(m_entries.begin(), m_entries.end(), [&](const Entry& entry) {
        return entry.metadata == nullptr || modules.contains(entry.module_id);
    });
}

inline bool AuthorityContributorRegistry::WriteMetadata(const ModuleRegistry& modules,
                                                        std::span<const std::string_view> required,
                                                        nlohmann::json& metadata) const {
    if (!ValidateRequiredModules(modules, required)) {
        return false;
    }
    for (const auto& entry : m_entries) {
        if (entry.metadata != nullptr) {
            entry.metadata->WriteMetadata(metadata);
        }
    }
    return true;
}

inline bool
AuthorityContributorRegistry::RestoreMetadata(const ModuleRegistry& modules,
                                              std::span<const std::string_view> required,
                                              const nlohmann::json& metadata,
                                              std::string& error) const {
    if (!ValidateRequiredModules(modules, required)) {
        error = "Saved-input admission refused: required authority module unavailable.";
        return false;
    }
    for (const auto& entry : m_entries) {
        if (entry.metadata != nullptr && !entry.metadata->PrepareRestore(metadata, error)) {
            return false;
        }
    }
    for (const auto& entry : m_entries) {
        if (entry.metadata != nullptr) {
            entry.metadata->CommitRestore();
        }
    }
    return true;
}

inline std::string_view
AuthorityContributorRegistry::LegacyCanonicalState(const ModuleRegistry& modules,
                                                   std::span<const std::string_view> required,
                                                   std::string_view contributor_id) const {
    if (!ValidateRequiredModules(modules, required)) {
        throw std::invalid_argument("required authority module unavailable");
    }
    const auto position =
        std::lower_bound(m_entries.begin(),
                         m_entries.end(),
                         contributor_id,
                         [](const Entry& entry, std::string_view key) { return entry.id < key; });
    if (position == m_entries.end() || position->id != contributor_id ||
        position->metadata == nullptr) {
        throw std::invalid_argument("legacy authority contributor unavailable");
    }
    const auto state = position->contributor.get().canonical_state();
    if (state.size() > kMaxCanonicalBytes) {
        throw std::length_error("authority contributor state exceeds canonical byte limit");
    }
    return state;
}

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
