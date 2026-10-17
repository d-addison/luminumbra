#include "CreaturePersistence.h"

#include "InstinctPlanner.h"
#include "StimulusChannels.h"
#include "components/AlarmComponents.h"
#include "components/CircadianComponents.h"
#include "components/CombustionComponents.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/DecayComponents.h"
#include "components/FieldEmitterComponents.h"
#include "components/ForagingComponents.h"
#include "components/GrazeableComponent.h"
#include "components/InstinctComponents.h"
#include "components/IrrigationComponents.h"
#include "components/MigratoryComponents.h"
#include "components/MortalComponents.h"
#include "components/PackHunterComponents.h"
#include "components/PlantComponents.h"
#include "components/ScavengerComponent.h"
#include "components/TerritoryComponents.h"
#include "components/ThirstComponents.h"
#include "core/WaterComponents.h"
#include "persistence/PlantPersistence.h"
#include "world/ActiveRegionLedger.h"
#include "world/GroundObjectStore.h"
#include <luminumbra/core/Types.h>

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>
#include <entt/entt.hpp>
#include <nlohmann/detail/exceptions.hpp>
#include <nlohmann/detail/input/json_sax.hpp>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace luminumbra::ai {
namespace {
namespace C = Luminumbra::Components;
using Json = nlohmann::json;
using Identity = std::pair<std::string, std::uint64_t>;
using IdMap = std::map<Identity, entt::entity>;

void Require(bool condition, const char* message) {
    if (!condition)
        throw std::invalid_argument(message);
}
// Credit only compact Controller metadata actually present, including its member key/comma.
// Whitespace and alternate numeric spellings cannot enlarge the fixed stamp allowance.
std::size_t ControllerBytes(const Json& value) {
    if (!value.is_object() || value.size() != 2 || !value.contains("version") ||
        !value.at("version").is_number_integer() || !value.contains("data"))
        return 0;
    const auto& data = value.at("data");
    if (!data.is_object() || data.size() > 1 ||
        (data.size() == 1 &&
         (!data.contains("physics_controlled") || !data.at("physics_controlled").is_boolean())))
        return 0;
    return std::min(kCreatureControllerStampBytes,
                    sizeof(R"(,"Controller":)") - 1 + value.dump().size());
}
// Build the DOM through the public SAX interface, checking budgets before insertion.
// Arrays/objects are never materialized by an unbounded intermediate DOM parser.
class BoundedCreatureJson : public nlohmann::json_sax<Json> {
    struct Frame {
        Json value;
        std::string key;
        std::size_t count = 0;
        std::size_t limit = 0;
        std::size_t controller_nodes = 0;
    };
    std::vector<Frame> frames;
    std::size_t nodes = 0;
    std::size_t record_count = 0;
    bool ControllerMember() const {
        return frames.size() >= 4 && frames[0].value.is_object() && frames[0].key == "entities" &&
               frames[1].value.is_array() && frames[2].value.is_object() &&
               frames[2].key == "components" && frames[3].value.is_object() &&
               frames[3].key == "Controller";
    }
    void Node() {
        if (ControllerMember() && frames[3].controller_nodes < kCreatureControllerStampNodes) {
            ++frames[3].controller_nodes;
            ++controller_nodes;
        }
        Require(++nodes <= CreatureNodeLimit(record_count, controller_nodes),
                "creature node limit");
    }
    void BeginValue() {
        Node();
        if (!frames.empty() && frames.back().value.is_array()) {
            Require(++frames.back().count <= frames.back().limit, "creature array count limit");
            if (frames.size() == 2 && frames[0].value.is_object() && frames[0].key == "entities")
                record_count = frames.back().count;
        }
    }
    void Attach(Json value) {
        if (frames.size() == 4 && ControllerMember())
            controller_bytes += ControllerBytes(value);
        if (frames.empty())
            result = std::move(value);
        else if (frames.back().value.is_array())
            frames.back().value.push_back(std::move(value));
        else
            frames.back().value[frames.back().key] = std::move(value);
    }
    bool Value(Json value) {
        BeginValue();
        Attach(std::move(value));
        return true;
    }
    bool Start(bool object, std::size_t count) {
        BeginValue();
        Require(frames.size() < 32, "creature nesting limit");
        const auto limit = object ? kMaxCreatureObjectKeys
                                  : (frames.size() == 1 && frames.back().value.is_object() &&
                                             frames.back().key == "entities"
                                         ? kMaxCreatures
                                         : kMaxCreatureVector);
        Require(count == std::size_t(-1) || count <= limit, "creature collection limit");
        frames.push_back({object ? Json::object() : Json::array(), {}, 0, limit});
        return true;
    }
    bool End() {
        Json value = std::move(frames.back().value);
        frames.pop_back();
        Attach(std::move(value));
        return true;
    }

