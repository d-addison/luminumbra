#include "GroundObjectStore.h"
#include "persistence/SavedInputLimits.h"

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
constexpr std::size_t kEnvelopeBytes = 128;

bool FitsFileBudget(std::size_t object_bytes, std::size_t notification_bytes) {
    constexpr auto kAvailable = GroundObjectStore::kMaxFileBytes - kEnvelopeBytes;
    return object_bytes <= kAvailable && notification_bytes <= kAvailable - object_bytes;
}

std::size_t NotificationBytes(const GroundNotifications& notifications) {
    std::size_t bytes = 0;
    for (const auto& event : notifications.events)
        bytes += event.canonical_size() + 1;
    return bytes;
}

std::string SerializeNotification(const luminumbra::simulation::SimulationEvent& event,
                                  std::size_t* rejected_payloads) {
    try {
        return event.canonical_bytes();
    } catch (const Json::type_error& error) {
        if (error.id != 316 ||
            (event.topic != "ground.spawned" && event.topic != "ground.despawned"))
            throw;
        if (rejected_payloads)
            ++*rejected_payloads;
        return {};
    }
}

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

std::size_t GroundObjectStore::ReservedBytes(GroundObject object) {
    object.active_age = std::numeric_limits<std::uint32_t>::max();
    object.y_mm = -GroundObjectStore::kMaxVerticalMm + 1;
    object.flags = GroundObject::kSettled;
    // ObjectText has ten punctuation bytes; JSON has 104 (keys, quotes, braces,
    // colons and separators). Reserve the per-object array comma as well.
    return ObjectText(object).size() + 95;
}

// True when `after` differs from `before` only in y_mm and the kSettled bit, and is still valid.
bool GroundObjectStore::OnlySettleChanged(const GroundObject& before, const GroundObject& after) {
    constexpr std::uint16_t kOtherFlags = static_cast<std::uint16_t>(~GroundObject::kSettled);
    return after.id == before.id && after.type == before.type && after.x_mm == before.x_mm &&
           after.z_mm == before.z_mm && after.spawn_tick == before.spawn_tick &&
           after.active_age == before.active_age && after.despawn_after == before.despawn_after &&
           after.payload == before.payload && ((after.flags ^ before.flags) & kOtherFlags) == 0 &&
           GroundObjectStore::valid(after);
}

