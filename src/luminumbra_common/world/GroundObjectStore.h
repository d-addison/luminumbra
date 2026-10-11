#pragma once

#include "ActiveRegionLedger.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Luminumbra::world {

// One persistent ground object (S5). Positions are integer millimetres; no floating point is ever
// persisted or hashed.
struct GroundObject {
    static constexpr std::uint16_t kSettled = 1;
    std::uint64_t id = 0;
    std::uint32_t type = 0;
    std::int64_t x_mm = 0, y_mm = 0, z_mm = 0;
    std::uint64_t spawn_tick = 0;
    // Ticks this object has aged while its region was due. Never advances for a frozen region.
    std::uint64_t active_age = 0;
    // 0 = never despawns; otherwise the object is removed when active_age reaches it.
    std::uint64_t despawn_after = 0;
    std::uint32_t payload = 0;
    std::uint16_t flags = 0;
    [[nodiscard]] bool settled() const {
        return (flags & kSettled) != 0;
    }
    bool operator==(const GroundObject&) const = default;
};

// Ordered, bounded store of ground objects keyed by id. Pure data: no IO, no engine access.
class GroundObjectStore {
public:
    // 16,384 objects at the worst-case serialized record (~300 bytes) stay under the file cap.
    static constexpr std::size_t kMaxObjects = 16384;
    static constexpr std::size_t kMaxFileBytes = 8u * 1024u * 1024u;
    // x and z must satisfy -kMaxCoordinateMm <= v < kMaxCoordinateMm (the simulation anchor
    // bound, half open: +2^24 m would fall in a region the ledger refuses). y must satisfy
    // |y| <= kMaxCoordinateMm.
    static constexpr std::int64_t kMaxCoordinateMm = 16'777'216'000;
    static constexpr std::int64_t kRegionSizeMm = 512'000;
    static constexpr const char* kSchema = "luminumbra.ground_objects.v1";
    static constexpr const char* kRelativePath = "chunks/region/ground-objects.json";

    // id != 0, type != 0, coordinates in range (see above), flags only kSettled, and
    // (despawn_after == 0 or active_age < despawn_after).
    [[nodiscard]] static bool valid(const GroundObject& object);
    // floor(x_mm / 512000), floor(z_mm / 512000) with integer arithmetic.
    [[nodiscard]] static RegionKey region_of(const GroundObject& object);

    // Refuses (false, store unchanged) on an invalid object, a duplicate id or a full store.
    bool insert(const GroundObject& object);
    // Removes by id. Returns false when absent.
    bool erase(std::uint64_t id);
    [[nodiscard]] const GroundObject* find(std::uint64_t id) const;
    void clear();
    [[nodiscard]] bool empty() const {
        return m_objects.empty();
    }
    [[nodiscard]] std::size_t size() const {
        return m_objects.size();
    }
    [[nodiscard]] const std::map<std::uint64_t, GroundObject>& objects() const {
        return m_objects;
    }
    // O(log n). True when at least one object lies in the region.
    [[nodiscard]] bool owns_region(RegionKey key) const;
    // Objects of the region in ascending id order.
    [[nodiscard]] std::vector<GroundObject> in_region(RegionKey key) const;
    // The set of regions that currently hold at least one object, ascending.
    [[nodiscard]] std::vector<RegionKey> regions() const;

    // Attempts to settle an unsettled object. May change ONLY y_mm and set kSettled; returns
    // whether the object is settled afterwards. age_region restores the complete pre-call object
    // (and treats it as unsettled, counting nothing) when the callback changed anything else or
    // left an invalid object.
    using SettleFn = std::function<bool(GroundObject&)>;
    struct AgeResult {
        std::uint64_t aged = 0;              // objects whose active_age advanced
        std::uint64_t settled = 0;           // objects that became settled this call
        std::vector<GroundObject> despawned; // ascending id, final state at removal
    };
    // One tick of ageing for one due region, in ascending id order: unsettled objects try
    // `settle` first, then active_age += 1 (saturating at UINT64_MAX), then an object with
    // despawn_after != 0 and active_age >= despawn_after is removed and reported.
    AgeResult age_region(RegionKey key, const SettleFn& settle);

    // Canonical file bytes. Empty string for an empty store (an empty store writes no file).
    // Format, exactly (no whitespace other than the single trailing newline):
    //   {"schema":"luminumbra.ground_objects.v1","objects":[{"id":N,"type":N,"x_mm":N,
    //   "y_mm":N,"z_mm":N,"spawn_tick":N,"active_age":N,"despawn_after":N,"payload":N,
    //   "flags":N},...]}\n
    // (the example is wrapped; the real output is one line). Objects ascend by id.
    [[nodiscard]] std::string serialize() const;
    // Strict transactional parse. Refuses (false, `error` set, `out` untouched) on: size >
    // kMaxFileBytes, malformed JSON, wrong schema, unknown/missing/duplicate keys, a non-integer
    // number, any invalid object, ids not strictly ascending, more than kMaxObjects, an id >=
    // `next_id`, or bytes that differ from serialize() of the parsed result (rejects alternate
    // encodings). An empty `bytes` is not valid input (callers handle absence themselves).
    static bool parse(std::string_view bytes,
                      std::uint64_t next_id,
                      GroundObjectStore& out,
                      std::string& error);

    // Hash text. Empty string when the store is empty, otherwise
    // "ground_objects:v1:" + region_bytes of every object, i.e. objects_bytes(all).
    [[nodiscard]] std::string canonical_bytes() const;
    // Empty string when the region has no objects; otherwise the per-object text of the region's
    // objects in id order: for each object
    // "<id>,<type>,<x_mm>,<y_mm>,<z_mm>,<spawn_tick>,<active_age>,<despawn_after>,<payload>,
    // <flags>;" (one line, decimal, no spaces). Used by the region digest projection.
    [[nodiscard]] std::string region_bytes(RegionKey key) const;

private:
    std::map<std::uint64_t, GroundObject> m_objects;
    std::map<RegionKey, std::set<std::uint64_t>> m_byRegion;
};

} // namespace Luminumbra::world