    Json result;
    std::size_t controller_bytes = 0;
    std::size_t controller_nodes = 0;

public:
    Json& Result() {
        return result;
    }
    std::size_t ControllerByteCount() const {
        return controller_bytes;
    }
    bool null() override {
        return Value(nullptr);
    }
    bool boolean(bool value) override {
        return Value(value);
    }
    bool number_integer(Json::number_integer_t value) override {
        return Value(value);
    }
    bool number_unsigned(Json::number_unsigned_t value) override {
        return Value(value);
    }
    bool number_float(Json::number_float_t value, const Json::string_t&) override {
        return Value(value);
    }
    bool string(Json::string_t& value) override {
        Require(value.size() <= kMaxCreatureString, "creature string limit");
        return Value(std::move(value));
    }
    bool binary(Json::binary_t&) override {
        return false;
    }
    bool start_object(std::size_t count) override {
        return Start(true, count);
    }
    bool key(Json::string_t& value) override {
        auto& frame = frames.back();
        Require(++frame.count <= frame.limit, "creature object key limit");
        Require(value.size() <= kMaxCreatureString, "creature string limit");
        Require(!frame.value.contains(value), "duplicate creature key");
        frame.key = std::move(value);
        Node();
        return true;
    }
    bool end_object() override {
        return End();
    }
    bool start_array(std::size_t count) override {
        return Start(false, count);
    }
    bool end_array() override {
        return End();
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }
};

// Apply the same budgets to writer input without parsing or allocating a second DOM.
void CheckJsonBudgets(const Json& document) {
    std::size_t nodes = 0, controller_nodes = 0, stamp_nodes = 0;
    const auto count = document.at("entities").size();
    const auto node = [&](bool controller) {
        if (controller && stamp_nodes < kCreatureControllerStampNodes) {
            ++stamp_nodes;
            ++controller_nodes;
        }
        Require(++nodes <= CreatureNodeLimit(count, controller_nodes), "creature node limit");
    };
    struct Frame {
        const Json* value = nullptr;
        Json::const_iterator next{};
        bool entities = false, controller = false, entered = false;
    };
    // At most 32 structured levels, plus a scalar leaf at depth 32.
    std::array<Frame, 33> frames{};
    frames[0].value = &document;
    std::size_t depth = 0;
    for (;;) {
        auto& frame = frames[depth];
        const auto& value = *frame.value;
        if (!frame.entered) {
            node(frame.controller);
            frame.entered = true;
            if (value.is_structured()) {
                Require(depth < 32, "creature nesting limit");
                if (value.is_object())
                    Require(value.size() <= kMaxCreatureObjectKeys, "creature object key limit");
                else
                    Require(value.size() <= (frame.entities ? kMaxCreatures : kMaxCreatureVector),
                            "creature array count limit");
                frame.next = value.begin();
            } else {
                if (value.is_string())
                    Require(value.get_ref<const std::string&>().size() <= kMaxCreatureString,
                            "creature string limit");
                if (depth == 0)
                    break;
                --depth;
                continue;
            }
        }
        if (frame.next == value.end()) {
            if (depth == 0)
                break;
            --depth;
            continue;
        }
        const auto it = frame.next++;
        bool controller = frame.controller;
        if (value.is_object()) {
            if (depth == 3 && it.key() == "Controller") {
                stamp_nodes = 0;
                controller = true;
            }
            node(controller);
            Require(it.key().size() <= kMaxCreatureString, "creature string limit");
        }
        const bool entities = depth == 0 && value.is_object() && it.key() == "entities";
        frames[++depth] = {.value = &it.value(),
                           .next = {},
                           .entities = entities,
                           .controller = controller,
                           .entered = false};
    }
}

void Keys(const Json& j, std::initializer_list<const char*> keys) {
    Require(j.is_object() && j.size() == keys.size(), "unknown or missing creature key");
    for (const auto* key : keys)
        Require(j.contains(key), "missing creature key");
}

template<class T>
void Scalar(const Json& j, T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        Require(j.is_boolean(), "invalid creature boolean");
    } else if constexpr (std::is_unsigned_v<T>) {
        Require(j.is_number_integer() && (j.is_number_unsigned() || j.get<std::int64_t>() >= 0) &&
                    j.get<std::uint64_t>() <= std::numeric_limits<T>::max(),
                "invalid creature unsigned integer");
    } else {
        Require(j.is_number_integer() &&
                    (!j.is_number_unsigned() ||
                     j.get<std::uint64_t>() <=
                         static_cast<std::uint64_t>(std::numeric_limits<T>::max())) &&
                    j.get<std::int64_t>() >= std::numeric_limits<T>::min() &&
                    j.get<std::int64_t>() <= std::numeric_limits<T>::max(),
                "invalid creature signed integer");
    }
    value = j.get<T>();
}
struct Context {
    const entt::registry* registry = nullptr;
    const IdMap* ids = nullptr;
};
Identity DurableReference(const entt::registry& registry, entt::entity e) {
    if (e == entt::null)
        return {"null", 0};
    Require(registry.valid(e), "dangling creature reference");
    if (const auto* owner = registry.try_get<C::CreatureRegionComponent>(e)) {
        Require(registry.all_of<C::CreatureComponent>(e) && owner->persistent_id > 0 &&
                    owner->persistent_id < (1ull << 63),
                "wrong-kind creature reference");
        return {"creature", owner->persistent_id};
    }
    if (const auto* owner = registry.try_get<C::PlantRegionComponent>(e)) {
        Require(registry.all_of<C::PlantTag>(e) && owner->persistent_id > 0 &&
                    owner->persistent_id < (1ull << 63),
                "wrong-kind plant reference");
        return {"plant", owner->persistent_id};
    }
    throw std::invalid_argument("creature reference has no supported durable identity");
}

template<class T>
Json Encode(T value, const Context& context);
template<class T>
void Decode(const Json& j, T& value, const Context& context);

class Archive {
    Json& data;
    bool reading;
    const Context& context;
    std::size_t fields = 0;

public:
    Archive(Json& value, bool read, const Context& references)
        : data(value)
        , reading(read)
        , context(references) {}
    bool IsReading() const {
        return reading;
    }
    bool Contains(const char* key) const {
        return data.contains(key);
    }
    template<class T>
    void operator()(const char* name, T& value) {
        const std::string key = std::string(name) + (std::is_floating_point_v<T> ? "_bits" : "");
        ++fields;
        if (reading)
            Decode(data.at(key), value, context);
        else
            data[key] = Encode(value, context);
    }
    void ref(const char* name, entt::entity& value) {
        ++fields;
        if (!reading) {
            const auto [kind, id] = DurableReference(*context.registry, value);
            data[name] = {{"kind", kind}, {"id", id}};
            return;
        }
        const auto& j = data.at(name);
        Keys(j, {"kind", "id"});
        Require(j.at("kind").is_string(), "invalid creature reference kind");
        std::uint64_t id = 0;
        Scalar(j.at("id"), id);
        const auto kind = j.at("kind").get<std::string>();
        if (kind == "null") {
            Require(id == 0, "invalid null creature reference");
            value = entt::null;
        } else {
            Require(id != 0 && (kind == "creature" || kind == "plant"),
                    "wrong-kind creature reference");
            const auto it = context.ids->find({kind, id});
            Require(it != context.ids->end(), "dangling or wrong-kind creature reference");
            value = it->second;
        }
    }
    void finish() {
        Require(data.is_object() && data.size() == fields, "unknown creature payload key");
    }
};
void Fields(Archive& a, C::CreatureCoarseNeed& c) {
    a("name", c.name);
    a("pressure_ppm", c.pressure_ppm);
}

void Fields(Archive& a, C::Need& c) {
    a("name", c.name);
    a("pressure", c.pressure);
    a("growth_per_tick", c.growth_per_tick);
}

void Fields(Archive& a, C::Action& c) {
    a("name", c.name);
    a.ref("target", c.target);
    a("duration", c.duration);
    a("flee", c.flee);
}

void Fields(Archive& a, C::StimulusSubscription& c) {
    a("channel", c.channel);
    a("need", c.need);
    a("gain", c.gain);
}

void Fields(Archive& a, Luminumbra::Vec2& c) {
    a("x", c.x);
    a("y", c.y);
}

void Fields(Archive& a, InstinctCandidate& c) {
    a("rank", c.rank);
    a("id", c.id);
    a("action", c.action);
    a("target", c.target);
    a("need", c.need);
    a("score", c.score);
    a("need_pressure", c.need_pressure);
    a("reason", c.reason);
}

void Fields(Archive& a, InstinctPlan& c) {
    a("actor_id", c.actor_id);
    a("archetype", c.archetype);
    a("decision_contract", c.decision_contract);
    a("candidates", c.candidates);
    a("selected_index", c.selected_index);
    a("checksum", c.checksum);
    a("passed", c.passed);
}

void Fields(Archive& a, C::TransformComponent& c) {
    a("position_x", c.position.x);
    a("position_y", c.position.y);
    a("position_z", c.position.z);
    a("rotation_w", c.rotation.w);
    a("rotation_x", c.rotation.x);
    a("rotation_y", c.rotation.y);
    a("rotation_z", c.rotation.z);
    a("scale_x", c.scale.x);
    a("scale_y", c.scale.y);
    a("scale_z", c.scale.z);
}

void Fields(Archive& a, C::CreatureRegionComponent& c) {
    a("persistent_id", c.persistent_id);
    a("owner_x", c.owner_x);
    a("owner_z", c.owner_z);
    a("region_stamp", c.region_stamp);
}

void Fields(Archive& a, C::CreatureCoarseComponent& c) {
    a("tier", c.tier);
    a("transition", c.transition);
    a("x_mm", c.x_mm);
    a("y_mm", c.y_mm);
    a("z_mm", c.z_mm);
    a("hunger_ppm", c.hunger_ppm);
    a("thirst_ppm", c.thirst_ppm);
    a("needs", c.needs);
    a("age_cadences", c.age_cadences);
    a("reproduce_cadences", c.reproduce_cadences);
    a("reproduce_cooldown_cadences", c.reproduce_cooldown_cadences);
    a("courting_cadences", c.courting_cadences);
    a("travel_pending", c.travel_pending);
    a("destination_x", c.destination_x);
    a("destination_z", c.destination_z);
}

