#pragma once

#include "world/Chunk.h"

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
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
        // Shared heap backing is charged to exactly one surviving mode.
        std::size_t entries = 0, payload_bytes = 0;
        std::size_t spill_hits = 0, spill_faults = 0, writer_seeded = 0;
    };
    struct Stats {
        std::size_t hits = 0, misses = 0, evictions = 0, entries = 0, payload_bytes = 0;
        std::array<ModeStats, 2> modes{}; // Permissive, strict.
        std::size_t first_strict_records = 0, first_strict_bytes = 0;
        std::size_t eviction_history_entries = 0, eviction_history_order_entries = 0;
        std::size_t eviction_history_peak_entries = 0;
        // Live disk payload bytes and the backing file's bounded high-water span.
        std::size_t spill_bytes = 0, spill_extent = 0;
        std::size_t peak_entries = 0, peak_payload_bytes = 0, peak_spill_bytes = 0;
    };
    static constexpr std::size_t kMaxEntries = 16384;
    static constexpr std::size_t kMaxPayloadBytes = std::size_t{64} << 20;
    static constexpr std::size_t kMaxSpillBytes = std::size_t{256} << 20;
    static constexpr std::size_t kMaxRecordBytes = std::size_t{1} << 20;
    // Exact compressed backing is shared across modes. Heap and spill allocations
    // are bounded before insertion; records over 1 MiB are never retained.
    explicit ChunkValidationCache(std::size_t entries = kMaxEntries,
                                  std::size_t bytes = kMaxPayloadBytes,
                                  std::size_t spill_bytes = kMaxSpillBytes);
    ~ChunkValidationCache();
    std::optional<Facts> find(const Key& key, std::string_view bytes);
    void remember(const Key& key,
                  std::string_view bytes,
                  const Facts& facts,
                  bool writer_seeded = false);

    enum class SpillFault {
        None,
        Create,
        Write,
        Read,
        ShortRead
    };
    // Deterministic cache-only faults: callers must fall back to ordinary validation.
    void spill_fault_for_testing(SpillFault fault);
    std::filesystem::path spill_directory_for_testing() const;

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

    Stats stats() const;

    void clear();
    // Disable optional spill backing if the configured temp root overlaps this save.
    void exclude_save_directory(const std::filesystem::path& save_dir);

    // Static persistence helpers join only the calling thread's scope. Sharing a
    // session cache between threads is safe: find/remember/stats/clear hold its mutex.
    // No lock is held during decoding, callbacks, save-file I/O or transaction work.
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
    class SpillStore;
    struct Payload {
        std::unique_ptr<char[]> bytes{};
        std::shared_ptr<SpillStore> store{};
        std::size_t offset = 0, length = 0;
        // Heap-owned identity of the validated bytes, shared by both modes.
        std::array<std::uint8_t, 32> digest{};
        bool charged_mode = false;
        ~Payload();
    };
    struct Entry {
        std::shared_ptr<Payload> payload{};
        Facts facts{};
        std::list<Key>::iterator position{};
        std::size_t segment = 0;
    };
    bool equal(const Payload& payload, std::string_view bytes, bool strict);
    std::shared_ptr<Payload> retain(const Key& key, std::string_view bytes);
    bool evict_one(bool strict);
    bool make_room(bool strict, std::size_t bytes);
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
    void erase(std::map<Key, Entry>::iterator entry);
    void erase_backing(std::map<Key, Entry>::iterator entry);
    mutable std::mutex m_mutex{};
    std::map<Key, Entry> m_entries{};
    std::array<std::list<Key>, 3> m_order{};
    std::set<Key> m_evicted;
    std::deque<Key> m_evicted_order;
    std::size_t m_protected_bytes = 0;
    bool m_inventory_active = false, m_inventory_complete = false;
    Stats m_stats{};
    std::size_t m_entry_limit = 0, m_byte_limit = 0, m_spill_limit = 0;
    std::shared_ptr<SpillStore> m_spill;
};

} // namespace Luminumbra::Persistence
