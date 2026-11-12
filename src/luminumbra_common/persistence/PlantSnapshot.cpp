#include "PlantSnapshot.h"

#include "PlantPersistence.h"
#include "SavedJsonAdmission.h"
#include "core/SystemConfig.h"
#include "simulation/SimulationIdentity.h"

#include <algorithm>
#include <array>
#include <exception>
#include <initializer_list>
#include <limits>
#include <locale>
#include <ostream>
#include <set>
#include <utility>

namespace Luminumbra::Persistence {
namespace {
namespace Detail = luminumbra::foliage::pp_detail;
using luminumbra::core::SimulationIdentityValues;
using Json = nlohmann::json;

bool Refuse(std::string& error) {
    error =
        "Corrupt plant v2 snapshot: invalid fields, identity, component or compatibility limit.";
    return false;
}

// SAX admission precedes proportional DOM storage. At most 64 frames, eight
// short keys per object, 2^20 records, 17 components and eight genes per array.
// The byte scanner separately enforces the 1 MiB/128 MiB component budgets.
class ShapeAdmission final : public nlohmann::json_sax<Json> {
public:
    bool null() override {
        return Scalar();
    }
    bool boolean(bool) override {
        return Scalar();
    }
    bool number_integer(number_integer_t) override {
        return Scalar();
    }
    bool number_unsigned(number_unsigned_t) override {
        return Scalar();
    }
    bool number_float(number_float_t, const string_t&) override {
        return Scalar();
    }
    bool string(string_t& value) override {
        if (value.size() > 64)
            return false;
        if (m_depth == 1 && m_frames[0].keys_used != 0 &&
            m_frames[0].keys[m_frames[0].keys_used - 1] == "schema")
            m_schema = value;
        return Scalar();
    }
    bool binary(binary_t&) override {
        return false;
    }
    bool start_object(std::size_t) override {
        return Start(false);
    }
    bool end_object() override {
        return End();
    }
    bool start_array(std::size_t) override {
        return Start(true);
    }
    bool end_array() override {
        return End();
    }
    bool key(string_t& value) override {
        if (m_depth == 0 || value.size() > 32)
            return false;
        auto& frame = m_frames[m_depth - 1];
        if (frame.keys_used == frame.keys.size())
            return false;
        for (std::size_t index = 0; index < frame.keys_used; ++index) {
            if (frame.keys[index] == value)
                return false;
        }
        frame.keys[frame.keys_used++] = value;
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }
    [[nodiscard]] const std::string& schema() const {
        return m_schema;
    }

private:
    struct Frame {
        bool array = false;
        std::size_t count = 0;
        std::array<std::string, 8> keys{};
        std::size_t keys_used = 0;
    };
    bool Scalar(bool container = false) {
        if (m_depth == 0)
            return false;
        auto& parent = m_frames[m_depth - 1];
        if (!parent.array)
            return true;
        if ((m_depth == 2 || m_depth == 4) != container)
            return false;
        const std::size_t maximum = m_depth == 2   ? kMaxPlantSnapshotRecords
                                    : m_depth == 4 ? kMaxPlantSnapshotComponents
                                                   : Components::kPlantGeneCount;
        return ++parent.count <= maximum;
    }
    bool Start(bool array) {
        if (m_depth == m_frames.size())
            return false;
        const std::string_view key =
            m_depth == 0 || m_frames[m_depth - 1].keys_used == 0
                ? std::string_view{}
                : m_frames[m_depth - 1].keys[m_frames[m_depth - 1].keys_used - 1];
        if (array
                ? !((m_depth == 1 && key == "entities") || (m_depth == 3 && key == "components") ||
                    (m_depth == 6 && (key == "genes" || key == "next_genome")))
                : !(m_depth == 0 || m_depth == 2 || m_depth == 4 ||
                    (m_depth == 5 && key == "data")))
            return false;
        if (m_depth != 0 && !Scalar(true))
            return false;
        m_frames[m_depth++] = Frame{.array = array};
        return true;
    }
    bool End() {
        if (m_depth == 0)
            return false;
        --m_depth;
        return true;
    }
    std::array<Frame, SavedInputLimits::kJsonNestingDepth> m_frames{};
    std::size_t m_depth = 0;
    std::string m_schema;
};

bool UnsignedFields(const Json& data) {
    for (const auto& item : data.items()) {
        if (item.key() == "owner_x" || item.key() == "owner_z")
            continue;
        if (item.value().is_number_integer() && !item.value().is_number_unsigned())
            return false;
    }
    return true;
}

template<typename T>
bool UInt(const Json& value) {
    return value.is_number_unsigned() && Detail::FitsNumber<T>(value);
}

bool ValidComponent(const Ecs::EntityComponentSnapshot& component) {
    const auto& data = component.data;
    if (!data.is_object() || !UnsignedFields(data))
        return false;
    if (component.type == "FlowingWaterSource")
        return data.size() == 1 &&
               Detail::ScalarInRange(data.at("flow_rate"), std::numeric_limits<float>::max());
    if (component.type == "Decay")
        return data.size() == 4 && UInt<std::uint32_t>(data.at("decay_ticks")) &&
               UInt<std::uint32_t>(data.at("decay_duration")) &&
               data.at("decay_ticks") <= data.at("decay_duration") &&
               UInt<std::uint16_t>(data.at("nutrient_released_milli")) &&
               data.at("nutrient_released_milli") <= 1000u &&
               UInt<std::uint8_t>(data.at("fully_decomposed")) &&
               data.at("fully_decomposed") <= 1u &&
               (data.at("fully_decomposed") == 0u ||
                data.at("decay_ticks") == data.at("decay_duration"));
    if (component.type == "FieldEmitter")
        return data.size() == 3 && UInt<int>(data.at("channel")) && data.at("channel") < 64u &&
               UInt<std::uint32_t>(data.at("rate_raw_per_tick")) &&
               UInt<int>(data.at("radius_cells")) && data.at("radius_cells") <= 31u;
    if (!Detail::ValidEnabledComponent(component))
        return false;
    if (component.type == "PlantGrowth")
        return data.at("planted_tick") <= data.at("last_tick") &&
               data.at("last_tick") < world::WorldClock::kTickLimit;
    if (component.type == "PlantRegion")
        return data.at("region_stamp") < world::WorldClock::kTickLimit;
    if (component.type == "Pollination")
        return data.at("last_pollen_tick") < world::WorldClock::kTickLimit;
    if (component.type == "Transform")
        return world::ActiveRegionLedger::valid_anchor(
            {data.at("x").get<float>(), data.at("y").get<float>(), data.at("z").get<float>()});
    return true;
}

bool ValidRecord(const Ecs::EntitySnapshotRecord& record,
                 std::uint64_t next_id,
                 std::size_t& aggregate) {
    if (record.name != "plant" || !simulation::IsValidSimulationId(record.entity_id) ||
        record.entity_id >= next_id || record.components.size() > kMaxPlantSnapshotComponents)
        return false;
    std::set<std::string_view> types;
    for (const auto& component : record.components) {
        std::string bytes;
        // The envelope adds five container levels before component data.
        if (!AdmitJsonValueDepth(component.data,
                                 SavedInputLimits::kJsonNestingDepth - 5,
                                 SavedInputLimits::kPlantComponentBytes) ||
            !BoundedJsonDump(component.data, SavedInputLimits::kPlantComponentBytes, bytes) ||
            bytes.size() > SavedInputLimits::kPlantComponentAggregateBytes - aggregate ||
            !types.insert(component.type).second || !ValidComponent(component))
            return false;
        aggregate += bytes.size();
    }
    return types.contains("Transform") && types.contains("PlantGenome") &&
           types.contains("PlantGrowth") &&
           (!types.contains("PlantRegion") || Detail::OwnerMatchesTransform(record));
}

bool ExactObject(const Json& value, std::initializer_list<std::string_view> keys) {
    if (!value.is_object() || value.size() != keys.size())
        return false;
    return std::all_of(keys.begin(), keys.end(), [&](auto key) { return value.contains(key); });
}
bool ReadRecord(const Json& entity,
                Ecs::EntitySnapshotRecord& record,
                std::uint64_t& previous_id,
                std::size_t& component_count,
                std::size_t& aggregate) {
    if (!ExactObject(entity, {"id", "components"}) || !UInt<std::uint64_t>(entity.at("id")) ||
        !entity.at("components").is_array() ||
        entity.at("components").size() > kMaxPlantSnapshotComponents)
        return false;
    record.entity_id = entity.at("id").get<std::uint64_t>();
    record.name = "plant";
    if (record.entity_id <= previous_id)
        return false;
    std::string previous_type;
    for (const auto& component : entity.at("components")) {
        if (!ExactObject(component, {"type", "version", "data"}) ||
            !component.at("type").is_string() || !UInt<std::uint32_t>(component.at("version")) ||
            component.at("version") != 1u)
            return false;
        const auto type = component.at("type").get<std::string>();
        if (type <= previous_type)
            return false;
        previous_type = type;
        record.components.push_back({type, component.at("data")});
        ++component_count;
    }
    if (!ValidRecord(record, world::ActiveRegionLedger::kPersistentIdLimit, aggregate))
        return false;
    previous_id = record.entity_id;
    return true;
}

// Retain one record at a time, using a concrete SAX reader rather than a
// type-erased parser callback. Header storage stays constant; each completed
// record is validated and removed from the one-element parsing array.
class DocumentAdmission final : public nlohmann::json_sax<Json> {
public:
    explicit DocumentAdmission(PlantSnapshot* output)
        : m_output(output) {}
    bool null() override {
        return Scalar(nullptr);
    }
    bool boolean(bool value) override {
        return Scalar(value);
    }
    bool number_integer(number_integer_t value) override {
        return Scalar(value);
    }
    bool number_unsigned(number_unsigned_t value) override {
        return Scalar(value);
    }
    bool number_float(number_float_t value, const string_t&) override {
        return Scalar(value);
    }
    bool string(string_t& value) override {
        return Scalar(value);
    }
    bool binary(binary_t&) override {
        return false;
    }
    bool key(string_t& value) override {
        m_key = value;
        return true;
    }
    bool start_object(std::size_t) override {
        return Start(false);
    }
    bool start_array(std::size_t) override {
        return Start(true);
    }
    bool end_object() override {
        return End();
    }
    bool end_array() override {
        return End();
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }
    [[nodiscard]] const Json& envelope() const {
        return m_envelope;
    }
    [[nodiscard]] std::size_t record_count() const {
        return m_record_count;
    }
    [[nodiscard]] std::size_t component_count() const {
        return m_component_count;
    }
    [[nodiscard]] std::uint64_t previous_id() const {
        return m_previous_id;
    }

private:
    Json* Insert(Json value) {
        if (m_depth == 0) {
            m_envelope = std::move(value);
            return &m_envelope;
        }
        auto* parent = m_stack[m_depth - 1];
        if (parent->is_array()) {
            parent->push_back(std::move(value));
            return &parent->back();
        }
        return &((*parent)[m_key] = std::move(value));
    }
    bool Scalar(Json value) {
        Insert(std::move(value));
        return true;
    }
    bool Start(bool array) {
        if (m_depth == m_stack.size())
            return false;
        auto* value = Insert(array ? Json::array() : Json::object());
        m_stack[m_depth++] = value;
        return true;
    }
    bool End() {
        if (m_depth == 0)
            return false;
        if (m_depth == 3) {
            Ecs::EntitySnapshotRecord record;
            if (!ReadRecord(*m_stack[2], record, m_previous_id, m_component_count, m_aggregate))
                return false;
            ++m_record_count;
            if (m_output)
                m_output->entities.push_back(std::move(record));
            auto* entities = m_stack[1];
            // Always exactly one record: removing the last element is O(1).
            entities->erase(entities->end() - 1);
        }
        --m_depth;
        return true;
    }
    PlantSnapshot* m_output;
    Json m_envelope;
    std::string m_key;
    std::array<Json*, SavedInputLimits::kJsonNestingDepth> m_stack{};
    std::size_t m_depth = 0;
    std::size_t m_record_count = 0;
    std::size_t m_component_count = 0;
    std::size_t m_aggregate = 0;
    std::uint64_t m_previous_id = 0;
};

// Validate all records and envelope metadata before allocating a roster. A
// second pass populates detached storage reserved to the admitted exact count.
bool ParsePlantDocument(std::string_view bytes,
                        PlantSnapshot* output,
                        std::size_t& admitted_count,
                        std::string& error) {
    DocumentAdmission admission(output);
    if (!Json::sax_parse(bytes.begin(), bytes.end(), &admission))
        return Refuse(error);
    const auto& envelope = admission.envelope();
    if (!envelope.contains("schema") || envelope.at("schema") != kPlantSnapshotSchema) {
        error = kUnsupportedPlantSnapshot;
        return false;
    }
    if (!ExactObject(envelope,
                     {"schema",
                      "order_contract",
                      "authority_revision",
                      "next_id",
                      "entity_count",
                      "component_count",
                      "entities"}) ||
        envelope.at("order_contract") != kPlantSnapshotOrder ||
        !UInt<std::uint32_t>(envelope.at("authority_revision")) ||
        envelope.at("authority_revision") != SimulationIdentityValues::kAuthorityRulesRevision ||
        !UInt<std::uint64_t>(envelope.at("next_id")) ||
        !UInt<std::uint32_t>(envelope.at("entity_count")) ||
        !UInt<std::uint32_t>(envelope.at("component_count")) ||
        !envelope.at("entities").is_array() ||
        admission.record_count() > kMaxPlantSnapshotRecords ||
        envelope.at("entity_count") != admission.record_count() ||
        envelope.at("component_count") != admission.component_count())
        return Refuse(error);
    const auto cursor = envelope.at("next_id").get<std::uint64_t>();
    if (cursor == 0 || cursor > world::ActiveRegionLedger::kPersistentIdLimit ||
        admission.previous_id() >= cursor)
        return Refuse(error);
    if (output)
        output->next_id = cursor;
    admitted_count = admission.record_count();
    return true;
}
} // namespace

bool ValidatePlantSnapshot(const PlantSnapshot& snapshot, std::string& error) {
    if (snapshot.next_id == 0 || snapshot.next_id > world::ActiveRegionLedger::kPersistentIdLimit ||
        snapshot.entities.size() > kMaxPlantSnapshotRecords)
        return Refuse(error);
    try {
        std::size_t aggregate = 0;
        // Save preparation only; bounded by 2^20 records, never a tick-path scan.
        std::set<std::uint64_t> ids;
        for (const auto& record : snapshot.entities) {
            if (!ValidRecord(record, snapshot.next_id, aggregate) ||
                !ids.insert(record.entity_id).second)
                return Refuse(error);
        }
        return true;
    } catch (const std::exception&) {
        return Refuse(error);
    }
}

bool ValidatePlantSnapshot(const PlantSnapshot& snapshot,
                           const world::ActiveRegionLedger& ledger,
                           const world::GroundObjectStore& ground,
                           std::string& error) {
    if (!ValidatePlantSnapshot(snapshot, error))
        return false;
    if (snapshot.next_id != ledger.next_persistent_id())
        return Refuse(error);
    // The ground store is separately bounded by GroundObjectStore::kMaxObjects.
    for (const auto& [id, object] : ground.objects()) {
        (void)object;
        if (!simulation::IsValidSimulationId(id) || id >= snapshot.next_id)
            return Refuse(error);
    }
    for (const auto& record : snapshot.entities) {
        if (ground.find(record.entity_id))
            return Refuse(error);
        const auto* owner = Detail::ComponentData(record, "PlantRegion");
        if (!owner)
            continue;
        const world::RegionKey key{owner->at("owner_x").get<std::int32_t>(),
                                   owner->at("owner_z").get<std::int32_t>()};
        const auto* growth = Detail::ComponentData(record, "PlantGrowth");
        if (!growth || !ledger.records().contains(key) ||
            owner->at("region_stamp").get<std::uint64_t>() > ledger.header_tick() ||
            growth->at("last_tick").get<std::uint64_t>() > ledger.header_tick())
            return Refuse(error);
    }
    return true;
}

bool EncodePlantSnapshot(const PlantSnapshot& snapshot,
                         std::string& bytes,
                         std::string& error) try {
    if (!ValidatePlantSnapshot(snapshot, error))
        return false;
    // Pointer scratch is capped at 2^20 records and 17 components per record.
    std::vector<const Ecs::EntitySnapshotRecord*> ordered;
    ordered.reserve(snapshot.entities.size());
    for (const auto& record : snapshot.entities)
        ordered.push_back(&record);
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return left->entity_id < right->entity_id;
    });
    std::size_t component_count = 0;
    for (const auto* record : ordered)
        component_count += record->components.size();
    // Bounded streaming avoids constructing a second roster-sized JSON tree.
    // LimitedJsonBuffer checks remaining capacity before every append/allocation.
    LimitedJsonBuffer buffer(SavedInputLimits::kPlantFileBytes);
    std::ostream stream(&buffer);
    stream.imbue(std::locale::classic());
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream << "{\"authority_revision\":" << SimulationIdentityValues::kAuthorityRulesRevision
           << ",\"component_count\":" << component_count << ",\"entities\":[";
    bool first_record = true;
    for (const auto* record : ordered) {
        if (!first_record)
            stream << ',';
        first_record = false;
        std::vector<const Ecs::EntityComponentSnapshot*> components;
        for (const auto& component : record->components)
            components.push_back(&component);
        std::sort(components.begin(), components.end(), [](const auto* left, const auto* right) {
            return left->type < right->type;
        });
        stream << "{\"components\":[";
        bool first_component = true;
        for (const auto* component : components) {
            if (!first_component)
                stream << ',';
            first_component = false;
            stream << Json{{"type", component->type}, {"version", 1u}, {"data", component->data}};
        }
        stream << "],\"id\":" << record->entity_id << '}';
    }
    stream << "],\"entity_count\":" << ordered.size() << ",\"next_id\":" << snapshot.next_id
           << ",\"order_contract\":\"" << kPlantSnapshotOrder << "\",\"schema\":\""
           << kPlantSnapshotSchema << "\"}";
    bytes = buffer.take_bytes();
    return true;
} catch (const std::exception&) {
    return Refuse(error);
}

bool DecodePlantSnapshot(std::string_view bytes, PlantSnapshot& out, std::string& error) try {
    if (bytes.size() > SavedInputLimits::kPlantFileBytes ||
        !SavedInputLimits::JsonDepthAllowed(bytes) || !AdmitPlantComponentBytes(bytes))
        return Refuse(error);
    ShapeAdmission shape;
    if (!Json::sax_parse(bytes.begin(), bytes.end(), &shape))
        return Refuse(error);
    if (shape.schema() != kPlantSnapshotSchema) {
        error = kUnsupportedPlantSnapshot;
        return false;
    }
    std::size_t admitted_count = 0;
    if (!ParsePlantDocument(bytes, nullptr, admitted_count, error))
        return false;
    PlantSnapshot candidate;
    // max_size checks count * sizeof(record) before proportional allocation,
    // including on 32-bit hosts. The format additionally caps count at 2^20.
    if (admitted_count > candidate.entities.max_size())
        return Refuse(error);
    candidate.entities.reserve(admitted_count);
    if (!ParsePlantDocument(bytes, &candidate, admitted_count, error))
        return false;
    out = std::move(candidate);
    return true;
} catch (const std::exception&) {
    return Refuse(error);
}
} // namespace Luminumbra::Persistence