void Fields(Archive& a, C::CreatureControllerComponent& c) {
    // Legacy v1 records carried no controller participation metadata.
    if (!a.IsReading() || a.Contains("physics_controlled"))
        a("physics_controlled", c.physics_controlled);
}

void Fields(Archive& a, C::CreatureLocomotionStateComponent& c) {
    a("velocity_x", c.velocity_x);
    a("velocity_y", c.velocity_y);
    a("velocity_z", c.velocity_z);
    a("grounded", c.grounded);
}

void Fields(Archive& a, C::CreatureComponent& c) {
    a("is_predator", c.is_predator);
    a("species_id", c.species_id);
    a("hunger", c.hunger);
    a("stamina", c.stamina);
    a("energy", c.energy);
    a("move_speed", c.move_speed);
    a("last_action", c.last_action);
    a("wish_x", c.wish_x);
    a("wish_z", c.wish_z);
    a("eaten", c.eaten);
}

void Fields(Archive& a, C::CreatureGenomeComponent& c) {
    a("move_speed", c.move_speed);
    a("vigilance", c.vigilance);
    a("hunger_threshold", c.hunger_threshold);
    a("size_scale", c.size_scale);
    a("vision_cos_half_fov", c.vision_cos_half_fov);
    a("vision_range", c.vision_range);
    a("hearing_range", c.hearing_range);
    a("age_ticks", c.age_ticks);
    a("reproduce_cooldown", c.reproduce_cooldown);
    a("generation", c.generation);
    a("female", c.female);
    a("courting_ticks", c.courting_ticks);
}

void Fields(Archive& a, C::MortalComponent& c) {
    a("age_ticks", c.age_ticks);
    a("lifespan_ticks", c.lifespan_ticks);
    a("dead", c.dead);
}

void Fields(Archive& a, C::DecayComponent& c) {
    a("decay_ticks", c.decay_ticks);
    a("decay_duration", c.decay_duration);
    a("nutrient_released_milli", c.nutrient_released_milli);
    a("fully_decomposed", c.fully_decomposed);
}

void Fields(Archive& a, C::ThirstComponent& c) {
    a("thirst", c.thirst);
    a("wish_x", c.wish_x);
    a("wish_z", c.wish_z);
    a("drinking", c.drinking);
    a("water_proximity", c.water_proximity);
}

void Fields(Archive& a, C::CircadianComponent& c) {
    a("nocturnal", c.nocturnal);
    a("activity", c.activity);
}

void Fields(Archive& a, C::AlarmComponent& c) {
    a("level", c.level);
    a("alarmed", c.alarmed);
}

void Fields(Archive& a, C::PackHunterComponent& c) {
    a("coord_x", c.coord_x);
    a("coord_z", c.coord_z);
    a("in_pack", c.in_pack);
}

void Fields(Archive& a, C::TerritoryComponent& c) {
    a("home_x", c.home_x);
    a("home_z", c.home_z);
    a("radius", c.radius);
    a("established", c.established);
}

void Fields(Archive& a, C::TerritoryBiasComponent& c) {
    a("wish_x", c.wish_x);
    a("wish_z", c.wish_z);
}

void Fields(Archive& a, C::MigratoryComponent& c) {
    a("drive", c.drive);
    a("wish_x", c.wish_x);
    a("wish_z", c.wish_z);
}

void Fields(Archive& a, C::ScavengerComponent& c) {
    a("wish_x", c.wish_x);
    a("wish_z", c.wish_z);
    a("feeding", c.feeding);
}

void Fields(Archive& a, C::NeedsComponent& c) {
    a("needs", c.needs);
}

void Fields(Archive& a, C::SensesComponent& c) {
    a.ref("nearest_food_source", c.nearest_food_source);
    a.ref("nearest_water_source", c.nearest_water_source);
    a.ref("nearest_threat", c.nearest_threat);
    a("threat_distance", c.threat_distance);
}

void Fields(Archive& a, C::OpportunityComponent& c) {
    a("id", c.id);
    a("action", c.action);
    a("target", c.target);
    a("need", c.need);
    a("satisfaction", c.satisfaction);
    a("urgency", c.urgency);
    a("risk", c.risk);
    a("stamina_cost", c.stamina_cost);
    a("radius", c.radius);
}

void Fields(Archive& a, C::ActionPlanComponent& c) {
    a("plan", c.plan);
    a("current_action_index", c.current_action_index);
    a("time_in_current_action", c.time_in_current_action);
}

void Fields(Archive& a, C::LocomotionProfile& c) {
    a("move_speed", c.move_speed);
    a("arrival_radius", c.arrival_radius);
    a("slow_radius", c.slow_radius);
    a("separation_radius", c.separation_radius);
    a("separation_strength", c.separation_strength);
    a("flock_radius", c.flock_radius);
    a("cohesion_strength", c.cohesion_strength);
    a("alignment_strength", c.alignment_strength);
}

void Fields(Archive& a, C::SensableComponent& c) {
    a("scent_channel", c.scent_channel);
    a("scent_deposit", c.scent_deposit);
    a("noise_loudness", c.noise_loudness);
    a("noise_pitch", c.noise_pitch);
    a("faction", c.faction);
}

void Fields(Archive& a, C::PerceptionComponent& c) {
    a("vision_cos_half_fov", c.vision_cos_half_fov);
    a("vision_range", c.vision_range);
    a("facing_x", c.facing_x);
    a("facing_z", c.facing_z);
    a("ear_range", c.ear.range);
    a("ear_peak_pitch", c.ear.peak_pitch);
    a("ear_bandwidth", c.ear.bandwidth);
    a("ear_gain", c.ear.gain);
    a("ear_threshold", c.ear.threshold);
    a("faction", c.faction);
}

void Fields(Archive& a, C::AwarenessComponent& c) {
    a("awareness_meter", c.awareness.meter);
    a("awareness_state", c.awareness.state);
    a("awareness_has_last_known", c.awareness.has_last_known);
    a("awareness_last_known_x", c.awareness.last_known_x);
    a("awareness_last_known_z", c.awareness.last_known_z);
    a("awareness_time_since_signal", c.awareness.time_since_signal);
    a("params_gain", c.params.gain);
    a("params_decay", c.params.decay);
    a("params_suspicious_at", c.params.suspicious_at);
    a("params_alert_at", c.params.alert_at);
    a("params_engaged_at", c.params.engaged_at);
}

void Fields(Archive& a, C::LocomotionIntentComponent& c) {
    a("wish_xz_x", c.wish_xz.x);
    a("wish_xz_y", c.wish_xz.y);
    a("arrived", c.arrived);
}

void Fields(Archive& a, C::ScentSenseComponent& c) {
    a("channel", c.channel);
    a("sign", c.sign);
    a("strength", c.strength);
    a("floor", c.floor);
    a("weber_k", c.weber_k);
}

void Fields(Archive& a, C::LocomotionPathComponent& c) {
    a("waypoints", c.waypoints);
    a("index", c.index);
}

void Fields(Archive& a, C::StimulusSubscriptionComponent& c) {
    a("subscriptions", c.subscriptions);
}

void Fields(Archive& a, C::InstinctAgentComponent& c) {
    a("actor_id", c.actor_id);
    a("archetype", c.archetype);
    a("replan_interval_ticks", c.replan_interval_ticks);
    a("last_planned_tick", c.last_planned_tick);
    a("plans_executed", c.plans_executed);
    a("current_plan", c.current_plan);
}

