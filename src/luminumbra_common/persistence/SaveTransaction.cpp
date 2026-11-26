#include "SaveTransaction.h"
#include "core/CaptureContentPaths.h"
#include "core/FilesystemPath.h"

#include "CreatedWorldArtifacts.h"
#include "RecoveryDurability.h"
#include "SavedInputFile.h"
#include "SavedInputLimits.h"
#include "SavedJsonAdmission.h"
#include "WorkerBoundary.h"
#include "WorldSaveService.h"
#include "fields/EnergyFieldState.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace Luminumbra::Persistence {

class TransactionOwnership {
private:
    WorldSaveService::SaveOwnership m_writer;
    detail::SnapshotWriteOwnership m_snapshot;
};

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::size_t kHashBlockBytes = 64u * 1024u;
#if defined(_WIN32)
constexpr bool kNeedsFileReceipts = true;
#else
constexpr bool kNeedsFileReceipts = false;
#endif

SaveTransaction*& ActiveTransaction() {
    static thread_local SaveTransaction* active = nullptr;
    return active;
}

SnapshotStepHook& StepHook() {
    static SnapshotStepHook hook = nullptr;
    return hook;
}

std::atomic<std::uint64_t>& ObservationRevision() {
    static std::atomic<std::uint64_t> revision{0};
    return revision;
}

void AddError(std::vector<std::string>* errors, const std::string& message) {
    if (errors) {
        errors->push_back(message);
    }
}

std::uint64_t Fnv1aUpdate(std::uint64_t hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

std::string HexFnv64(std::uint64_t value) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
    return std::string(buf);
}

