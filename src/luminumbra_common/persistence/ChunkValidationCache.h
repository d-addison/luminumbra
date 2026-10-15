#pragma once

#include "world/Chunk.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
    struct Stats {
        std::size_t hits = 0, misses = 0, evictions = 0, entries = 0, payload_bytes = 0;
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
        const auto found = m_entries.find(key);
        // string_view equality compares length and every compressed byte.
        if (found == m_entries.end() ||
            std::string_view(found->second.bytes.get(), found->second.length) != bytes) {
            ++m_stats.misses;
            return std::nullopt;
        }
        ++m_stats.hits;
        m_order.splice(m_order.end(), m_order, found->second.position);
        return found->second.facts;
    }

    void remember(const Key& key, std::string_view bytes, const Facts& facts) {
        const std::lock_guard lock(m_mutex);
        if (m_entry_limit == 0 || m_byte_limit == 0 || bytes.size() > m_byte_limit ||
            bytes.size() > kMaxRecordBytes)
            return;
        if (const auto old = m_entries.find(key); old != m_entries.end())
            erase(old);
        while (m_entries.size() >= m_entry_limit ||
               m_stats.payload_bytes > m_byte_limit - bytes.size()) {
            erase(m_entries.find(m_order.front()));
            ++m_stats.evictions;
        }
        // Exact allocation size avoids implementation-dependent string capacity
        // and keeps the byte bound true even during insertion into a full cache.
        std::unique_ptr<char[]> owned(new char[bytes.size()]);
        std::copy(bytes.begin(), bytes.end(), owned.get());
        m_order.push_back(key);
        try {
            m_entries.emplace(
                key, Entry{std::move(owned), bytes.size(), facts, std::prev(m_order.end())});
        } catch (...) {
            m_order.pop_back();
            throw;
        }
        m_stats.payload_bytes += bytes.size();
        m_stats.entries = m_entries.size();
    }

    Stats stats() const {
        const std::lock_guard lock(m_mutex);
        return m_stats;
    }

    void clear() {
        const std::lock_guard lock(m_mutex);
        m_entries.clear();
        m_order.clear();
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
    };
    void erase(std::map<Key, Entry>::iterator entry) {
        m_stats.payload_bytes -= entry->second.length;
        m_order.erase(entry->second.position);
        m_entries.erase(entry);
        m_stats.entries = m_entries.size();
    }
    mutable std::mutex m_mutex{};
    std::map<Key, Entry> m_entries{};
    std::list<Key> m_order{};
    Stats m_stats{};
    std::size_t m_entry_limit = 0, m_byte_limit = 0;
};

} // namespace Luminumbra::Persistence