void Fields(Archive& a, C::ForagerComponent& c) {
    a("cell_x", c.cell_x);
    a("cell_z", c.cell_z);
    a("home_x", c.home_x);
    a("home_z", c.home_z);
    a("carrying_food", c.carrying_food);
    a("deliveries", c.deliveries);
}

void Fields(Archive& a, C::FoodSourceComponent& c) {
    a("cell_x", c.cell_x);
    a("cell_z", c.cell_z);
    a("amount", c.amount);
}

void Fields(Archive& a, C::FieldEmitterComponent& c) {
    a("channel", c.channel);
    a("rate_raw_per_tick", c.rate_raw_per_tick);
    a("radius_cells", c.radius_cells);
}

void Fields(Archive& a, C::GrazeableComponent& c) {
    a("biomass", c.biomass);
    a("last_grazed_tick", c.last_grazed_tick);
}

void Fields(Archive& a, C::WaterHoleComponent& c) {
    a("radius", c.radius);
}

void Fields(Archive& a, C::CombustibleComponent& c) {
    a("fuel_milli", c.fuel_milli);
    a("moisture_milli", c.moisture_milli);
    a("burn_state", c.burn_state);
    a("burn_ticks_remaining", c.burn_ticks_remaining);
    a("ignition_radius", c.ignition_radius);
}

void Fields(Archive& a, C::IgnitionSourceComponent& c) {
    a("radius", c.radius);
    a("intensity", c.intensity);
}

void Fields(Archive& a, C::WaterSourceComponent& c) {
    a("output", c.output);
}

void Fields(Archive& a, C::WaterFlowSourceComponent& c) {
    a("flow_rate", c.flow_rate);
}
template<class T>
struct IsVector : std::false_type {};
template<class T, class A>
struct IsVector<std::vector<T, A>> : std::true_type {};

template<class T>
Json Encode(T value, const Context& context) {
    if constexpr (std::is_floating_point_v<T>) {
        Require(std::isfinite(value), "non-finite creature scalar");
        using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
        return std::bit_cast<Bits>(value);
    } else if constexpr (std::is_enum_v<T>) {
        return static_cast<std::underlying_type_t<T>>(value);
    } else if constexpr (std::is_integral_v<T>) {
        return value;
    } else if constexpr (std::is_same_v<T, std::string>) {
        Require(value.size() <= kMaxCreatureString, "creature string limit");
        return value;
    } else if constexpr (IsVector<T>::value) {
        Require(value.size() <= kMaxCreatureVector, "creature vector limit");
        Json j = Json::array();
        for (const auto& item : value)
            j.push_back(Encode(item, context));
        return j;
    } else {
        Json j = Json::object();
        Archive a{j, false, context};
        Fields(a, value);
        a.finish();
        return j;
    }
}
template<class T>
void Decode(const Json& j, T& value, const Context& context) {
    if constexpr (std::is_floating_point_v<T>) {
        using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
        Bits bits{};
        Scalar(j, bits);
        value = std::bit_cast<T>(bits);
        Require(std::isfinite(value), "non-finite creature scalar");
    } else if constexpr (std::is_enum_v<T>) {
        std::underlying_type_t<T> raw{};
        Scalar(j, raw);
        value = static_cast<T>(raw);
    } else if constexpr (std::is_integral_v<T>) {
        Scalar(j, value);
    } else if constexpr (std::is_same_v<T, std::string>) {
        Require(j.is_string(), "invalid creature string");
        value = j.get<std::string>();
        Require(value.size() <= kMaxCreatureString, "creature string limit");
    } else if constexpr (IsVector<T>::value) {
        Require(j.is_array() && j.size() <= kMaxCreatureVector, "creature vector limit");
        value.clear();
        for (const auto& item : j) {
            typename T::value_type decoded{};
            Decode(item, decoded, context);
            value.push_back(std::move(decoded));
        }
    } else {
        Json data = j;
        Require(data.is_object(), "invalid creature component payload");
        Archive a{data, true, context};
        Fields(a, value);
        a.finish();
    }
}
void Normal(float v) {
    Require(v >= 0 && v <= 1, "creature normalized scalar outside domain");
}
void Nonnegative(float v) {
    Require(v >= 0, "negative creature scalar");
}

