#include "SaveTransaction.h"

#include "../ai/CreaturePersistence.h"
#include "../core/BoundedFileReader.h"
#include "../fields/FieldPageState.h"
#include "../world/ActiveRegionLedger.h"
#include "../world/GroundObjectStore.h"
#include "SimulationPendingState.h"
#include "WorldSaveService.h"

#include "nlohmann/json.hpp"
#include <nlohmann/detail/exceptions.hpp>
#include <nlohmann/json_fwd.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <ios>
#include <istream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <stop_token>
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
constexpr std::size_t kHashBlockBytes = 65536; // Fixed 64 KiB hash working buffer.

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
    for (const char c : text) {
        std::uint64_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<std::uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<std::uint64_t>(c - 'a') + 10u;
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<std::uint64_t>(c - 'A') + 10u;
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
    for (const char c : text) {
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

std::size_t& JournalBytesRead() {
    static thread_local std::size_t bytes = 0;
    return bytes;
}

std::size_t MemberByteLimit(std::string_view path) {
    if (path == kSnapshotIndexRelativePath)
        return kMaxSnapshotIndexBytes;
    if (path == "world_info.json")
        return kMaxSaveMetadataBytes;
    if (path == "chunks/region/world-manifest.json")
        return kMaxSaveManifestBytes;
    if (path == "aether_state.efs")
        return kMaxSaveEnergyBytes;
    if (path == SimulationPendingState::kMember)
        return SimulationPendingState::kMaxFileBytes;
    if (path == "chunks/region/active-regions.arl")
        return world::ActiveRegionLedger::kMaxFileBytes;
    if (path == "chunks/region/ground-objects.json")
        return world::GroundObjectStore::kMaxFileBytes;
    if (path == "chunks/region/field-pages.fpg")
        return world::FieldPageState::kMaxFileBytes;
    if (path == "chunks/region/plant-entities.json")
        return kMaxSaveEntityBytes;
    if (path == "chunks/region/creature-entities.json")
        return luminumbra::ai::kMaxStampedCreatureBytes;
    return kMaxSaveMemberBytes;
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

// Validate directly from the bounded stream. No input copy or JSON DOM is retained;
// each entry is checked before it enters the recovery list.
class JournalParser {
public:
    using number_integer_t = nlohmann::json::number_integer_t;
    using number_unsigned_t = nlohmann::json::number_unsigned_t;
    using number_float_t = nlohmann::json::number_float_t;
    using string_t = nlohmann::json::string_t;
    using binary_t = nlohmann::json::binary_t;

    JournalParser(JournalDocument& document, std::string& error)
        : m_document(document)
        , m_error(error) {}

    bool null() {
        return Fail("journal field type invalid");
    }
    bool boolean(bool /*value*/) {
        return Fail("journal field type invalid");
    }
    bool number_integer(number_integer_t /*value*/) {
        return Fail("journal unsigned field invalid");
    }
    bool number_float(number_float_t /*value*/, const string_t& /*text*/) {
        return Fail("journal unsigned field invalid");
    }
    bool binary(binary_t& /*value*/) {
        return Fail("journal field type invalid");
    }

    bool number_unsigned(number_unsigned_t value) {
        if (m_state == State::Root) {
            switch (m_field) {
                case Field::TxnId:
                    m_document.txn_id = value;
                    return true;
                case Field::Base:
                    m_document.base_generation = value;
                    return true;
                case Field::Next:
                    m_document.new_generation = value;
                    return true;
                default:
                    break;
            }
        } else if (m_state == State::Entry && m_field == Field::PrevBytes) {
            m_entry.prev_bytes = value;
            return true;
        }
        return Fail("journal unsigned field invalid");
    }

    bool string(string_t& value) {
        if (value.size() > kMaxJournalStringBytes)
            return Fail("journal string exceeds cap");
        if (m_state == State::Root && m_field == Field::Schema)
            return value == kJournalSchema || Fail("unsupported journal schema");
        if (m_state == State::Entry) {
            switch (m_field) {
                case Field::Path:
                    m_entry.path = value;
                    return true;
                case Field::Action:
                    m_entry.action = value;
                    return true;
                case Field::PrevFnv:
                    return ParseHex64(value, m_entry.prev_fnv64) ||
                           Fail("journal entry digest invalid");
                case Field::Undo:
                    m_entry.undo = value;
                    return true;
                default:
                    break;
            }
        }
        return Fail("journal string field invalid");
    }

    bool start_object(std::size_t /*elements*/) {
        if (m_state == State::Start) {
            m_state = State::Root;
            return true;
        }
        if (m_state != State::Entries)
            return Fail("journal nesting invalid");
        if (m_document.entries.size() >= kMaxJournalEntries)
            return Fail("journal entry count exceeds cap");
        m_entry = JournalEntry{};
        m_seen_entry = 0;
        m_field = Field::None;
        m_state = State::Entry;
        return true;
    }

    bool key(string_t& value) {
        if (value.size() > kMaxJournalStringBytes)
            return Fail("journal string exceeds cap");
        m_field = Field::None;
        if (m_state == State::Root) {
            if (value == "schema")
                m_field = Field::Schema;
            else if (value == "txn_id")
                m_field = Field::TxnId;
            else if (value == "base_generation")
                m_field = Field::Base;
            else if (value == "new_generation")
                m_field = Field::Next;
            else if (value == "entries")
                m_field = Field::Entries;
        } else if (m_state == State::Entry) {
            if (value == "path")
                m_field = Field::Path;
            else if (value == "action")
                m_field = Field::Action;
            else if (value == "prev_bytes")
                m_field = Field::PrevBytes;
            else if (value == "prev_fnv64")
                m_field = Field::PrevFnv;
            else if (value == "undo")
                m_field = Field::Undo;
        }
        if (m_field == Field::None)
            return Fail("journal field unknown");
        auto& seen = m_state == State::Root ? m_seen_root : m_seen_entry;
        const auto bit = Bit(m_field);
        if ((seen & bit) != 0)
            return Fail("journal field duplicated");
        seen |= bit;
        return true;
    }

    bool end_object() {
        if (m_state == State::Root) {
            const auto required = Bit(Field::Schema) | Bit(Field::TxnId) | Bit(Field::Base) |
                                  Bit(Field::Next) | Bit(Field::Entries);
            if (m_seen_root != required)
                return Fail("journal header fields invalid");
            m_state = State::Done;
            return true;
        }
        const auto required = Bit(Field::Path) | Bit(Field::Action) | Bit(Field::PrevBytes) |
                              Bit(Field::PrevFnv) | Bit(Field::Undo);
        if (m_state != State::Entry || m_seen_entry != required)
            return Fail("journal entry fields invalid");
        if (!member_path_allowed(m_entry.path) && m_entry.path != kSnapshotIndexRelativePath)
            return Fail("journal entry path not allowed");
        if (m_entry.action != "replace" && m_entry.action != "create")
            return Fail("journal entry action invalid");
        if (m_entry.prev_bytes > MemberByteLimit(m_entry.path))
            return Fail("journal undo size exceeds cap");
        if (!IsUndoName(m_entry.undo))
            return Fail("journal undo name invalid");
        m_document.entries.push_back(std::move(m_entry));
        m_state = State::Entries;
        return true;
    }

    bool start_array(std::size_t /*elements*/) {
        if (m_state != State::Root || m_field != Field::Entries)
            return Fail("journal nesting invalid");
        m_state = State::Entries;
        return true;
    }
    bool end_array() {
        if (m_state != State::Entries)
            return Fail("journal nesting invalid");
        m_state = State::Root;
        m_field = Field::None;
        return true;
    }
    bool parse_error(std::size_t /*position*/,
                     const std::string& /*token*/,
                     const nlohmann::detail::exception& /*exception*/) {
        return Fail("malformed journal JSON");
    }

private:
    enum class State : std::uint8_t {
        Start,
        Root,
        Entries,
        Entry,
        Done
    };
    enum class Field : std::uint8_t {
        None,
        Schema,
        TxnId,
        Base,
        Next,
        Entries,
        Path,
        Action,
        PrevBytes,
        PrevFnv,
        Undo
    };
    static constexpr std::uint32_t Bit(Field field) {
        return std::uint32_t{1} << static_cast<unsigned>(field);
    }
    bool Fail(const char* message) {
        m_error = message;
        return false;
    }
    JournalDocument& m_document;
    std::string& m_error;
    State m_state = State::Start;
    Field m_field = Field::None;
    std::uint32_t m_seen_root = 0;
    std::uint32_t m_seen_entry = 0;
    JournalEntry m_entry;
};

bool ParseJournal(std::istream& input, JournalDocument& out, std::string& error) {
    out = JournalDocument{};
    JournalParser parser(out, error);
    return nlohmann::json::sax_parse(input, &parser);
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
            if (!hash_file(
                    destination, bytes, fnv64, exists, errors, MemberByteLimit(entry.path))) {
                return false;
            }
            const bool matches = exists && bytes == entry.prev_bytes && fnv64 == entry.prev_fnv64;
            if (!matches) {
                const fs::path undo_path = journal_directory(save_dir) / entry.undo;
                std::uint64_t undo_bytes = 0;
                std::uint64_t undo_fnv64 = 0;
                bool undo_exists = false;
                if (!hash_file(undo_path,
                               undo_bytes,
                               undo_fnv64,
                               undo_exists,
                               errors,
                               MemberByteLimit(entry.path))) {
                    return false;
                }
                if (!undo_exists || undo_bytes != entry.prev_bytes ||
                    undo_fnv64 != entry.prev_fnv64) {
                    AddError(errors, "journal undo copy missing or corrupt: " + entry.path);
                    return false;
                }
                std::string restored;
                if (!read_bounded_save_file(
                        undo_path, restored, MemberByteLimit(entry.path), errors)) {
                    return false;
                }
                // Recheck the bytes actually read: the undo handle may differ from the hashed file.
                if (restored.size() != entry.prev_bytes ||
                    fnv1a64_bytes(restored.data(), restored.size()) != entry.prev_fnv64) {
                    AddError(errors, "journal undo copy changed: " + entry.path);
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

bool read_bounded_save_file(const fs::path& path,
                            std::string& bytes,
                            std::size_t limit,
                            std::vector<std::string>* errors,
                            const std::stop_token& stop) {
    bytes.clear();
    try {
        return Filesystem::ReadBoundedFile(path, bytes, limit, stop);
    } catch (const std::exception& error) {
        bytes.clear();
        AddError(errors,
                 std::string("cannot read save file ") + path.string() + ": " + error.what());
        return false;
    }
}

std::size_t recovery_journal_bytes_read() {
    return JournalBytesRead();
}

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
        relative_path == "chunks/region/creature-entities.json" ||
        relative_path == "chunks/region/ground-objects.json" ||
        relative_path == "chunks/region/field-pages.fpg" ||
        relative_path == "chunks/region/simulation-pending.spn" ||
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
    std::string bytes;
    if (!read_bounded_save_file(path, bytes, kMaxSnapshotIndexBytes, errors)) {
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

std::size_t save_member_byte_limit(std::string_view path) {
    return MemberByteLimit(path);
}

bool hash_file(const fs::path& path,
               std::uint64_t& bytes,
               std::uint64_t& fnv64,
               bool& exists,
               std::vector<std::string>* errors,
               std::size_t limit) {
    bytes = 0;
    fnv64 = 0;
    exists = false;
    try {
        std::error_code ec;
        const auto status = fs::symlink_status(path, ec);
        if (status.type() == fs::file_type::not_found &&
            (!ec || ec == std::errc::no_such_file_or_directory))
            return true;
        if (ec)
            throw std::runtime_error("cannot inspect file for hashing: " + path.string());
        exists = true;
        Filesystem::BoundedFileInput input(path, limit);
        report_snapshot_step("hash-opened", path);
        auto& in = input.stream();
        std::array<char, kHashBlockBytes> block{};
        fnv64 = kFnvOffset;
        while (bytes < input.size()) {
            const auto count =
                std::min(block.size(), input.size() - static_cast<std::size_t>(bytes));
            in.read(block.data(), static_cast<std::streamsize>(count));
            if (!in)
                throw std::runtime_error("cannot read for hashing: " + path.string());
            fnv64 = Fnv1aUpdate(fnv64, block.data(), count);
            bytes += count;
        }
        if (in.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("file grew while hashing: " + path.string());
        return true;
    } catch (const std::exception& ex) {
        AddError(errors, ex.what());
        return false;
    }
}

bool journal_pending(const fs::path& save_dir) {
    std::error_code ec;
    const bool exists = fs::exists(journal_directory(save_dir), ec);
    return !ec && exists;
}

RecoveryOutcome recover_pending_journal(const fs::path& save_dir,
                                        std::vector<std::string>* errors) {
    JournalBytesRead() = 0;
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
        JournalDocument document;
        std::string error;
        {
            Filesystem::BoundedFileInput buffer(txn_path, kMaxJournalBytes);
            auto& input = buffer.stream();
            bool parsed = false;
            try {
                parsed = ParseJournal(input, document, error);
            } catch (...) {
                JournalBytesRead() = buffer.bytes_read();
                throw;
            }
            JournalBytesRead() = buffer.bytes_read();
            if (!parsed) {
                AddError(errors, "corrupt snapshot journal: " + error);
                return RecoveryOutcome::Failed;
            }
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

SaveTransaction::SaveTransaction(std::filesystem::path save_dir,
                                 std::uint64_t tick,
                                 std::vector<std::array<float, 3>> anchors,
                                 MemberDigestFn digest_fn)
    : m_save_dir(std::move(save_dir))
    , m_tick(tick)
    , m_anchors(std::move(anchors))
    , m_digest_fn(digest_fn) {
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
            std::fputs("Snapshot rollback failed; journal retained for recovery.\n", stderr);
        }
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
        std::vector<std::string> queued_paths;
        std::set<std::string> pending;
        auto append = [this, &queued_paths, &pending, errors](const std::string& rel) {
            if (m_protected_paths.contains(rel) || !pending.insert(rel).second)
                return true;
            // Bound the complete batch before creating undo files or publishing a journal.
            if (queued_paths.size() >= kMaxJournalEntries - m_entries.size()) {
                AddError(errors, "journal entry count exceeds cap");
                return false;
            }
            queued_paths.push_back(rel);
            return true;
        };
        if (m_entries.empty() && !append(kSnapshotIndexRelativePath))
            return false;
        for (const auto& rel : relative_paths)
            if (!append(rel))
                return false;

        const fs::path undo_directory = journal_directory(m_save_dir) / "undo";
        bool added = false;
        for (const std::string& rel : queued_paths) {
            if (m_protected_paths.contains(rel)) {
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
            if (!hash_file(destination, bytes, fnv64, exists, errors, MemberByteLimit(rel))) {
                return false;
            }
            Entry entry;
            entry.path = rel;
            if (exists) {
                std::string previous;
                if (!read_bounded_save_file(destination, previous, MemberByteLimit(rel), errors)) {
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
            m_protected_paths.insert(rel);
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
    if (bytes.size() > kMaxJournalBytes) {
        AddError(errors, "journal exceeds size cap");
        return false;
    }
    return WorldSaveService::durable_write_file(
        journal_directory(m_save_dir) / "txn.json", bytes, errors);
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

        std::optional<MemberDigest> retained_pending;
        auto digest_member = [this, errors, &retained_pending](const std::string& rel,
                                                               MemberDigest& digest) {
            if (rel == "chunks/region/simulation-pending.spn" && retained_pending) {
                digest = *retained_pending;
                return true;
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

        if (m_digest_fn == &WorldSaveService::member_digest) {
            const std::string pending_path = "chunks/region/simulation-pending.spn";
            const auto pending_entry = std::find_if(
                m_entries.begin(), m_entries.end(), [&pending_path](const Entry& entry) {
                    return entry.path == pending_path;
                });
            const auto ledger_entry =
                std::find_if(m_entries.begin(), m_entries.end(), [](const Entry& entry) {
                    return entry.path == "chunks/region/active-regions.arl";
                });
            // Installing/replacing/removing a ledger makes retained pending state a
            // dependency. A no-op removal of an absent ledger leaves legacy state ignored.
            const bool ledger_dependency =
                ledger_entry != m_entries.end() &&
                (ledger_entry->action == "replace" ||
                 fs::exists(WorldSaveService::active_regions_path(m_save_dir)));
            if (pending_entry != m_entries.end() || ledger_dependency) {
                if (auto* facts = WorldSaveService::SaveFactsScope::active_for(m_save_dir))
                    facts->m_pending_enabled = true;
                MemberDigest previous_pending;
                if (pending_entry != m_entries.end()) {
                    // The journal fingerprints the original bytes before any writer runs.
                    // Comparing final replacement bytes with the old index would be wrong.
                    previous_pending = {.present = pending_entry->action == "replace",
                                        .bytes = pending_entry->prev_bytes,
                                        .fnv64 = pending_entry->prev_fnv64};
                } else {
                    if (!digest_member(pending_path, previous_pending)) {
                        abort(errors);
                        return false;
                    }
                    retained_pending = previous_pending;
                }
                const auto indexed = members.find(pending_path);
                if (m_previous.generation != 0 &&
                    ((indexed == members.end() && previous_pending.present) ||
                     (indexed != members.end() &&
                      (indexed->second.present != previous_pending.present ||
                       (previous_pending.present &&
                        (indexed->second.bytes != previous_pending.bytes ||
                         indexed->second.fnv64 != previous_pending.fnv64)))))) {
                    AddError(errors, "inconsistent snapshot: " + pending_path);
                    abort(errors);
                    return false;
                }
            }
        }

        if (m_previous.generation == 0) {
            // First indexing of this directory: enumerate the baseline members.
            std::vector<std::string> baseline = {
                "world_info.json",
                "aether_state.efs",
                "chunks/region/world-manifest.json",
                "chunks/region/plant-entities.json",
                "chunks/region/creature-entities.json",
                "chunks/region/ground-objects.json",
                "chunks/region/field-pages.fpg",
                "chunks/region/active-regions.arl",
                "chunks/region/simulation-pending.spn",
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
            std::sort(baseline.begin(), baseline.end());
            baseline.erase(std::unique(baseline.begin(), baseline.end()), baseline.end());
            for (const std::string& rel : baseline) {
                // Protected members are fingerprinted below after all writers finish.
                if (std::any_of(m_entries.begin(), m_entries.end(), [&rel](const Entry& entry) {
                        return entry.path == rel;
                    }))
                    continue;
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

        // C4 keeps all-zero energy cadence in the ledger. Its omitted EFS payload is
        // an intentional absence, including the first indexed save (distant-world.md).
        // Generic transactions with a caller-supplied digest do not interpret world records.
        if (m_digest_fn == &WorldSaveService::member_digest &&
            !members.contains("aether_state.efs")) {
            world::ActiveRegionLedger ledger;
            if (!WorldSaveService::load_active_regions(ledger, m_save_dir, errors)) {
                abort(errors);
                return false;
            }
            if (!ledger.energy_ages().empty()) {
                MemberDigest energy;
                if (!digest_member("aether_state.efs", energy)) {
                    abort(errors);
                    return false;
                }
                members.emplace("aether_state.efs",
                                SnapshotMember{.path = "aether_state.efs",
                                               .cls = MemberClass::Authoritative,
                                               .present = energy.present,
                                               .bytes = energy.bytes,
                                               .fnv64 = energy.fnv64,
                                               .generation = m_new_generation});
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
            if (!digest.present && entry.action != "replace" && !known &&
                entry.path != "chunks/region/creature-entities.json") {
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
                const fs::path parent = ancestor.parent_path();
                if (parent == ancestor)
                    break;
                ancestor = parent;
            }
        }
        if (!WorldSaveService::durable_write_file(index_path, encoded, errors)) {
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
        FinishJournal(m_save_dir);
        return true;
    } catch (const std::exception& ex) {
        AddError(errors, std::string("snapshot commit failed: ") + ex.what());
        if (m_state == State::Committed)
            return true; // cleanup faults cannot undo a published durable index
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
    m_committed_actions.clear();
}

} // namespace Luminumbra::Persistence
