#include "SaveTransaction.h"

#include "WorldSaveService.h"

#include "nlohmann/json.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace Luminumbra::Persistence {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::size_t kHashBlockBytes = 1024u * 1024u;

thread_local SaveTransaction* g_active = nullptr;

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

bool ReadAllBytes(const fs::path& file, std::string& out, std::vector<std::string>* errors) {
    out.clear();
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        AddError(errors, "cannot open for reading: " + file.string());
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        AddError(errors, "cannot read: " + file.string());
        return false;
    }
    return true;
}

bool SamePath(const fs::path& lhs, const fs::path& rhs) {
    std::error_code ec;
    fs::path lhs_canonical = fs::weakly_canonical(lhs, ec);
    if (ec) {
        lhs_canonical = lhs.lexically_normal();
    }
    fs::path rhs_canonical = fs::weakly_canonical(rhs, ec);
    if (ec) {
        rhs_canonical = rhs.lexically_normal();
    }
    return lhs_canonical == rhs_canonical;
}

// Removes stale staging files (names containing ".tmp.") directly in save_dir and chunks/region.
// Returns false when a listed temporary could not be removed. Affected directories are flushed.
bool SweepTemporaries(const fs::path& save_dir) {
    bool ok = true;
    const std::array<fs::path, 2> directories = {save_dir, save_dir / "chunks" / "region"};
    for (const fs::path& directory : directories) {
        std::error_code ec;
        fs::directory_iterator it(directory, ec);
        if (ec) {
            continue;
        }
        bool removed_any = false;
        const fs::directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) {
                ok = false;
                break;
            }
            std::error_code status_ec;
            const fs::directory_entry& entry = *it;
            if (!entry.is_regular_file(status_ec) || status_ec) {
                continue;
            }
            if (entry.path().filename().string().find(".tmp.") == std::string::npos) {
                continue;
            }
            std::error_code remove_ec;
            fs::remove(entry.path(), remove_ec);
            if (remove_ec) {
                ok = false;
            } else {
                removed_any = true;
            }
        }
        if (removed_any) {
            WorldSaveService::durable_flush_directory(directory, nullptr);
        }
    }
    return ok;
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
};

struct JournalDocument {
    std::uint64_t txn_id = 0;
    std::uint64_t base_generation = 0;
    std::uint64_t new_generation = 0;
    std::vector<JournalEntry> entries;
};

bool ParseJournal(std::string_view bytes, JournalDocument& out, std::string& error) {
    out = JournalDocument{};
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
        !next->is_number_unsigned() || !entries || !entries->is_array()) {
        error = "journal header fields invalid";
        return false;
    }
    JournalDocument parsed;
    parsed.txn_id = txn_id->get<std::uint64_t>();
    parsed.base_generation = base->get<std::uint64_t>();
    parsed.new_generation = next->get<std::uint64_t>();
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
        if (!IsUndoName(entry.undo)) {
            error = "journal undo name invalid";
            return false;
        }
        parsed.entries.push_back(std::move(entry));
    }
    out = std::move(parsed);
    return true;
}

std::string EncodeJournal(std::uint64_t txn_id,
                          std::uint64_t base_generation,
                          std::uint64_t new_generation,
                          const std::vector<JournalEntry>& entries) {
    nlohmann::json root = nlohmann::json::object();
    root["schema"] = kJournalSchema;
    root["txn_id"] = txn_id;
    root["base_generation"] = base_generation;
    root["new_generation"] = new_generation;
    nlohmann::json list = nlohmann::json::array();
    for (const JournalEntry& entry : entries) {
        nlohmann::json item = nlohmann::json::object();
        item["path"] = entry.path;
        item["action"] = entry.action;
        item["prev_bytes"] = entry.prev_bytes;
        item["prev_fnv64"] = HexFnv64(entry.prev_fnv64);
        item["undo"] = entry.undo;
        list.push_back(std::move(item));
    }
    root["entries"] = std::move(list);
    return root.dump() + "\n";
}