void CheckComponents(const entt::registry& r, entt::entity e) {
    const auto& cr = r.get<C::CreatureComponent>(e);
    Normal(cr.hunger);
    Normal(cr.stamina);
    Normal(cr.energy);
    Nonnegative(cr.move_speed);
    Require(cr.species_id != 0 && cr.last_action >= 0 && cr.last_action <= 6,
            "invalid creature species or action");
    const auto& tf = r.get<C::TransformComponent>(e);
    Require(Luminumbra::world::ActiveRegionLedger::valid_anchor(tf.position) &&
                tf.position.y >= -1048576 && tf.position.y < 1048576,
            "invalid creature position");
    const auto& owner = r.get<C::CreatureRegionComponent>(e);
    const Luminumbra::world::RegionKey key{owner.owner_x, owner.owner_z};
    Require(owner.owner_x >= -32768 && owner.owner_x < 32768 && owner.owner_z >= -32768 &&
                owner.owner_z < 32768 &&
                Luminumbra::world::ActiveRegionLedger::region_at(tf.position) == key,
            "invalid creature owner");
    const auto& coarse = r.get<C::CreatureCoarseComponent>(e);
    Require(static_cast<unsigned>(coarse.tier) <= 1 &&
                static_cast<unsigned>(coarse.transition) <= 2 && coarse.hunger_ppm <= 1000000 &&
                coarse.thirst_ppm <= 1000000 && coarse.x_mm >= -16777216000ll &&
                coarse.x_mm < 16777216000ll && coarse.z_mm >= -16777216000ll &&
                coarse.z_mm < 16777216000ll && coarse.y_mm >= -1048576000ll &&
                coarse.y_mm < 1048576000ll && coarse.destination_x >= -32768 &&
                coarse.destination_x < 32768 && coarse.destination_z >= -32768 &&
                coarse.destination_z < 32768,
            "invalid creature coarse state");
    std::set<std::string> coarse_needs;
    for (const auto& need : coarse.needs)
        Require(!need.name.empty() && need.pressure_ppm <= 1000000 &&
                    coarse_needs.insert(need.name).second,
                "invalid creature coarse need");
    if (coarse.tier == C::CreatureTier::Coarse) {
        const auto floor_region = [](std::int64_t mm) {
            auto q = mm / 512000;
            if (mm < 0 && mm % 512000)
                --q;
            return q;
        };
        Require(floor_region(coarse.x_mm) == owner.owner_x &&
                    floor_region(coarse.z_mm) == owner.owner_z,
                "invalid coarse creature owner");
    }
    const auto flag = [](std::uint8_t v) {
        Require(v <= 1, "invalid creature flag");
    };
    if (const auto* c = r.try_get<C::CreatureGenomeComponent>(e)) {
        Nonnegative(c->move_speed);
        Normal(c->vigilance);
        Normal(c->hunger_threshold);
        Nonnegative(c->size_scale);
        Nonnegative(c->vision_range);
        Nonnegative(c->hearing_range);
        Require(c->vision_cos_half_fov >= -1 && c->vision_cos_half_fov <= 1,
                "invalid creature FOV");
    }
    if (const auto* c = r.try_get<C::MortalComponent>(e))
        flag(c->dead);
    if (const auto* c = r.try_get<C::DecayComponent>(e)) {
        flag(c->fully_decomposed);
        Require(c->decay_ticks <= c->decay_duration && c->nutrient_released_milli <= 1000,
                "invalid creature decay");
    }
    if (const auto* c = r.try_get<C::ThirstComponent>(e)) {
        Normal(c->thirst);
        Normal(c->water_proximity);
        flag(c->drinking);
    }
    if (const auto* c = r.try_get<C::CircadianComponent>(e)) {
        Normal(c->activity);
    }
    if (const auto* c = r.try_get<C::AlarmComponent>(e)) {
        flag(c->alarmed);
        Normal(c->level);
    }
    if (const auto* c = r.try_get<C::PackHunterComponent>(e))
        flag(c->in_pack);
    if (const auto* c = r.try_get<C::TerritoryComponent>(e)) {
        flag(c->established);
        Nonnegative(c->radius);
    }
    if (const auto* c = r.try_get<C::MigratoryComponent>(e))
        Normal(c->drive);
    if (const auto* c = r.try_get<C::ScavengerComponent>(e))
        flag(c->feeding);
    if (const auto* c = r.try_get<C::NeedsComponent>(e)) {
        std::set<std::string> names;
        for (const auto& need : c->needs) {
            Require(!need.name.empty() && names.insert(need.name).second,
                    "invalid creature need name");
            Normal(need.pressure);
            Nonnegative(need.growth_per_tick);
        }
    }
    if (const auto* c = r.try_get<C::ActionPlanComponent>(e)) {
        Require(c->current_action_index <= c->plan.size(), "invalid creature action index");
        Nonnegative(c->time_in_current_action);
        for (const auto& action : c->plan)
            Nonnegative(action.duration);
    }
    if (const auto* c = r.try_get<C::LocomotionPathComponent>(e))
        Require(c->index <= c->waypoints.size(), "invalid creature path index");
    if (const auto* c = r.try_get<C::StimulusSubscriptionComponent>(e))
        for (const auto& sub : c->subscriptions)
            Require(static_cast<unsigned>(sub.channel) < kStimulusChannelCount,
                    "invalid creature stimulus channel");
    if (const auto* c = r.try_get<C::AwarenessComponent>(e)) {
        Normal(c->awareness.meter);
        Nonnegative(c->awareness.time_since_signal);
        Require(static_cast<unsigned>(c->awareness.state) <= 4, "invalid creature awareness state");
        Nonnegative(c->params.gain);
        Nonnegative(c->params.decay);
        Normal(c->params.suspicious_at);
        Normal(c->params.alert_at);
        Normal(c->params.engaged_at);
    }
    if (const auto* c = r.try_get<C::InstinctAgentComponent>(e))
        Require(c->current_plan.selected_index >= -1 &&
                    (c->current_plan.selected_index == -1 ||
                     static_cast<std::size_t>(c->current_plan.selected_index) <
                         c->current_plan.candidates.size()),
                "invalid creature ranking index");
    if (const auto* c = r.try_get<C::FieldEmitterComponent>(e))
        Require(c->channel >= 0 && c->radius_cells >= 0 &&
                    c->radius_cells <= kMaxCreatureEmitterRadius,
                "invalid creature emitter");
    if (const auto* c = r.try_get<C::GrazeableComponent>(e))
        Normal(c->biomass);
    if (const auto* c = r.try_get<C::WaterHoleComponent>(e))
        Nonnegative(c->radius);
    if (const auto* c = r.try_get<C::CombustibleComponent>(e)) {
        Require(c->fuel_milli <= 1000 && c->moisture_milli <= 1000 && c->burn_state <= 2,
                "invalid creature combustion");
        Nonnegative(c->ignition_radius);
    }
    if (const auto* c = r.try_get<C::IgnitionSourceComponent>(e)) {
        Nonnegative(c->radius);
        Normal(c->intensity);
    }
    if (const auto* c = r.try_get<C::WaterFlowSourceComponent>(e))
        Nonnegative(c->flow_rate);
    if (const auto* c = r.try_get<C::PerceptionComponent>(e)) {
        Require(c->vision_cos_half_fov >= -1 && c->vision_cos_half_fov <= 1,
                "invalid creature FOV");
        Nonnegative(c->vision_range);
        Nonnegative(c->ear.range);
        Normal(c->ear.peak_pitch);
        Nonnegative(c->ear.bandwidth);
        Nonnegative(c->ear.gain);
        Nonnegative(c->ear.threshold);
    }
    if (const auto* c = r.try_get<C::LocomotionProfile>(e)) {
        Nonnegative(c->move_speed);
        Nonnegative(c->arrival_radius);
        Nonnegative(c->slow_radius);
        Nonnegative(c->separation_radius);
        Nonnegative(c->separation_strength);
        Nonnegative(c->flock_radius);
        Nonnegative(c->cohesion_strength);
        Nonnegative(c->alignment_strength);
    }
    if (const auto* c = r.try_get<C::SensableComponent>(e)) {
        Require(c->scent_channel >= -1, "invalid creature scent channel");
        Nonnegative(c->scent_deposit);
        Nonnegative(c->noise_loudness);
        Normal(c->noise_pitch);
    }
    if (const auto* c = r.try_get<C::ScentSenseComponent>(e)) {
        Require(c->channel >= -1 && (c->sign == -1 || c->sign == 1),
                "invalid creature scent sense");
        Nonnegative(c->strength);
        Nonnegative(c->floor);
        Nonnegative(c->weber_k);
    }
}

void CheckController(const entt::registry& r, entt::entity e) {
    Require(!r.get<C::CreatureControllerComponent>(e).physics_controlled ||
                r.all_of<C::CreatureLocomotionStateComponent>(e),
            "missing creature controller continuation state");
}

void CheckRecord(const CreatureRecord& record) {
    Require(record.id > 0 && record.id < (1ull << 63), "invalid creature identity");
    Require(record.components.is_object(), "invalid creature component inventory");
    for (const auto* type : {"Transform", "Creature", "Region", "Coarse"})
        Require(record.components.contains(type), "incomplete creature record");
    Require(record.components.at("Region").at("data").at("persistent_id") == record.id,
            "creature identity mismatch");
}

void AdoptLegacyController(CreatureRecord& record) {
    if (!record.components.contains("Controller"))
        record.components["Controller"] = {{"version", 1},
                                           {"data", {{"physics_controlled", false}}}};
    else {
        auto& controller = record.components.at("Controller");
        if (controller.is_object() && controller.contains("data") &&
            controller.at("data").is_object() &&
            !controller.at("data").contains("physics_controlled"))
            controller.at("data")["physics_controlled"] = false;
    }
}

