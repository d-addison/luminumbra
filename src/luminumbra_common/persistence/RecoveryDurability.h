#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Luminumbra::Persistence::detail {

// v6: exact GUID bits on Windows; {st_dev, 1} on POSIX. Zero means unavailable,
// including historical v5 receipts. This is separate from the legacy object identity.
struct StableVolumeIdentity {
    std::array<std::uint64_t, 2> words{};
    bool known() const {
        return words != std::array<std::uint64_t, 2>{};
    }
    bool operator==(const StableVolumeIdentity&) const = default;
};

// Parse only the local GUID prefix of a handle's final path, independent of its
// drive letter, mount point, file spelling or 8.3 aliases. At most 49 UTF-16 units.
inline StableVolumeIdentity VolumeIdentityFromGuidPath(std::wstring_view path) {
    constexpr std::wstring_view prefix = L"\\\\?\\Volume{";
    constexpr std::size_t guid_end = 47;
    if (path.size() < 49 || !path.starts_with(prefix) || path[guid_end] != L'}' ||
        path[guid_end + 1] != L'\\')
        return {};
    StableVolumeIdentity identity;
    std::size_t digits = 0;
    for (std::size_t i = prefix.size(); i < guid_end; ++i) {
        const auto unit = path[i];
        if (i == 19 || i == 24 || i == 29 || i == 34) {
            if (unit != L'-')
                return {};
            continue;
        }
        unsigned digit = 0;
        if (unit >= L'0' && unit <= L'9')
            digit = static_cast<unsigned>(unit - L'0');
        else if (unit >= L'a' && unit <= L'f')
            digit = static_cast<unsigned>(unit - L'a') + 10u;
        else if (unit >= L'A' && unit <= L'F')
            digit = static_cast<unsigned>(unit - L'A') + 10u;
        else
            return {};
        auto& word = identity.words[digits / 16];
        word = (word << 4) | digit;
        ++digits;
    }
    return identity;
}

