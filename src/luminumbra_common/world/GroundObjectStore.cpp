#include "GroundObjectStore.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Luminumbra::world {
namespace {

using Json = nlohmann::json;
using RegionIndex = std::map<RegionKey, std::set<std::uint64_t>>;

constexpr std::int64_t kRegionMm = GroundObjectStore::kRegionSizeMm;
constexpr std::uint64_t kU64Max = std::numeric_limits<std::uint64_t>::max();

constexpr const char* kObjectKeys[] = {"id",
                                       "type",
                                       "x_mm",
                                       "y_mm",
                                       "z_mm",
                                       "spawn_tick",
                                       "active_age",
                                       "despawn_after",
                                       "payload",
                                       "flags"};

std::int32_t FloorRegion(std::int64_t value) {
    std::int64_t quotient = value / kRegionMm;
    if (value % kRegionMm != 0 && value < 0) {
        --quotient;
    }
    return static_cast<std::int32_t>(quotient);
}

// Per-object text used by the hash projections: "<id>,...,<flags>;".
std::string ObjectText(const GroundObject& object) {
    return std::to_string(object.id) + ',' + std::to_string(object.type) + ',' +
           std::to_string(object.x_mm) + ',' + std::to_string(object.y_mm) + ',' +
           std::to_string(object.z_mm) + ',' + std::to_string(object.spawn_tick) + ',' +
           std::to_string(object.active_age) + ',' + std::to_string(object.despawn_after) + ',' +
           std::to_string(object.payload) + ',' + std::to_string(object.flags) + ';';
}

void AppendField(std::string& out, const char* name, const std::string& value) {
    out += '"';
    out += name;
    out += "\":";
    out += value;
}

void Detach(RegionIndex& index, RegionKey key, std::uint64_t id) {
    const auto it = index.find(key);
    if (it == index.end()) {
        return;
    }
    it->second.erase(id);
    if (it->second.empty()) {
        index.erase(it);
    }
}

// True when `after` differs from `before` only in y_mm and the kSettled bit, and is still valid.
bool OnlySettleChanged(const GroundObject& before, const GroundObject& after) {
    constexpr std::uint16_t kOtherFlags = static_cast<std::uint16_t>(~GroundObject::kSettled);
    return after.id == before.id && after.type == before.type && after.x_mm == before.x_mm &&
           after.z_mm == before.z_mm && after.spawn_tick == before.spawn_tick &&
           after.active_age == before.active_age && after.despawn_after == before.despawn_after &&
           after.payload == before.payload && ((after.flags ^ before.flags) & kOtherFlags) == 0 &&
           GroundObjectStore::valid(after);
}

// Reads an unsigned JSON integer no larger than `max`. Negative, fractional and oversized values
// fail.
bool ReadUnsigned(const Json& object, const char* key, std::uint64_t max, std::uint64_t& value) {
    const Json& field = object.at(key);
    if (!field.is_number_unsigned()) {
        return false;
    }
    value = field.get<std::uint64_t>();
    return value <= max;
}

// Reads a signed JSON integer. Fractional values and values above INT64_MAX fail.
bool ReadSigned(const Json& object, const char* key, std::int64_t& value) {
    const Json& field = object.at(key);
    if (field.is_number_unsigned()) {
        const std::uint64_t magnitude = field.get<std::uint64_t>();
        if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        value = static_cast<std::int64_t>(magnitude);
        return true;
    }
    if (field.is_number_integer()) {
        value = field.get<std::int64_t>();
        return true;
    }
    return false;
}

} // namespace

bool GroundObjectStore::valid(const GroundObject& object) {
    constexpr std::int64_t kMax = kMaxCoordinateMm;
    if (object.id == 0 || object.type == 0) {
        return false;
    }
    if ((object.flags & static_cast<std::uint16_t>(~GroundObject::kSettled)) != 0) {
        return false;
    }
    if (object.x_mm < -kMax || object.x_mm >= kMax) {
        return false;
    }
    if (object.z_mm < -kMax || object.z_mm >= kMax) {
        return false;
    }
    if (object.y_mm < -kMax || object.y_mm > kMax) {
        return false;
    }
    if (object.despawn_after != 0 && object.active_age >= object.despawn_after) {
        return false;
    }
    return true;
}