void DecodeComponents(entt::registry& registry,
                      entt::entity entity,
                      const CreatureRecord& record,
                      const Context& context) {
    CheckRecord(record);
    if (!record.components.contains("Controller"))
        registry.emplace<C::CreatureControllerComponent>(entity);
    for (auto it = record.components.begin(); it != record.components.end(); ++it) {
        Keys(it.value(), {"version", "data"});
        std::uint32_t version = 0;
        Scalar(it.value().at("version"), version);
        Require(version == 1, "unsupported creature component version");
        const auto& data = it.value().at("data");
        if (it.key() == "Transform")
            Decode(data, registry.emplace<C::TransformComponent>(entity), context);
        else if (it.key() == "Region")
            Decode(data, registry.emplace<C::CreatureRegionComponent>(entity), context);
        else if (it.key() == "Coarse")
            Decode(data, registry.emplace<C::CreatureCoarseComponent>(entity), context);
        else if (it.key() == "Controller")
            Decode(data, registry.emplace<C::CreatureControllerComponent>(entity), context);
        else if (it.key() == "LocomotionState")
            Decode(data, registry.emplace<C::CreatureLocomotionStateComponent>(entity), context);
        else if (it.key() == "Creature")
            Decode(data, registry.emplace<C::CreatureComponent>(entity), context);
        else if (it.key() == "Genome")
            Decode(data, registry.emplace<C::CreatureGenomeComponent>(entity), context);
        else if (it.key() == "Mortal")
            Decode(data, registry.emplace<C::MortalComponent>(entity), context);
        else if (it.key() == "Decay")
            Decode(data, registry.emplace<C::DecayComponent>(entity), context);
        else if (it.key() == "Thirst")
            Decode(data, registry.emplace<C::ThirstComponent>(entity), context);
        else if (it.key() == "Circadian")
            Decode(data, registry.emplace<C::CircadianComponent>(entity), context);
        else if (it.key() == "Alarm")
            Decode(data, registry.emplace<C::AlarmComponent>(entity), context);
        else if (it.key() == "PackHunter")
            Decode(data, registry.emplace<C::PackHunterComponent>(entity), context);
        else if (it.key() == "Territory")
            Decode(data, registry.emplace<C::TerritoryComponent>(entity), context);
        else if (it.key() == "TerritoryBias")
            Decode(data, registry.emplace<C::TerritoryBiasComponent>(entity), context);
        else if (it.key() == "Migratory")
            Decode(data, registry.emplace<C::MigratoryComponent>(entity), context);
        else if (it.key() == "Scavenger")
            Decode(data, registry.emplace<C::ScavengerComponent>(entity), context);
        else if (it.key() == "Needs")
            Decode(data, registry.emplace<C::NeedsComponent>(entity), context);
        else if (it.key() == "Senses")
            Decode(data, registry.emplace<C::SensesComponent>(entity), context);
        else if (it.key() == "Opportunity")
            Decode(data, registry.emplace<C::OpportunityComponent>(entity), context);
        else if (it.key() == "ActionPlan")
            Decode(data, registry.emplace<C::ActionPlanComponent>(entity), context);
        else if (it.key() == "InstinctAgent") {
            Keys(data, {});
            registry.emplace<C::InstinctAgent>(entity);
        } else if (it.key() == "LocomotionProfile")
            Decode(data, registry.emplace<C::LocomotionProfile>(entity), context);
        else if (it.key() == "Sensable")
            Decode(data, registry.emplace<C::SensableComponent>(entity), context);
        else if (it.key() == "Perception")
            Decode(data, registry.emplace<C::PerceptionComponent>(entity), context);
        else if (it.key() == "Awareness")
            Decode(data, registry.emplace<C::AwarenessComponent>(entity), context);
        else if (it.key() == "LocomotionIntent")
            Decode(data, registry.emplace<C::LocomotionIntentComponent>(entity), context);
        else if (it.key() == "ScentSense")
            Decode(data, registry.emplace<C::ScentSenseComponent>(entity), context);
        else if (it.key() == "LocomotionPath")
            Decode(data, registry.emplace<C::LocomotionPathComponent>(entity), context);
        else if (it.key() == "StimulusSubscription")
            Decode(data, registry.emplace<C::StimulusSubscriptionComponent>(entity), context);
        else if (it.key() == "InstinctAgentState")
            Decode(data, registry.emplace<C::InstinctAgentComponent>(entity), context);
        else if (it.key() == "Forager")
            Decode(data, registry.emplace<C::ForagerComponent>(entity), context);
        else if (it.key() == "FoodSource")
            Decode(data, registry.emplace<C::FoodSourceComponent>(entity), context);
        else if (it.key() == "FieldEmitter")
            Decode(data, registry.emplace<C::FieldEmitterComponent>(entity), context);
        else if (it.key() == "PendingDeletion") {
            Keys(data, {});
            registry.emplace<C::PendingDeletion>(entity);
        } else if (it.key() == "Grazeable")
            Decode(data, registry.emplace<C::GrazeableComponent>(entity), context);
        else if (it.key() == "WaterHole")
            Decode(data, registry.emplace<C::WaterHoleComponent>(entity), context);
        else if (it.key() == "Combustible")
            Decode(data, registry.emplace<C::CombustibleComponent>(entity), context);
        else if (it.key() == "IgnitionSource")
            Decode(data, registry.emplace<C::IgnitionSourceComponent>(entity), context);
        else if (it.key() == "WaterSource")
            Decode(data, registry.emplace<C::WaterSourceComponent>(entity), context);
        else if (it.key() == "WaterFlowSource")
            Decode(data, registry.emplace<C::WaterFlowSourceComponent>(entity), context);
        else
            throw std::invalid_argument("unknown creature component");
    }
    CheckComponents(registry, entity);
    CheckController(registry, entity);
}

IdMap ExistingIdentities(const entt::registry& registry) {
    IdMap ids;
    std::set<std::uint64_t> shared;
    const auto add = [&](Identity id, entt::entity e) {
        Require(id.second > 0 && id.second < (1ull << 63) && shared.insert(id.second).second &&
                    ids.emplace(id, e).second,
                "duplicate shared durable identity");
    };
    for (auto e : registry.view<const C::PlantRegionComponent>()) {
        const auto id = DurableReference(registry, e);
        Require(id.first == "plant", "wrong-kind plant identity");
        add(id, e);
    }
    for (auto e : registry.view<const C::CreatureRegionComponent>()) {
        const auto id = DurableReference(registry, e);
        Require(id.first == "creature", "wrong-kind creature identity");
        add(id, e);
    }
    return ids;
}
} // namespace