#if defined(_WIN32)
inline StableVolumeIdentity VolumeIdentityFromHandle(HANDLE file) {
    // The Win32 extended path bound includes its terminating UTF-16 null.
    std::array<wchar_t, 32768> name{};
    const auto length = ::GetFinalPathNameByHandleW(file,
                                                    name.data(),
                                                    static_cast<DWORD>(name.size()),
                                                    FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
    if (length == 0 || length >= name.size())
        return {};
    return VolumeIdentityFromGuidPath(std::wstring_view(name.data(), length));
}
#endif

// An observation is usable only when it comes from the SAME handle as the later
// successful file flush. Unknown volume metadata deliberately cannot retire a receipt.
struct NamespaceFlushObservation {
    bool succeeded = false;
    bool volume_known = false;
    bool ntfs = false;
    std::uint64_t volume = 0;
    StableVolumeIdentity stable_volume{};
};

inline bool QualifiesNamespaceFlush(const StableVolumeIdentity& artifact,
                                    const NamespaceFlushObservation& journal) {
    return journal.succeeded && journal.volume_known && journal.ntfs && artifact.known() &&
           journal.stable_volume.known() && artifact == journal.stable_volume;
}

enum class NamespaceReceiptPhase : std::uint8_t {
    Prepared,
    NamespaceDone,
    FlushObserved
};

struct NamespaceReceipt {
    std::array<std::uint64_t, 3> identity{};
    NamespaceReceiptPhase phase = NamespaceReceiptPhase::Prepared;
    StableVolumeIdentity stable_volume{};
};

enum class NamespaceRetirementOutcome : std::uint8_t {
    Failed,
    // Evidence remains owned, but durable absence has not been qualified. This
    // outcome cannot authorize dependent index-before-payload rollback work.
    Retained,
    Retired
};

enum class NamespaceEntryState : std::uint8_t {
    Refused,
    Absent,
    Owned
};

// The caller must durably record ownership before invoking this operation. Concrete
// operations retain the inspected binding through removal and recheck identity at
// removal itself. Phase records must be durable before reporting success.
// The receipt stays discoverable independently of the
// control file. No operation below may infer completion from matching bytes.
//
// Recovery repeats inspection even for a recorded completion. A resurrected entry
// invalidates that completion durably BEFORE another removal. An absent entry reuses
// recorded phases, so failed retirement does not append records on every retry.
// POSIX callers select RequireNtfs=false only after completing their directory barriers.
// There are at most seven operations, no scan or allocation. Storage operations own
// the existing journal byte/artifact limits and refuse before exceeding them.
template<bool RequireNtfs = true, typename Operations>
NamespaceRetirementOutcome CompleteNamespaceRetirement(NamespaceReceipt& receipt,
                                                       Operations& operations) {
    if ((receipt.identity[1] == 0 && receipt.identity[2] == 0) ||
        (receipt.phase != NamespaceReceiptPhase::Prepared &&
         receipt.phase != NamespaceReceiptPhase::NamespaceDone &&
         receipt.phase != NamespaceReceiptPhase::FlushObserved))
        return NamespaceRetirementOutcome::Failed;
    const NamespaceEntryState entry = operations.inspect_namespace(receipt.identity);
    if (entry == NamespaceEntryState::Refused)
        return NamespaceRetirementOutcome::Failed;
    if (entry != NamespaceEntryState::Absent && entry != NamespaceEntryState::Owned)
        return NamespaceRetirementOutcome::Failed;
    if (entry == NamespaceEntryState::Owned) {
        if (receipt.phase != NamespaceReceiptPhase::Prepared) {
            if (!operations.record_prepared(receipt.identity))
                return NamespaceRetirementOutcome::Failed;
            receipt.phase = NamespaceReceiptPhase::Prepared;
        }
        if (!operations.remove_owned(receipt.identity))
            return NamespaceRetirementOutcome::Failed;
    }
    if (receipt.phase == NamespaceReceiptPhase::Prepared) {
        if (!operations.record_namespace_done(receipt.identity))
            return NamespaceRetirementOutcome::Failed;
        receipt.phase = NamespaceReceiptPhase::NamespaceDone;
    }
    if (receipt.phase == NamespaceReceiptPhase::NamespaceDone) {
        const NamespaceFlushObservation observation = operations.flush_later_commit();
        if (!observation.succeeded)
            return NamespaceRetirementOutcome::Failed;
        if constexpr (RequireNtfs) {
            if (!QualifiesNamespaceFlush(receipt.stable_volume, observation))
                return NamespaceRetirementOutcome::Retained;
        }
        if (!operations.record_flush_observed(receipt.identity, observation))
            return NamespaceRetirementOutcome::Failed;
        receipt.phase = NamespaceReceiptPhase::FlushObserved;
    }
    if constexpr (RequireNtfs) {
        // A historical serial-only FlushObserved phase cannot bypass qualification.
        if (!receipt.stable_volume.known())
            return NamespaceRetirementOutcome::Retained;
    }
    if (!operations.retire_receipt(receipt.identity))
        return NamespaceRetirementOutcome::Failed;
    return NamespaceRetirementOutcome::Retired;
}

// Concrete operations bind and validate the recorded object, flush that same writable
// handle on Windows, and report close errors. Content equality never completes recovery.
// POSIX repeats every supplied parent barrier, even after an earlier rename succeeded.
// Callers supply at most four parents (destination, temporary and two private stages).
template<bool FlushFile, typename Operations>
bool CompleteRestoration(Operations& operations,
                         const std::array<std::uint64_t, 3>& expected,
                         const std::array<std::filesystem::path, 4>& parents) {
    const bool owned = operations.bind(expected);
    bool flushed = owned;
    if constexpr (FlushFile)
        flushed = owned && operations.flush_file();
    // Release even on ownership/flush refusal; a close failure also blocks progression.
    const bool closed = operations.close_file();
    if (!owned || !flushed || !closed)
        return false;
    if constexpr (!FlushFile) {
        // The destination parent is mandatory; empty optional staging slots are allowed.
        if (parents.front().empty())
            return false;
        for (const auto& parent : parents)
            if (!parent.empty() && !operations.flush_parent(parent))
                return false;
    }
    return true;
}

} // namespace Luminumbra::Persistence::detail