bool ParseHex64(std::string_view text, std::uint64_t& out) {
    if (text.size() != 16) {
        return false;
    }
    std::uint64_t value = 0;
    for (char c : text) {
        std::uint64_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<std::uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<std::uint64_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<std::uint64_t>(c - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }
    out = value;
    return true;
}

bool AllDigits(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

bool IsTemporaryName(std::string_view name, const fs::path& prefix_path, bool ledger) {
    const auto prefix = Filesystem::GenericUtf8PathString(prefix_path) + ".tmp.";
    if (!name.starts_with(prefix))
        return false;
    name.remove_prefix(prefix.size());
    const auto process_end = name.find('.');
    if (process_end == std::string_view::npos || !AllDigits(name.substr(0, process_end)))
        return false;
    name.remove_prefix(process_end + 1);
    const auto sequence_end = name.find('.');
    if (sequence_end == std::string_view::npos)
        return AllDigits(name);
    if (!ledger || !AllDigits(name.substr(0, sequence_end)))
        return false;
    name.remove_prefix(sequence_end + 1);
    // Ledger staging adds exactly 128 bits of lowercase hexadecimal entropy.
    // Sidecar extensions and extra path components are never payload names.
    return name.size() == 32 && std::all_of(name.begin(), name.end(), [](char unit) {
               return (unit >= '0' && unit <= '9') || (unit >= 'a' && unit <= 'f');
           });
}

// Optional leading '-' followed by 1 to 10 decimal digits.
bool IsRegionCoordinate(std::string_view text) {
    if (!text.empty() && text.front() == '-') {
        text.remove_prefix(1);
    }
    return text.size() <= 10 && AllDigits(text);
}

bool IsUndoName(const std::string& undo) {
    if (undo.empty()) {
        return true;
    }
    constexpr std::string_view kPrefix = "undo/";
    if (undo.size() <= kPrefix.size() || undo.compare(0, kPrefix.size(), kPrefix) != 0) {
        return false;
    }
    return AllDigits(std::string_view(undo).substr(kPrefix.size()));
}

const nlohmann::json* Field(const nlohmann::json& object, const char* key) {
    if (!object.is_object()) {
        return nullptr;
    }
    auto it = object.find(key);
    return it == object.end() ? nullptr : &(*it);
}

// Undo payloads obey the largest shipping member bound. JSON control records
// use the stricter index budget. All lengths are admitted before allocation.
constexpr std::uint64_t kMaxUndoBytes = SavedInputLimits::kRegionCompressedAggregateBytes + 8ull +
                                        SavedInputLimits::kRegionRecords * 18ull;

bool SamePath(const fs::path& lhs, const fs::path& rhs) {
    std::error_code ec;
    fs::path lhs_canonical = Filesystem::WeaklyCanonicalPath(lhs, ec);
    if (ec) {
        return false;
    }
    fs::path rhs_canonical = Filesystem::WeaklyCanonicalPath(rhs, ec);
    if (ec) {
        return false;
    }
    return Filesystem::PathSpellingEqual(lhs_canonical, rhs_canonical);
}

// Resolve both spellings exactly as active_for does, but do not follow the leaf: a
// substituted member symlink must be refused by the native publication checks.
bool RelativePublicationPath(const fs::path& root, const fs::path& path, std::string& relative) {
    std::error_code error;
    const auto canonical_root = Filesystem::WeaklyCanonicalPath(root, error);
    if (error || path.empty())
        return false;
    const auto parent = Filesystem::WeaklyCanonicalPath(
        path.has_parent_path() ? path.parent_path() : fs::path("."), error);
    if (error)
        return false;
    const auto canonical_path = parent / path.filename();
    auto component = canonical_path.begin();
    for (const auto& root_component : canonical_root) {
        if (component == canonical_path.end() ||
            !Filesystem::PathSpellingEqual(root_component, *component++))
            return false;
    }
    fs::path suffix;
    for (; component != canonical_path.end(); ++component) {
        if (*component == ".." || *component == ".")
            return false;
        suffix /= *component;
    }
    relative = Filesystem::GenericUtf8PathString(suffix);
    return !relative.empty();
}

// Devel stages outside strict save inventories. Legacy-looking temporary names
// inside a save have no ownership proof, so refuse them without deleting data.
bool ValidateTemporaryInventory(const fs::path& save_dir) {
    // The save root is devel's neutral staging area and is not a strict inventory.
    const std::array<fs::path, 1> directories = {save_dir / "chunks" / "region"};
    for (const fs::path& directory : directories) {
        std::error_code ec;
        fs::directory_iterator it(directory, ec);
        if (ec) {
            if (ec == std::errc::no_such_file_or_directory)
                continue;
            return false;
        }
        std::size_t count = 0;
        // The snapshot member ceiling also caps recovery inventory work.
        for (const fs::directory_iterator end; it != end; it.increment(ec)) {
            if (ec || count++ == kMaxSnapshotMembers)
                return false;
            if (Filesystem::GenericUtf8PathString(it->path().filename()).find(".tmp.") !=
                std::string::npos)
                return false;
        }
        if (ec)
            return false;
    }
    return true;
}

// Nearest ancestor of `directory` (inclusive) that exists, for flushing entries under directories
// that a crashed first transaction never created.
fs::path NearestExistingDirectory(fs::path directory) {
    std::error_code ec;
    while (!directory.empty() && !fs::exists(directory, ec)) {
        const fs::path parent = directory.parent_path();
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
    return directory;
}

struct JournalEntry {
    std::string path;
    std::string action; // "replace" | "create"
    std::uint64_t prev_bytes = 0;
    std::uint64_t prev_fnv64 = 0;
    std::string undo; // "undo/<n>", empty for create
    std::array<std::uint64_t, 3> previous_identity{};
    std::array<std::uint64_t, 3> installed_identity{};
    std::array<std::uint64_t, 3> incoming_identity{};
    std::array<std::uint64_t, 3> restored_identity{};
    std::array<std::uint64_t, 3> undo_identity{};
    std::array<std::uint64_t, 3> rollback_retired_identity{};
    std::string undo_temporary;
    std::string incoming_temporary;
    std::string restored_temporary;
    // Derived from preceding records, without changing the journal encoding.
    bool installed_from_incoming = false;
    // Live rollback also checks publication stamps. Recovery retains its journal byte checks.
    std::array<SavedInputFile::Version, 3> live_versions{};
};

struct RetirementReceipt {
    std::string path;
    std::uint64_t generation = 0;
    detail::NamespaceReceipt completion;
    bool directory = false;
    bool absent_only = false;
    bool retired = false;
    // Derived from an explicit reset record; no extra encoded field.
    bool reset = false;
};

struct JournalDocument {
    bool modern = true;
    std::string record = "entry";
    bool cleanup = false;
    bool external_control = false;
    // Live forward cleanup must preserve the already admitted rollback budget.
    std::size_t append_reserve = 0;
    std::array<std::uint64_t, 3> root_identity{};
    std::array<std::uint64_t, 3> control_identity{};
    std::map<std::pair<std::string, std::uint64_t>, RetirementReceipt> receipts;
    // Derived only from ownership records, bounded by the control-stream byte limit.
    std::map<std::string, std::set<std::array<std::uint64_t, 3>>> recorded_slots;
    std::set<std::string> recorded_absences;
    std::uint64_t txn_id = 0;
    std::uint64_t base_generation = 0;
    std::uint64_t new_generation = 0;
    std::vector<JournalEntry> entries;
    std::array<std::uint64_t, 3> journal_identity{};
    std::array<std::uint64_t, 3> undo_directory_identity{};
};

auto LatestReceipt(JournalDocument& document, const std::string& path) {
    const auto found =
        document.receipts.upper_bound({path, std::numeric_limits<std::uint64_t>::max()});
    if (found == document.receipts.begin() || std::prev(found)->first.first != path)
        return document.receipts.end();
    return std::prev(found);
}

bool HasReceipt(const JournalDocument& document, const std::string& path) {
    const auto found = document.receipts.lower_bound({path, 0});
    return found != document.receipts.end() && found->first.first == path;
}

RetirementReceipt& ReceiptFor(JournalDocument& document, const std::string& path) {
    const auto found = LatestReceipt(document, path);
    if (found == document.receipts.end())
        throw std::out_of_range("snapshot receipt missing");
    return found->second;
}

bool ReadIdentity(const nlohmann::json& object,
                  const char* key,
                  std::array<std::uint64_t, 3>& identity) {
    const auto* value = Field(object, key);
    if (!value || !value->is_array() || value->size() != identity.size())
        return false;
    for (std::size_t i = 0; i < identity.size(); ++i) {
        if (!(*value)[i].is_number_unsigned())
            return false;
        identity[i] = (*value)[i].get<std::uint64_t>();
    }
    return true;
}

bool ParseJournalRecord(std::string_view bytes, JournalDocument& out, std::string& error) {
    out = JournalDocument{};
    if (bytes.size() > kMaxSnapshotIndexBytes || !SavedInputLimits::JsonDepthAllowed(bytes)) {
        error = "snapshot control JSON compatibility limit";
        return false;
    }
    const std::string text(bytes);
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "malformed journal JSON";
        return false;
    }
    const nlohmann::json* schema = Field(root, "schema");
    if (!schema || !schema->is_string() ||
        (schema->get<std::string>() != kJournalSchema &&
         schema->get<std::string>() != "luminumbra.journal.v4")) {
        error = "unsupported journal schema";
        return false;
    }
    const nlohmann::json* txn_id = Field(root, "txn_id");
    const nlohmann::json* base = Field(root, "base_generation");
    const nlohmann::json* next = Field(root, "new_generation");
    const nlohmann::json* entries = Field(root, "entries");
    if (!txn_id || !txn_id->is_number_unsigned() || !base || !base->is_number_unsigned() || !next ||
        !next->is_number_unsigned() || !entries || !entries->is_array() ||
        entries->size() > kMaxSnapshotMembers) {
        error = "journal header fields invalid";
        return false;
    }
    JournalDocument parsed;
    parsed.modern = schema->get<std::string>() == kJournalSchema;
    parsed.txn_id = txn_id->get<std::uint64_t>();
    parsed.base_generation = base->get<std::uint64_t>();
    parsed.new_generation = next->get<std::uint64_t>();
    if (!ReadIdentity(root, "journal_identity", parsed.journal_identity) ||
        !ReadIdentity(root, "undo_directory_identity", parsed.undo_directory_identity)) {
        error = "journal directory identity invalid";
        return false;
    }
    if (parsed.txn_id == 0 || parsed.base_generation == std::numeric_limits<std::uint64_t>::max() ||
        parsed.new_generation != parsed.base_generation + 1) {
        error = "journal generation identity invalid";
        return false;
    }
    if (parsed.modern) {
        const auto* kind = Field(root, "record");
        if (!kind || !kind->is_string()) {
            error = "journal record kind missing";
            return false;
        }
        parsed.record = kind->get<std::string>();
        const std::size_t expected_fields = parsed.record == "entry"     ? 8u
                                            : parsed.record == "cleanup" ? 10u
                                                                         : 10u;
        if (root.size() != expected_fields) {
            error = "journal record fields invalid";
            return false;
        }
        if (parsed.record == "cleanup") {
            if (!entries->empty() || !ReadIdentity(root, "root_identity", parsed.root_identity) ||
                !ReadIdentity(root, "control_identity", parsed.control_identity) ||
                (parsed.root_identity[1] == 0 && parsed.root_identity[2] == 0) ||
                (parsed.control_identity[1] == 0 && parsed.control_identity[2] == 0)) {
                error = "journal cleanup identity invalid";
                return false;
            }
            parsed.cleanup = true;
        } else if (parsed.record == "receipt" || parsed.record == "receipt-reset") {
            const auto* phase = Field(root, "phase");
            const auto* receipts = Field(root, "receipts");
            if (!entries->empty() || !phase || !phase->is_number_unsigned() ||
                phase->get<std::uint64_t>() > 3 || !receipts || !receipts->is_array() ||
                receipts->empty() || receipts->size() > kMaxSnapshotMembers ||
                (parsed.record == "receipt-reset" && phase->get<unsigned>() != 0)) {
                error = "journal receipt invalid";
                return false;
            }
            for (const auto& item : *receipts) {
                const auto* path = Field(item, "path");
                const auto* directory = Field(item, "directory");
                const auto* absent_only = Field(item, "absent_only");
                RetirementReceipt receipt;
                const auto* generation = Field(item, "generation");
                if (!item.is_object() || item.size() != (generation ? 5u : 4u) ||
                    (generation && !generation->is_number_unsigned()) || !path ||
                    !path->is_string() || !directory || !directory->is_boolean() || !absent_only ||
                    !absent_only->is_boolean() ||
                    (directory->get<bool>() && absent_only->get<bool>()) ||
                    !ReadIdentity(item, "identity", receipt.completion.identity) ||
                    (receipt.completion.identity[1] == 0 && receipt.completion.identity[2] == 0)) {
                    error = "journal receipt invalid";
                    return false;
                }
                receipt.path = path->get<std::string>();
                receipt.generation = generation ? generation->get<std::uint64_t>() : 0;
                // Inventory validation additionally confines every name before removal.
                if (receipt.path.empty() || receipt.path.front() == '/' ||
                    receipt.path.find('\\') != std::string::npos ||
                    receipt.path.find(':') != std::string::npos ||
                    receipt.path.find("..") != std::string::npos ||
                    Filesystem::GenericUtf8PathString(fs::path(receipt.path).lexically_normal()) !=
                        receipt.path) {
                    error = "journal receipt path invalid";
                    return false;
                }
                const auto phase_number = phase->get<unsigned>();
                receipt.retired = phase_number == 3;
                receipt.completion.phase =
                    static_cast<detail::NamespaceReceiptPhase>(std::min(phase_number, 2u));
                receipt.directory = directory->get<bool>();
                receipt.absent_only = absent_only->get<bool>();
                if (!parsed.receipts.emplace(std::pair{receipt.path, receipt.generation}, receipt)
                         .second) {
                    error = "journal duplicate receipt path";
                    return false;
                }
            }
        } else if (parsed.record != "entry") {
            error = "unsupported journal record kind";
            return false;
        }
    }
    std::unordered_set<std::string> paths;
    for (const nlohmann::json& item : *entries) {
        if (parsed.modern && (!item.is_object() || item.size() != 14)) {
            error = "journal entry fields invalid";
            return false;
        }
        const nlohmann::json* path = Field(item, "path");
        const nlohmann::json* action = Field(item, "action");
        const nlohmann::json* prev_bytes = Field(item, "prev_bytes");
        const nlohmann::json* prev_fnv = Field(item, "prev_fnv64");
        const nlohmann::json* undo = Field(item, "undo");
        if (!path || !path->is_string() || !action || !action->is_string() || !prev_bytes ||
            !prev_bytes->is_number_unsigned() || !prev_fnv || !prev_fnv->is_string() || !undo ||
            !undo->is_string()) {
            error = "journal entry fields invalid";
            return false;
        }
        JournalEntry entry;
        entry.path = path->get<std::string>();
        entry.action = action->get<std::string>();
        entry.prev_bytes = prev_bytes->get<std::uint64_t>();
        entry.undo = undo->get<std::string>();
        if (!ReadIdentity(item, "previous_identity", entry.previous_identity) ||
            !ReadIdentity(item, "installed_identity", entry.installed_identity) ||
            !ReadIdentity(item, "incoming_identity", entry.incoming_identity) ||
            !ReadIdentity(item, "restored_identity", entry.restored_identity) ||
            !ReadIdentity(item, "undo_identity", entry.undo_identity) ||
            !ReadIdentity(item, "rollback_retired_identity", entry.rollback_retired_identity)) {
            error = "journal publication identity invalid";
            return false;
        }
        const auto temporary_name = [&](const char* key,
                                        const fs::path& prefix_path,
                                        std::string& output,
                                        bool ledger = false) {
            const auto* value = Field(item, key);
            if (!value || !value->is_string())
                return false;
            output = value->get<std::string>();
            if (output.empty())
                return true;
            return IsTemporaryName(output, prefix_path, ledger);
        };
        const bool ledger = entry.path == "chunks/region/active-regions.arl";
        if ((!temporary_name(
                 "undo_temporary", fs::path("journal") / entry.undo, entry.undo_temporary) &&
             !temporary_name(
                 "undo_temporary", fs::path(entry.undo).filename(), entry.undo_temporary)) ||
            (!temporary_name("incoming_temporary",
                             fs::path(entry.path).filename(),
                             entry.incoming_temporary,
                             ledger) &&
             !temporary_name("incoming_temporary",
                             fs::path("journal") / fs::path(entry.path).filename(),
                             entry.incoming_temporary,
                             ledger) &&
             !temporary_name(
                 "incoming_temporary", fs::path(entry.path), entry.incoming_temporary, ledger)) ||
            (!temporary_name("restored_temporary",
                             fs::path("journal") / fs::path(entry.path).filename(),
                             entry.restored_temporary) &&
             !temporary_name("restored_temporary",
                             fs::path(entry.path).filename(),
                             entry.restored_temporary))) {
            error = "journal temporary name invalid";
            return false;
        }
        if (!member_path_allowed(entry.path) && entry.path != kSnapshotIndexRelativePath) {
            error = "journal entry path not allowed";
            return false;
        }
        if (entry.action != "replace" && entry.action != "create") {
            error = "journal entry action invalid";
            return false;
        }
        if (!ParseHex64(prev_fnv->get<std::string>(), entry.prev_fnv64)) {
            error = "journal entry digest invalid";
            return false;
        }
        if (!paths.insert(entry.path).second || (entry.action == "replace" && entry.undo.empty()) ||
            (entry.action == "create" &&
             (!entry.undo.empty() || entry.prev_bytes != 0 || entry.prev_fnv64 != 0)) ||
            entry.prev_bytes > kMaxUndoBytes) {
            error = "journal entry identity invalid";
            return false;
        }
        if (!IsUndoName(entry.undo)) {
            error = "journal undo name invalid";
            return false;
        }
        parsed.entries.push_back(std::move(entry));
    }
    out = std::move(parsed);
    return true;
}

// Only newline-terminated records publish ownership. Writers flush the body before
// the delimiter; an interrupted tail cannot authorize any member mutation.
bool ParseJournal(std::string_view bytes, JournalDocument& out, std::string& error) {
    out = {};
    if (bytes.empty() || bytes.size() > kMaxSnapshotIndexBytes) {
        error = "incomplete or oversized journal";
        return false;
    }
    std::unordered_map<std::string, std::size_t> entries;
    bool first = true;
    while (!bytes.empty()) {
        const auto end = bytes.find('\n');
        if (end == std::string_view::npos) {
            break; // An unpublished tail follows the last complete ownership record.
        }
        JournalDocument record;
        if (!ParseJournalRecord(bytes.substr(0, end), record, error))
            return false;
        bytes.remove_prefix(end + 1);
        if (first) {
            out = std::move(record);
            if (!out.entries.empty() || out.record != "entry") {
                error = "journal must begin with ownership header";
                return false;
            }
            first = false;
            continue;
        }
        if (record.txn_id != out.txn_id || record.base_generation != out.base_generation ||
            record.new_generation != out.new_generation ||
            record.journal_identity != out.journal_identity ||
            record.undo_directory_identity != out.undo_directory_identity ||
            record.modern != out.modern ||
            (record.record == "entry" && record.entries.size() != 1)) {
            error = "journal record identity changed";
            return false;
        }
        if (record.record == "cleanup") {
            if (out.cleanup && (out.root_identity != record.root_identity ||
                                out.control_identity != record.control_identity)) {
                error = "journal cleanup identity changed";
                return false;
            }
            out.cleanup = true;
            out.root_identity = record.root_identity;
            out.control_identity = record.control_identity;
            continue;
        }
        if (record.record == "receipt" || record.record == "receipt-reset") {
            // Validate the entire atomic record before advancing any receipt.
            std::size_t additions = 0;
            for (const auto& [name, receipt] : record.receipts) {
                const auto found = out.receipts.find(name);
                const auto latest = LatestReceipt(out, receipt.path);
                if (found == out.receipts.end()) {
                    if (record.record == "receipt-reset" ||
                        (latest == out.receipts.end()
                             ? receipt.generation != 0
                             : latest->second.generation ==
                                       std::numeric_limits<std::uint64_t>::max() ||
                                   receipt.generation != latest->second.generation + 1 ||
                                   (!latest->second.retired && !latest->second.reset)) ||
                        receipt.completion.phase != detail::NamespaceReceiptPhase::Prepared ||
                        receipt.retired || additions == kMaxSnapshotMembers - out.receipts.size()) {
                        error = "journal receipt requires preparation";
                        return false;
                    }
                    if (receipt.absent_only && !out.recorded_absences.contains(receipt.path)) {
                        const auto entry = entries.find(receipt.path);
                        if (entry == entries.end() ||
                            out.entries[entry->second].action != "create" ||
                            out.entries[entry->second].rollback_retired_identity !=
                                std::array<std::uint64_t, 3>{} ||
                            receipt.completion.identity != out.journal_identity) {
                            error = "journal absent receipt outside creation inventory";
                            return false;
                        }
                    }
                    ++additions;
                } else {
                    const auto& previous = found->second;
                    const auto phase_number = [](const RetirementReceipt& value) {
                        return value.retired ? 3u : static_cast<unsigned>(value.completion.phase);
                    };
                    const auto next_phase = phase_number(receipt);
                    const auto previous_phase = phase_number(previous);
                    // Resurrection invalidates completion explicitly, never through an
                    // ordinary phase regression. Identity and flags remain immutable.
                    const bool ordered =
                        record.record == "receipt-reset"
                            ? next_phase == 0 && previous_phase != 0
                            : next_phase == previous_phase || next_phase == previous_phase + 1;
                    if (latest != found ||
                        receipt.completion.identity != previous.completion.identity ||
                        receipt.directory != previous.directory ||
                        receipt.absent_only != previous.absent_only || !ordered) {
                        error = "journal receipt transition invalid";
                        return false;
                    }
                }
            }
            for (auto& [name, receipt] : record.receipts) {
                receipt.reset = record.record == "receipt-reset";
                if (receipt.absent_only)
                    out.recorded_absences.insert(receipt.path);
                out.receipts.insert_or_assign(name, receipt);
            }
            continue;
        }
        if (out.cleanup) {
            error = "journal publication after cleanup";
            return false;
        }
        auto& entry = record.entries.front();
        for (const auto& [phase, identity] :
             {std::pair{"incoming", entry.incoming_identity},
              std::pair{"retired", entry.installed_identity},
              std::pair{"restored", entry.restored_identity},
              std::pair{"rollback-retired", entry.rollback_retired_identity}}) {
            const auto slot = "journal/undo/" + std::string(phase) + "." +
                              Filesystem::GenericUtf8PathString(fs::path(entry.path).filename());
            if (identity != std::array<std::uint64_t, 3>{})
                out.recorded_slots[slot].insert(identity);
        }
        for (const auto& [name, identity] :
             {std::pair{entry.undo_temporary, entry.undo_identity},
              std::pair{entry.incoming_temporary, entry.incoming_identity},
              std::pair{entry.restored_temporary, entry.restored_identity}}) {
            if (!name.empty() && identity != std::array<std::uint64_t, 3>{}) {
                out.recorded_slots[name].insert(identity);
                const auto slot = "journal/undo/cleanup." +
                                  Filesystem::GenericUtf8PathString(fs::path(name).filename());
                out.recorded_slots[slot].insert(identity);
            }
        }
        const auto known = entries.find(entry.path);
        if (known == entries.end()) {
            if (out.entries.size() == kMaxSnapshotMembers) {
                error = "journal member count limit";
                return false;
            }
            entries.emplace(entry.path, out.entries.size());
            out.entries.push_back(std::move(entry));
        } else {
            const auto& previous = out.entries[known->second];
            if (entry.action != previous.action || entry.prev_bytes != previous.prev_bytes ||
                entry.prev_fnv64 != previous.prev_fnv64 || entry.undo != previous.undo ||
                entry.undo_identity != previous.undo_identity ||
                entry.previous_identity != previous.previous_identity) {
                error = "journal undo identity changed";
                return false;
            }
            entry.installed_from_incoming =
                entry.installed_identity != decltype(entry.installed_identity){} &&
                (entry.installed_identity == previous.incoming_identity ||
                 (previous.installed_from_incoming &&
                  entry.installed_identity == previous.installed_identity));
            out.entries[known->second] = std::move(entry);
        }
    }
    return !first;
}

std::string EncodeJournal(std::uint64_t txn_id,
                          std::uint64_t base_generation,
                          std::uint64_t new_generation,
                          const std::vector<JournalEntry>& entries,
                          const std::array<std::uint64_t, 3>& journal_identity,
                          const std::array<std::uint64_t, 3>& undo_directory_identity,
                          bool modern = true) {
    nlohmann::json root = nlohmann::json::object();
    root["schema"] = modern ? kJournalSchema : "luminumbra.journal.v4";
    if (modern)
        root["record"] = "entry";
    root["txn_id"] = txn_id;
    root["base_generation"] = base_generation;
    root["new_generation"] = new_generation;
    root["journal_identity"] = journal_identity;
    root["undo_directory_identity"] = undo_directory_identity;
    nlohmann::json list = nlohmann::json::array();
    for (const JournalEntry& entry : entries) {
        nlohmann::json item = nlohmann::json::object();
        item["path"] = entry.path;
        item["action"] = entry.action;
        item["prev_bytes"] = entry.prev_bytes;
        item["prev_fnv64"] = HexFnv64(entry.prev_fnv64);
        item["undo"] = entry.undo;
        item["previous_identity"] = entry.previous_identity;
        item["installed_identity"] = entry.installed_identity;
        item["incoming_identity"] = entry.incoming_identity;
        item["restored_identity"] = entry.restored_identity;
        item["undo_identity"] = entry.undo_identity;
        item["rollback_retired_identity"] = entry.rollback_retired_identity;
        item["undo_temporary"] = entry.undo_temporary;
        item["incoming_temporary"] = entry.incoming_temporary;
        item["restored_temporary"] = entry.restored_temporary;
        list.push_back(std::move(item));
    }
    root["entries"] = std::move(list);
    std::string bytes;
    if (!BoundedJsonDump(root, kMaxSnapshotIndexBytes - 1, bytes))
        throw std::length_error("snapshot control JSON compatibility limit");
    return bytes + "\n";
}

using ArtifactIdentity = WorldSaveService::ArtifactIdentity;

BoundPublicationStage JournalStage(const fs::path& save_dir,
                                   const ArtifactIdentity& directory_identity,
                                   std::string_view phase,
                                   const fs::path& member) {
    return {.directory = journal_directory(save_dir) / "undo",
            .name = std::string(phase) + "." + Filesystem::GenericUtf8PathString(member.filename()),
            .directory_identity = directory_identity,
            .report = &report_snapshot_step};
}

BoundPublicationStage CleanupStage(const fs::path& save_dir,
                                   const ArtifactIdentity& directory_identity,
                                   const fs::path& path) {
    auto stage = JournalStage(save_dir, directory_identity, "cleanup", path);
    if (path.parent_path() == stage.directory)
        stage.name = path.filename(); // already in the owned private staging directory
    return stage;
}

bool MatchesIdentity(const fs::path& path,
                     const ArtifactIdentity& expected,
                     bool directory = false) {
    ArtifactIdentity actual{};
    bool missing = false;
    return WorldSaveService::artifact_identity(path, actual, missing, directory) && !missing &&
           actual == expected;
}

bool MatchesPublicationVersion(const SavedInputFile::Version& current,
                               const SavedInputFile::Version& expected) {
    // Rename changes ctime; native identity, size and nanosecond mtime survive publication.
    return std::equal(current.begin(), current.begin() + 6, expected.begin());
}

bool OwnedOccupant(const fs::path& destination,
                   const JournalEntry& entry,
                   ArtifactIdentity& current,
                   bool& missing,
                   bool& previous_bytes,
                   std::vector<std::string>* errors) {
    previous_bytes = false;
    const SavedInputFile input(destination);
    missing = input.missing();
    if (missing) {
        current = {};
        return true;
    }
    if (!input.identity(current) ||
        (current != entry.previous_identity && current != entry.installed_identity &&
         current != entry.incoming_identity && current != entry.restored_identity)) {
        AddError(errors, "snapshot rollback occupant changed: " + entry.path);
        return false;
    }
    // An incoming publication can reuse the original native identity. A later record
    // can move that identity to installed and clear incoming before publication fails.
    // Only the current publication roles qualify; an older identity alone is not enough.
    bool previous_occupant =
        current == entry.previous_identity && current != entry.incoming_identity &&
        !(entry.installed_from_incoming && current == entry.installed_identity);
    if (entry.live_versions != decltype(entry.live_versions){} &&
        current != entry.restored_identity) {
        SavedInputFile::Version version{};
        if (!input.version(version) || std::none_of(entry.live_versions.begin(),
                                                    entry.live_versions.end(),
                                                    [&version](const auto& expected) {
                                                        return MatchesPublicationVersion(version,
                                                                                         expected);
                                                    })) {
            AddError(errors, "snapshot rollback occupant changed: " + entry.path);
            return false;
        }
        // A later publication can reuse the original native identity after its handle closes.
        previous_occupant = MatchesPublicationVersion(version, entry.live_versions.front());
    }
    if (entry.action == "replace") {
        // Recovery cannot retain pre-crash handles. Verify the prior bytes through the same
        // handle as the identity, so a recycled native ID cannot bless changed foreign bytes.
        std::uint64_t length = 0;
        if (!input.length(length) || length > kMaxUndoBytes) {
            AddError(errors, "snapshot rollback previous occupant changed: " + entry.path);
            return false;
        }
        // Fixed 64 KiB storage; work is capped by the journal's admitted undo length.
        std::array<char, kHashBlockBytes> block{};
        std::uint64_t digest = kFnvOffset;
        for (std::uint64_t offset = 0; offset < length;) {
            const auto count =
                static_cast<std::size_t>(std::min<std::uint64_t>(block.size(), length - offset));
            if (!input.read_exact(block.data(), count)) {
                AddError(errors, "snapshot rollback previous occupant unreadable: " + entry.path);
                return false;
            }
            digest = Fnv1aUpdate(digest, block.data(), count);
            offset += count;
        }
        if (!input.at_end()) {
            AddError(errors, "snapshot rollback previous occupant changed: " + entry.path);
            return false;
        }
        previous_bytes = length == entry.prev_bytes && digest == entry.prev_fnv64;
        if (previous_occupant && !previous_bytes) {
            AddError(errors, "snapshot rollback previous occupant changed: " + entry.path);
            return false;
        }
    }
    return true;
}

bool AppendEntry(const fs::path& save_dir,
                 const JournalDocument& document,
                 const JournalEntry& entry,
                 ArtifactIdentity& control,
                 std::vector<std::string>* errors) {
    return WorldSaveService::append_journal_record(journal_directory(save_dir) / "txn.json",
                                                   EncodeJournal(document.txn_id,
                                                                 document.base_generation,
                                                                 document.new_generation,
                                                                 {entry},
                                                                 document.journal_identity,
                                                                 document.undo_directory_identity,
                                                                 document.modern),
                                                   control,
                                                   errors);
}

fs::path ReceiptCarrier(const fs::path& save_dir, const JournalDocument& document) {
    // The retained control identity prevents collisions after an aborted initial
    // creation reuses transaction 1; its inode/file ID cannot recycle while retained.
    std::string name = "journal.receipts." + std::to_string(document.txn_id);
    for (const auto component : document.control_identity)
        name += "." + std::to_string(component);
    return save_dir / name;
}

fs::path RecoveryControl(const fs::path& save_dir, const JournalDocument& document) {
    return document.external_control ? ReceiptCarrier(save_dir, document)
                                     : journal_directory(save_dir) / "txn.json";
}

// Publication can leave the carrier and either recorded control name linked.
// Admit only those three names, and verify their total against the native link
// count. An unrelated alias never gains admission through the higher bound.
unsigned ControlAliasLinks(const fs::path& save_dir, const JournalDocument& document) {
    const std::array names{ReceiptCarrier(save_dir, document),
                           journal_directory(save_dir) / "txn.json",
                           journal_directory(save_dir) / "undo/cleanup.txn.json"};
    unsigned present = 0;
    for (const auto& name : names) {
        ArtifactIdentity actual{};
        bool missing = false;
        if (!WorldSaveService::artifact_identity(name, actual, missing, false, 3) ||
            (!missing && actual != document.control_identity))
            return 0;
        if (!missing)
            ++present;
        else if (name == names.front())
            return 0;
    }
    for (const auto& name : names) {
        ArtifactIdentity actual{};
        bool missing = false;
        if (!WorldSaveService::artifact_identity(name, actual, missing, false, present) ||
            (!missing && actual != document.control_identity))
            return 0;
    }
    return present;
}

nlohmann::json MetadataRecord(const JournalDocument& document, const char* kind) {
    return {{"schema", kJournalSchema},
            {"record", kind},
            {"txn_id", document.txn_id},
            {"base_generation", document.base_generation},
            {"new_generation", document.new_generation},
            {"journal_identity", document.journal_identity},
            {"undo_directory_identity", document.undo_directory_identity},
            {"entries", nlohmann::json::array()}};
}

bool ReserveReceiptPhaseBytes(const std::string& path, std::size_t& reserve) {
    // Four records per receipt suffice; 1024 bytes bounds each fixed header,
    // identity, generation and punctuation. Admitted paths are ASCII names.
    if (reserve > kMaxSnapshotIndexBytes || path.size() > kMaxSnapshotIndexBytes - 1024u ||
        1024u + path.size() > (kMaxSnapshotIndexBytes - reserve) / 4u)
        return false;
    reserve += 4u * (1024u + path.size());
    return true;
}

bool ReservePendingReceipts(const JournalDocument& document, std::size_t& reserve) {
    if (reserve > kMaxSnapshotIndexBytes)
        return false;
    if (reserve == 0)
        return true; // Recovery consumes the capacity reserved by forward writes.
    // Historical temporary names can outlive the current publication, so every
    // forward append must preserve their phases beyond the member rollback budget.
    for (const auto& [key, receipt] : document.receipts) {
        if (!receipt.retired && !receipt.reset && !ReserveReceiptPhaseBytes(key.first, reserve))
            return false;
    }
    return true;
}

bool AppendMetadata(const fs::path& save_dir,
                    const JournalDocument& document,
                    const nlohmann::json& record,
                    ArtifactIdentity& control,
                    std::vector<std::string>* errors,
                    detail::NamespaceFlushObservation* observation = nullptr) {
    std::string bytes;
    if (!BoundedJsonDump(record, kMaxSnapshotIndexBytes - 1, bytes)) {
        AddError(errors, "snapshot receipt exceeds control byte limit");
        return false;
    }
    bytes += '\n';
    auto reserve = document.append_reserve;
    if (!ReservePendingReceipts(document, reserve))
        return false;
    if (reserve != 0) {
        if (record.at("record") == "receipt" && record.at("phase") == 0u) {
            for (const auto& item : record.at("receipts")) {
                const auto name = item.at("path").get<std::string>();
                const auto generation = item.at("generation").get<std::uint64_t>();
                if (!document.receipts.contains({name, generation}) &&
                    !ReserveReceiptPhaseBytes(name, reserve))
                    return false;
            }
        }
    }
    return WorldSaveService::append_journal_record(
        RecoveryControl(save_dir, document), bytes, control, errors, reserve, observation);
}

// Each buffer is bounded by kMaxSnapshotIndexBytes, including its delimiter.
// Split in caller inventory order at exact encoded byte boundaries. Every append
// publishes all its receipts together; a torn tail publishes none of them.
bool RecordReceipts(const fs::path& save_dir,
                    const JournalDocument& document,
                    const std::vector<RetirementReceipt*>& receipts,
                    unsigned phase,
                    ArtifactIdentity& control,
                    std::vector<std::string>* errors,
                    detail::NamespaceFlushObservation* observation = nullptr,
                    bool reset = false) {
    auto record = MetadataRecord(document, reset ? "receipt-reset" : "receipt");
    record["phase"] = phase;
    record["receipts"] = nlohmann::json::array();
    std::string header;
    if (!BoundedJsonDump(record, kMaxSnapshotIndexBytes - 1, header))
        return false;
    const auto header_bytes = header.size();
    std::size_t batch_bytes = header_bytes;
    std::size_t batch_begin = 0;
    const auto append = [&](std::size_t batch_end) {
        if (!AppendMetadata(save_dir, document, record, control, errors, observation))
            return false;
        for (auto i = batch_begin; i < batch_end; ++i) {
            receipts[i]->completion.phase =
                static_cast<detail::NamespaceReceiptPhase>(std::min(phase, 2u));
            receipts[i]->retired = phase == 3;
            receipts[i]->reset = reset;
        }
        return true;
    };
    for (std::size_t i = 0; i < receipts.size(); ++i) {
        const auto& receipt = *receipts[i];
        nlohmann::json item{{"path", receipt.path},
                            {"generation", receipt.generation},
                            {"identity", receipt.completion.identity},
                            {"directory", receipt.directory},
                            {"absent_only", receipt.absent_only}};
        std::string encoded;
        if (!BoundedJsonDump(item, kMaxSnapshotIndexBytes - 1 - header_bytes, encoded)) {
            AddError(errors, "snapshot receipt exceeds control byte limit");
            return false;
        }
        const auto comma = record["receipts"].empty() ? 0u : 1u;
        if (comma > kMaxSnapshotIndexBytes - 1 - batch_bytes ||
            encoded.size() > kMaxSnapshotIndexBytes - 1 - batch_bytes - comma) {
            if (!append(i))
                return false;
            record["receipts"] = nlohmann::json::array();
            batch_bytes = header_bytes;
            batch_begin = i;
        }
        batch_bytes += encoded.size() + (record["receipts"].empty() ? 0u : 1u);
        record["receipts"].push_back(std::move(item));
    }
    return receipts.empty() || append(receipts.size());
}

bool PrepareReceipts(const fs::path& save_dir,
                     JournalDocument& document,
                     std::map<std::string, RetirementReceipt>& candidates,
                     ArtifactIdentity& control,
                     std::vector<std::string>* errors,
                     bool new_artifact = false) {
    std::vector<RetirementReceipt*> resets;
    for (const auto& [name, candidate] : candidates) {
        const auto found = LatestReceipt(document, name);
        if (found == document.receipts.end() || found->second.retired || found->second.reset ||
            ((!new_artifact ||
              found->second.completion.phase == detail::NamespaceReceiptPhase::Prepared) &&
             found->second.completion.identity == candidate.completion.identity &&
             found->second.directory == candidate.directory &&
             found->second.absent_only == candidate.absent_only))
            continue;
        ArtifactIdentity actual{};
        bool missing = false;
        if (found->second.completion.phase == detail::NamespaceReceiptPhase::Prepared ||
            !WorldSaveService::artifact_identity(
                save_dir / name, actual, missing, found->second.directory) ||
            !missing)
            return false;
        // Unqualified filesystems retain the old immutable evidence. Explicit
        // reset permits reusing an absent slot without inventing NTFS completion.
        resets.push_back(&found->second);
    }
    if (!RecordReceipts(save_dir, document, resets, 0, control, errors, nullptr, true))
        return false;
    std::vector<RetirementReceipt*> pending;
    for (auto& [name, receipt] : candidates) {
        const auto found = LatestReceipt(document, name);
        if (found != document.receipts.end()) {
            receipt.generation = found->second.generation;
            if ((!new_artifact ||
                 (!found->second.retired &&
                  found->second.completion.phase == detail::NamespaceReceiptPhase::Prepared)) &&
                !found->second.reset &&
                found->second.completion.identity == receipt.completion.identity &&
                found->second.directory == receipt.directory &&
                found->second.absent_only == receipt.absent_only)
                continue;
            if ((!found->second.retired && !found->second.reset) ||
                found->second.generation == std::numeric_limits<std::uint64_t>::max())
                return false;
            ++receipt.generation;
        }
        if (pending.size() == kMaxSnapshotMembers - document.receipts.size())
            return false;
        pending.push_back(&receipt);
    }
    if (!RecordReceipts(save_dir, document, pending, 0, control, errors))
        return false;
    for (auto* receipt : pending) {
        if (receipt->absent_only)
            document.recorded_absences.insert(receipt->path);
        document.receipts.emplace(std::pair{receipt->path, receipt->generation}, *receipt);
    }
    return true;
}

bool PrepareReceipt(const fs::path& save_dir,
                    JournalDocument& document,
                    const fs::path& path,
                    const ArtifactIdentity& identity,
                    bool directory,
                    ArtifactIdentity& control,
                    std::vector<std::string>* errors,
                    bool absent_only = false,
                    bool new_artifact = false) {
    if (identity == ArtifactIdentity{})
        return true;
    const auto name = Filesystem::GenericUtf8PathString(path.lexically_relative(save_dir));
    std::map<std::string, RetirementReceipt> candidates;
    candidates.emplace(name,
                       RetirementReceipt{.path = name,
                                         .completion = {.identity = identity},
                                         .directory = directory,
                                         .absent_only = absent_only,
                                         .retired = false});
    return PrepareReceipts(save_dir, document, candidates, control, errors, new_artifact);
}

#if defined(_WIN32)
// Retrying an already-recorded namespace phase must not consume another log record.
// Rebind, flush and obtain the observation through one writable control-file handle.
detail::NamespaceFlushObservation FlushReceiptCarrier(const fs::path& path,
                                                      const ArtifactIdentity& expected) {
    const HANDLE file = ::CreateFileW(path.c_str(),
                                      GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH,
                                      nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    BY_HANDLE_FILE_INFORMATION info{};
    const bool identified = ::GetFileInformationByHandle(file, &info) != 0;
    const ArtifactIdentity actual{
        info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
    detail::NamespaceFlushObservation observation{};
    if (identified && actual == expected && (actual[1] != 0 || actual[2] != 0) &&
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0 &&
        ::FlushFileBuffers(file)) {
        observation.succeeded = true;
        // Match appended-commit observations: remote NTFS reports do not qualify.
        // The fixed extended-path buffer includes its terminating UTF-16 null.
        std::array<wchar_t, 32768> final_name{};
        const auto name_length =
            ::GetFinalPathNameByHandleW(file,
                                        final_name.data(),
                                        static_cast<DWORD>(final_name.size()),
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
        const bool local_volume =
            name_length != 0 && name_length < final_name.size() &&
            std::wstring_view(final_name.data(), name_length).starts_with(L"\\\\?\\Volume{");
        std::array<wchar_t, 64> filesystem_name{};
        DWORD serial = 0;
        if (local_volume &&
            ::GetVolumeInformationByHandleW(file,
                                            nullptr,
                                            0,
                                            &serial,
                                            nullptr,
                                            nullptr,
                                            filesystem_name.data(),
                                            static_cast<DWORD>(filesystem_name.size())) &&
            serial == actual.front()) {
            observation.volume_known = true;
            observation.volume = serial;
            observation.ntfs = std::wstring_view(filesystem_name.data()) == L"NTFS";
        }
    }
    if (!::CloseHandle(file))
        return {};
    return observation;
}
#endif

struct ReceiptRemoval {
    RetirementReceipt* receipt;
    BoundPublicationStage stage;
};

ReceiptRemoval ReceiptRemovalFor(const fs::path& save_dir,
                                 const JournalDocument& document,
                                 RetirementReceipt& receipt,
                                 const BoundPublicationStage* stage) {
    const auto path = save_dir / receipt.path;
    const auto parent = path.parent_path();
    ArtifactIdentity parent_identity{};
    if (parent == save_dir)
        parent_identity = document.root_identity;
    else if (parent == journal_directory(save_dir))
        parent_identity = document.journal_identity;
    else if (parent == journal_directory(save_dir) / "undo")
        parent_identity = document.undo_directory_identity;
    // Control aliases use their own bound names, including occupied cleanup slots.
    if (stage &&
        !(document.external_control && receipt.completion.identity == document.control_identity))
        return {.receipt = &receipt, .stage = *stage};
    return {.receipt = &receipt,
            .stage = {.directory = parent,
                      .name = path.filename(),
                      .directory_identity = parent_identity,
                      .report = &report_snapshot_step}};
}

// Inventory order is stable. Preparation precedes all removals, and a single later
// carrier flush can qualify every earlier removal on that same NTFS volume.
// Vectors are bounded by the existing journal byte and member limits.
detail::NamespaceRetirementOutcome CompleteReceipts(const fs::path& save_dir,
                                                    JournalDocument& document,
                                                    const std::vector<ReceiptRemoval>& removals,
                                                    ArtifactIdentity& control,
                                                    std::vector<std::string>* errors) {
    const auto fail = [&] {
        AddError(errors, "cannot complete recorded snapshot namespaces");
        return detail::NamespaceRetirementOutcome::Failed;
    };
    std::vector<RetirementReceipt*> resets;
    std::vector<bool> owned;
    owned.reserve(removals.size());
    for (const auto& removal : removals) {
        auto& receipt = *removal.receipt;
        ArtifactIdentity actual{};
        bool missing = false;
        const auto links =
            document.external_control && receipt.completion.identity == document.control_identity
                ? ControlAliasLinks(save_dir, document)
                : std::numeric_limits<unsigned>::max();
        if (links == 0 ||
            !WorldSaveService::artifact_identity(
                save_dir / receipt.path, actual, missing, receipt.directory, links) ||
            (!missing && (receipt.absent_only || actual != receipt.completion.identity)))
            return fail();
        owned.push_back(!missing);
        if (!missing && (receipt.retired ||
                         receipt.completion.phase != detail::NamespaceReceiptPhase::Prepared))
            resets.push_back(&receipt);
    }
    // Explicit invalidation must be durable before a resurrected entry is removed.
    if (!RecordReceipts(save_dir, document, resets, 0, control, errors, nullptr, true))
        return fail();
    std::vector<RetirementReceipt*> namespace_done;
    for (std::size_t i = 0; i < removals.size(); ++i) {
        const auto& removal = removals[i];
        auto& receipt = *removal.receipt;
        if (owned[i] && !WorldSaveService::remove_owned_entry(save_dir / receipt.path,
                                                              receipt.completion.identity,
                                                              errors,
                                                              receipt.directory,
                                                              &removal.stage))
            return fail();
#if !defined(_WIN32)
        // Keep each directory barrier before advancing to its parent's removal.
        if (!WorldSaveService::durable_flush_directory(
                NearestExistingDirectory((save_dir / receipt.path).parent_path()), errors) ||
            !WorldSaveService::durable_flush_directory(
                NearestExistingDirectory(removal.stage.directory), errors))
            return fail();
#endif
        if (receipt.completion.phase == detail::NamespaceReceiptPhase::Prepared)
            namespace_done.push_back(&receipt);
    }
    detail::NamespaceFlushObservation latest_flush{};
    if (!RecordReceipts(save_dir, document, namespace_done, 1, control, errors, &latest_flush))
        return fail();
    std::vector<RetirementReceipt*> flush_observed;
    bool all_retired = true;
    for (const auto& removal : removals) {
        auto& receipt = *removal.receipt;
        if (receipt.completion.phase != detail::NamespaceReceiptPhase::NamespaceDone)
            continue;
        report_snapshot_step("crash-before-namespace-completion", save_dir / receipt.path);
#if defined(_WIN32)
        // Retrying a recorded phase uses one later flush, without another log record.
        if (!latest_flush.succeeded)
            latest_flush = FlushReceiptCarrier(RecoveryControl(save_dir, document), control);
        if (!latest_flush.succeeded)
            return fail();
        if (!latest_flush.volume_known || !latest_flush.ntfs ||
            latest_flush.volume != receipt.completion.identity.front()) {
            all_retired = false;
            continue;
        }
#endif
        flush_observed.push_back(&receipt);
    }
    if (!RecordReceipts(save_dir, document, flush_observed, 2, control, errors))
        return fail();
    std::vector<RetirementReceipt*> retired;
    for (const auto& removal : removals) {
        auto& receipt = *removal.receipt;
        if (!receipt.retired &&
            receipt.completion.phase == detail::NamespaceReceiptPhase::FlushObserved)
            retired.push_back(&receipt);
    }
    if (!RecordReceipts(save_dir, document, retired, 3, control, errors))
        return fail();
    return all_retired ? detail::NamespaceRetirementOutcome::Retired
                       : detail::NamespaceRetirementOutcome::Retained;
}

detail::NamespaceRetirementOutcome CompleteReceipt(const fs::path& save_dir,
                                                   JournalDocument& document,
                                                   RetirementReceipt& receipt,
                                                   ArtifactIdentity& control,
                                                   const BoundPublicationStage* stage,
                                                   std::vector<std::string>* errors) {
    return CompleteReceipts(save_dir,
                            document,
                            {ReceiptRemovalFor(save_dir, document, receipt, stage)},
                            control,
                            errors);
}

bool CompleteRemoval(const fs::path& save_dir,
                     JournalDocument& document,
                     const fs::path& path,
                     const ArtifactIdentity& identity,
                     ArtifactIdentity& control,
                     std::vector<std::string>* errors,
                     bool directory = false,
                     const BoundPublicationStage* stage = nullptr) {
#if !defined(_WIN32)
    const auto recorded_slot = stage ? stage->directory / stage->name : fs::path{};
    if (document.modern &&
        !HasReceipt(document,
                    Filesystem::GenericUtf8PathString(path.lexically_relative(save_dir))) &&
        (recorded_slot.empty() || !HasReceipt(document,
                                              Filesystem::GenericUtf8PathString(
                                                  recorded_slot.lexically_relative(save_dir))))) {
        // POSIX has a namespace barrier. The existing ownership records plus
        // repeated parent fsync suffice; per-file NTFS phase records would
        // consume the bounded control stream before large saves can roll back.
        const auto slot = stage ? stage->directory / stage->name : fs::path{};
        for (const auto& candidate : {path, slot}) {
            if (candidate.empty())
                continue;
            ArtifactIdentity parent_identity{};
            bool parent_missing = false;
            if (!WorldSaveService::artifact_identity(
                    candidate.parent_path(), parent_identity, parent_missing, true))
                return false;
            const auto expected_parent =
                candidate.parent_path() == journal_directory(save_dir) / "undo"
                    ? document.undo_directory_identity
                : candidate.parent_path() == journal_directory(save_dir) ? document.journal_identity
                : candidate.parent_path() == save_dir                    ? document.root_identity
                                                                         : ArtifactIdentity{};
            if (!parent_missing && expected_parent != ArtifactIdentity{} &&
                parent_identity != expected_parent)
                return false;
            const BoundPublicationStage local_stage{.directory = candidate.parent_path(),
                                                    .name = candidate.filename(),
                                                    .directory_identity = parent_identity,
                                                    .report = &report_snapshot_step};
            const bool control_alias =
                document.external_control && identity == document.control_identity;
            const auto* removal_stage =
                candidate == path && stage && !control_alias ? stage : &local_stage;
            ArtifactIdentity actual{};
            bool missing = false;
            const auto links = document.external_control && identity == document.control_identity
                                   ? ControlAliasLinks(save_dir, document)
                                   : std::numeric_limits<unsigned>::max();
            if (links == 0 ||
                !WorldSaveService::artifact_identity(
                    candidate, actual, missing, directory, links) ||
                (!missing && (actual != identity ||
                              !WorldSaveService::remove_owned_entry(
                                  candidate, identity, errors, directory, removal_stage))))
                return false;
            if (candidate == path && !document.cleanup)
                report_snapshot_step("crash-before-namespace-completion", candidate);
            if (!WorldSaveService::durable_flush_directory(
                    NearestExistingDirectory(candidate.parent_path()), errors))
                return false;
            if (candidate == slot)
                break;
        }
        return true;
    }
#endif
    if (!document.modern) {
        ArtifactIdentity actual{};
        bool missing = false;
        return WorldSaveService::artifact_identity(path, actual, missing, directory) &&
               (missing ||
                WorldSaveService::remove_owned_entry(path, identity, errors, directory, stage));
    }
    const auto slot = stage ? stage->directory / stage->name : fs::path{};
    std::map<std::string, RetirementReceipt> candidates;
    for (const auto& candidate : {path, slot}) {
        if (candidate.empty() || identity == ArtifactIdentity{})
            continue;
        const auto name = Filesystem::GenericUtf8PathString(candidate.lexically_relative(save_dir));
        candidates.emplace(name,
                           RetirementReceipt{.path = name,
                                             .completion = {.identity = identity},
                                             .directory = directory,
                                             .absent_only = false,
                                             .retired = false});
    }
    if (!PrepareReceipts(save_dir, document, candidates, control, errors))
        return false;
    std::vector<ReceiptRemoval> removals;
    for (const auto& candidate : {path, slot}) {
        if (candidate.empty() || identity == ArtifactIdentity{})
            continue;
        const auto name = Filesystem::GenericUtf8PathString(candidate.lexically_relative(save_dir));
        removals.push_back(ReceiptRemovalFor(
            save_dir, document, ReceiptFor(document, name), candidate == path ? stage : nullptr));
        if (candidate == slot)
            break;
    }
    return CompleteReceipts(save_dir, document, removals, control, errors) ==
           detail::NamespaceRetirementOutcome::Retired;
}

bool ReadUndo(const fs::path& save_dir,
              const JournalDocument& document,
              const JournalEntry& entry,
              std::string& bytes) {
    // A crash can interrupt the undo publication itself after its first move.
    // Both names and the payload identity were recorded before either move.
    auto input = std::make_unique<SavedInputFile>(journal_directory(save_dir) / entry.undo);
    if (input->missing()) {
        const auto stage =
            JournalStage(save_dir, document.undo_directory_identity, "undo", entry.undo);
        input = std::make_unique<SavedInputFile>(stage.directory / stage.name);
    }
    ArtifactIdentity identity{};
    return input->identity(identity) && identity == entry.undo_identity &&
           input->read(bytes, kMaxUndoBytes) && bytes.size() == entry.prev_bytes &&
           fnv1a64_bytes(bytes.data(), bytes.size()) == entry.prev_fnv64;
}

bool VerifyRetiredIndexUndo(const fs::path& save_dir,
                            const JournalDocument& document,
                            std::vector<std::string>* errors) {
    const auto entry =
        std::find_if(document.entries.begin(), document.entries.end(), [](const auto& candidate) {
            return candidate.path == kSnapshotIndexRelativePath && candidate.action == "replace";
        });
    if (entry == document.entries.end())
        return false;
    std::string bytes;
    SnapshotIndex previous;
    std::string error;
    if (!ReadUndo(save_dir, document, *entry, bytes) ||
        !decode_snapshot_index(bytes, previous, error) ||
        previous.generation != document.base_generation || previous.txn_id != document.txn_id - 1) {
        AddError(errors, "retired snapshot index undo identity invalid");
        return false;
    }
    return true;
}

// Windows publication links the destination before retiring the source name, so an
// interruption can leave a recorded temporary (or its cleanup slot) as a second name of the
// occupant. Retire only those recorded names; any other link refuses before mutation.
bool RetirePublicationAliases(const fs::path& save_dir,
                              JournalDocument& document,
                              ArtifactIdentity& control,
                              const JournalEntry& entry,
                              const fs::path& destination,
                              const ArtifactIdentity& current,
                              bool retire,
                              std::vector<std::string>* errors) {
    if (entry.action != "replace" || current == ArtifactIdentity{})
        return true;
    std::vector<std::pair<fs::path, BoundPublicationStage>> aliases;
    for (const auto& [name, identity] :
         {std::pair{entry.incoming_temporary, entry.incoming_identity},
          std::pair{entry.restored_temporary, entry.restored_identity}}) {
        if (name.empty() || identity != current)
            continue;
        const auto temporary = save_dir / name;
        const auto slot = CleanupStage(save_dir, document.undo_directory_identity, temporary);
        for (const auto& candidate : {temporary, slot.directory / slot.name}) {
            ArtifactIdentity actual{};
            bool missing = false;
            if (!WorldSaveService::artifact_identity(candidate, actual, missing)) {
                AddError(errors, "snapshot publication alias unreadable: " + entry.path);
                return false;
            }
            if (!missing && actual == current)
                aliases.emplace_back(candidate, slot);
        }
    }
    if (aliases.empty())
        return true;
    ArtifactIdentity occupant{};
    bool missing = false;
    if (!WorldSaveService::artifact_identity(
            destination, occupant, missing, false, static_cast<unsigned>(aliases.size() + 1)) ||
        missing || occupant != current) {
        AddError(errors, "foreign snapshot publication alias: " + entry.path);
        return false;
    }
    if (!retire)
        return true;
    for (const auto& [alias, slot] : aliases)
        if (!CompleteRemoval(save_dir, document, alias, current, control, errors, false, &slot))
            return false;
    return true;
}

// Undo is replayable: the identity of a restoration is itself journaled before publication.
// Never overwrite an occupant merely because its bytes differ from the old commit.
bool RollbackEntries(const fs::path& save_dir,
                     JournalDocument& document,
                     ArtifactIdentity& control,
                     std::vector<std::string>* errors) {
    if (!MatchesIdentity(journal_directory(save_dir), document.journal_identity, true) ||
        !MatchesIdentity(
            journal_directory(save_dir) / "undo", document.undo_directory_identity, true)) {
        AddError(errors, "snapshot journal directory ownership changed");
        return false;
    }
    std::vector<JournalEntry*> order;
    order.reserve(document.entries.size());
    for (auto& entry : document.entries)
        if (entry.path == kSnapshotIndexRelativePath)
            order.push_back(&entry);
    for (auto it = document.entries.rbegin(); it != document.entries.rend(); ++it)
        if (it->path != kSnapshotIndexRelativePath)
            order.push_back(&*it);
    for (const auto* entry : order) {
        ArtifactIdentity current{};
        bool missing = false;
        bool previous_bytes = false;
        if (!OwnedOccupant(
                save_dir / entry->path, *entry, current, missing, previous_bytes, errors) ||
            !RetirePublicationAliases(save_dir,
                                      document,
                                      control,
                                      *entry,
                                      save_dir / entry->path,
                                      current,
                                      false,
                                      errors))
            return false;
    }
    for (auto* entry : order) {
        const auto destination = save_dir / entry->path;
        ArtifactIdentity current{};
        bool missing = false;
        bool previous_bytes = false;
        if (!OwnedOccupant(destination, *entry, current, missing, previous_bytes, errors) ||
            !RetirePublicationAliases(
                save_dir, document, control, *entry, destination, current, true, errors))
            return false;
        const auto retired_stage = JournalStage(
            save_dir, document.undo_directory_identity, "rollback-retired", destination);
        const auto restored_stage =
            JournalStage(save_dir, document.undo_directory_identity, "restored", destination);
        if (!missing && !previous_bytes && entry->rollback_retired_identity != current) {
            entry->rollback_retired_identity = current;
            if (!AppendEntry(save_dir, document, *entry, control, errors))
                return false;
        }
        if (entry->action == "create") {
            const auto identity = missing ? entry->rollback_retired_identity : current;
            if (document.modern && identity == ArtifactIdentity{} &&
                (kNeedsFileReceipts || HasReceipt(document, entry->path))) {
                if (!PrepareReceipt(save_dir,
                                    document,
                                    destination,
                                    document.journal_identity,
                                    false,
                                    control,
                                    errors,
                                    true))
                    return false;
                auto& receipt = ReceiptFor(document, entry->path);
                if (CompleteReceipt(save_dir, document, receipt, control, nullptr, errors) !=
                    detail::NamespaceRetirementOutcome::Retired)
                    return false;
            } else if (!CompleteRemoval(save_dir,
                                        document,
                                        destination,
                                        identity,
                                        control,
                                        errors,
                                        false,
                                        &retired_stage)) {
                return false;
            }
        } else {
            if (!previous_bytes) {
                std::string restored;
                {
                    if (!ReadUndo(save_dir, document, *entry, restored)) {
                        AddError(errors, "journal undo copy missing or corrupt: " + entry->path);
                        return false;
                    }
                }
                fs::path temporary;
                bool temporary_missing = true;
                if (!entry->restored_temporary.empty()) {
                    temporary = save_dir / entry->restored_temporary;
                    ArtifactIdentity actual{};
                    if (!WorldSaveService::artifact_identity(
                            temporary, actual, temporary_missing) ||
                        (!temporary_missing && actual != entry->restored_identity)) {
                        AddError(errors, "snapshot restoration ownership changed");
                        return false;
                    }
                }
                if (temporary_missing && entry->restored_identity != ArtifactIdentity{}) {
                    temporary = restored_stage.directory / restored_stage.name;
                    ArtifactIdentity actual{};
                    if (!WorldSaveService::artifact_identity(
                            temporary, actual, temporary_missing) ||
                        (!temporary_missing && actual != entry->restored_identity)) {
                        AddError(errors, "snapshot restoration quarantine ownership changed");
                        return false;
                    }
                }
                if (temporary_missing) {
                    if (!WorldSaveService::prepare_restoration(save_dir / destination.filename(),
                                                               restored,
                                                               temporary,
                                                               entry->restored_identity,
                                                               errors))
                        return false;
                    entry->restored_temporary =
                        Filesystem::GenericUtf8PathString(temporary.lexically_relative(save_dir));
                    if (!AppendEntry(save_dir, document, *entry, control, errors))
                        return false;
                }
                if (!WorldSaveService::publish_restoration(temporary,
                                                           destination,
                                                           current,
                                                           entry->restored_identity,
                                                           errors,
                                                           &restored_stage,
                                                           &retired_stage))
                    return false;
            }
            // A prior rename may have succeeded before its flush failed. Matching bytes
            // still require completion before any later (especially payload) entry changes.
            if (document.modern && previous_bytes)
                report_snapshot_step("crash-before-restoration-completion", destination);
            if (previous_bytes &&
                !WorldSaveService::complete_restoration(destination,
                                                        current,
                                                        {destination.parent_path(),
                                                         save_dir,
                                                         restored_stage.directory,
                                                         retired_stage.directory},
                                                        errors))
                return false;
        }
        if (document.modern && entry->action == "replace" &&
            entry->rollback_retired_identity != ArtifactIdentity{} &&
            !CompleteRemoval(save_dir,
                             document,
                             retired_stage.directory / retired_stage.name,
                             entry->rollback_retired_identity,
                             control,
                             errors))
            return false;
        report_snapshot_step("rollback-entry", destination);
        if (!WorldSaveService::durable_flush_directory(
                NearestExistingDirectory(destination.parent_path()), errors))
            return false;
    }
    return true;
}

// Inventory and bind every artifact before retirement. A substituted directory/file or unknown
// leaf refuses cleanup; recursive deletion cannot prove ownership.
bool JournalInventory(const fs::path& save_dir,
                      const JournalDocument& document,
                      const ArtifactIdentity& control,
                      std::map<fs::path, ArtifactIdentity>& files,
                      std::vector<std::string>* errors,
                      bool retiring = false,
                      bool inventory_only = false) {
    const auto journal = journal_directory(save_dir);
    const auto directory_owned = [&](const fs::path& path, const ArtifactIdentity& expected) {
        ArtifactIdentity actual{};
        bool missing = false;
        return WorldSaveService::artifact_identity(path, actual, missing, true) &&
               ((missing && retiring) || (!missing && actual == expected));
    };
    if (control != ArtifactIdentity{})
        files.emplace(journal / "txn.json", control);
    for (const auto& entry : document.entries) {
        if (!entry.undo.empty())
            files.emplace(journal / entry.undo, entry.undo_identity);
        for (const auto& [name, identity] :
             {std::pair{entry.undo_temporary, entry.undo_identity},
              std::pair{entry.incoming_temporary, entry.incoming_identity},
              std::pair{entry.restored_temporary, entry.restored_identity}}) {
            if (!name.empty())
                files.emplace(save_dir / name, identity);
        }
        for (const auto& [phase, identity] :
             {std::pair{"incoming", entry.incoming_identity},
              std::pair{"retired", entry.installed_identity},
              std::pair{"restored", entry.restored_identity},
              std::pair{"rollback-retired", entry.rollback_retired_identity}}) {
            const auto slot =
                JournalStage(save_dir, document.undo_directory_identity, phase, entry.path);
            files.emplace(slot.directory / slot.name, identity);
        }
        if (!entry.undo.empty()) {
            const auto slot =
                JournalStage(save_dir, document.undo_directory_identity, "undo", entry.undo);
            files.emplace(slot.directory / slot.name, entry.undo_identity);
        }
    }
    // Outside-undo temporary/control files also move only through known slots.
    // Register their deterministic cleanup names before inventory validation.
    // map insertion preserves iterators; a newly added slot is already inside
    // undo, so its next lookup adds nothing. At most one slot per original file.
    for (const auto& [path, identity] : files) {
        const auto slot = CleanupStage(save_dir, document.undo_directory_identity, path);
        files.emplace(slot.directory / slot.name, identity);
    }
    std::map<std::string, ArtifactIdentity> creates;
    for (const auto& entry : document.entries)
        if (entry.action == "create")
            creates.emplace(entry.path, entry.rollback_retired_identity);
    for (const auto& [key, receipt] : document.receipts) {
        const auto& name = key.first;
        const auto path = save_dir / name;
        bool allowed =
            receipt.directory
                ? (path == journal && receipt.completion.identity == document.journal_identity) ||
                      (path == journal / "undo" &&
                       receipt.completion.identity == document.undo_directory_identity)
                : files.contains(path) && files.at(path) == receipt.completion.identity;
        if (!allowed && !receipt.directory) {
            const auto history = document.recorded_slots.find(name);
            allowed = history != document.recorded_slots.end() &&
                      history->second.contains(receipt.completion.identity);
        }
        if (!allowed && !receipt.directory) {
            const auto created = creates.find(name);
            allowed =
                created != creates.end() &&
                (receipt.absent_only ? document.recorded_absences.contains(name) &&
                                           receipt.completion.identity == document.journal_identity
                                     : receipt.completion.identity == created->second);
        }
        if (receipt.absent_only && files.contains(path))
            allowed = false;
        if (!allowed) {
            AddError(errors, "receipt outside recorded snapshot inventory");
            return false;
        }
    }
    // Historical temporary receipts remain live inventory after a writer has
    // moved on to another temporary name. Only the latest generation owns a
    // physical slot; previous generations retain their immutable evidence.
    for (const auto& [key, receipt] : document.receipts) {
        if (receipt.directory || member_path_allowed(key.first) ||
            key.first == kSnapshotIndexRelativePath)
            continue;
        const auto latest =
            document.receipts.upper_bound({key.first, std::numeric_limits<std::uint64_t>::max()});
        if (latest == document.receipts.begin() || std::prev(latest)->first != key)
            continue;
        const auto path = save_dir / key.first;
        if (!files.contains(path)) {
            files.emplace(path, receipt.completion.identity);
            const auto slot = CleanupStage(save_dir, document.undo_directory_identity, path);
            files.emplace(slot.directory / slot.name, receipt.completion.identity);
        }
    }
    if (inventory_only)
        return true;
    const auto control_links =
        document.external_control ? ControlAliasLinks(save_dir, document) : 1u;
    if (control_links == 0) {
        AddError(errors, "snapshot receipt control alias ownership changed");
        return false;
    }
    if (!directory_owned(journal, document.journal_identity) ||
        !directory_owned(journal / "undo", document.undo_directory_identity)) {
        AddError(errors, "snapshot journal directory ownership changed");
        return false;
    }
    std::size_t inspected = 0;
    for (const auto& directory : {journal, journal / "undo"}) {
        ArtifactIdentity actual{};
        bool missing = false;
        if (!WorldSaveService::artifact_identity(directory, actual, missing, true))
            return false;
        if (missing && retiring)
            continue;
        for (const auto& child : fs::directory_iterator(directory)) {
            if (inspected++ == kMaxSnapshotMembers ||
                (child.path() != journal / "undo" && !files.contains(child.path()))) {
                AddError(errors, "unknown snapshot journal artifact");
                return false;
            }
        }
    }
    for (const auto& [path, identity] : files) {
        ArtifactIdentity actual{};
        bool missing = false;
        const auto links = document.external_control && identity == document.control_identity
                               ? control_links
                               : std::numeric_limits<unsigned>::max();
        if (!WorldSaveService::artifact_identity(path, actual, missing, false, links) ||
            (!missing && actual != identity)) {
            AddError(errors, "snapshot cleanup artifact ownership changed");
            return false;
        }
    }
    return true;
}

// Initial creation owns files in its artifact registry rather than a previous
// committed tree. Complete its admitted files before releasing that registry or
// the control stream. The index batch always completes before the payload batch.
bool CompleteCreatedFiles(const fs::path& save_dir,
                          JournalDocument& document,
                          ArtifactIdentity& control,
                          std::vector<CreatedWorldArtifacts::OwnedFile>& files,
                          std::vector<std::string>* errors) {
    std::map<fs::path, ArtifactIdentity> inventory;
    if (!JournalInventory(save_dir, document, control, inventory, errors))
        return false;
    std::erase_if(files, [&](const auto& file) {
        const auto name = Filesystem::GenericUtf8PathString(file.path.lexically_relative(save_dir));
        return name != kSnapshotIndexRelativePath && !member_path_allowed(name) &&
               !inventory.contains(file.path);
    });
    // Admission of the whole registry subset precedes the first namespace change.
    for (const auto& file : files) {
        const auto name = Filesystem::GenericUtf8PathString(file.path.lexically_relative(save_dir));
        const auto entry = std::find_if(document.entries.begin(),
                                        document.entries.end(),
                                        [&](const auto& value) { return value.path == name; });
        if (entry == document.entries.end()) {
            const auto found = inventory.find(file.path);
            if (found == inventory.end() || found->second != file.identity)
                return false;
        } else if (file.identity != entry->previous_identity &&
                   file.identity != entry->incoming_identity &&
                   file.identity != entry->installed_identity)
            return false;
    }
    if (!document.cleanup) {
        for (const auto& file : files) {
            const auto name =
                Filesystem::GenericUtf8PathString(file.path.lexically_relative(save_dir));
            const auto entry = std::find_if(document.entries.begin(),
                                            document.entries.end(),
                                            [&](const auto& value) { return value.path == name; });
            if (entry != document.entries.end() && entry->installed_identity != file.identity) {
                entry->installed_identity = file.identity;
                if (!AppendEntry(save_dir, document, *entry, control, errors))
                    return false;
            }
        }
        bool missing = false;
        if (!WorldSaveService::artifact_identity(save_dir, document.root_identity, missing, true) ||
            missing)
            return false;
        document.control_identity = control;
        auto record = MetadataRecord(document, "cleanup");
        record["root_identity"] = document.root_identity;
        record["control_identity"] = control;
        // No previous committed world exists. This existing cleanup record must
        // be durable before registry removals: recovery must never recreate a
        // registry file whose identity has already been released.
        if (!AppendMetadata(save_dir, document, record, control, errors))
            return false;
        document.cleanup = true;
    }
    for (const bool index_batch : {true, false}) {
        std::map<std::string, RetirementReceipt> candidates;
        std::vector<std::pair<fs::path, BoundPublicationStage>> removals;
        for (const auto& file : files) {
            const auto name =
                Filesystem::GenericUtf8PathString(file.path.lexically_relative(save_dir));
            if ((name == kSnapshotIndexRelativePath) != index_batch)
                continue;
            const auto entry = std::find_if(document.entries.begin(),
                                            document.entries.end(),
                                            [&](const auto& value) { return value.path == name; });
            const auto stage =
                entry == document.entries.end()
                    ? CleanupStage(save_dir, document.undo_directory_identity, file.path)
                    : JournalStage(
                          save_dir, document.undo_directory_identity, "retired", file.path);
            removals.emplace_back(file.path, stage);
            if (kNeedsFileReceipts) {
                // The member's retirement slot is already admitted by its entry.
                // A temporary also needs its original name covered on retry.
                for (const auto& candidate :
                     {entry == document.entries.end() ? file.path : fs::path{},
                      stage.directory / stage.name}) {
                    if (candidate.empty())
                        continue;
                    const auto relative =
                        Filesystem::GenericUtf8PathString(candidate.lexically_relative(save_dir));
                    candidates.emplace(
                        relative,
                        RetirementReceipt{.path = relative,
                                          .completion = {.identity = file.identity}});
                }
            }
        }
        if (!PrepareReceipts(save_dir, document, candidates, control, errors))
            return false;
        std::vector<ReceiptRemoval> completions;
        for (const auto& [path, stage] : removals) {
            const auto file = std::find_if(
                files.begin(), files.end(), [&](const auto& value) { return value.path == path; });
            ArtifactIdentity actual{};
            bool missing = false;
            if (!WorldSaveService::artifact_identity(path, actual, missing) ||
                (!missing &&
                 (actual != file->identity || !WorldSaveService::remove_owned_entry(
                                                  path, file->identity, errors, false, &stage))))
                return false;
            if (!kNeedsFileReceipts) {
                if (!CompleteRemoval(
                        save_dir, document, path, file->identity, control, errors, false, &stage))
                    return false;
                continue;
            }
            for (const auto& candidate : {path, stage.directory / stage.name}) {
                const auto name =
                    Filesystem::GenericUtf8PathString(candidate.lexically_relative(save_dir));
                if (candidates.contains(name))
                    completions.push_back(
                        ReceiptRemovalFor(save_dir, document, ReceiptFor(document, name), &stage));
            }
        }
        if (CompleteReceipts(save_dir, document, completions, control, errors) !=
            detail::NamespaceRetirementOutcome::Retired)
            return false;
    }
    return true;
}

bool FinishLegacyJournal(const fs::path& save_dir,
                         const JournalDocument& document,
                         const ArtifactIdentity& control,
                         std::vector<std::string>* errors) {
    std::map<fs::path, ArtifactIdentity> files;
    if (!JournalInventory(save_dir, document, control, files, errors))
        return false;
    const auto journal = journal_directory(save_dir);
    for (const auto& [path, identity] : files) {
        if (path == journal / "txn.json" || path == journal / "undo/cleanup.txn.json")
            continue;
        ArtifactIdentity actual{};
        bool missing = false;
        const auto slot = CleanupStage(save_dir, document.undo_directory_identity, path);
        if (!WorldSaveService::artifact_identity(path, actual, missing) ||
            (!missing &&
             !WorldSaveService::remove_owned_entry(path, identity, errors, false, &slot)))
            return false;
    }
    const auto control_slot =
        CleanupStage(save_dir, document.undo_directory_identity, journal / "txn.json");
    if (control != ArtifactIdentity{} &&
        !WorldSaveService::remove_owned_entry(
            journal / "txn.json", control, errors, false, &control_slot))
        return false;
    report_snapshot_step("journal-unlinked", journal / "txn.json");
    const BoundPublicationStage undo_slot{.directory = journal,
                                          .name = "undo",
                                          .directory_identity = document.journal_identity,
                                          .report = &report_snapshot_step};
    return WorldSaveService::remove_owned_entry(
               journal / "undo", document.undo_directory_identity, errors, true, &undo_slot) &&
           WorldSaveService::remove_owned_entry(journal, document.journal_identity, errors, true);
}

// The original control stream becomes the independently discoverable receipt carrier.
// Its identity is recorded before the move. It remains outside both directories being
// retired, and is itself released only after all recorded completions succeed.
bool FinishJournal(const fs::path& save_dir,
                   JournalDocument& document,
                   ArtifactIdentity control,
                   std::vector<std::string>* errors) {
    if (!document.modern)
        return FinishLegacyJournal(save_dir, document, control, errors);
    const auto journal = journal_directory(save_dir);
    std::map<fs::path, ArtifactIdentity> files;
    if (!JournalInventory(save_dir, document, control, files, errors, document.cleanup))
        return false;
    if (!document.cleanup) {
        bool missing = false;
        if (!WorldSaveService::artifact_identity(save_dir, document.root_identity, missing, true) ||
            missing)
            return false;
        document.control_identity = control;
        auto record = MetadataRecord(document, "cleanup");
        record["root_identity"] = document.root_identity;
        record["control_identity"] = control;
        if (!AppendMetadata(save_dir, document, record, control, errors))
            return false;
        document.cleanup = true;
    }
    if (document.control_identity != control ||
        !MatchesIdentity(save_dir, document.root_identity, true)) {
        AddError(errors, "snapshot receipt carrier ownership changed");
        return false;
    }
    // POSIX needs only directory receipts unless recovery already recorded a file.
    std::map<std::string, RetirementReceipt> candidates;
    const auto prepare = [&](const fs::path& path,
                             const ArtifactIdentity& identity,
                             bool directory) {
        if (identity != ArtifactIdentity{}) {
            const auto name = Filesystem::GenericUtf8PathString(path.lexically_relative(save_dir));
            candidates.emplace(name,
                               RetirementReceipt{.path = name,
                                                 .completion = {.identity = identity},
                                                 .directory = directory,
                                                 .absent_only = false,
                                                 .retired = false});
        }
    };
    for (const auto& [path, identity] : files)
        if (kNeedsFileReceipts)
            prepare(path, identity, false);
    prepare(journal / "undo", document.undo_directory_identity, true);
    prepare(journal, document.journal_identity, true);
    if (!PrepareReceipts(save_dir, document, candidates, control, errors))
        return false;
    if (!document.external_control) {
        ArtifactIdentity occupant{};
        bool missing = false;
        const auto carrier = ReceiptCarrier(save_dir, document);
        if (!WorldSaveService::artifact_identity(carrier, occupant, missing) || !missing) {
            AddError(errors, "snapshot receipt carrier already occupied");
            return false;
        }
        const auto slot =
            CleanupStage(save_dir, document.undo_directory_identity, journal / "txn.json");
        if (!WorldSaveService::publish_restoration(
                journal / "txn.json", carrier, {}, control, errors, &slot))
            return false;
        document.external_control = true;
        report_snapshot_step("crash-receipt-carrier-published", carrier);
    }
    std::vector<ReceiptRemoval> file_removals;
    for (const auto& [path, identity] : files) {
        if (identity == ArtifactIdentity{})
            continue;
        const auto name = Filesystem::GenericUtf8PathString(path.lexically_relative(save_dir));
        const auto slot = CleanupStage(save_dir, document.undo_directory_identity, path);
        const auto found = LatestReceipt(document, name);
        if (found == document.receipts.end()) {
            ArtifactIdentity actual{};
            bool missing = false;
            const auto links = document.external_control && identity == document.control_identity
                                   ? ControlAliasLinks(save_dir, document)
                                   : std::numeric_limits<unsigned>::max();
            if (links == 0 ||
                !WorldSaveService::artifact_identity(path, actual, missing, false, links) ||
                (!missing && actual != identity))
                return false;
            // Missing POSIX inventory slots need no individual phase records:
            // the directory receipts below repeat their namespace barriers.
            if (missing)
                continue;
            if (!CompleteRemoval(save_dir, document, path, identity, control, errors, false, &slot))
                return false;
            continue;
        }
        file_removals.push_back(ReceiptRemovalFor(save_dir, document, found->second, &slot));
    }
    const auto file_outcome = CompleteReceipts(save_dir, document, file_removals, control, errors);
    if (file_outcome == detail::NamespaceRetirementOutcome::Failed)
        return false;
    bool all_retired = file_outcome == detail::NamespaceRetirementOutcome::Retired;
    report_snapshot_step("journal-unlinked", journal / "txn.json");
    std::vector<ReceiptRemoval> directory_removals;
    for (const auto& path : {journal / "undo", journal}) {
        const auto name = Filesystem::GenericUtf8PathString(path.lexically_relative(save_dir));
        directory_removals.push_back(
            ReceiptRemovalFor(save_dir, document, ReceiptFor(document, name), nullptr));
    }
    const auto directory_outcome =
        CompleteReceipts(save_dir, document, directory_removals, control, errors);
    if (directory_outcome == detail::NamespaceRetirementOutcome::Failed)
        return false;
    all_retired = all_retired && directory_outcome == detail::NamespaceRetirementOutcome::Retired;
    all_retired =
        all_retired &&
        std::all_of(document.receipts.begin(), document.receipts.end(), [](const auto& item) {
            return item.second.retired || item.second.reset;
        });
    if (!all_retired) {
        AddError(errors, "namespace receipts retained; filesystem durability unqualified");
        return true;
    }
    report_snapshot_step("crash-before-receipt-retirement", ReceiptCarrier(save_dir, document));
    const BoundPublicationStage carrier_slot{.directory = save_dir,
                                             .name = ReceiptCarrier(save_dir, document).filename(),
                                             .directory_identity = document.root_identity,
                                             .report = &report_snapshot_step};
    return WorldSaveService::remove_owned_entry(
        ReceiptCarrier(save_dir, document), control, errors, false, &carrier_slot);
}

// Receipt names are a closed, bounded inventory in the save root. They remain
// discoverable after txn.json and both journal directories have disappeared.
bool RecoverReceiptCarriers(const fs::path& save_dir, std::vector<std::string>* errors) {
    std::vector<fs::path> carriers;
    std::error_code iterator_error;
    fs::directory_iterator iterator(save_dir, iterator_error);
    if (iterator_error == std::errc::no_such_file_or_directory)
        return true;
    if (iterator_error)
        return false;
    std::size_t inspected = 0;
    constexpr std::string_view prefix = "journal.receipts.";
    for (const fs::directory_iterator end; iterator != end; iterator.increment(iterator_error)) {
        if (iterator_error || inspected++ == kMaxSnapshotMembers)
            return false;
        const auto name = Filesystem::GenericUtf8PathString(iterator->path().filename());
        if (name.starts_with(prefix))
            carriers.push_back(iterator->path());
    }
    if (iterator_error)
        return false;
    std::sort(carriers.begin(), carriers.end());
    for (const auto& carrier : carriers) {
        std::string bytes;
        ArtifactIdentity identity{};
        {
            const SavedInputFile input(carrier);
            if (!input.identity(identity) || !input.read(bytes, kMaxSnapshotIndexBytes)) {
                AddError(errors, "unreadable snapshot receipt carrier");
                return false;
            }
        }
        JournalDocument document;
        std::string error;
        if (!ParseJournal(bytes, document, error) || !document.modern || !document.cleanup ||
            ReceiptCarrier(save_dir, document) != carrier ||
            document.control_identity != identity ||
            !MatchesIdentity(save_dir, document.root_identity, true)) {
            AddError(errors, "invalid snapshot receipt carrier");
            return false;
        }
        document.external_control = true;
        std::map<fs::path, ArtifactIdentity> files;
        if (!JournalInventory(save_dir, document, identity, files, errors, true, true))
            return false;
        ArtifactIdentity journal_identity{};
        bool missing = false;
        if (!WorldSaveService::artifact_identity(
                journal_directory(save_dir), journal_identity, missing, true))
            return false;
        if (!missing && journal_identity != document.journal_identity) {
            // An unqualified completed cleanup may coexist with a later transaction.
            // Refuse every removal through the old names and retain its evidence.
            // Prepared receipts cannot reach this state: cleanup was incomplete.
            const bool unqualified = std::any_of(
                document.receipts.begin(), document.receipts.end(), [](const auto& item) {
                    return !item.second.retired && !item.second.reset;
                });
            if (!unqualified ||
                std::any_of(document.receipts.begin(),
                            document.receipts.end(),
                            [](const auto& item) {
                                return !item.second.reset &&
                                       item.second.completion.phase ==
                                           detail::NamespaceReceiptPhase::Prepared;
                            }) ||
                !HasReceipt(document, "journal") || !HasReceipt(document, "journal/undo")) {
                AddError(errors, "snapshot receipt directory ownership changed");
                return false;
            }
            AddError(errors, "namespace receipts retained; later journal owns reused names");
            continue;
        }
        if (!FinishJournal(save_dir, document, identity, errors))
            return false;
    }
    return true;
}

} // namespace

fs::path snapshot_index_path(const fs::path& save_dir) {
    return save_dir / "chunks" / "region" / "snapshot.json";
}

fs::path journal_directory(const fs::path& save_dir) {
    return save_dir / "journal";
}

bool member_path_allowed(std::string_view relative_path) {
    if (relative_path.empty() || relative_path.front() == '/') {
        return false;
    }
    if (relative_path.find('\\') != std::string_view::npos ||
        relative_path.find(':') != std::string_view::npos ||
        relative_path.find("..") != std::string_view::npos) {
        return false;
    }
    if (relative_path == "world_info.json" || relative_path == "aether_state.efs" ||
        relative_path == "chunks/region/world-manifest.json" ||
        relative_path == "chunks/region/plant-entities.json" ||
        relative_path == "chunks/region/plant-identities.json" ||
        relative_path == "chunks/region/ground-objects.json" ||
        relative_path == "chunks/region/field-pages.fpg" ||
        relative_path == "chunks/region/active-regions.arl") {
        return true;
    }
    constexpr std::string_view kPrefix = "chunks/region/r.";
    constexpr std::string_view kSuffix = ".lmr";
    if (relative_path.size() <= kPrefix.size() + kSuffix.size() ||
        relative_path.substr(0, kPrefix.size()) != kPrefix ||
        relative_path.substr(relative_path.size() - kSuffix.size()) != kSuffix) {
        return false;
    }
    const std::string_view middle = relative_path.substr(
        kPrefix.size(), relative_path.size() - kPrefix.size() - kSuffix.size());
    const std::size_t dot = middle.find('.');
    if (dot == std::string_view::npos || middle.find('.', dot + 1) != std::string_view::npos) {
        return false;
    }
    return IsRegionCoordinate(middle.substr(0, dot)) && IsRegionCoordinate(middle.substr(dot + 1));
}

std::string encode_snapshot_index(const SnapshotIndex& index) {
    nlohmann::json root = nlohmann::json::object();
    root["schema"] = kSnapshotIndexSchema;
    root["generation"] = index.generation;
    root["tick"] = index.tick;
    root["txn_id"] = index.txn_id;
    nlohmann::json anchors = nlohmann::json::array();
    for (const auto& anchor : index.anchors) {
        anchors.push_back(nlohmann::json::array({anchor[0], anchor[1], anchor[2]}));
    }
    root["anchors"] = std::move(anchors);
    nlohmann::json members = nlohmann::json::array();
    for (const SnapshotMember& member : index.members) {
        nlohmann::json item = nlohmann::json::object();
        item["path"] = member.path;
        item["class"] = member.cls == MemberClass::Authoritative ? "authoritative" : "derived";
        item["state"] = member.present ? "present" : "absent";
        item["bytes"] = member.bytes;
        item["fnv64"] = HexFnv64(member.fnv64);
        item["generation"] = member.generation;
        members.push_back(std::move(item));
    }
    root["members"] = std::move(members);
    std::string bytes;
    if (!BoundedJsonDump(root, kMaxSnapshotIndexBytes - 1, bytes))
        throw std::length_error("snapshot control JSON compatibility limit");
    return bytes + "\n";
}

bool decode_snapshot_index(std::string_view bytes, SnapshotIndex& out, std::string& error) {
    out = SnapshotIndex{};
    if (bytes.size() > kMaxSnapshotIndexBytes) {
        error = "snapshot index exceeds size cap";
        return false;
    }
    if (bytes.size() > kMaxSnapshotIndexBytes || !SavedInputLimits::JsonDepthAllowed(bytes)) {
        error = "snapshot control JSON compatibility limit";
        return false;
    }
    const std::string text(bytes);
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "malformed snapshot JSON";
        return false;
    }
    const nlohmann::json* schema = Field(root, "schema");
    if (!schema || !schema->is_string() || schema->get<std::string>() != kSnapshotIndexSchema) {
        error = "unsupported snapshot schema";
        return false;
    }
    const nlohmann::json* generation = Field(root, "generation");
    const nlohmann::json* tick = Field(root, "tick");
    const nlohmann::json* txn_id = Field(root, "txn_id");
    if (!generation || !generation->is_number_unsigned() || generation->get<std::uint64_t>() < 1 ||
        !tick || !tick->is_number_unsigned() || !txn_id || !txn_id->is_number_unsigned()) {
        error = "snapshot header fields invalid";
        return false;
    }
    SnapshotIndex index;
    index.generation = generation->get<std::uint64_t>();
    index.tick = tick->get<std::uint64_t>();
    index.txn_id = txn_id->get<std::uint64_t>();

    const nlohmann::json* anchors = Field(root, "anchors");
    if (!anchors || !anchors->is_array()) {
        error = "snapshot anchors invalid";
        return false;
    }
    for (const nlohmann::json& anchor : *anchors) {
        if (!anchor.is_array() || anchor.size() != 3 || !anchor[0].is_number() ||
            !anchor[1].is_number() || !anchor[2].is_number()) {
            error = "snapshot anchor invalid";
            return false;
        }
        for (const auto& component : anchor) {
            const auto number = component.get<double>();
            if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max()) {
                error = "snapshot anchor outside float range";
                return false;
            }
        }
        index.anchors.push_back(
            {anchor[0].get<float>(), anchor[1].get<float>(), anchor[2].get<float>()});
    }

    const nlohmann::json* members = Field(root, "members");
    if (!members || !members->is_array() || members->size() > kMaxSnapshotMembers) {
        error = "snapshot members invalid";
        return false;
    }
    index.members.reserve(members->size());
    for (const nlohmann::json& item : *members) {
        const nlohmann::json* path = Field(item, "path");
        const nlohmann::json* cls = Field(item, "class");
        const nlohmann::json* state = Field(item, "state");
        const nlohmann::json* member_bytes = Field(item, "bytes");
        const nlohmann::json* fnv = Field(item, "fnv64");
        const nlohmann::json* member_generation = Field(item, "generation");
        if (!path || !path->is_string() || !cls || !cls->is_string() || !state ||
            !state->is_string() || !member_bytes || !member_bytes->is_number_unsigned() || !fnv ||
            !fnv->is_string() || !member_generation || !member_generation->is_number_unsigned()) {
            error = "snapshot member fields invalid";
            return false;
        }
        SnapshotMember member;
        member.path = path->get<std::string>();
        if (!member_path_allowed(member.path)) {
            error = "snapshot member path not allowed";
            return false;
        }
        if (!index.members.empty() && member.path <= index.members.back().path) {
            error = "snapshot members not sorted or duplicated";
            return false;
        }
        const std::string class_name = cls->get<std::string>();
        if (class_name == "authoritative") {
            member.cls = MemberClass::Authoritative;
        } else if (class_name == "derived") {
            member.cls = MemberClass::Derived;
        } else {
            error = "snapshot member class invalid";
            return false;
        }
        const std::string state_name = state->get<std::string>();
        if (state_name == "present") {
            member.present = true;
        } else if (state_name == "absent") {
            member.present = false;
        } else {
            error = "snapshot member state invalid";
            return false;
        }
        if (!ParseHex64(fnv->get<std::string>(), member.fnv64)) {
            error = "snapshot member digest invalid";
            return false;
        }
        member.bytes = member_bytes->get<std::uint64_t>();
        member.generation = member_generation->get<std::uint64_t>();
        index.members.push_back(std::move(member));
    }
    out = std::move(index);
    return true;
}

bool read_snapshot_index(const fs::path& save_dir,
                         SnapshotIndex& out,
                         std::vector<std::string>* errors,
                         const std::stop_token& stop) {
    out = SnapshotIndex{};
    if (stop.stop_requested()) {
        AddError(errors, "Saved-world validation cancelled.");
        return false;
    }
    const fs::path path = snapshot_index_path(save_dir);
    const SavedInputFile input(path);
    if (stop.stop_requested()) {
        AddError(errors, "Saved-world validation cancelled.");
        return false;
    }
    if (input.missing())
        return true;
    std::string bytes;
    if (!input.read(bytes, kMaxSnapshotIndexBytes, stop)) {
        AddError(errors,
                 stop.stop_requested() ? "Saved-world validation cancelled."
                                       : "unreadable or oversized snapshot index");
        return false;
    }
    std::string error;
    if (!decode_snapshot_index(bytes, out, error)) {
        out = SnapshotIndex{};
        AddError(errors, "corrupt snapshot index: " + error);
        return false;
    }
    return true;
}

bool snapshot_index_exists(const fs::path& save_dir) {
    // An invalid present input is not a clean miss; the reader must refuse it.
    const SavedInputFile input(snapshot_index_path(save_dir));
    return !input.missing();
}

std::uint64_t fnv1a64_bytes(const void* data, std::size_t size) {
    return Fnv1aUpdate(kFnvOffset, data, size);
}

bool hash_file(const fs::path& path,
               std::uint64_t& bytes,
               std::uint64_t& fnv64,
               bool& exists,
               std::vector<std::string>* errors,
               const std::stop_token& stop) {
    bytes = 0;
    fnv64 = 0;
    exists = false;
    if (stop.stop_requested()) {
        AddError(errors, "Saved-world validation cancelled.");
        return false;
    }
    const SavedInputFile input(path);
    if (stop.stop_requested()) {
        AddError(errors, "Saved-world validation cancelled.");
        return false;
    }
    if (input.missing())
        return true;
    std::uint64_t length = 0;
    if (!input.length(length) || length > kMaxUndoBytes) {
        AddError(errors,
                 "unreadable or oversized snapshot member: " +
                     Filesystem::GenericUtf8PathString(path));
        return false;
    }
    // Fixed storage; work is bounded by the admitted encoded member length.
    std::array<char, kHashBlockBytes> block{};
    fnv64 = kFnvOffset;
    while (bytes < length) {
        const auto count =
            static_cast<std::size_t>(std::min<std::uint64_t>(block.size(), length - bytes));
        if (!input.read_exact(block.data(), count, stop)) {
            AddError(errors, "partial snapshot member read");
            return false;
        }
        fnv64 = Fnv1aUpdate(fnv64, block.data(), count);
        bytes += count;
    }
    if (!input.at_end(stop)) {
        AddError(errors, "snapshot member changed while reading");
        return false;
    }
    exists = true;
    return true;
}

bool journal_pending(const fs::path& save_dir) {
    std::error_code ec;
    const auto status = fs::symlink_status(journal_directory(save_dir), ec);
    if (ec != std::errc::no_such_file_or_directory && (ec || fs::exists(status)))
        return true;
    ec.clear();
    fs::directory_iterator iterator(save_dir, ec);
    if (ec == std::errc::no_such_file_or_directory)
        return false;
    if (ec)
        return true;
    std::size_t inspected = 0;
    for (const fs::directory_iterator end; iterator != end; iterator.increment(ec)) {
        if (ec || inspected++ == kMaxSnapshotMembers ||
            Filesystem::GenericUtf8PathString(iterator->path().filename())
                .starts_with("journal.receipts."))
            return true;
    }
    return bool(ec);
}

RecoveryOutcome recover_pending_journal(const fs::path& save_dir,
                                        std::vector<std::string>* errors) {
    const WorldSaveService::SaveOwnership ownership;
    const detail::SnapshotWriteOwnership snapshot;
    try {
        if (!RecoverReceiptCarriers(save_dir, errors)) {
            AddError(errors, "snapshot receipt recovery refused");
            return RecoveryOutcome::Failed;
        }
        const fs::path journal = journal_directory(save_dir);
        std::error_code ec;
        const auto journal_status = fs::symlink_status(journal, ec);
        if (ec == std::errc::no_such_file_or_directory)
            ec.clear();
        const bool journal_exists = fs::exists(journal_status);
        if (journal_exists &&
            (!fs::is_directory(journal_status) || fs::is_symlink(journal_status))) {
            AddError(errors, "invalid snapshot journal directory");
            return RecoveryOutcome::Failed;
        }
        if (ec) {
            AddError(errors,
                     "cannot stat snapshot journal: " + Filesystem::GenericUtf8PathString(journal));
            return RecoveryOutcome::Failed;
        }
        if (!journal_exists) {
            // Unknown files in the strict inventory require refusal, never speculative cleanup.
            if (snapshot_index_exists(save_dir) && !ValidateTemporaryInventory(save_dir)) {
                AddError(errors,
                         "unknown temporary artifact under " +
                             Filesystem::GenericUtf8PathString(save_dir));
                return RecoveryOutcome::Failed;
            }
            return RecoveryOutcome::NothingToDo;
        }
        if (!ValidateTemporaryInventory(save_dir)) {
            AddError(errors, "unknown temporary artifact in snapshot inventory");
            return RecoveryOutcome::Failed;
        }
        const fs::path txn_path = journal / "txn.json";
        std::string bytes;
        ArtifactIdentity control{};
        bool missing = false;
        {
            const SavedInputFile transaction_input(txn_path);
            missing = transaction_input.missing();
            if (!missing && (!transaction_input.identity(control) ||
                             !transaction_input.read(bytes, kMaxSnapshotIndexBytes))) {
                AddError(errors, "unreadable or oversized snapshot journal");
                return RecoveryOutcome::Failed;
            }
        } // Close the handle and release its stripe before recovery publishes or removes files.
        if (missing) {
            // Cleanup can be interrupted between moving the control file into
            // its recorded private slot and unlinking it. Restore that same object
            // before replay; no undo payload is needed for already restored members.
            JournalDocument retiring;
            std::string parse_error;
            {
                const SavedInputFile staged(journal / "undo/cleanup.txn.json");
                if (!staged.missing()) {
                    if (!staged.identity(control) || !staged.read(bytes, kMaxSnapshotIndexBytes) ||
                        !ParseJournal(bytes, retiring, parse_error) ||
                        !MatchesIdentity(journal, retiring.journal_identity, true) ||
                        !MatchesIdentity(
                            journal / "undo", retiring.undo_directory_identity, true)) {
                        AddError(errors, "invalid retiring snapshot journal");
                        return RecoveryOutcome::Failed;
                    }
                    missing = false;
                }
            }
            if (!missing) {
                std::map<fs::path, ArtifactIdentity> inventory;
                if (!JournalInventory(save_dir, retiring, control, inventory, errors))
                    return RecoveryOutcome::Failed;
                const auto slot =
                    CleanupStage(save_dir, retiring.undo_directory_identity, txn_path);
                if (!WorldSaveService::publish_restoration(
                        slot.directory / slot.name, txn_path, {}, control, errors, &slot))
                    return RecoveryOutcome::Failed;
            }
        }
        if (missing) {
            // Only empty, bound containers are recognizable without a control record.
            // Unknown leaves are never debris that recovery may speculate about.
            ArtifactIdentity journal_identity{}, undo_identity{};
            bool absent = false;
            if (!WorldSaveService::artifact_identity(journal, journal_identity, absent, true) ||
                absent)
                return RecoveryOutcome::Failed;
            const auto undo = journal / "undo";
            if (!WorldSaveService::artifact_identity(undo, undo_identity, absent, true))
                return RecoveryOutcome::Failed;
            for (const auto& child : fs::directory_iterator(journal)) {
                if (child.path() != undo || absent || !fs::is_empty(undo)) {
                    AddError(errors, "unknown snapshot journal artifact without control record");
                    return RecoveryOutcome::Failed;
                }
            }
            const BoundPublicationStage undo_slot{.directory = journal,
                                                  .name = "undo",
                                                  .directory_identity = journal_identity,
                                                  .report = &report_snapshot_step};
            if ((!absent && !WorldSaveService::remove_owned_entry(
                                undo, undo_identity, errors, true, &undo_slot)) ||
                !WorldSaveService::remove_owned_entry(journal, journal_identity, errors, true))
                return RecoveryOutcome::Failed;
            return RecoveryOutcome::DiscardedStale;
        }
        JournalDocument document;
        std::string error;
        if (!ParseJournal(bytes, document, error)) {
            AddError(errors, "corrupt snapshot journal: " + error);
            return RecoveryOutcome::Failed;
        }
        if (document.cleanup)
            return FinishJournal(save_dir, document, control, errors)
                       ? RecoveryOutcome::DiscardedStale
                       : RecoveryOutcome::Failed;
        SnapshotIndex committed;
        if (!read_snapshot_index(save_dir, committed, errors)) {
            return RecoveryOutcome::Failed;
        }
        // Exclusive publication can be interrupted after retiring the old index and before
        // installing the new one. The journal's protected index is the undo authority then.
        const bool retired_index =
            committed.generation == 0 &&
            std::any_of(document.entries.begin(), document.entries.end(), [](const auto& entry) {
                return entry.path == kSnapshotIndexRelativePath && entry.action == "replace";
            });
        if (retired_index && !VerifyRetiredIndexUndo(save_dir, document, errors))
            return RecoveryOutcome::Failed;
        if (!retired_index && committed.generation != document.base_generation &&
            committed.generation != document.new_generation) {
            AddError(errors, "journal does not belong to committed snapshot generation");
            return RecoveryOutcome::Failed;
        }
        if (committed.generation == document.base_generation &&
            committed.txn_id != document.txn_id - 1) {
            AddError(errors, "journal does not follow committed transaction");
            return RecoveryOutcome::Failed;
        }
        if (committed.generation == document.new_generation &&
            committed.txn_id != document.txn_id) {
            AddError(errors, "journal does not belong to committed transaction");
            return RecoveryOutcome::Failed;
        }
        if (committed.generation != 0 && committed.generation == document.new_generation) {
            // The commit point was reached; only the cleanup is left.
            return FinishJournal(save_dir, document, control, errors)
                       ? RecoveryOutcome::DiscardedStale
                       : RecoveryOutcome::Failed;
        }
        std::map<fs::path, ArtifactIdentity> inventory;
        if (!JournalInventory(save_dir, document, control, inventory, errors))
            return RecoveryOutcome::Failed;
        if (!RollbackEntries(save_dir, document, control, errors)) {
            return RecoveryOutcome::Failed;
        }
        return FinishJournal(save_dir, document, control, errors) ? RecoveryOutcome::RolledBack
                                                                  : RecoveryOutcome::Failed;
    } catch (const std::exception& ex) {
        AddError(errors, std::string("snapshot recovery failed: ") + ex.what());
        return RecoveryOutcome::Failed;
    }
}

void set_snapshot_step_hook_for_testing(SnapshotStepHook hook) {
    StepHook() = hook;
}

void report_snapshot_step(const char* step, const fs::path& path) {
    if (SnapshotStepHook hook = StepHook()) {
        // Receipt relocation is a control-file disk transition, like the
        // initial journal publication. Keep it in the exhaustive disk walk;
        // member-publication walks count only logical payload boundaries.
        const std::string_view event(step);
        if ((event == "before-replace" || event == "after-replace" ||
             event == "publication-quarantined" || event == "retirement-quarantined") &&
            Filesystem::GenericUtf8PathString(path.filename()).starts_with("journal.receipts.")) {
            if (event == "before-replace")
                step = "crash-receipt-before-publication";
            else if (event == "after-replace")
                step = "crash-receipt-after-publication";
            else if (event == "publication-quarantined")
                step = "crash-receipt-publication-quarantined";
            else
                step = "crash-receipt-retirement-quarantined";
        }
        invalidate_snapshot_observations();
        hook(step, path);
    }
}

std::uint64_t snapshot_observation_revision() {
    return ObservationRevision().load(std::memory_order_relaxed);
}

void invalidate_snapshot_observations() {
    ObservationRevision().fetch_add(1, std::memory_order_relaxed);
}

SaveTransaction::SaveTransaction(std::filesystem::path save_dir,
                                 std::uint64_t tick,
                                 std::vector<std::array<float, 3>> anchors,
                                 MemberDigestFn digest_fn,
                                 CreatedWorldArtifacts* artifacts)
    : m_save_dir(std::move(save_dir))
    , m_tick(tick)
    , m_anchors(std::move(anchors))
    , m_digest_fn(digest_fn)
    , m_artifacts(artifacts) {
    if (ActiveTransaction() == nullptr) {
        ActiveTransaction() = this;
        m_registered = true;
    }
}

SaveTransaction::~SaveTransaction() {
    if (m_state == State::Open || m_state == State::Committing) {
        try {
            abort(nullptr);
        } catch (...) {
            // Best-effort rollback must not throw during unwinding; always release registration.
        }
    }
    release_registration();
}

void SaveTransaction::release_registration() noexcept {
    if (m_registered && ActiveTransaction() == this)
        ActiveTransaction() = nullptr;
    m_registered = false;
}

bool detail::CurrentThreadOwnsSaveTransaction() {
    return ActiveTransaction() != nullptr;
}

SaveTransaction* SaveTransaction::active_for(const fs::path& save_dir) {
    SaveTransaction* active = ActiveTransaction();
    if (active == nullptr || active->m_state != State::Open) {
        return nullptr;
    }
    return SamePath(active->m_save_dir, save_dir) ? active : nullptr;
}

bool SaveTransaction::begin(std::vector<std::string>* errors) {
    if (m_state != State::Idle) {
        AddError(errors, "save transaction already begun");
        return false;
    }
    // Publication hooks consult the thread's active slot; a transaction that does not
    // own it would journal nothing while another transaction's journal is consulted.
    if (!m_registered && ActiveTransaction() == nullptr) {
        ActiveTransaction() = this;
        m_registered = true;
    }
    if (!m_registered) {
        AddError(errors, "another save transaction is active on this thread");
        return false;
    }
    try {
        auto ownership = std::make_unique<TransactionOwnership>();
        if (recover_pending_journal(m_save_dir, errors) == RecoveryOutcome::Failed) {
            release_registration();
            return false;
        }
        if (!read_snapshot_index(m_save_dir, m_previous, errors)) {
            release_registration();
            return false;
        }
        if (m_previous.generation == std::numeric_limits<std::uint64_t>::max() ||
            m_previous.txn_id == std::numeric_limits<std::uint64_t>::max()) {
            AddError(errors, "snapshot generation identity exhausted");
            release_registration();
            return false;
        }
        m_new_generation = m_previous.generation + 1;
        m_txn_id = m_previous.txn_id + 1;
        // From here on, even a directory-create/flush failure needs journal cleanup.
        m_ownership = std::move(ownership);
        m_state = State::Open;
        std::error_code ec;
        fs::create_directories(m_save_dir, ec);
        if (!ec) {
            fs::create_directory(journal_directory(m_save_dir), ec);
            if (!ec)
                report_snapshot_step("crash-journal-created", journal_directory(m_save_dir));
        }
        if (!ec) {
            fs::create_directory(journal_directory(m_save_dir) / "undo", ec);
            if (!ec)
                report_snapshot_step("crash-undo-created", journal_directory(m_save_dir) / "undo");
        }
        if (ec) {
            AddError(errors,
                     "cannot create snapshot journal: " +
                         Filesystem::GenericUtf8PathString(m_save_dir));
            abort(errors);
            return false;
        }
        bool missing = false;
        if (!WorldSaveService::artifact_identity(
                journal_directory(m_save_dir), m_journal_identity, missing, true) ||
            missing ||
            !WorldSaveService::artifact_identity(
                journal_directory(m_save_dir) / "undo", m_undo_directory_identity, missing, true) ||
            missing) {
            AddError(errors, "cannot bind snapshot journal directories");
            abort(errors);
            return false;
        }
        if (!WorldSaveService::durable_flush_directory(journal_directory(m_save_dir), errors)) {
            abort(errors);
            return false;
        }
        if (!WorldSaveService::durable_flush_directory(m_save_dir, errors)) {
            abort(errors);
            return false;
        }
        if (!write_journal(errors)) {
            abort(errors);
            return false;
        }
        // A fixed reserve is released before rollback, even if other work consumes the
        // remaining process descriptors. Member protection itself retains no handles.
        {
            const SavedInputFile control(journal_directory(m_save_dir) / "txn.json");
            for (auto& handle : m_rollback_handles) {
                handle = control.retain_identity();
                if (!handle) {
                    AddError(errors, "cannot reserve snapshot rollback handles");
                    throw std::runtime_error("cannot reserve snapshot rollback handles");
                }
            }
        }
        report_snapshot_step("journal-published", journal_directory(m_save_dir) / "txn.json");
        return true;
    } catch (const std::exception& ex) {
        AddError(errors, std::string("cannot begin snapshot transaction: ") + ex.what());
        abort(errors);
        release_registration();
        return false;
    }
}

bool SaveTransaction::prepare_publication(const fs::path& destination,
                                          const fs::path& incoming,
                                          std::vector<std::string>* errors) {
    auto* transaction = ActiveTransaction();
    if (!transaction ||
        (transaction->m_state != State::Open && transaction->m_state != State::Committing))
        return true;
    std::string relative;
    if (!RelativePublicationPath(transaction->m_save_dir, destination, relative)) {
        AddError(errors, "snapshot publication path escapes transaction root");
        return false;
    }
    const auto position = transaction->m_entry_indices.find(relative);
    if (position == transaction->m_entry_indices.end()) {
        const bool control =
            !member_path_allowed(relative) && relative != kSnapshotIndexRelativePath;
        if (!control)
            AddError(errors, "snapshot publication member is not protected: " + relative);
        return control;
    }
    auto* entry = &transaction->m_entries[position->second];
    if (!check_publication_target(destination, errors))
        return false;
    // Failed retries must preserve the installed stamp even when its native identity was reused.
    ArtifactIdentity installed{};
    bool missing = false;
    if (!WorldSaveService::artifact_identity(destination, installed, missing))
        return false;
    if (missing)
        entry->installed_version = {};
    else if (installed == entry->incoming_identity)
        entry->installed_version = entry->incoming_version;
    else if (installed != entry->installed_identity && installed == entry->previous_identity)
        entry->installed_version = entry->previous_version;
    if (!missing)
        entry->installed_identity = installed;
    entry->incoming_identity = {};
    entry->incoming_temporary.clear();
    if (!incoming.empty() &&
        !RelativePublicationPath(transaction->m_save_dir, incoming, entry->incoming_temporary)) {
        AddError(errors, "snapshot incoming path escapes transaction root");
        return false;
    }
    entry->incoming_version = {};
    if (!incoming.empty()) {
        const SavedInputFile input(incoming);
        if (!input.identity(entry->incoming_identity) || !input.version(entry->incoming_version))
            return false;
    }
    transaction->m_changed_entries.push_back(position->second);
    return transaction->write_journal(errors);
}

bool SaveTransaction::publication_identity(const fs::path& destination,
                                           std::array<std::uint64_t, 3>& expected,
                                           std::array<std::uint64_t, 3>& incoming) {
    auto* transaction = ActiveTransaction();
    if (!transaction ||
        (transaction->m_state != State::Open && transaction->m_state != State::Committing))
        return false;
    std::string relative;
    if (!RelativePublicationPath(transaction->m_save_dir, destination, relative))
        return false;
    const auto entry = transaction->m_entry_indices.find(relative);
    if (entry == transaction->m_entry_indices.end())
        return false;
    expected = transaction->m_entries[entry->second].installed_identity;
    incoming = transaction->m_entries[entry->second].incoming_identity;
    return true;
}

bool SaveTransaction::publication_stages(const fs::path& destination,
                                         BoundPublicationStage& source,
                                         BoundPublicationStage& retired) {
    auto* transaction = ActiveTransaction();
    if (!transaction ||
        (transaction->m_state != State::Open && transaction->m_state != State::Committing))
        return false;
    std::string relative;
    if (!RelativePublicationPath(transaction->m_save_dir, destination, relative) ||
        !transaction->m_entry_indices.contains(relative))
        return false;
    source = JournalStage(
        transaction->m_save_dir, transaction->m_undo_directory_identity, "incoming", destination);
    retired = JournalStage(
        transaction->m_save_dir, transaction->m_undo_directory_identity, "retired", destination);
    return true;
}

std::optional<bool> SaveTransaction::complete_owned_removal(const fs::path& path,
                                                            std::vector<std::string>* errors,
                                                            bool publication) {
    auto* transaction = ActiveTransaction();
    if (!transaction ||
        (transaction->m_state != State::Open && transaction->m_state != State::Committing))
        return std::nullopt;
    std::string relative;
    if (!RelativePublicationPath(transaction->m_save_dir, path, relative))
        return publication ? std::nullopt : std::optional<bool>{false};
    const auto owned_path = transaction->m_save_dir / relative;
    const auto position = transaction->m_entry_indices.find(relative);
    if (publication && position == transaction->m_entry_indices.end())
        return std::nullopt;
    JournalDocument document;
    {
        const SavedInputFile input(journal_directory(transaction->m_save_dir) / "txn.json");
        ArtifactIdentity identity{};
        std::string bytes, error;
        if (!input.identity(identity) || identity != transaction->m_control_identity ||
            !input.read(bytes, kMaxSnapshotIndexBytes) || !ParseJournal(bytes, document, error) ||
            document.cleanup || !document.modern)
            return false;
    }
    document.append_reserve = transaction->m_rollback_reserve;
    std::map<fs::path, ArtifactIdentity> inventory;
    if (!JournalInventory(
            transaction->m_save_dir, document, transaction->m_control_identity, inventory, errors))
        return false;
    if (!publication) {
        const auto found = inventory.find(owned_path);
        if (found == inventory.end()) {
            AddError(errors, "staging outside recorded snapshot inventory");
            return false;
        }
        const auto slot =
            CleanupStage(transaction->m_save_dir, document.undo_directory_identity, owned_path);
        return CompleteRemoval(transaction->m_save_dir,
                               document,
                               owned_path,
                               found->second,
                               transaction->m_control_identity,
                               errors,
                               false,
                               &slot);
    }
    const auto& entry = transaction->m_entries[position->second];
    const auto identity = entry.installed_identity;
    if (!transaction->check_member_version(entry, errors))
        return false;
    const auto stage =
        JournalStage(transaction->m_save_dir, document.undo_directory_identity, "retired", path);
    if (identity == ArtifactIdentity{}) {
        // No object has ever been installed. POSIX still repeats its barrier;
        // Windows records the already-absent create using the existing protocol.
#if defined(_WIN32)
        if (!PrepareReceipt(transaction->m_save_dir,
                            document,
                            owned_path,
                            document.journal_identity,
                            false,
                            transaction->m_control_identity,
                            errors,
                            true))
            return false;
        return CompleteReceipt(transaction->m_save_dir,
                               document,
                               ReceiptFor(document, relative),
                               transaction->m_control_identity,
                               nullptr,
                               errors) != detail::NamespaceRetirementOutcome::Failed;
#else
        return WorldSaveService::durable_flush_directory(owned_path.parent_path(), errors);
#endif
    }
    // Publication reuses a private slot, never the destination's receipt name:
    // a later publication may install a different identity at the destination.
    const auto retired = stage.directory / stage.name;
    const auto retired_name =
        Filesystem::GenericUtf8PathString(retired.lexically_relative(transaction->m_save_dir));
    ArtifactIdentity actual{};
    bool missing = false;
    if (!WorldSaveService::artifact_identity(owned_path, actual, missing) ||
        (!missing && actual != identity))
        return false;
    // A fresh occupant starts another generation even when its native identity
    // equals a historical one. An absent retry keeps the existing generation.
    if ((kNeedsFileReceipts || HasReceipt(document, retired_name)) &&
        !PrepareReceipt(transaction->m_save_dir,
                        document,
                        retired,
                        identity,
                        false,
                        transaction->m_control_identity,
                        errors,
                        false,
                        !missing))
        return false;
    if (!missing && !WorldSaveService::remove_owned_entry(path, identity, errors, false, &stage))
        return false;
        // Retained is usable for forward saves on unqualified filesystems, but
        // never for dependent rollback work (CompleteRemoval still requires Retired).
#if defined(_WIN32)
    const auto name =
        Filesystem::GenericUtf8PathString(retired.lexically_relative(transaction->m_save_dir));
    return CompleteReceipt(transaction->m_save_dir,
                           document,
                           ReceiptFor(document, name),
                           transaction->m_control_identity,
                           &stage,
                           errors) != detail::NamespaceRetirementOutcome::Failed;
#else
    return CompleteRemoval(transaction->m_save_dir,
                           document,
                           retired,
                           identity,
                           transaction->m_control_identity,
                           errors,
                           false,
                           &stage) &&
           WorldSaveService::durable_flush_directory(owned_path.parent_path(), errors);
#endif
}

bool SaveTransaction::check_member_version(const Entry& entry,
                                           std::vector<std::string>* errors) const {
    SavedInputFile::Version current{};
    bool missing = false;
    const bool inspected =
        WorldSaveService::artifact_version(m_save_dir / entry.path, current, missing);
    if (inspected && missing)
        return true;
    if (!inspected || (!MatchesPublicationVersion(current, entry.previous_version) &&
                       !MatchesPublicationVersion(current, entry.installed_version) &&
                       !MatchesPublicationVersion(current, entry.incoming_version))) {
        AddError(errors, "snapshot publication occupant changed: " + entry.path);
        return false;
    }
    return true;
}

bool SaveTransaction::check_publication_target(const fs::path& destination,
                                               std::vector<std::string>* errors) {
    auto* transaction = ActiveTransaction();
    if (!transaction ||
        (transaction->m_state != State::Open && transaction->m_state != State::Committing))
        return true;
    std::string relative;
    if (!RelativePublicationPath(transaction->m_save_dir, destination, relative)) {
        AddError(errors, "snapshot publication path escapes transaction root");
        return false;
    }
    const auto position = transaction->m_entry_indices.find(relative);
    if (position == transaction->m_entry_indices.end()) {
        const bool control =
            !member_path_allowed(relative) && relative != kSnapshotIndexRelativePath;
        if (!control)
            AddError(errors, "snapshot publication member is not protected: " + relative);
        return control;
    }
    const auto* entry = &transaction->m_entries[position->second];
    if (!transaction->check_member_version(*entry, errors))
        return false;
    if (!entry->incoming_temporary.empty()) {
        SavedInputFile::Version incoming{};
        bool missing_incoming = false;
        if (!WorldSaveService::artifact_version(
                transaction->m_save_dir / entry->incoming_temporary, incoming, missing_incoming) ||
            (!missing_incoming && !MatchesPublicationVersion(incoming, entry->incoming_version))) {
            AddError(errors, "snapshot publication occupant changed: " + relative);
            return false;
        }
    }
    std::array<std::uint64_t, 3> current{};
    bool missing = false;
    if (!WorldSaveService::artifact_identity(destination, current, missing) ||
        (!missing && current != entry->previous_identity && current != entry->installed_identity &&
         current != entry->incoming_identity)) {
        AddError(errors, "snapshot publication occupant changed: " + relative);
        return false;
    }
    return true;
}

bool SaveTransaction::protect(const std::vector<std::string>& relative_paths,
                              std::vector<std::string>* errors) {
    if (m_state != State::Open) {
        AddError(errors, "save transaction is not open");
        return false;
    }
    try {
        std::vector<std::string> todo;
        if (m_entries.empty()) {
            todo.emplace_back(kSnapshotIndexRelativePath);
        }
        todo.insert(todo.end(), relative_paths.begin(), relative_paths.end());

        const fs::path undo_directory = journal_directory(m_save_dir) / "undo";
        bool added = false;
        for (const std::string& rel : todo) {
            if (m_protected.contains(rel))
                continue;
            if (m_entries.size() == kMaxSnapshotMembers) {
                AddError(errors, "snapshot member count limit");
                return false;
            }
            if (rel != kSnapshotIndexRelativePath && !member_path_allowed(rel)) {
                AddError(errors, "path not allowed in snapshot transaction: " + rel);
                return false;
            }
            const fs::path destination = m_save_dir / rel;
            Entry entry;
            fs::path undo_temporary;
            entry.path = rel;
            std::string previous;
            bool missing = false;
            {
                const SavedInputFile input(destination);
                missing = input.missing();
                if (!missing && (!input.identity(entry.previous_identity) ||
                                 !input.version(entry.previous_version) ||
                                 !input.read(previous, kMaxUndoBytes))) {
                    AddError(errors, "unreadable or oversized transaction member: " + rel);
                    return false;
                }
            } // An undo filename can share this reader's publication stripe.
            if (!missing) {
                const std::string undo_name = std::to_string(m_next_undo);
                fs::path temporary;
                if (!WorldSaveService::prepare_restoration(
                        m_save_dir / undo_name, previous, temporary, entry.undo_identity, errors))
                    return false;
                ++m_next_undo;
                entry.action = "replace";
                entry.prev_bytes = previous.size();
                entry.prev_fnv64 = fnv1a64_bytes(previous.data(), previous.size());
                entry.undo = "undo/" + undo_name;
                entry.undo_temporary =
                    Filesystem::GenericUtf8PathString(temporary.lexically_relative(m_save_dir));
                undo_temporary = std::move(temporary);
            } else {
                entry.action = "create";
            }
            const auto index = m_entries.size();
            m_protected.insert(entry.path);
            m_entry_indices.emplace(entry.path, index);
            m_entries.push_back(std::move(entry));
            m_changed_entries.push_back(index);
            if (!write_journal(errors))
                return false;
            const auto& protected_entry = m_entries.back();
            const auto undo_stage =
                JournalStage(m_save_dir, m_undo_directory_identity, "undo", protected_entry.undo);
            if (!protected_entry.undo.empty() &&
                !WorldSaveService::publish_restoration(undo_temporary,
                                                       journal_directory(m_save_dir) /
                                                           protected_entry.undo,
                                                       {},
                                                       protected_entry.undo_identity,
                                                       errors,
                                                       &undo_stage))
                return false;
            added = true;
        }
        if (!added) {
            return true;
        }
        if (!WorldSaveService::durable_flush_directory(undo_directory, errors)) {
            return false;
        }
        if (!write_journal(errors)) {
            return false;
        }
        report_snapshot_step("journal-published", journal_directory(m_save_dir) / "txn.json");
        return true;
    } catch (const std::exception& ex) {
        AddError(errors, std::string("cannot protect snapshot members: ") + ex.what());
        return false;
    }
}

bool SaveTransaction::write_journal(std::vector<std::string>* errors) {
    if (m_control_identity == std::array<std::uint64_t, 3>{}) {
        // Control name, its quarantine, and the two directories each need at
        // most five small completion records, plus the cleanup header.
        m_rollback_reserve = 32u * 1024u;
        const auto header = EncodeJournal(m_txn_id,
                                          m_previous.generation,
                                          m_new_generation,
                                          {},
                                          m_journal_identity,
                                          m_undo_directory_identity);
        if (!WorldSaveService::append_journal_record(journal_directory(m_save_dir) / "txn.json",
                                                     header,
                                                     m_control_identity,
                                                     errors,
                                                     m_rollback_reserve))
            return false;
    }
    for (const auto index : m_changed_entries) {
        auto& entry = m_entries[index];
        JournalEntry copy;
        copy.path = entry.path;
        copy.action = entry.action;
        copy.prev_bytes = entry.prev_bytes;
        copy.prev_fnv64 = entry.prev_fnv64;
        copy.undo = entry.undo;
        copy.previous_identity = entry.previous_identity;
        copy.installed_identity = entry.installed_identity;
        copy.incoming_identity = entry.incoming_identity;
        copy.restored_identity = entry.restored_identity;
        copy.undo_identity = entry.undo_identity;
        copy.rollback_retired_identity = entry.rollback_retired_identity;
        copy.undo_temporary = entry.undo_temporary;
        copy.incoming_temporary = entry.incoming_temporary;
        copy.restored_temporary = entry.restored_temporary;
        const auto record = EncodeJournal(m_txn_id,
                                          m_previous.generation,
                                          m_new_generation,
                                          {copy},
                                          m_journal_identity,
                                          m_undo_directory_identity);
        // Recovery appends at most retirement plus restoration for each member.
        // Reserve their largest encodings before admitting any forward mutation.
        copy.rollback_retired_identity.fill(std::numeric_limits<std::uint64_t>::max());
        copy.restored_identity.fill(std::numeric_limits<std::uint64_t>::max());
        copy.installed_identity.fill(std::numeric_limits<std::uint64_t>::max());
        copy.incoming_identity.fill(std::numeric_limits<std::uint64_t>::max());
        const auto maximum_counter = std::to_string(std::numeric_limits<std::uint64_t>::max());
        copy.restored_temporary =
            Filesystem::GenericUtf8PathString(fs::path(copy.path).filename()) + ".tmp." +
            maximum_counter + "." + maximum_counter;
        copy.incoming_temporary = copy.path + ".tmp." + maximum_counter + "." + maximum_counter;
        if (copy.path == "chunks/region/active-regions.arl")
            copy.incoming_temporary += ".0123456789abcdef0123456789abcdef";
        const auto rollback_entry_bytes = 2 * EncodeJournal(m_txn_id,
                                                            m_previous.generation,
                                                            m_new_generation,
                                                            {copy},
                                                            m_journal_identity,
                                                            m_undo_directory_identity)
                                                  .size();
        // At most sixteen member/temporary/quarantine names, five phase records
        // per name. Writer-generated names above bound the encoded path length;
        // 1024 bytes covers the fixed header, identities and JSON punctuation.
        const auto maximum_path_bytes =
            std::max(copy.incoming_temporary.size(), copy.restored_temporary.size()) + 64u;
        if (maximum_path_bytes > kMaxSnapshotIndexBytes / 80u - 1024u) {
            AddError(errors, "snapshot receipt reservation exceeds control byte limit");
            return false;
        }
        const auto receipt_bytes = kNeedsFileReceipts ? 80u * (1024u + maximum_path_bytes) : 0u;
        if (rollback_entry_bytes > kMaxSnapshotIndexBytes - receipt_bytes) {
            AddError(errors, "snapshot rollback reservation exceeds control byte limit");
            return false;
        }
        const auto rollback_bytes = rollback_entry_bytes + receipt_bytes;
        if (rollback_bytes > entry.rollback_reserve) {
            const auto additional = rollback_bytes - entry.rollback_reserve;
            if (additional > kMaxSnapshotIndexBytes - m_rollback_reserve) {
                AddError(errors, "snapshot journal rollback capacity exhausted");
                return false;
            }
            m_rollback_reserve += additional;
            entry.rollback_reserve = rollback_bytes;
        }
        auto append_reserve = m_rollback_reserve;
        {
            const SavedInputFile input(journal_directory(m_save_dir) / "txn.json");
            ArtifactIdentity identity{};
            JournalDocument document;
            std::string bytes, error;
            if (!input.identity(identity) || identity != m_control_identity ||
                !input.read(bytes, kMaxSnapshotIndexBytes) ||
                !ParseJournal(bytes, document, error) ||
                !ReservePendingReceipts(document, append_reserve)) {
                AddError(errors, "cannot reserve pending snapshot completion records");
                return false;
            }
        }
        if (!WorldSaveService::append_journal_record(journal_directory(m_save_dir) / "txn.json",
                                                     record,
                                                     m_control_identity,
                                                     errors,
                                                     append_reserve))
            return false;
    }
    m_changed_entries.clear();
    return true;
}

void SaveTransaction::defer_until_commit(std::function<void()> action) {
    if (m_state != State::Open)
        throw std::logic_error("cannot defer acknowledgement on a closed save transaction");
    m_committed_actions.push_back(std::move(action));
}

void SaveTransaction::remember_written_digest(const fs::path& destination,
                                              const MemberDigest& digest,
                                              const std::array<std::uint64_t, 8>& version) {
    if (m_state != State::Open || m_digest_fn != &WorldSaveService::member_digest)
        return;
    std::string relative;
    if (RelativePublicationPath(m_save_dir, destination, relative) &&
        m_protected.contains(relative))
        m_written_digests.insert_or_assign(relative, WrittenDigest{digest, version});
}

bool SaveTransaction::commit(std::vector<std::string>* errors) {
    if (m_state != State::Open) {
        AddError(errors, "save transaction is not open");
        return false;
    }
    try {
        SnapshotIndex next = m_previous;
        next.generation = m_new_generation;
        next.tick = m_tick;
        next.txn_id = m_txn_id;
        next.anchors = m_anchors;

        auto digest_member = [this, errors](const std::string& rel, MemberDigest& digest) {
            const auto written = m_written_digests.find(rel);
            if (written != m_written_digests.end()) {
                const SavedInputFile input(m_save_dir / rel);
                SavedInputFile::Version version{};
                if (input.version(version) && version == written->second.version) {
                    digest = written->second.digest;
                    return true;
                }
            }
            std::string error;
            if (m_digest_fn == nullptr || !m_digest_fn(m_save_dir, rel, digest, error)) {
                AddError(errors,
                         "cannot validate snapshot member " + rel +
                             (error.empty() ? std::string() : ": " + error));
                return false;
            }
            return true;
        };

        std::map<std::string, SnapshotMember> members;
        for (const SnapshotMember& member : m_previous.members) {
            members.emplace(member.path, member);
        }

        // Nothing writes between the baseline and entry passes of one commit, so an entry
        // the baseline already digested reuses that result.
        std::map<std::string, MemberDigest> baseline_digests;
        if (m_previous.generation == 0) {
            // First indexing of this directory: enumerate the baseline members.
            std::vector<std::string> baseline = {
                "world_info.json",
                "aether_state.efs",
                "chunks/region/world-manifest.json",
                "chunks/region/plant-entities.json",
                "chunks/region/plant-identities.json",
                "chunks/region/ground-objects.json",
                "chunks/region/field-pages.fpg",
                "chunks/region/active-regions.arl",
            };
            std::error_code ec;
            fs::directory_iterator it(m_save_dir / "chunks" / "region", ec);
            const fs::directory_iterator end;
            if (ec == std::errc::no_such_file_or_directory)
                ec.clear();
            std::size_t inspected = 0;
            // Count every entry, including ignored leaves, before extending the inventory.
            if (!ec) {
                for (; it != end; it.increment(ec)) {
                    if (ec) {
                        break;
                    }
                    if (inspected++ == kMaxSnapshotMembers) {
                        AddError(errors, "snapshot inventory count limit");
                        abort(errors);
                        return false;
                    }
                    const std::string rel =
                        "chunks/region/" + Filesystem::GenericUtf8PathString(it->path().filename());
                    if (member_path_allowed(rel)) {
                        baseline.push_back(rel);
                    }
                }
            }
            if (ec) {
                AddError(errors, "cannot enumerate complete snapshot inventory");
                abort(errors);
                return false;
            }
            for (const std::string& rel : baseline) {
                MemberDigest digest;
                if (!digest_member(rel, digest)) {
                    abort(errors);
                    return false;
                }
                baseline_digests.insert_or_assign(rel, digest);
                if (!digest.present) {
                    continue;
                }
                SnapshotMember member;
                member.path = rel;
                member.cls = MemberClass::Authoritative;
                member.present = true;
                member.bytes = digest.bytes;
                member.fnv64 = digest.fnv64;
                member.generation = m_new_generation;
                members[rel] = std::move(member);
            }
        }

        for (const Entry& entry : m_entries) {
            if (entry.path == kSnapshotIndexRelativePath) {
                continue;
            }
            MemberDigest digest;
            if (const auto baseline = baseline_digests.find(entry.path);
                baseline != baseline_digests.end()) {
                digest = baseline->second;
            } else if (!digest_member(entry.path, digest)) {
                abort(errors);
                return false;
            }
            const bool known = members.count(entry.path) != 0;
            if (!digest.present && entry.action != "replace" && !known) {
                continue; // absent and never indexed: not recorded
            }
            if (known) {
                const auto& previous = members.at(entry.path);
                if (previous.present == digest.present &&
                    previous.bytes == (digest.present ? digest.bytes : 0) &&
                    previous.fnv64 == (digest.present ? digest.fnv64 : 0)) {
                    continue; // unchanged content retains its member generation (design.md D5)
                }
            }
            SnapshotMember member;
            member.path = entry.path;
            member.cls = MemberClass::Authoritative;
            member.present = digest.present;
            member.bytes = digest.present ? digest.bytes : 0;
            member.fnv64 = digest.present ? digest.fnv64 : 0;
            member.generation = m_new_generation;
            members[entry.path] = std::move(member);
        }

        if (members.size() > kMaxSnapshotMembers) {
            AddError(errors, "too many snapshot members");
            abort(errors);
            return false;
        }
        next.members.clear();
        next.members.reserve(members.size());
        for (auto& [path, member] : members) {
            next.members.push_back(std::move(member));
        }
        const std::string encoded = encode_snapshot_index(next);
        if (encoded.size() > kMaxSnapshotIndexBytes) {
            AddError(errors, "snapshot index exceeds size cap");
            abort(errors);
            return false;
        }

        m_state = State::Committing;
        const fs::path index_path = snapshot_index_path(m_save_dir);
        std::error_code directory_error;
        if (m_artifacts) {
            for (const auto& directory : {m_save_dir / "chunks", index_path.parent_path()}) {
                if (!fs::exists(directory))
                    m_artifacts->create_directory(directory);
            }
        } else {
            fs::create_directories(index_path.parent_path(), directory_error);
        }
        if (directory_error) {
            AddError(errors, "cannot create snapshot index directory");
            abort(errors);
            return false;
        }
        // First saves create chunks/region (and possibly the save directory itself): persist each
        // new directory entry through its parent before the index can become the commit point.
        for (const fs::path& directory :
             {index_path.parent_path().parent_path(), m_save_dir, m_save_dir.parent_path()}) {
            std::error_code exists_error;
            if (!directory.empty() && fs::exists(directory, exists_error) &&
                !WorldSaveService::durable_flush_directory(directory, errors)) {
                abort(errors);
                return false;
            }
        }
        // Best effort for the ancestors above the save directory (a first world may have created
        // worlds/saves): a failure there cannot be repaired by rolling back.
        {
            fs::path ancestor = m_save_dir.parent_path();
            for (int level = 0; level < 3 && !ancestor.empty(); ++level) {
                WorldSaveService::durable_flush_directory(ancestor.parent_path(), nullptr);
                const fs::path parent = ancestor.parent_path();
                if (parent == ancestor)
                    break;
                ancestor = parent;
            }
        }
        const bool published =
            m_artifacts
                ? WorldSaveService::write_file_durably(index_path, encoded, errors, m_artifacts)
                : WorldSaveService::durable_write_file(
                      index_path, encoded, errors, m_save_dir / "snapshot.json");
        if (!published) {
            abort(errors);
            return false;
        }
        m_state = State::Committed;
        release_registration();
        m_rollback_handles = {};
        auto actions = std::move(m_committed_actions);
        for (auto& action : actions) {
            try {
                action();
            } catch (const std::exception& error) {
                AddError(errors,
                         std::string("snapshot committed but memory acknowledgement failed: ") +
                             error.what());
            }
        }
        report_snapshot_step("commit-point", index_path);
        recover_pending_journal(m_save_dir, errors);
        m_ownership.reset();
        return true;
    } catch (const std::exception& ex) {
        AddError(errors, std::string("snapshot commit failed: ") + ex.what());
        if (m_state == State::Committed) {
            m_ownership.reset();
            return true; // cleanup faults cannot undo a published durable index
        }
        abort(errors);
        return false;
    }
}

void SaveTransaction::abort(std::vector<std::string>* errors) {
    if (m_state == State::CleanupPending) {
        // A failed cleanup already appended phases; replay that stream instead
        // of reconstructing a pre-cleanup document from the live entries.
        const TransactionOwnership ownership;
        JournalDocument document;
        document.txn_id = m_txn_id;
        document.control_identity = m_control_identity;
        const auto journal = journal_directory(m_save_dir);
        if (!MatchesIdentity(ReceiptCarrier(m_save_dir, document), m_control_identity) &&
            !(MatchesIdentity(journal, m_journal_identity, true) &&
              (MatchesIdentity(journal / "txn.json", m_control_identity) ||
               MatchesIdentity(journal / "undo/cleanup.txn.json", m_control_identity)))) {
            if (journal_pending(m_save_dir))
                AddError(errors, "snapshot retry cleanup ownership changed");
            else if (m_artifacts && m_previous.generation == 0) {
                const auto error = m_artifacts->rollback();
                if (!error.empty())
                    AddError(errors, error);
                else
                    m_state = State::RolledBack;
            } else
                m_state = State::RolledBack;
            return;
        }
        if (recover_pending_journal(m_save_dir, errors) != RecoveryOutcome::Failed) {
            if (m_artifacts && m_previous.generation == 0) {
                const auto error = m_artifacts->rollback();
                if (!error.empty()) {
                    AddError(errors, error);
                    return;
                }
            }
            m_state = State::RolledBack;
        }
        return;
    }
    if (m_state != State::Open && m_state != State::Committing) {
        return;
    }
    m_rollback_handles = {};
    try {
        report_snapshot_step("before-rollback", m_save_dir);
        m_state = State::RollingBack;
        if (!m_artifacts || m_previous.generation != 0) {
            for (const auto& entry : m_entries) {
                if (!check_member_version(entry, errors)) {
                    m_state = State::RolledBack;
                    m_committed_actions.clear();
                    m_ownership.reset();
                    release_registration();
                    return;
                }
            }
        }
        if (!m_ownership)
            m_ownership = std::make_unique<TransactionOwnership>();
        JournalDocument document;
        {
            const SavedInputFile input(journal_directory(m_save_dir) / "txn.json");
            ArtifactIdentity identity{};
            std::string bytes, error;
            if (!input.identity(identity) || identity != m_control_identity ||
                !input.read(bytes, kMaxSnapshotIndexBytes) ||
                !ParseJournal(bytes, document, error) || document.txn_id != m_txn_id ||
                document.journal_identity != m_journal_identity ||
                document.undo_directory_identity != m_undo_directory_identity)
                throw std::runtime_error("cannot replay owned snapshot completion records");
        }
        for (auto& entry : document.entries) {
            const auto position = m_entry_indices.find(entry.path);
            if (position == m_entry_indices.end())
                throw std::runtime_error("unexpected live snapshot member");
            const auto& live = m_entries[position->second];
            entry.live_versions = {
                live.previous_version, live.installed_version, live.incoming_version};
        }
        if (m_artifacts && m_previous.generation == 0) {
            // Creation has no previous commit to restore. Its created-artifacts registry
            // retains identities and failed-rollback diagnostics until cleanup succeeds, so
            // the journal may retire first even if artifact rollback subsequently refuses.
            // Replacements must retain the recovery journal whenever rollback fails.
            std::map<fs::path, ArtifactIdentity> creation_inventory;
            if (!JournalInventory(
                    m_save_dir, document, m_control_identity, creation_inventory, errors))
                throw std::runtime_error("cannot admit initial snapshot cleanup inventory");
            m_artifacts->complete_files(
                journal_directory(m_save_dir),
                [&](const auto& path) { return creation_inventory.contains(path); },
                [&](auto& files) {
                    return CompleteCreatedFiles(
                        m_save_dir, document, m_control_identity, files, errors);
                });
            m_state = State::CleanupPending;
            if (FinishJournal(m_save_dir, document, m_control_identity, errors)) {
                const auto error = m_artifacts->rollback();
                if (!error.empty())
                    AddError(errors, error);
                else
                    m_state = State::RolledBack;
            }
        } else {
            std::map<fs::path, ArtifactIdentity> inventory;
            if (JournalInventory(m_save_dir, document, m_control_identity, inventory, errors) &&
                RollbackEntries(m_save_dir, document, m_control_identity, errors)) {
                m_state = State::CleanupPending;
                if (FinishJournal(m_save_dir, document, m_control_identity, errors))
                    m_state = State::RolledBack;
            }
        }
    } catch (const std::exception& ex) {
        AddError(errors, std::string("snapshot rollback failed: ") + ex.what());
    }
    if (m_state != State::CleanupPending && m_state != State::RolledBack)
        m_state = m_artifacts && m_previous.generation == 0 ? State::Open : State::RolledBack;
    m_committed_actions.clear();
    m_ownership.reset();
    release_registration();
}

} // namespace Luminumbra::Persistence