bool GroundObjectStore::valid(const GroundObject& object) {
    constexpr std::int64_t kMax = kMaxCoordinateMm;
    if (object.id == 0 || object.id >= ActiveRegionLedger::kPersistentIdLimit || object.type == 0) {
        return false;
    }
    if ((object.flags & static_cast<std::uint16_t>(~GroundObject::kSettled)) != 0) {
        return false;
    }
    if (object.x_mm <= -kMax || object.x_mm >= kMax) {
        return false;
    }
    if (object.z_mm <= -kMax || object.z_mm >= kMax) {
        return false;
    }
    if (object.y_mm <= -kMaxVerticalMm || object.y_mm >= kMaxVerticalMm) {
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

bool GroundObjectStore::insert(const GroundObject& object, std::size_t notification_bytes) {
    if (!valid(object) || m_objects.size() >= kMaxObjects) {
        return false;
    }
    if (m_objects.find(object.id) != m_objects.end()) {
        return false;
    }
    const auto reserved = ReservedBytes(object);
    // The envelope includes a notification cursor at its maximum width. Each object reservation
    // also covers its eventual despawn event at maximum tick/sequence widths.
    if (!FitsFileBudget(m_reserved_bytes + reserved, notification_bytes))
        return false;
    m_objects.emplace(object.id, object);
    m_byRegion[region_of(object)].insert(object.id);
    m_reserved_bytes += reserved;
    return true;
}

bool GroundObjectStore::erase(std::uint64_t id) {
    const auto it = m_objects.find(id);
    if (it == m_objects.end()) {
        return false;
    }
    Detach(m_byRegion, region_of(it->second), id);
    m_reserved_bytes -= ReservedBytes(it->second);
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
    m_reserved_bytes = 0;
}

bool GroundObjectStore::owns_region(RegionKey key) const {
    return m_byRegion.find(key) != m_byRegion.end();
}

std::size_t GroundObjectStore::region_size(RegionKey key) const {
    const auto it = m_byRegion.find(key);
    return it == m_byRegion.end() ? 0 : it->second.size();
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
    if (settle) {
        return age_region(key, [&settle](GroundObject& object) { return settle(object); });
    }
    return AgeRegion<false>(key, [](GroundObject&) { return false; });
}

std::string GroundObjectStore::serialize(const GroundNotifications& notifications,
                                         std::size_t* rejected_payloads) const {
    if (rejected_payloads)
        *rejected_payloads = 0;
    const bool has_notifications =
        notifications.next_sequence != 0 || !notifications.events.empty();
    if (m_objects.empty() && !has_notifications) {
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
    out += ']';
    if (has_notifications) {
        out +=
            ",\"notifications\":{\"next_sequence\":" + std::to_string(notifications.next_sequence) +
            ",\"events\":[";
        bool first_event = true;
        for (const auto& event : notifications.events) {
            const auto event_bytes = SerializeNotification(event, rejected_payloads);
            if (event_bytes.empty())
                continue;
            if (!first_event)
                out += ',';
            first_event = false;
            out += event_bytes;
        }
        out += "]}";
    }
    out += "}\n";
    return out;
}

std::string GroundNotifications::canonical_bytes() const noexcept {
    try {
        const auto bytes = GroundObjectStore{}.serialize(*this);
        return bytes.empty() ? std::string{} : "ground_notifications:v1:" + bytes;
    } catch (...) {
        return "!";
    }
}

bool GroundObjectStore::fits_file_budget(const GroundNotifications& notifications) const {
    return FitsFileBudget(m_reserved_bytes, NotificationBytes(notifications));
}

bool GroundObjectStore::parse(std::string_view bytes,
                              std::uint64_t next_id,
                              GroundObjectStore& out,
                              std::string& error,
                              GroundNotifications* notifications) {
    if (bytes.empty()) {
        error = "ground objects: empty input";
        return false;
    }
    if (bytes.size() > kMaxFileBytes) {
        error = "ground objects: file exceeds size cap";
        return false;
    }
    if (!Persistence::SavedInputLimits::JsonDepthAllowed(bytes)) {
        error = "ground objects: JSON nesting depth exceeds compatibility limit";
        return false;
    }
    try {
        const Json document =
            Json::parse(bytes.data(), bytes.data() + bytes.size(), nullptr, false);
        if (document.is_discarded() || !document.is_object() ||
            document.size() != (document.contains("notifications") ? 3u : 2u) ||
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
            std::uint64_t active_age = 0, despawn_after = 0;
            const bool numbers =
                ReadUnsigned(entry, "id", kU64Max, object.id) &&
                ReadUnsigned(entry, "type", std::numeric_limits<std::uint32_t>::max(), type) &&
                ReadSigned(entry, "x_mm", object.x_mm) && ReadSigned(entry, "y_mm", object.y_mm) &&
                ReadSigned(entry, "z_mm", object.z_mm) &&
                ReadUnsigned(entry, "spawn_tick", kU64Max, object.spawn_tick) &&
                ReadUnsigned(
                    entry, "active_age", std::numeric_limits<std::uint32_t>::max(), active_age) &&
                ReadUnsigned(entry,
                             "despawn_after",
                             std::numeric_limits<std::uint32_t>::max(),
                             despawn_after) &&
                ReadUnsigned(
                    entry, "payload", std::numeric_limits<std::uint32_t>::max(), payload) &&
                ReadUnsigned(entry, "flags", std::numeric_limits<std::uint16_t>::max(), flags);
            if (!numbers) {
                error = "ground objects: non-integer or out-of-range field";
                return false;
            }
            object.type = static_cast<std::uint32_t>(type);
            object.active_age = static_cast<std::uint32_t>(active_age);
            object.despawn_after = static_cast<std::uint32_t>(despawn_after);
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

        GroundNotifications pending;
        if (document.contains("notifications")) {
            const auto& state = document.at("notifications");
            if (!state.is_object() || state.size() != 2 || !state.contains("next_sequence") ||
                !state.contains("events") ||
                !ReadUnsigned(state, "next_sequence", kU64Max, pending.next_sequence) ||
                !state.at("events").is_array()) {
                error = "ground objects: malformed notifications";
                return false;
            }
            for (const auto& value : state.at("events")) {
                luminumbra::simulation::SimulationEvent event;
                std::int64_t lane = 0;
                if (!value.is_object() || value.size() != 5 || !value.contains("tick") ||
                    !value.contains("lane") || !value.contains("sequence") ||
                    !value.contains("topic") || !value.contains("payload") ||
                    !ReadUnsigned(value, "tick", kU64Max, event.tick) ||
                    !ReadSigned(value, "lane", lane) ||
                    lane < std::numeric_limits<std::int32_t>::min() ||
                    lane > std::numeric_limits<std::int32_t>::max() ||
                    !ReadUnsigned(value, "sequence", kU64Max, event.sequence) ||
                    event.sequence >= pending.next_sequence ||
                    (!pending.events.empty() && event.sequence <= pending.events.back().sequence) ||
                    !value.at("topic").is_string() || !value.at("payload").is_string()) {
                    error = "ground objects: malformed notification";
                    return false;
                }
                event.lane = static_cast<std::int32_t>(lane);
                event.topic = value.at("topic").get<std::string>();
                event.payload = value.at("payload").get<std::string>();
                if (event.topic != "ground.spawned" && event.topic != "ground.despawned") {
                    error = "ground objects: unsupported notification topic";
                    return false;
                }
                pending.events.push_back(std::move(event));
            }
        }
        if (!temporary.fits_file_budget(pending)) {
            error = "ground objects: notification capacity reached";
            return false;
        }
        const std::string canonical = temporary.serialize(pending);
        if (std::string_view(canonical) != bytes) {
            error = "ground objects: bytes are not canonical";
            return false;
        }
        out = std::move(temporary);
        if (notifications)
            *notifications = std::move(pending);
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