// Restores entries in reverse order. Shared by in-process abort and crash recovery.
bool RollbackEntries(const fs::path& save_dir,
                     const std::vector<JournalEntry>& entries,
                     std::vector<std::string>* errors) {
    // The index goes first: a half-visible new index must never survive a partially completed
    // rollback, or recovery would mistake the transaction for a committed one and drop the undo
    // copies. Remaining entries follow in reverse order.
    std::vector<const JournalEntry*> order;
    order.reserve(entries.size());
    for (const JournalEntry& entry : entries) {
        if (entry.path == kSnapshotIndexRelativePath) {
            order.push_back(&entry);
        }
    }
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->path != kSnapshotIndexRelativePath) {
            order.push_back(&*it);
        }
    }
    for (const JournalEntry* entry_ptr : order) {
        const JournalEntry& entry = *entry_ptr;
        const fs::path destination = save_dir / entry.path;
        if (entry.action == "replace") {
            std::uint64_t bytes = 0;
            std::uint64_t fnv64 = 0;
            bool exists = false;
            if (!hash_file(destination, bytes, fnv64, exists, errors)) {
                return false;
            }
            const bool matches = exists && bytes == entry.prev_bytes && fnv64 == entry.prev_fnv64;
            if (!matches) {
                const fs::path undo_path = journal_directory(save_dir) / entry.undo;
                std::uint64_t undo_bytes = 0;
                std::uint64_t undo_fnv64 = 0;
                bool undo_exists = false;
                if (!hash_file(undo_path, undo_bytes, undo_fnv64, undo_exists, errors)) {
                    return false;
                }
                if (!undo_exists || undo_bytes != entry.prev_bytes ||
                    undo_fnv64 != entry.prev_fnv64) {
                    AddError(errors, "journal undo copy missing or corrupt: " + entry.path);
                    return false;
                }
                std::string restored;
                if (!ReadAllBytes(undo_path, restored, errors)) {
                    return false;
                }
                if (!WorldSaveService::durable_write_file(destination, restored, errors)) {
                    return false;
                }
            }
        } else if (entry.action == "create") {
            if (!WorldSaveService::durable_remove(destination, errors)) {
                return false;
            }
        } else {
            AddError(errors, "journal entry action invalid: " + entry.path);
            return false;
        }
        report_snapshot_step("rollback-entry", destination);
        if (entry.path == kSnapshotIndexRelativePath) {
            // The restored (or already restored/absent) index must be durable before any other
            // member is touched, or a power loss could resurrect the new generation.
            if (!WorldSaveService::durable_flush_directory(
                    NearestExistingDirectory(destination.parent_path()), errors)) {
                return false;
            }
        }
    }
    // Flush every affected directory (including entries a replay found already restored) before
    // the caller retires the journal.
    std::vector<fs::path> flushed;
    for (const JournalEntry& entry : entries) {
        const fs::path directory = NearestExistingDirectory((save_dir / entry.path).parent_path());
        bool seen = false;
        for (const fs::path& done : flushed) {
            seen = seen || done == directory;
        }
        if (seen) {
            continue;
        }
        flushed.push_back(directory);
        if (!WorldSaveService::durable_flush_directory(directory, errors)) {
            return false;
        }
    }
    return true;
}

// Deletes txn.json, the journal directory and temporaries. Errors are ignored by design.
void FinishJournal(const fs::path& save_dir) {
    const fs::path txn_path = journal_directory(save_dir) / "txn.json";
    WorldSaveService::durable_remove(txn_path, nullptr);
    report_snapshot_step("journal-unlinked", txn_path);
    std::error_code ec;
    fs::remove_all(journal_directory(save_dir), ec);
    SweepTemporaries(save_dir);
    WorldSaveService::durable_flush_directory(save_dir, nullptr);
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
    return root.dump() + "\n";
}

bool decode_snapshot_index(std::string_view bytes, SnapshotIndex& out, std::string& error) {
    out = SnapshotIndex{};
    if (bytes.size() > kMaxSnapshotIndexBytes) {
        error = "snapshot index exceeds size cap";
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
        const nlohmann::json* bytes = Field(item, "bytes");
        const nlohmann::json* fnv = Field(item, "fnv64");
        const nlohmann::json* member_generation = Field(item, "generation");
        if (!path || !path->is_string() || !cls || !cls->is_string() || !state ||
            !state->is_string() || !bytes || !bytes->is_number_unsigned() || !fnv ||
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
        member.bytes = bytes->get<std::uint64_t>();
        member.generation = member_generation->get<std::uint64_t>();
        index.members.push_back(std::move(member));
    }
    out = std::move(index);
    return true;
}

bool read_snapshot_index(const fs::path& save_dir,
                         SnapshotIndex& out,
                         std::vector<std::string>* errors) {
    out = SnapshotIndex{};
    const fs::path path = snapshot_index_path(save_dir);
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec) {
        AddError(errors, "cannot stat snapshot index: " + path.string());
        return false;
    }
    if (!exists) {
        return true;
    }
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        AddError(errors, "cannot stat snapshot index: " + path.string());
        return false;
    }
    if (size > kMaxSnapshotIndexBytes) {
        AddError(errors, "snapshot index too large");
        return false;
    }
    std::string bytes;
    if (!ReadAllBytes(path, bytes, errors)) {
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
    std::error_code ec;
    const bool exists = fs::exists(snapshot_index_path(save_dir), ec);
    return !ec && exists;
}

