#pragma once

#include "SavedInputFile.h"
#include "fields/EnergyFieldState.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace Luminumbra::Persistence {
// First pass uses fixed storage to admit bytes, header channels and physical page
// lines before buffering the record. The second pass checks exact bytes again and
// the parser admits syntax before allocating pages. Both passes use the same handle.
inline bool ReadSavedEnergy(const SavedInputFile& input, std::string& record) {
    using namespace luminumbra::fields;
    std::uint64_t length = 0;
    if (!input.length(length) || length > kEnergyReadRecordBytes)
        return false;
    std::array<char, 64u * 1024u> block{}; // Fixed 64 KiB file admission scratch.
    std::string header;
    // Three header tokens, each no longer than a u64 decimal token (20 bytes).
    constexpr std::size_t kHeaderTokenBytes = 20;
    std::size_t token_bytes = 0;
    std::size_t tokens = 0;
    std::size_t pages = 0;
    bool in_header = true;
    bool in_token = false;
    bool line_started = false;
    for (std::uint64_t offset = 0; offset < length;) {
        const auto count =
            static_cast<std::size_t>(std::min<std::uint64_t>(block.size(), length - offset));
        if (!input.read_exact(block.data(), count))
            return false;
        offset += count;
        for (std::size_t index = 0; index < count; ++index) {
            const char ch = block[index];
            if (in_header) {
                if (ch == '\n') {
                    header += '\n';
                    if (!EnergyFieldState::AdmitRecord(header))
                        return false;
                    in_header = false;
                } else if (ch == ' ' || ch == '\t' || ch == '\r') {
                    in_token = false;
                } else {
                    if (!in_token) {
                        if (++tokens > 3)
                            return false;
                        if (!header.empty())
                            header += ' ';
                        token_bytes = 0;
                        in_token = true;
                    }
                    if (++token_bytes > kHeaderTokenBytes)
                        return false;
                    header += ch;
                }
            } else if (ch == '\n') {
                line_started = false;
            } else if (!line_started) {
                if (++pages > kEnergyReadPages)
                    return false;
                line_started = true;
            }
        }
    }
    if (in_header && !EnergyFieldState::AdmitRecord(header))
        return false;
    std::string candidate;
    if (!input.at_end() || !input.rewind() || !input.read(candidate, kEnergyReadRecordBytes) ||
        !EnergyFieldState::AdmitRecord(candidate))
        return false;
    record = std::move(candidate);
    return true;
}
} // namespace Luminumbra::Persistence