RegionKey GroundObjectStore::region_of(const GroundObject& object) {
    return RegionKey{FloorRegion(object.x_mm), FloorRegion(object.z_mm)};
}

bool GroundObjectStore::insert(const GroundObject& object) {
    if (!valid(object) || m_objects.size() >= kMaxObjects) {
        return false;
    }
    if (m_objects.find(object.id) != m_objects.end()) {
        return false;
    }
    m_objects.emplace(object.id, object);
    m_byRegion[region_of(object)].insert(object.id);
    return true;
}

bool GroundObjectStore::erase(std::uint64_t id) {
    const auto it = m_objects.find(id);
    if (it == m_objects.end()) {
        return false;
    }
    Detach(m_byRegion, region_of(it->second), id);
    m_objects.erase(it);
    return true;
}

const GroundObject* GroundObjectStore::find(std::uint64_t id) const {
    const auto it = m_objects.find(id);
    return it == m_objects.end() ? nullptr : &it->second;
}

void GroundObjectStore::clear() {
    m_objects.clear();
    m_byRegion.clear();
}

bool GroundObjectStore::owns_region(RegionKey key) const {
    return m_byRegion.find(key) != m_byRegion.end();
}

std::vector<GroundObject> GroundObjectStore::in_region(RegionKey key) const {
    std::vector<GroundObject> out;
    const auto it = m_byRegion.find(key);
    if (it == m_byRegion.end()) {
        return out;
    }
    out.reserve(it->second.size());
    for (const std::uint64_t id : it->second) {
        out.push_back(m_objects.at(id));
    }
    return out;
}

std::vector<RegionKey> GroundObjectStore::regions() const {
    std::vector<RegionKey> out;
    out.reserve(m_byRegion.size());
    for (const auto& entry : m_byRegion) {
        out.push_back(entry.first);
    }
    return out;
}

GroundObjectStore::AgeResult GroundObjectStore::age_region(RegionKey key, const SettleFn& settle) {
    AgeResult result;
    const auto regionIt = m_byRegion.find(key);
    if (regionIt == m_byRegion.end()) {
        return result;
    }
    // Copy the id set: despawns below mutate the live index while we walk it.
    const std::vector<std::uint64_t> ids(regionIt->second.begin(), regionIt->second.end());
    for (const std::uint64_t id : ids) {
        const auto objectIt = m_objects.find(id);
        if (objectIt == m_objects.end()) {
            continue;
        }
        GroundObject& live = objectIt->second;

        if (!live.settled() && settle) {
            const GroundObject before = live;
            settle(live);
            if (!OnlySettleChanged(before, live)) {
                live = before;
            } else if (live.settled()) {
                ++result.settled;
            }
        }

        if (live.active_age != kU64Max) {
            ++live.active_age;
            ++result.aged;
        }

        if (live.despawn_after != 0 && live.active_age >= live.despawn_after) {
            result.despawned.push_back(live);
            Detach(m_byRegion, key, id);
            m_objects.erase(objectIt);
        }
    }
    return result;
}

std::string GroundObjectStore::serialize() const {
    if (m_objects.empty()) {
        return {};
    }
    std::string out = std::string("{\"schema\":\"") + kSchema + "\",\"objects\":[";
    bool first = true;
    for (const auto& entry : m_objects) {
        const GroundObject& object = entry.second;
        if (!first) {
            out += ',';
        }
        first = false;
        out += '{';
        AppendField(out, "id", std::to_string(object.id));
        out += ',';
        AppendField(out, "type", std::to_string(object.type));
        out += ',';
        AppendField(out, "x_mm", std::to_string(object.x_mm));
        out += ',';
        AppendField(out, "y_mm", std::to_string(object.y_mm));
        out += ',';
        AppendField(out, "z_mm", std::to_string(object.z_mm));
        out += ',';
        AppendField(out, "spawn_tick", std::to_string(object.spawn_tick));
        out += ',';
        AppendField(out, "active_age", std::to_string(object.active_age));
        out += ',';
        AppendField(out, "despawn_after", std::to_string(object.despawn_after));
        out += ',';
        AppendField(out, "payload", std::to_string(object.payload));
        out += ',';
        AppendField(out, "flags", std::to_string(object.flags));
        out += '}';
    }
    out += "]}\n";
    return out;
}

