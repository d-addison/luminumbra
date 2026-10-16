#include "ChunkValidationCache.h"

#include "../core/FilesystemPath.h"
#include "../core/Sha256.h"

#include <exception>
#include <fstream>
#include <random>
#include <string>
#include <system_error>

namespace Luminumbra::Persistence {
namespace {
std::filesystem::path CanonicalCachePath(const std::filesystem::path& path,
                                         std::error_code& error) {
    const auto absolute = Filesystem::AbsolutePath(path, error);
    return error ? std::filesystem::path{} : std::filesystem::weakly_canonical(absolute, error);
}
} // namespace

class ChunkValidationCache::SpillStore {
public:
    explicit SpillStore(std::size_t limit)
        : m_limit(limit) {}
    ~SpillStore() {
        reset(0);
    }
    void reset(std::size_t limit) {
        m_file.close();
        m_file.clear();
        std::error_code error;
        if (!directory.empty())
            std::filesystem::remove_all(directory, error);
        directory.clear();
        m_free.clear();
        extent = m_end = 0;
        m_limit = limit;
        fault = SpillFault::None;
    }
    bool available(std::size_t size) const {
        return size <= m_limit - m_end ||
               std::any_of(m_free.begin(), m_free.end(), [size](const auto& hole) {
                   return hole.second >= size;
               });
    }
    bool open() {
        if (m_file.is_open())
            return true;
        if (fault == SpillFault::Create || m_limit == 0)
            return false;
        std::error_code error;
        const auto temporary = std::filesystem::temp_directory_path(error);
        if (error)
            return false;
        const auto root = CanonicalCachePath(temporary, error);
        if (error)
            return false;
        std::random_device random;
        for (int attempt = 0; attempt < 8; ++attempt) {
            const auto candidate = root / ("luminumbra-validation-" + std::to_string(random()) +
                                           "-" + std::to_string(random()));
            if (!std::filesystem::create_directory(candidate, error))
                continue;
            directory = candidate;
            std::filesystem::permissions(directory,
                                         std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace,
                                         error);
            if (error) {
                std::filesystem::remove_all(directory, error);
                directory.clear();
                return false;
            }
            m_file.open(directory / "payloads",
                        std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
            if (m_file.is_open())
                return true;
            std::filesystem::remove_all(directory, error);
            directory.clear();
            return false;
        }
        return false;
    }
    std::optional<std::size_t> write(std::string_view bytes) try {
        if (!available(bytes.size()) || !open() || fault == SpillFault::Write)
            return std::nullopt;
        const auto offset = allocate(bytes.size());
        m_file.clear();
        m_file.seekp(static_cast<std::streamoff>(offset));
        m_file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        m_file.flush();
        if (!m_file) {
            release(offset, bytes.size());
            return std::nullopt;
        }
        return offset;
    } catch (const std::exception&) {
        return std::nullopt;
    }
    // nullopt denotes I/O failure; false denotes an exact-byte mismatch.
    std::optional<bool> equal(std::size_t offset, std::string_view bytes) {
        if (fault == SpillFault::Read || !m_file.is_open())
            return std::nullopt;
        m_file.clear();
        m_file.seekg(static_cast<std::streamoff>(offset));
        std::array<char, 65536> scratch{};
        while (!bytes.empty()) {
            const auto count = std::min(bytes.size(), scratch.size());
            m_file.read(scratch.data(), static_cast<std::streamsize>(count));
            if (!m_file || m_file.gcount() != static_cast<std::streamsize>(count) ||
                fault == SpillFault::ShortRead)
                return std::nullopt;
            if (std::string_view(scratch.data(), count) != bytes.substr(0, count))
                return false;
            bytes.remove_prefix(count);
        }
        return true;
    }
    void release(std::size_t offset, std::size_t size) noexcept try {
        auto next = m_free.lower_bound(offset);
        if (next != m_free.begin()) {
            const auto previous = std::prev(next);
            if (previous->first + previous->second == offset) {
                offset = previous->first;
                size += previous->second;
                m_free.erase(previous);
            }
        }
        if (next != m_free.end() && offset + size == next->first) {
            size += next->second;
            m_free.erase(next);
        }
        // Reclaim the tail address range too, so a larger replacement can use
        // freed tail bytes together with the remaining capacity. The bounded
        // file is overwritten in place rather than growing indefinitely.
        if (offset + size == m_end)
            m_end = offset;
        else
            m_free.emplace(offset, size);
    } catch (const std::exception&) {
        // Losing a free extent only reduces optional cache capacity.
    }
    std::filesystem::path directory;
    std::size_t extent = 0;
    SpillFault fault = SpillFault::None;

private:
    std::size_t allocate(std::size_t size) {
        for (auto hole = m_free.begin(); hole != m_free.end(); ++hole) {
            if (hole->second < size)
                continue;
            const auto offset = hole->first, remaining = hole->second - size;
            m_free.erase(hole);
            if (remaining != 0)
                m_free.emplace(offset + size, remaining);
            return offset;
        }
        const auto offset = m_end;
        m_end += size;
        extent = std::max(extent, m_end);
        return offset;
    }
    std::size_t m_limit, m_end = 0;
    std::map<std::size_t, std::size_t> m_free;
    std::fstream m_file;
};

ChunkValidationCache::Payload::~Payload() {
    if (store)
        store->release(offset, length);
}

ChunkValidationCache::ChunkValidationCache(std::size_t entries,
                                           std::size_t bytes,
                                           std::size_t spill_bytes)
    : m_entry_limit(std::min(entries, kMaxEntries))
    , m_byte_limit(std::min(bytes, kMaxPayloadBytes))
    , m_spill_limit(std::min(spill_bytes, kMaxSpillBytes))
    , m_spill(std::make_shared<SpillStore>(m_spill_limit)) {}
ChunkValidationCache::~ChunkValidationCache() = default;

bool ChunkValidationCache::equal(const Payload& payload, std::string_view bytes, bool strict) {
    if (payload.length != bytes.size())
        return false;
    if (!payload.store)
        return std::string_view(payload.bytes.get(), payload.length) == bytes;
    const auto result = payload.store->equal(payload.offset, bytes);
    if (!result)
        ++m_stats.modes[strict].spill_faults;
    // Spill bytes can change independently of this heap-owned identity. Hash
    // the same fresh buffer that passed exact comparison, never another read.
    return result.value_or(false) && Core::Sha256(bytes) == payload.digest;
}

std::optional<ChunkValidationCache::Facts> ChunkValidationCache::find(const Key& key,
                                                                      std::string_view bytes) {
    const std::lock_guard lock(m_mutex);
    auto& mode = m_stats.modes[key.strict_water];
    if (key.strict_water && m_inventory_active) {
        ++m_stats.first_strict_records;
        m_stats.first_strict_bytes += bytes.size();
    }
    const auto found = m_entries.find(key);
    if (found != m_entries.end() && equal(*found->second.payload, bytes, key.strict_water)) {
        ++m_stats.hits;
        ++mode.hits;
        if (found->second.payload->store)
            ++mode.spill_hits;
        auto& order = m_order[found->second.segment];
        order.splice(order.end(), order, found->second.position);
        return found->second.facts;
    }
    ++m_stats.misses;
    ++mode.misses;
    if (bytes.size() > kMaxRecordBytes ||
        (bytes.size() > m_byte_limit && bytes.size() > m_spill_limit))
        ++mode.oversize;
    else if (found != m_entries.end())
        ++mode.byte_mismatch;
    else if (m_evicted.contains(key))
        ++mode.evicted;
    else
        ++mode.absent;
    if (found != m_entries.end() && found->second.payload->store)
        erase_backing(found);
    return std::nullopt;
}

bool ChunkValidationCache::evict_one(bool strict) {
    const auto segment = !m_order[0].empty() ? 0u : 1u;
    if ((!strict && segment != 0) || m_order[segment].empty())
        return false;
    const auto victim = m_entries.find(m_order[segment].front());
    record_eviction(victim->first);
    ++m_stats.evictions;
    ++m_stats.modes[victim->first.strict_water].evictions;
    erase(victim);
    return true;
}

bool ChunkValidationCache::make_room(bool strict, std::size_t bytes) {
    while (m_entries.size() >= m_entry_limit ||
           (bytes > m_byte_limit - m_stats.payload_bytes && !m_spill->available(bytes))) {
        if (!evict_one(strict))
            return false;
    }
    return true;
}

std::shared_ptr<ChunkValidationCache::Payload>
ChunkValidationCache::retain(const Key& key, std::string_view bytes) {
    auto other = key;
    other.strict_water = !other.strict_water;
    const auto found = m_entries.find(other);
    if (found != m_entries.end()) {
        if (equal(*found->second.payload, bytes, key.strict_water))
            return found->second.payload;
        if (found->second.payload->store)
            erase_backing(found);
    }
    if (!make_room(key.strict_water, bytes.size()))
        return {};
    auto payload = std::make_shared<Payload>();
    payload->length = bytes.size();
    payload->digest = Core::Sha256(bytes);
    payload->charged_mode = key.strict_water;
    if (bytes.size() <= m_byte_limit - m_stats.payload_bytes) {
        payload->bytes = std::make_unique<char[]>(bytes.size());
        std::copy(bytes.begin(), bytes.end(), payload->bytes.get());
        return payload;
    }
    if (const auto offset = m_spill->write(bytes)) {
        payload->offset = *offset;
        payload->store = m_spill;
        return payload;
    }
    ++m_stats.modes[key.strict_water].spill_faults;
    return {};
}

void ChunkValidationCache::remember(const Key& key,
                                    std::string_view bytes,
                                    const Facts& facts,
                                    bool writer_seeded) try {
    const std::lock_guard lock(m_mutex);
    if (m_entry_limit == 0 || bytes.size() > kMaxRecordBytes ||
        (bytes.size() > m_byte_limit && bytes.size() > m_spill_limit))
        return;
    if (const auto old = m_entries.find(key); old != m_entries.end())
        erase(old);
    while (m_entries.size() >= m_entry_limit) {
        if (!evict_one(key.strict_water))
            return;
    }
    auto payload = retain(key, bytes);
    if (!payload)
        return;
    const bool protect = key.strict_water && m_order[2].size() < m_entry_limit / 2 &&
                         bytes.size() <= m_byte_limit / 2 - m_protected_bytes;
    const std::size_t segment = protect ? 2 : static_cast<std::size_t>(key.strict_water);
    auto& order = m_order[segment];
    order.push_back(key);
    try {
        m_entries.emplace(key, Entry{payload, facts, std::prev(order.end()), segment});
    } catch (...) {
        order.pop_back();
        throw;
    }
    auto& mode = m_stats.modes[key.strict_water];
    ++mode.insertions;
    ++mode.entries;
    if (writer_seeded)
        ++mode.writer_seeded;
    if (payload.use_count() == 2) {
        if (payload->store)
            m_stats.spill_bytes += bytes.size();
        else {
            m_stats.payload_bytes += bytes.size();
            mode.payload_bytes += bytes.size();
        }
    }
    m_stats.spill_extent = m_spill->extent;
    m_stats.entries = m_entries.size();
    m_stats.peak_entries = std::max(m_stats.peak_entries, m_stats.entries);
    m_stats.peak_payload_bytes = std::max(m_stats.peak_payload_bytes, m_stats.payload_bytes);
    m_stats.peak_spill_bytes = std::max(m_stats.peak_spill_bytes, m_stats.spill_bytes);
    if (protect)
        m_protected_bytes += bytes.size();
} catch (const std::exception&) {
    // Retention is optional, including allocation and temporary-storage failures.
}

void ChunkValidationCache::erase_backing(std::map<Key, Entry>::iterator entry) {
    // At most two mode entries can own this backing. Drop both before a miss
    // returns, so neither mode can retain facts from the invalidated extent.
    auto other = entry->first;
    other.strict_water = !other.strict_water;
    const auto peer = m_entries.find(other);
    if (peer != m_entries.end() && peer->second.payload == entry->second.payload)
        erase(peer);
    erase(entry);
}

void ChunkValidationCache::erase(std::map<Key, Entry>::iterator entry) {
    auto& mode = m_stats.modes[entry->first.strict_water];
    const auto& payload = entry->second.payload;
    --mode.entries;
    if (payload.use_count() == 1) {
        if (payload->store)
            m_stats.spill_bytes -= payload->length;
        else {
            m_stats.payload_bytes -= payload->length;
            m_stats.modes[payload->charged_mode].payload_bytes -= payload->length;
        }
    } else if (!payload->store && payload->charged_mode == entry->first.strict_water) {
        mode.payload_bytes -= payload->length;
        payload->charged_mode = !payload->charged_mode;
        m_stats.modes[payload->charged_mode].payload_bytes += payload->length;
    }
    if (entry->second.segment == 2)
        m_protected_bytes -= payload->length;
    m_order[entry->second.segment].erase(entry->second.position);
    m_entries.erase(entry);
    m_stats.entries = m_entries.size();
}

ChunkValidationCache::Stats ChunkValidationCache::stats() const {
    const std::lock_guard lock(m_mutex);
    auto stats = m_stats;
    stats.spill_extent = m_spill->extent;
    stats.eviction_history_entries = m_evicted.size();
    stats.eviction_history_order_entries = m_evicted_order.size();
    return stats;
}

void ChunkValidationCache::clear() {
    const std::lock_guard lock(m_mutex);
    m_entries.clear();
    for (auto& order : m_order)
        order.clear();
    m_evicted.clear();
    m_evicted_order.clear();
    m_spill->reset(m_spill_limit);
    m_protected_bytes = 0;
    m_inventory_active = m_inventory_complete = false;
    m_stats = {};
}
namespace {
bool IsWithin(std::filesystem::path path, const std::filesystem::path& parent) {
    // equivalent also handles case-insensitive spelling on Windows.
    while (!path.empty()) {
        std::error_code error;
        if (path == parent || std::filesystem::equivalent(path, parent, error))
            return true;
        const auto next = path.parent_path();
        if (next == path)
            break;
        path = next;
    }
    return false;
}
} // namespace

void ChunkValidationCache::exclude_save_directory(const std::filesystem::path& save_dir) try {
    const std::lock_guard lock(m_mutex);
    if (m_spill_limit == 0)
        return;
    std::error_code error;
    const auto save = CanonicalCachePath(save_dir, error);
    const bool save_error = static_cast<bool>(error);
    const auto temporary = std::filesystem::temp_directory_path(error);
    const auto root = error ? std::filesystem::path{} : CanonicalCachePath(temporary, error);
    if (!save_error && !error && !IsWithin(root, save) &&
        (m_spill->directory.empty() || !IsWithin(m_spill->directory, save)))
        return;
    // Only derived facts are discarded. No save or validation outcome changes.
    for (auto entry = m_entries.begin(); entry != m_entries.end();) {
        const auto current = entry++;
        if (current->second.payload->store)
            erase(current);
    }
    m_spill_limit = 0;
    m_spill->reset(0);
    m_stats.spill_extent = 0;
} catch (const std::exception&) {
    // The ordinary validation path remains available if path inspection fails.
}

void ChunkValidationCache::spill_fault_for_testing(SpillFault fault) {
    const std::lock_guard lock(m_mutex);
    m_spill->fault = fault;
}
std::filesystem::path ChunkValidationCache::spill_directory_for_testing() const {
    const std::lock_guard lock(m_mutex);
    return m_spill->directory;
}
} // namespace Luminumbra::Persistence
