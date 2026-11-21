#include "AuthorityContributor.h"

#include "WorldPersistenceRoundtrip.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <stdexcept>

namespace Luminumbra::Persistence {
namespace {
constexpr std::string_view kPrefix = "|authorityContributors:v1:";

bool ValidId(std::string_view id) {
    return !id.empty() && id.size() <= AuthorityContributorRegistry::kMaxIdBytes &&
           std::all_of(id.begin(), id.end(), [](char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= '0' && character <= '9') || character == '_' ||
                      character == '.' || character == ':' || character == '-';
           });
}

std::size_t CheckedSize(std::size_t size, std::size_t extra) {
    if (extra > AuthorityContributorRegistry::kMaxCanonicalBytes - size) {
        throw std::length_error("authority contributor state exceeds canonical byte limit");
    }
    return size + extra;
}

void AppendSize(std::string& output, std::size_t size) {
    auto value = static_cast<std::uint64_t>(size);
    for (unsigned index = 0; index < 8; ++index) {
        output.push_back(std::bit_cast<char>(static_cast<unsigned char>(value & 0xffu)));
        value >>= 8;
    }
}
} // namespace

std::string IAuthorityStateContributor::ComputeHash() const {
    const auto state = canonical_state();
    if (state.empty()) {
        return {};
    }
    CheckedSize(0, state.size());
    return StableChecksum(std::string(state));
}

bool AuthorityContributorRegistry::Register(const IAuthorityStateContributor& contributor) {
    const auto id = contributor.contributor_id();
    if (!ValidId(id) || m_entries.size() == kMaxContributors) {
        return false;
    }
    const auto position = std::lower_bound(
        m_entries.begin(), m_entries.end(), id, [](const Entry& entry, std::string_view key) {
            return entry.id < key;
        });
    if (position != m_entries.end() && position->id == id) {
        return false;
    }
    m_entries.insert(position, Entry{std::string(id), std::cref(contributor)});
    return true;
}

std::string AuthorityContributorRegistry::SerializeCanonicalState() const {
    // At most 256 borrowed state views; no state copies until all sizes pass.
    std::array<std::string_view, kMaxContributors> states{};
    std::size_t count = 0;
    std::size_t size = kPrefix.size() + 8;
    for (std::size_t index = 0; index < m_entries.size(); ++index) {
        states[index] = m_entries[index].contributor.get().canonical_state();
        if (!states[index].empty()) {
            size = CheckedSize(size, 16 + m_entries[index].id.size());
            size = CheckedSize(size, states[index].size());
            ++count;
        }
    }
    if (count == 0) {
        return {};
    }
    std::string output;
    output.reserve(size);
    output.append(kPrefix);
    AppendSize(output, count);
    for (std::size_t index = 0; index < m_entries.size(); ++index) {
        if (!states[index].empty()) {
            AppendSize(output, m_entries[index].id.size());
            output.append(m_entries[index].id);
            AppendSize(output, states[index].size());
            output.append(states[index]);
        }
    }
    return output;
}
} // namespace Luminumbra::Persistence