bool GroundObjectStore::parse(std::string_view bytes,
                              std::uint64_t next_id,
                              GroundObjectStore& out,
                              std::string& error) {
    if (bytes.empty()) {
        error = "ground objects: empty input";
        return false;
    }
    if (bytes.size() > kMaxFileBytes) {
        error = "ground objects: file exceeds size cap";
        return false;
    }
    try {
        const Json document =
            Json::parse(bytes.data(), bytes.data() + bytes.size(), nullptr, false);
        if (document.is_discarded() || !document.is_object() || document.size() != 2 ||
            !document.contains("schema") || !document.contains("objects")) {
            error = "ground objects: malformed document";
            return false;
        }
        const Json& schema = document.at("schema");
        if (!schema.is_string() || schema.get<std::string>() != kSchema) {
            error = "ground objects: wrong schema";
            return false;
        }
        const Json& objects = document.at("objects");
        if (!objects.is_array() || objects.size() > kMaxObjects) {
            error = "ground objects: malformed or oversized object list";
            return false;
        }

        GroundObjectStore temporary;
        std::uint64_t previous = 0;
        for (const Json& entry : objects) {
            if (!entry.is_object() || entry.size() != std::size(kObjectKeys)) {
                error = "ground objects: malformed object";
                return false;
            }
            for (const char* key : kObjectKeys) {
                if (!entry.contains(key)) {
                    error = "ground objects: missing or unknown object key";
                    return false;
                }
            }

            GroundObject object;
            std::uint64_t type = 0;
            std::uint64_t payload = 0;
            std::uint64_t flags = 0;
            const bool numbers =
                ReadUnsigned(entry, "id", kU64Max, object.id) &&
                ReadUnsigned(entry, "type", std::numeric_limits<std::uint32_t>::max(), type) &&
                ReadSigned(entry, "x_mm", object.x_mm) && ReadSigned(entry, "y_mm", object.y_mm) &&
                ReadSigned(entry, "z_mm", object.z_mm) &&
                ReadUnsigned(entry, "spawn_tick", kU64Max, object.spawn_tick) &&
                ReadUnsigned(entry, "active_age", kU64Max, object.active_age) &&
                ReadUnsigned(entry, "despawn_after", kU64Max, object.despawn_after) &&
                ReadUnsigned(
                    entry, "payload", std::numeric_limits<std::uint32_t>::max(), payload) &&
                ReadUnsigned(entry, "flags", std::numeric_limits<std::uint16_t>::max(), flags);
            if (!numbers) {
                error = "ground objects: non-integer or out-of-range field";
                return false;
            }
            object.type = static_cast<std::uint32_t>(type);
            object.payload = static_cast<std::uint32_t>(payload);
            object.flags = static_cast<std::uint16_t>(flags);

            if (!valid(object)) {
                error = "ground objects: invalid object";
                return false;
            }
            if (object.id >= next_id) {
                error = "ground objects: id not below next persistent id";
                return false;
            }
            if (object.id <= previous) {
                error = "ground objects: ids not strictly ascending";
                return false;
            }
            previous = object.id;
            if (!temporary.insert(object)) {
                error = "ground objects: insert refused";
                return false;
            }
        }

        const std::string canonical = temporary.serialize();
        if (std::string_view(canonical) != bytes) {
            error = "ground objects: bytes are not canonical";
            return false;
        }
        out = std::move(temporary);
        return true;
    } catch (const std::exception&) {
        error = "ground objects: parse exception";
        return false;
    }
}

std::string GroundObjectStore::canonical_bytes() const {
    if (m_objects.empty()) {
        return {};
    }
    std::string out = "ground_objects:v1:";
    for (const auto& entry : m_objects) {
        out += ObjectText(entry.second);
    }
    return out;
}

std::string GroundObjectStore::region_bytes(RegionKey key) const {
    std::string out;
    const auto it = m_byRegion.find(key);
    if (it == m_byRegion.end()) {
        return out;
    }
    for (const std::uint64_t id : it->second) {
        out += ObjectText(m_objects.at(id));
    }
    return out;
}

} // namespace Luminumbra::world