std::uint64_t fnv1a64_bytes(const void* data, std::size_t size) {
    return Fnv1aUpdate(kFnvOffset, data, size);
}

bool hash_file(const fs::path& path,
               std::uint64_t& bytes,
               std::uint64_t& fnv64,
               bool& exists,
               std::vector<std::string>* errors) {
    bytes = 0;
    fnv64 = 0;
    exists = false;
    std::error_code ec;
    const bool present = fs::exists(path, ec);
    if (ec) {
        AddError(errors, "cannot stat file: " + path.string());
        return false;
    }
    if (!present) {
        return true;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        AddError(errors, "cannot open for hashing: " + path.string());
        return false;
    }
    exists = true;
    fnv64 = kFnvOffset;
    std::vector<char> block(kHashBlockBytes);
    while (in) {
        in.read(block.data(), static_cast<std::streamsize>(block.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            fnv64 = Fnv1aUpdate(fnv64, block.data(), static_cast<std::size_t>(got));
            bytes += static_cast<std::uint64_t>(got);
        }
    }
    if (!in.eof()) {
        AddError(errors, "cannot read for hashing: " + path.string());
        return false;
    }
    return true;
}

bool journal_pending(const fs::path& save_dir) {
    std::error_code ec;
    const bool exists = fs::exists(journal_directory(save_dir), ec);
    return !ec && exists;
}

RecoveryOutcome recover_pending_journal(const fs::path& save_dir,
                                        std::vector<std::string>* errors) {
    try {
        const fs::path journal = journal_directory(save_dir);
        std::error_code ec;
        const bool journal_exists = fs::exists(journal, ec);
        if (ec) {
            AddError(errors, "cannot stat snapshot journal: " + journal.string());
            return RecoveryOutcome::Failed;
        }
        if (!journal_exists) {
            // Far-only upserts run without a transaction; a crash can leave their staging file
            // behind, which an indexed directory would refuse at load. Legacy directories keep
            // their historical behaviour.
            if (snapshot_index_exists(save_dir) && !SweepTemporaries(save_dir)) {
                AddError(errors, "cannot remove stale staging files under " + save_dir.string());
                return RecoveryOutcome::Failed;
            }
            return RecoveryOutcome::NothingToDo;
        }
        const fs::path txn_path = journal / "txn.json";
        const bool txn_exists = fs::exists(txn_path, ec);
        if (ec) {
            AddError(errors, "cannot stat snapshot journal: " + txn_path.string());
            return RecoveryOutcome::Failed;
        }
        if (!txn_exists) {
            // Debris from a journal that was never published.
            fs::remove_all(journal, ec);
            if (ec) {
                AddError(errors, "cannot remove stale snapshot journal: " + journal.string());
                return RecoveryOutcome::Failed;
            }
            SweepTemporaries(save_dir);
            return RecoveryOutcome::DiscardedStale;
        }
        std::string bytes;
        if (!ReadAllBytes(txn_path, bytes, errors)) {
            return RecoveryOutcome::Failed;
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
        if (committed.generation != 0 && committed.generation == document.new_generation) {
            // The commit point was reached; only the cleanup is left.
            FinishJournal(save_dir);
            return RecoveryOutcome::DiscardedStale;
        }
        if (!RollbackEntries(save_dir, document.entries, errors)) {
            return RecoveryOutcome::Failed;
        }
        FinishJournal(save_dir);
        return RecoveryOutcome::RolledBack;
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

SaveTransaction::SaveTransaction(fs::path save_dir,
                                 std::uint64_t tick,
                                 std::vector<std::array<float, 3>> anchors,
                                 MemberDigestFn digest_fn)
    : m_save_dir(std::move(save_dir))
    , m_tick(tick)
    , m_anchors(std::move(anchors))
    , m_digest_fn(digest_fn) {
    if (g_active == nullptr) {
        g_active = this;
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
        g_active = nullptr;
    }
}

SaveTransaction* SaveTransaction::active_for(const fs::path& save_dir) {
    SaveTransaction* active = g_active;
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
        if (recover_pending_journal(m_save_dir, errors) == RecoveryOutcome::Failed) {
            return false;
        }
        if (!read_snapshot_index(m_save_dir, m_previous, errors)) {
            return false;
        }
        m_new_generation = m_previous.generation + 1;
        m_txn_id = m_previous.txn_id + 1;
        std::error_code ec;
        fs::create_directories(journal_directory(m_save_dir) / "undo", ec);
        if (ec) {
            AddError(errors, "cannot create snapshot journal: " + m_save_dir.string());
            return false;
        }
        if (!WorldSaveService::durable_flush_directory(journal_directory(m_save_dir), errors)) {
            return false;
        }
        if (!WorldSaveService::durable_flush_directory(m_save_dir, errors)) {
            return false;
        }
        m_state = State::Open;
        return true;
    } catch (const std::exception& ex) {
        AddError(errors, std::string("cannot begin snapshot transaction: ") + ex.what());
        return false;
    }
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
            bool already = false;
            for (const Entry& entry : m_entries) {
                if (entry.path == rel) {
                    already = true;
                    break;
                }
            }
            if (already) {
                continue;
            }
            if (rel != kSnapshotIndexRelativePath && !member_path_allowed(rel)) {
                AddError(errors, "path not allowed in snapshot transaction: " + rel);
                return false;
            }
            const fs::path destination = m_save_dir / rel;
            std::uint64_t bytes = 0;
            std::uint64_t fnv64 = 0;
            bool exists = false;
            if (!hash_file(destination, bytes, fnv64, exists, errors)) {
                return false;
            }
            Entry entry;
            entry.path = rel;
            if (exists) {
                std::string previous;
                if (!ReadAllBytes(destination, previous, errors)) {
                    return false;
                }
                const std::string undo_name = std::to_string(m_next_undo);
                if (!WorldSaveService::durable_write_file(
                        undo_directory / undo_name, previous, errors)) {
                    return false;
                }
                ++m_next_undo;
                entry.action = "replace";
                entry.prev_bytes = previous.size();
                entry.prev_fnv64 = fnv1a64_bytes(previous.data(), previous.size());
                entry.undo = "undo/" + undo_name;
            } else {
                entry.action = "create";
            }
            m_entries.push_back(std::move(entry));
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
    std::vector<JournalEntry> entries;
    entries.reserve(m_entries.size());
    for (const Entry& entry : m_entries) {
        JournalEntry copy;
        copy.path = entry.path;
        copy.action = entry.action;
        copy.prev_bytes = entry.prev_bytes;
        copy.prev_fnv64 = entry.prev_fnv64;
        copy.undo = entry.undo;
        entries.push_back(std::move(copy));
    }
    const std::string bytes =
        EncodeJournal(m_txn_id, m_previous.generation, m_new_generation, entries);
    return WorldSaveService::durable_write_file(
        journal_directory(m_save_dir) / "txn.json", bytes, errors);
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
                "chunks/region/ground-objects.json",
                "chunks/region/field-pages.fpg",
                "chunks/region/active-regions.arl",
            };
            std::error_code ec;
            fs::directory_iterator it(m_save_dir / "chunks" / "region", ec);
            const fs::directory_iterator end;
            if (!ec) {
                for (; it != end; it.increment(ec)) {
                    if (ec) {
                        break;
                    }
                    std::error_code status_ec;
                    if (!it->is_regular_file(status_ec) || status_ec) {
                        continue;
                    }
                    const std::string rel = "chunks/region/" + it->path().filename().string();
                    if (member_path_allowed(rel)) {
                        baseline.push_back(rel);
                    }
                }
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
        fs::create_directories(index_path.parent_path(), directory_error);
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
                const fs::path next = ancestor.parent_path();
                if (next == ancestor)
                    break;
                ancestor = next;
            }
        }
        if (!WorldSaveService::durable_write_file(index_path, encoded, errors)) {
            abort(errors);
            return false;
        }
        m_state = State::Committed;
        report_snapshot_step("commit-point", index_path);
        FinishJournal(m_save_dir);
        return true;
    } catch (const std::exception& ex) {
        AddError(errors, std::string("snapshot commit failed: ") + ex.what());
        abort(errors);
        return false;
    }
}

void SaveTransaction::abort(std::vector<std::string>* errors) {
    if (m_state != State::Open && m_state != State::Committing) {
        return;
    }
    try {
        std::vector<JournalEntry> entries;
        entries.reserve(m_entries.size());
        for (const Entry& entry : m_entries) {
            JournalEntry copy;
            copy.path = entry.path;
            copy.action = entry.action;
            copy.prev_bytes = entry.prev_bytes;
            copy.prev_fnv64 = entry.prev_fnv64;
            copy.undo = entry.undo;
            entries.push_back(std::move(copy));
        }
        // On rollback failure the journal stays in place for recovery.
        if (RollbackEntries(m_save_dir, entries, errors)) {
            FinishJournal(m_save_dir);
        }
    } catch (const std::exception& ex) {
        AddError(errors, std::string("snapshot rollback failed: ") + ex.what());
    }
    m_state = State::RolledBack;
}

} // namespace Luminumbra::Persistence
