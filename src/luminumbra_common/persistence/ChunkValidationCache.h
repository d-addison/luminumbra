#pragma once

#include "world/Chunk.h"

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace Luminumbra::Persistence {

// Session-owned, derived facts only. Each mode has its own exact-byte key; neither
// file metadata nor the snapshot's non-cryptographic fingerprint authorizes a hit.
class ChunkValidationCache {
public:
    struct Facts {
        IVec3 coords{};
        std::size_t sdf_size = 0;
        bool authoritative = false;
    };
    struct Key {
        std::uint64_t id = 0;
        std::uint32_t raw_size = 0;
        std::uint8_t lod = 0;
        std::uint8_t flags = 0;
        bool strict_water = false;
        auto operator<=>(const Key&) const = default;
    };
    struct ModeStats {
        std::size_t hits = 0, misses = 0, insertions = 0, evictions = 0, decodes = 0;
        std::size_t absent = 0, byte_mismatch = 0, oversize = 0, evicted = 0;
        std::size_t entries = 0, payload_bytes = 0;
    };
    struct Stats {
        std::size_t hits = 0, misses = 0, evictions = 0, entries = 0, payload_bytes = 0;
        std::array<ModeStats, 2> modes{}; // Permissive, strict.
        std::size_t first_strict_records = 0, first_strict_bytes = 0;
        std::size_t eviction_history_entries = 0, eviction_history_order_entries = 0;
        std::size_t eviction_history_peak_entries = 0;
    };
    static constexpr std::size_t kMaxEntries = 16384;
    static constexpr std::size_t kMaxPayloadBytes = std::size_t{64} << 20;
    static constexpr std::size_t kMaxRecordBytes = std::size_t{1} << 20;
    // At most 16,384 entries and 64 MiB of exact compressed byte buffers,
    // plus bounded map/list nodes and compact facts. Evict before allocating.
    // Records over 1 MiB are validated normally without retaining a copy.
    // Smaller limits permit deterministic eviction regressions; zero disables reuse.
    explicit ChunkValidationCache(std::size_t entries = kMaxEntries,
                                  std::size_t bytes = kMaxPayloadBytes)
        : m_entry_limit(std::min(entries, kMaxEntries))
        , m_byte_limit(std::min(bytes, kMaxPayloadBytes)) {}

    std::optional<Facts> find(const Key& key, std::string_view bytes) {
        const std::lock_guard lock(m_mutex);
        auto& mode = m_stats.modes[key.strict_water];
        if (key.strict_water && m_inventory_active) {
            ++m_stats.first_strict_records;
            m_stats.first_strict_bytes += bytes.size();
        }
        const auto found = m_entries.find(key);
        // string_view equality compares length and every compressed byte.
        if (found != m_entries.end() &&
            std::string_view(found->second.bytes.get(), found->second.length) == bytes) {
            ++m_stats.hits;
            ++mode.hits;
            auto& order = m_order[found->second.segment];
            order.splice(order.end(), order, found->second.position);
            return found->second.facts;
        }
        ++m_stats.misses;
        ++mode.misses;
        if (bytes.size() > kMaxRecordBytes || bytes.size() > m_byte_limit)
            ++mode.oversize;
        else if (found != m_entries.end())
            ++mode.byte_mismatch;
        else if (m_evicted.contains(key))
            ++mode.evicted;
        else
            ++mode.absent;
        return std::nullopt;
    }

    void remember(const Key& key, std::string_view bytes, const Facts& facts) {
        const std::lock_guard lock(m_mutex);
        if (m_entry_limit == 0 || m_byte_limit == 0 || bytes.size() > m_byte_limit ||
            bytes.size() > kMaxRecordBytes)
            return;
        if (const auto old = m_entries.find(key); old != m_entries.end())
            erase(old);
        // Fill a bounded strict protected segment without requiring a second hit.
        // Ordered scans larger than capacity retain this stable reusable subset.
        // Permissive one-shot entries can evict only other permissive entries.
        const bool protect = key.strict_water && m_order[2].size() < m_entry_limit / 2 &&
                             bytes.size() <= m_byte_limit / 2 - m_protected_bytes;
        if (!make_room(key.strict_water, bytes.size()))
            return;
        const std::size_t segment = protect ? 2 : static_cast<std::size_t>(key.strict_water);
        // Allocate exactly the payload length, after eviction, so the byte bound
        // also holds during insertion and does not depend on string capacity.
        std::unique_ptr<char[]> owned(new char[bytes.size()]);
        std::copy(bytes.begin(), bytes.end(), owned.get());
        auto& order = m_order[segment];
        order.push_back(key);
        try {
            m_entries.emplace(
                key, Entry{std::move(owned), bytes.size(), facts, std::prev(order.end()), segment});
        } catch (...) {
            order.pop_back();
            throw;
        }
        auto& mode = m_stats.modes[key.strict_water];
        ++mode.insertions;
        ++mode.entries;
        mode.payload_bytes += bytes.size();
        m_stats.payload_bytes += bytes.size();
        m_stats.entries = m_entries.size();
        if (protect)
            m_protected_bytes += bytes.size();
    }

    // Captures one complete strict inventory walk, including cache hits. This is
    // observational state only; it never participates in validation or admission.
    class InventoryScope {
    public:
        explicit InventoryScope(ChunkValidationCache* cache)
            : m_cache(cache) {
            if (!m_cache)
                return;
            const std::lock_guard lock(m_cache->m_mutex);
            m_owner = !m_cache->m_inventory_active && !m_cache->m_inventory_complete;
            if (m_owner)
                m_cache->m_inventory_active = true;
        }
        ~InventoryScope() {
            if (!m_owner)
                return;
            const std::lock_guard lock(m_cache->m_mutex);
            m_cache->m_inventory_active = false;
            m_cache->m_inventory_complete = m_cache->m_stats.first_strict_records != 0;
        }
        InventoryScope(const InventoryScope&) = delete;
        InventoryScope& operator=(const InventoryScope&) = delete;

    private:
        ChunkValidationCache* m_cache;
        bool m_owner = false;
    };

    void record_decode(bool strict_water) {
        const std::lock_guard lock(m_mutex);
        ++m_stats.modes[strict_water].decodes;
    }

    Stats stats() const {
        const std::lock_guard lock(m_mutex);
        auto stats = m_stats;
        stats.eviction_history_entries = m_evicted.size();
        stats.eviction_history_order_entries = m_evicted_order.size();
        return stats;
    }

    void clear() {
        const std::lock_guard lock(m_mutex);
        m_entries.clear();
        for (auto& order : m_order)
            order.clear();
        m_evicted.clear();
        m_evicted_order.clear();
        m_protected_bytes = 0;
        m_inventory_active = m_inventory_complete = false;
        m_stats = {};
    }

    // Static persistence helpers join only the calling thread's scope. Sharing a
    // session cache between threads is safe: find/remember/stats/clear hold its mutex.
    // No lock is held during decoding, callbacks, file I/O or transaction work.
    class Scope {
    public:
        explicit Scope(ChunkValidationCache& cache)
            : m_previous(active()) {
            active() = &cache;
        }
        ~Scope() {
            active() = m_previous;
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        ChunkValidationCache* m_previous = nullptr;
    };
    static ChunkValidationCache*& active() {
        static thread_local ChunkValidationCache* cache = nullptr;
        return cache;
    }

private:
    struct Entry {
        std::unique_ptr<char[]> bytes{};
        std::size_t length = 0;
        Facts facts{};
        std::list<Key>::iterator position{};
        std::size_t segment = 0;
    };
    bool make_room(bool strict, std::size_t bytes) {
        while (m_entries.size() >= m_entry_limit || m_stats.payload_bytes > m_byte_limit - bytes) {
            const auto segment = !m_order[0].empty() ? 0u : 1u;
            if ((!strict && segment != 0) || m_order[segment].empty())
                return false;
            const auto victim = m_entries.find(m_order[segment].front());
            record_eviction(victim->first);
            ++m_stats.evictions;
            ++m_stats.modes[victim->first.strict_water].evictions;
            erase(victim);
        }
        return true;
    }
    void record_eviction(const Key& key) {
        // Bounded key-only history distinguishes recent evictions from cold misses.
        if (m_evicted.contains(key))
            return;
        // Leave room in both containers before either insertion, even transiently.
        if (m_evicted_order.size() >= m_entry_limit) {
            m_evicted.erase(m_evicted_order.front());
            m_evicted_order.pop_front();
        }
        m_evicted_order.push_back(key);
        m_stats.eviction_history_peak_entries =
            std::max(m_stats.eviction_history_peak_entries, m_evicted_order.size());
        try {
            m_evicted.insert(key);
            m_stats.eviction_history_peak_entries =
                std::max(m_stats.eviction_history_peak_entries, m_evicted.size());
        } catch (...) {
            m_evicted_order.pop_back();
            throw;
        }
    }
    void erase(std::map<Key, Entry>::iterator entry) {
        auto& mode = m_stats.modes[entry->first.strict_water];
        --mode.entries;
        mode.payload_bytes -= entry->second.length;
        m_stats.payload_bytes -= entry->second.length;
        if (entry->second.segment == 2)
            m_protected_bytes -= entry->second.length;
        m_order[entry->second.segment].erase(entry->second.position);
        m_entries.erase(entry);
        m_stats.entries = m_entries.size();
    }
    mutable std::mutex m_mutex{};
    std::map<Key, Entry> m_entries{};
    std::array<std::list<Key>, 3> m_order{};
    std::set<Key> m_evicted;
    std::deque<Key> m_evicted_order;
    std::size_t m_protected_bytes = 0;
    bool m_inventory_active = false, m_inventory_complete = false;
    Stats m_stats{};
    std::size_t m_entry_limit = 0, m_byte_limit = 0;
};

} // namespace Luminumbra::Persistence
