#include "SaveTransaction.h"
#include "core/CaptureContentPaths.h"
#include "core/FilesystemPath.h"

#include "CreatedWorldArtifacts.h"
#include "SavedInputFile.h"
#include "SavedInputLimits.h"
#include "SavedJsonAdmission.h"
#include "WorldSaveService.h"
#include "fields/EnergyFieldState.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
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

SaveTransaction*& ActiveTransaction() {
    static thread_local SaveTransaction* active = nullptr;
    return active;
}

SnapshotStepHook& StepHook() {
    static SnapshotStepHook hook = nullptr;
    return hook;
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
};

struct JournalDocument {
    std::uint64_t txn_id = 0;
    std::uint64_t base_generation = 0;
    std::uint64_t new_generation = 0;
    std::vector<JournalEntry> entries;
    std::array<std::uint64_t, 3> journal_identity{};
    std::array<std::uint64_t, 3> undo_directory_identity{};
};

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
    if (!schema || !schema->is_string() || schema->get<std::string>() != kJournalSchema) {
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
    std::unordered_set<std::string> paths;
    for (const nlohmann::json& item : *entries) {
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
        if (!temporary_name(
                "undo_temporary", fs::path("journal") / entry.undo, entry.undo_temporary) ||
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
            !temporary_name("restored_temporary",
                            fs::path("journal") / fs::path(entry.path).filename(),
                            entry.restored_temporary)) {
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

// Journal v3 is a bounded stream of complete JSON records. Each append durably records
// ownership intent before publication. A partial record refuses recovery and retains evidence.
bool ParseJournal(std::string_view bytes, JournalDocument& out, std::string& error) {
    out = {};
    if (bytes.empty() || bytes.size() > kMaxSnapshotIndexBytes || bytes.back() != '\n') {
        error = "incomplete or oversized journal";
        return false;
    }
    std::unordered_map<std::string, std::size_t> entries;
    bool first = true;
    while (!bytes.empty()) {
        const auto end = bytes.find('\n');
        if (end == std::string_view::npos) {
            error = "incomplete journal record";
            return false;
        }
        JournalDocument record;
        if (!ParseJournalRecord(bytes.substr(0, end), record, error))
            return false;
        bytes.remove_prefix(end + 1);
        if (first) {
            out = std::move(record);
            if (!out.entries.empty()) {
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
            record.entries.size() != 1) {
            error = "journal record identity changed";
            return false;
        }
        auto& entry = record.entries.front();
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
                          const std::array<std::uint64_t, 3>& undo_directory_identity) {
    nlohmann::json root = nlohmann::json::object();
    root["schema"] = kJournalSchema;
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
        if (current == entry.previous_identity && !previous_bytes) {
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
                                                                 document.undo_directory_identity),
                                                   control,
                                                   errors);
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
                save_dir / entry->path, *entry, current, missing, previous_bytes, errors))
            return false;
    }
    for (auto* entry : order) {
        const auto destination = save_dir / entry->path;
        ArtifactIdentity current{};
        bool missing = false;
        bool previous_bytes = false;
        if (!OwnedOccupant(destination, *entry, current, missing, previous_bytes, errors))
            return false;
        const auto retired_stage = JournalStage(
            save_dir, document.undo_directory_identity, "rollback-retired", destination);
        const auto restored_stage =
            JournalStage(save_dir, document.undo_directory_identity, "restored", destination);
        if (!missing && !previous_bytes) {
            entry->rollback_retired_identity = current;
            if (!AppendEntry(save_dir, document, *entry, control, errors))
                return false;
        }
        if (entry->action == "create") {
            if (!missing && !WorldSaveService::remove_owned_entry(
                                destination, current, errors, false, &retired_stage))
                return false;
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
                    if (!WorldSaveService::prepare_restoration(journal_directory(save_dir) /
                                                                   destination.filename(),
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
        }
        report_snapshot_step("rollback-entry", destination);
        if (!WorldSaveService::durable_flush_directory(
                NearestExistingDirectory(destination.parent_path()), errors))
            return false;
    }
    return true;
}

// Inventory and bind every artifact before retirement. A substituted directory/file or unknown
// leaf refuses cleanup; recursive deletion cannot prove ownership.
bool FinishJournal(const fs::path& save_dir,
                   const JournalDocument& document,
                   const ArtifactIdentity& control,
                   std::vector<std::string>* errors) {
    const auto journal = journal_directory(save_dir);
    if (!MatchesIdentity(journal, document.journal_identity, true) ||
        !MatchesIdentity(journal / "undo", document.undo_directory_identity, true)) {
        AddError(errors, "snapshot journal directory ownership changed");
        return false;
    }
    std::map<fs::path, ArtifactIdentity> files;
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
    std::size_t inspected = 0;
    for (const auto& directory : {journal, journal / "undo"}) {
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
        if (!WorldSaveService::artifact_identity(path, actual, missing) ||
            (!missing && actual != identity)) {
            AddError(errors, "snapshot cleanup artifact ownership changed");
            return false;
        }
    }
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
    return ec != std::errc::no_such_file_or_directory && (ec || fs::exists(status));
}

RecoveryOutcome recover_pending_journal(const fs::path& save_dir,
                                        std::vector<std::string>* errors) {
    const WorldSaveService::SaveOwnership ownership;
    const detail::SnapshotWriteOwnership snapshot;
    try {
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
        hook(step, path);
    }
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
        } catch (...) {}
    }
    if (m_registered) {
        ActiveTransaction() = nullptr;
    }
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
    try {
        auto ownership = std::make_unique<TransactionOwnership>();
        if (recover_pending_journal(m_save_dir, errors) == RecoveryOutcome::Failed) {
            return false;
        }
        if (!read_snapshot_index(m_save_dir, m_previous, errors)) {
            return false;
        }
        if (m_previous.generation == std::numeric_limits<std::uint64_t>::max() ||
            m_previous.txn_id == std::numeric_limits<std::uint64_t>::max()) {
            AddError(errors, "snapshot generation identity exhausted");
            return false;
        }
        m_new_generation = m_previous.generation + 1;
        m_txn_id = m_previous.txn_id + 1;
        // From here on, even a directory-create/flush failure needs journal cleanup.
        m_ownership = std::move(ownership);
        m_state = State::Open;
        std::error_code ec;
        fs::create_directories(journal_directory(m_save_dir) / "undo", ec);
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
        report_snapshot_step("journal-published", journal_directory(m_save_dir) / "txn.json");
        return true;
    } catch (const std::exception& ex) {
        AddError(errors, std::string("cannot begin snapshot transaction: ") + ex.what());
        abort(errors);
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
    bool missing = false;
    if (!WorldSaveService::artifact_identity(destination, entry->installed_identity, missing))
        return false;
    entry->installed_retention = entry->installed_identity == entry->incoming_identity
                                     ? entry->incoming_retention
                                     : entry->previous_retention;
    entry->incoming_identity = {};
    entry->incoming_temporary.clear();
    if (!incoming.empty() &&
        !RelativePublicationPath(transaction->m_save_dir, incoming, entry->incoming_temporary)) {
        AddError(errors, "snapshot incoming path escapes transaction root");
        return false;
    }
    entry->incoming_retention.reset();
    if (!incoming.empty()) {
        const SavedInputFile input(incoming);
        if (!input.identity(entry->incoming_identity) ||
            !(entry->incoming_retention = input.retain_identity()))
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
            entry.path = rel;
            std::string previous;
            bool missing = false;
            {
                const SavedInputFile input(destination);
                missing = input.missing();
                if (!missing && (!input.identity(entry.previous_identity) ||
                                 !(entry.previous_retention = input.retain_identity()) ||
                                 !input.read(previous, kMaxUndoBytes))) {
                    AddError(errors, "unreadable or oversized transaction member: " + rel);
                    return false;
                }
            } // An undo filename can share this reader's publication stripe.
            if (!missing) {
                const std::string undo_name = std::to_string(m_next_undo);
                fs::path temporary;
                if (!WorldSaveService::prepare_restoration(undo_directory / undo_name,
                                                           previous,
                                                           temporary,
                                                           entry.undo_identity,
                                                           errors))
                    return false;
                ++m_next_undo;
                entry.action = "replace";
                entry.prev_bytes = previous.size();
                entry.prev_fnv64 = fnv1a64_bytes(previous.data(), previous.size());
                entry.undo = "undo/" + undo_name;
                entry.undo_temporary =
                    Filesystem::GenericUtf8PathString(temporary.lexically_relative(m_save_dir));
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
                !WorldSaveService::publish_restoration(m_save_dir / protected_entry.undo_temporary,
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
        const auto header = EncodeJournal(m_txn_id,
                                          m_previous.generation,
                                          m_new_generation,
                                          {},
                                          m_journal_identity,
                                          m_undo_directory_identity);
        if (!WorldSaveService::append_journal_record(
                journal_directory(m_save_dir) / "txn.json", header, m_control_identity, errors))
            return false;
    }
    for (const auto index : m_changed_entries) {
        const auto& entry = m_entries[index];
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
        if (!WorldSaveService::append_journal_record(
                journal_directory(m_save_dir) / "txn.json", record, m_control_identity, errors))
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

        if (m_previous.generation == 0) {
            // First indexing of this directory: enumerate the baseline members.
            std::vector<std::string> baseline = {
                "world_info.json",
                "aether_state.efs",
                "chunks/region/world-manifest.json",
                "chunks/region/plant-entities.json",
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
            if (!digest_member(entry.path, digest)) {
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
                      index_path, encoded, errors, journal_directory(m_save_dir) / "snapshot.json");
        if (!published) {
            abort(errors);
            return false;
        }
        m_state = State::Committed;
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
    if (m_state != State::Open && m_state != State::Committing) {
        return;
    }
    try {
        report_snapshot_step("before-rollback", m_save_dir);
        m_state = State::RollingBack;
        JournalDocument document;
        document.txn_id = m_txn_id;
        document.base_generation = m_previous.generation;
        document.new_generation = m_new_generation;
        document.journal_identity = m_journal_identity;
        document.undo_directory_identity = m_undo_directory_identity;
        for (const auto& entry : m_entries) {
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
            document.entries.push_back(std::move(copy));
        }
        if (m_artifacts && m_previous.generation == 0) {
            // Creation has no previous commit to restore. Its created-artifacts registry
            // retains identities and failed-rollback diagnostics until cleanup succeeds, so
            // the journal may retire first even if artifact rollback subsequently refuses.
            // Replacements must retain the recovery journal whenever rollback fails.
            if (FinishJournal(m_save_dir, document, m_control_identity, errors)) {
                const auto error = m_artifacts->rollback();
                if (!error.empty())
                    AddError(errors, error);
            }
        } else if (RollbackEntries(m_save_dir, document, m_control_identity, errors)) {
            FinishJournal(m_save_dir, document, m_control_identity, errors);
        }
    } catch (const std::exception& ex) {
        AddError(errors, std::string("snapshot rollback failed: ") + ex.what());
    }
    m_state = State::RolledBack;
    m_committed_actions.clear();
    m_ownership.reset();
}

} // namespace Luminumbra::Persistence
