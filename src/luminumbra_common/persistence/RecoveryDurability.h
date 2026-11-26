#pragma once

#include <array>
#include <cstdint>
#include <filesystem>

namespace Luminumbra::Persistence::detail {

// An observation is usable only when it comes from the SAME handle as the later
// successful file flush. Unknown volume metadata deliberately cannot retire a receipt.
struct NamespaceFlushObservation {
    bool succeeded = false;
    bool volume_known = false;
    bool ntfs = false;
    std::uint64_t volume = 0;
};

enum class NamespaceReceiptPhase : std::uint8_t {
    Prepared,
    NamespaceDone,
    FlushObserved
};

struct NamespaceReceipt {
    std::array<std::uint64_t, 3> identity{};
    NamespaceReceiptPhase phase = NamespaceReceiptPhase::Prepared;
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
// There are at most seven operations, no scan or allocation. Storage operations own
// the existing journal byte/artifact limits and refuse before exceeding them.
template<typename Operations>
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
        if (!observation.volume_known || !observation.ntfs ||
            observation.volume != receipt.identity.front())
            return NamespaceRetirementOutcome::Retained;
        if (!operations.record_flush_observed(receipt.identity, observation))
            return NamespaceRetirementOutcome::Failed;
        receipt.phase = NamespaceReceiptPhase::FlushObserved;
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