CreatureSnapshot BuildCreatureSnapshot(const entt::registry& registry) {
    CreatureSnapshot snapshot;
    const auto ids = ExistingIdentities(registry);
    const Context context{&registry, &ids};
    for (auto e : registry.view<const C::CreatureComponent>()) {
        Require(registry.all_of<C::TransformComponent,
                                C::CreatureRegionComponent,
                                C::CreatureCoarseComponent>(e),
                "unadmitted persistent creature");
        CheckComponents(registry, e);
        CreatureRecord record{registry.get<C::CreatureRegionComponent>(e).persistent_id};
        const auto* saved_controller = registry.try_get<C::CreatureControllerComponent>(e);
        const C::CreatureControllerComponent controller{
            registry.all_of<C::CreaturePhysicsComponent>(e) ||
            (saved_controller && saved_controller->physics_controlled)};
        Require(!controller.physics_controlled ||
                    registry.all_of<C::CreatureLocomotionStateComponent>(e),
                "missing creature controller continuation state");
        record.components["Controller"] = {{"version", 1}, {"data", Encode(controller, context)}};
        if (const auto* c = registry.try_get<C::TransformComponent>(e))
            record.components["Transform"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::CreatureRegionComponent>(e))
            record.components["Region"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::CreatureCoarseComponent>(e))
            record.components["Coarse"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::CreatureLocomotionStateComponent>(e))
            record.components["LocomotionState"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::CreatureComponent>(e))
            record.components["Creature"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::CreatureGenomeComponent>(e))
            record.components["Genome"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::MortalComponent>(e))
            record.components["Mortal"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::DecayComponent>(e))
            record.components["Decay"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::ThirstComponent>(e))
            record.components["Thirst"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::CircadianComponent>(e))
            record.components["Circadian"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::AlarmComponent>(e))
            record.components["Alarm"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::PackHunterComponent>(e))
            record.components["PackHunter"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::TerritoryComponent>(e))
            record.components["Territory"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::TerritoryBiasComponent>(e))
            record.components["TerritoryBias"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::MigratoryComponent>(e))
            record.components["Migratory"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::ScavengerComponent>(e))
            record.components["Scavenger"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::NeedsComponent>(e))
            record.components["Needs"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::SensesComponent>(e))
            record.components["Senses"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::OpportunityComponent>(e))
            record.components["Opportunity"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::ActionPlanComponent>(e))
            record.components["ActionPlan"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (registry.all_of<C::InstinctAgent>(e))
            record.components["InstinctAgent"] = {{"version", 1}, {"data", Json::object()}};
        if (const auto* c = registry.try_get<C::LocomotionProfile>(e))
            record.components["LocomotionProfile"] = {{"version", 1},
                                                      {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::SensableComponent>(e))
            record.components["Sensable"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::PerceptionComponent>(e))
            record.components["Perception"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::AwarenessComponent>(e))
            record.components["Awareness"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::LocomotionIntentComponent>(e))
            record.components["LocomotionIntent"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::ScentSenseComponent>(e))
            record.components["ScentSense"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::LocomotionPathComponent>(e))
            record.components["LocomotionPath"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::StimulusSubscriptionComponent>(e))
            record.components["StimulusSubscription"] = {{"version", 1},
                                                         {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::InstinctAgentComponent>(e))
            record.components["InstinctAgentState"] = {{"version", 1},
                                                       {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::ForagerComponent>(e))
            record.components["Forager"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::FoodSourceComponent>(e))
            record.components["FoodSource"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::FieldEmitterComponent>(e))
            record.components["FieldEmitter"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (registry.all_of<C::PendingDeletion>(e))
            record.components["PendingDeletion"] = {{"version", 1}, {"data", Json::object()}};
        if (const auto* c = registry.try_get<C::GrazeableComponent>(e))
            record.components["Grazeable"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::WaterHoleComponent>(e))
            record.components["WaterHole"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::CombustibleComponent>(e))
            record.components["Combustible"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::IgnitionSourceComponent>(e))
            record.components["IgnitionSource"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::WaterSourceComponent>(e))
            record.components["WaterSource"] = {{"version", 1}, {"data", Encode(*c, context)}};
        if (const auto* c = registry.try_get<C::WaterFlowSourceComponent>(e))
            record.components["WaterFlowSource"] = {{"version", 1}, {"data", Encode(*c, context)}};
        snapshot.entities.push_back(std::move(record));
        Require(snapshot.entities.size() <= kMaxCreatures, "creature count limit");
    }
    std::sort(snapshot.entities.begin(), snapshot.entities.end(), [](const auto& a, const auto& b) {
        return a.id < b.id;
    });
    return snapshot;
}

std::string SerializeCreatureSnapshot(const CreatureSnapshot& snapshot) {
    Require(snapshot.entities.size() <= kMaxCreatures, "creature count limit");
    Json records = Json::array();
    std::size_t controller_bytes = 0;
    std::uint64_t previous = 0;
    for (const auto& record : snapshot.entities) {
        CheckRecord(record);
        Require(record.id > previous, "unordered or duplicate creature identity");
        previous = record.id;
        auto normalized = record;
        AdoptLegacyController(normalized);
        controller_bytes += ControllerBytes(normalized.components.at("Controller"));
        records.push_back({{"id", record.id}, {"components", std::move(normalized.components)}});
    }
    const Json document{{"schema", kCreatureSchema}, {"entities", std::move(records)}};
    CheckJsonBudgets(document);
    std::string bytes = document.dump() + "\n";
    Require(bytes.size() <= CreatureByteLimit(snapshot.entities.size(), controller_bytes),
            "creature byte limit");
    return bytes;
}

bool LoadCreatureSnapshot(std::string_view bytes, CreatureSnapshot& out, std::string& error) {
    try {
        // Bound allocation before SAX parsing, then credit only metadata present in the input.
        Require(bytes.size() <= kMaxStampedCreatureBytes, "creature byte limit");
        BoundedCreatureJson decoder;
        Require(Json::sax_parse(bytes.begin(), bytes.end(), &decoder), "invalid creature JSON");
        Json& j = decoder.Result();
        Keys(j, {"schema", "entities"});
        Require(j.at("schema") == kCreatureSchema, "unsupported creature schema");
        auto& records = j.at("entities");
        Require(records.is_array() && records.size() <= kMaxCreatures, "creature count limit");
        Require(bytes.size() <= CreatureByteLimit(records.size(), decoder.ControllerByteCount()),
                "creature byte limit");
        CreatureSnapshot candidate;
        std::uint64_t previous = 0;
        for (auto& item : records) {
            Keys(item, {"id", "components"});
            CreatureRecord record;
            Scalar(item.at("id"), record.id);
            record.components = std::move(item.at("components"));
            CheckRecord(record);
            AdoptLegacyController(record);
            Require(record.id > previous, "unordered or duplicate creature identity");
            previous = record.id;
            candidate.entities.push_back(std::move(record));
        }
        // Payload decode also validates unknown components, versions, bounds and references.
        // Plant targets are collected as placeholders here; their durable existence is checked
        // against the actual plant snapshot during cross-store validation.
        entt::registry scratch;
        std::set<std::uint64_t> plants;
        // The SAX decoder has already enforced at most 32 structured levels.
        std::array<Json::const_iterator, 32> next{}, end{};
        for (const auto& record : candidate.entities) {
            std::size_t depth = 0;
            const Json* value = &record.components;
            while (value != nullptr) {
                if (value->is_object() && value->contains("kind") && value->contains("id") &&
                    value->at("kind") == "plant") {
                    std::uint64_t id = 0;
                    Scalar(value->at("id"), id);
                    Require(id > 0 && id < (1ull << 63), "invalid plant reference");
                    plants.insert(id);
                }
                if (value->is_structured() && !value->empty()) {
                    Require(depth < next.size(), "creature nesting limit");
                    next[depth] = value->begin();
                    end[depth] = value->end();
                    value = &*next[depth];
                    ++next[depth];
                    ++depth;
                } else {
                    value = nullptr;
                    while (depth > 0) {
                        if (next[depth - 1] != end[depth - 1]) {
                            value = &*next[depth - 1];
                            ++next[depth - 1];
                            break;
                        }
                        --depth;
                    }
                }
            }
        }
        for (const auto id : plants) {
            auto e = scratch.create();
            scratch.emplace<C::PlantTag>(e);
            scratch.emplace<C::PlantRegionComponent>(e).persistent_id = id;
        }
        ApplyCreatureSnapshot(scratch, candidate);
        // Adoption and canonical spelling must also satisfy the same writer budgets.
        // Keep refusal transactional: no accepted snapshot can fail its next stamped save.
        SerializeCreatureSnapshot(candidate);
        out = std::move(candidate);
        return true;
    } catch (const std::exception& e) {
        error = std::string("Corrupt creature authority: ") + e.what();
        return false;
    }
}

void ApplyCreatureSnapshot(entt::registry& registry, const CreatureSnapshot& snapshot) {
    Require(snapshot.entities.size() <= kMaxCreatures, "creature count limit");
    auto destination_ids = ExistingIdentities(registry);
    entt::registry scratch;
    IdMap scratch_ids;
    std::set<std::uint64_t> shared;
    for (const auto& [id, entity] : destination_ids) {
        shared.insert(id.second);
        const auto placeholder = scratch.create();
        scratch_ids.emplace(id, placeholder);
    }
    std::vector<entt::entity> prepared;
    prepared.reserve(snapshot.entities.size());
    std::uint64_t previous = 0;
    for (const auto& record : snapshot.entities) {
        CheckRecord(record);
        Require(record.id > previous && shared.insert(record.id).second,
                "unordered or duplicate shared creature identity");
        previous = record.id;
        const auto e = scratch.create();
        prepared.push_back(e);
        scratch_ids.emplace(Identity{"creature", record.id}, e);
    }
    const Context context{&scratch, &scratch_ids};
    for (std::size_t i = 0; i < prepared.size(); ++i)
        DecodeComponents(scratch, prepared[i], snapshot.entities[i], context);

    // All schema/domain/reference refusals have completed. Publish typed values only;
    // never decode input into the destination or destroy entities to undo a refusal.
    std::map<entt::entity, entt::entity> remap;
    for (const auto& [id, entity] : destination_ids)
        remap.emplace(scratch_ids.at(id), entity);
    for (const auto source : prepared)
        remap.emplace(source, entt::null);
    for (const auto source : prepared)
        remap.at(source) = registry.create();
    const auto reference = [&remap](entt::entity e) {
        return e == entt::null ? entt::entity(entt::null) : remap.at(e);
    };
    const auto copy = [&]<class T>(entt::entity source, entt::entity destination) {
        if constexpr (std::is_empty_v<T>) {
            if (scratch.all_of<T>(source))
                registry.emplace<T>(destination);
        } else if (auto* value = scratch.try_get<T>(source)) {
            T component = std::move(*value);
            if constexpr (std::is_same_v<T, C::SensesComponent>) {
                component.nearest_food_source = reference(component.nearest_food_source);
                component.nearest_water_source = reference(component.nearest_water_source);
                component.nearest_threat = reference(component.nearest_threat);
            } else if constexpr (std::is_same_v<T, C::ActionPlanComponent>) {
                for (auto& action : component.plan)
                    action.target = reference(action.target);
            }
            registry.emplace<T>(destination, std::move(component));
        }
    };
    for (const auto source : prepared) {
        const auto destination = remap.at(source);
        copy.template operator()<C::TransformComponent>(source, destination);
        copy.template operator()<C::CreatureRegionComponent>(source, destination);
        copy.template operator()<C::CreatureCoarseComponent>(source, destination);
        copy.template operator()<C::CreatureControllerComponent>(source, destination);
        copy.template operator()<C::CreatureLocomotionStateComponent>(source, destination);
        copy.template operator()<C::CreatureComponent>(source, destination);
        copy.template operator()<C::CreatureGenomeComponent>(source, destination);
        copy.template operator()<C::MortalComponent>(source, destination);
        copy.template operator()<C::DecayComponent>(source, destination);
        copy.template operator()<C::ThirstComponent>(source, destination);
        copy.template operator()<C::CircadianComponent>(source, destination);
        copy.template operator()<C::AlarmComponent>(source, destination);
        copy.template operator()<C::PackHunterComponent>(source, destination);
        copy.template operator()<C::TerritoryComponent>(source, destination);
        copy.template operator()<C::TerritoryBiasComponent>(source, destination);
        copy.template operator()<C::MigratoryComponent>(source, destination);
        copy.template operator()<C::ScavengerComponent>(source, destination);
        copy.template operator()<C::NeedsComponent>(source, destination);
        copy.template operator()<C::SensesComponent>(source, destination);
        copy.template operator()<C::OpportunityComponent>(source, destination);
        copy.template operator()<C::ActionPlanComponent>(source, destination);
        copy.template operator()<C::InstinctAgent>(source, destination);
        copy.template operator()<C::LocomotionProfile>(source, destination);
        copy.template operator()<C::SensableComponent>(source, destination);
        copy.template operator()<C::PerceptionComponent>(source, destination);
        copy.template operator()<C::AwarenessComponent>(source, destination);
        copy.template operator()<C::LocomotionIntentComponent>(source, destination);
        copy.template operator()<C::ScentSenseComponent>(source, destination);
        copy.template operator()<C::LocomotionPathComponent>(source, destination);
        copy.template operator()<C::StimulusSubscriptionComponent>(source, destination);
        copy.template operator()<C::InstinctAgentComponent>(source, destination);
        copy.template operator()<C::ForagerComponent>(source, destination);
        copy.template operator()<C::FoodSourceComponent>(source, destination);
        copy.template operator()<C::FieldEmitterComponent>(source, destination);
        copy.template operator()<C::PendingDeletion>(source, destination);
        copy.template operator()<C::GrazeableComponent>(source, destination);
        copy.template operator()<C::WaterHoleComponent>(source, destination);
        copy.template operator()<C::CombustibleComponent>(source, destination);
        copy.template operator()<C::IgnitionSourceComponent>(source, destination);
        copy.template operator()<C::WaterSourceComponent>(source, destination);
        copy.template operator()<C::WaterFlowSourceComponent>(source, destination);
    }
}

bool ValidateCreatureSnapshot(const CreatureSnapshot& snapshot,
                              const Luminumbra::world::ActiveRegionLedger& ledger,
                              const Luminumbra::world::GroundObjectStore& ground,
                              const Luminumbra::Ecs::EntityRegistrySnapshot& plants,
                              std::string& error) {
    try {
        std::set<std::uint64_t> ids;
        for (const auto& [id, object] : ground.objects())
            ids.insert(id);
        for (const auto& plant : plants.entities)
            Require(ids.insert(plant.entity_id).second, "duplicate shared plant identity");
        for (const auto& record : snapshot.entities) {
            Require(record.id < ledger.next_persistent_id() && ids.insert(record.id).second,
                    "duplicate or invalid shared creature identity");
            const auto& owner = record.components.at("Region").at("data");
            std::int32_t x = 0, z = 0;
            std::uint64_t stamp = 0;
            Scalar(owner.at("owner_x"), x);
            Scalar(owner.at("owner_z"), z);
            Scalar(owner.at("region_stamp"), stamp);
            Require(ledger.records().contains({x, z}) && stamp <= ledger.header_tick(),
                    "invalid creature owner or stamp");
        }
        entt::registry scratch;
        luminumbra::foliage::ApplyPlantEntitySnapshot(scratch, plants);
        ApplyCreatureSnapshot(scratch, snapshot);
        Require(SerializeCreatureSnapshot(BuildCreatureSnapshot(scratch)) ==
                    SerializeCreatureSnapshot(snapshot),
                "noncanonical creature payload");
        return true;
    } catch (const std::exception& e) {
        error = std::string("Corrupt creature authority: ") + e.what();
        return false;
    }
}

std::string CreatureRegionProjection(const entt::registry& registry,
                                     Luminumbra::world::RegionKey key) {
    auto snapshot = BuildCreatureSnapshot(registry);
    std::erase_if(snapshot.entities, [key](const CreatureRecord& record) {
        const auto& owner = record.components.at("Region").at("data");
        return owner.at("owner_x") != key.x || owner.at("owner_z") != key.z;
    });
    return snapshot.entities.empty() ? std::string{} : SerializeCreatureSnapshot(snapshot);
}
} // namespace luminumbra::ai
