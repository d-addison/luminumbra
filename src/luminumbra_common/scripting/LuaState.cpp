#include "LuaState.h"

#include "LuaApiManifest.h"

// The pimpl in LuaState.h keeps the sol2 dependency confined to this
// translation unit.
#include <sol/error.hpp>
#include <sol/forward.hpp>
#include <sol/optional_implementation.hpp>
#include <sol/protected_function_result.hpp>
#include <sol/state.hpp>
#include <sol/state_handling.hpp>
#include <sol/types.hpp>

#include <cmath>
#include <memory>
#include <optional>
#include <string>

#include "../fields/EnergyFieldState.h"
#include "../systems/AetherFieldSystem.h" // kAetherCellSizeM — the shared 24 m grid identity

namespace Luminumbra::scripting {

struct LuaState::Impl {
    // NO open_libraries: the sandbox exposes exactly the surface registered
    // below (manifest-gated) — no io/os/debug/package, no load/dofile, no
    // wall-clock, no math.random (the escape corpus in test/scripting/ pins
    // the manifest side of this contract).
    sol::state lua;
};

LuaState::LuaState()
    : m_impl(std::make_unique<Impl>()) {
    // Both manifest entry points share this read-only implementation.
    const auto sampler = [this](double x, double y, double z) {
        return sample_energy_field(x, y, z);
    };
    m_impl->lua.set_function("sample_energy_field", sampler);
    sol::table world = m_impl->lua.create_named_table("world");
    world.set_function("sample_energy_field", sampler);
}

LuaState::~LuaState() = default;

const LuaApiManifest& LuaState::api_manifest() {
    return GetLuaApiManifest();
}

void LuaState::set_energy_field(const luminumbra::fields::EnergyFieldState* field) {
    m_energy_field = field;
}

double LuaState::sample_energy_field(double x, double y, double z) const {
    (void)y; // columnar (2.5D) field: cells are keyed by (cx, cz) only
    if (m_energy_field == nullptr) {
        return 0.0; // no session / sim.aether_state OFF: the field reads 0
    }
    // World -> cell quantization matches GameSession's anchor quantization
    // byte-for-byte (std::floor over the shared 24 m cell size). This is a
    // one-way READ path — float here never reaches hashed state.
    const auto cell_size = static_cast<double>(Systems::kAetherCellSizeM);
    const int cx = static_cast<int>(std::floor(x / cell_size));
    const int cz = static_cast<int>(std::floor(z / cell_size));
    return static_cast<double>(m_energy_field->at_cell(cx, cz, /*channel=*/0)) /
           static_cast<double>(luminumbra::fields::kEnergyRawPerUnit);
}

LuaEvaluationResult LuaState::Evaluate(const std::string& chunk) const {
    const sol::protected_function_result result =
        m_impl->lua.safe_script(chunk, sol::script_pass_on_error);
    if (!result.valid()) {
        LuaEvaluationStatus status = LuaEvaluationStatus::OtherError;
        if (result.status() == sol::call_status::syntax) {
            status = LuaEvaluationStatus::SyntaxError;
        } else if (result.status() == sol::call_status::runtime) {
            status = LuaEvaluationStatus::RuntimeError;
        }
        const sol::error error = result;
        return {status, std::nullopt, error.what()};
    }
    LuaEvaluationResult evaluation;
    if (result.return_count() > 0) {
        const sol::optional<double> value = result.get<sol::optional<double>>();
        if (value) {
            evaluation.number = *value;
        }
    }
    return evaluation;
}

bool LuaState::EvalNumber(const std::string& chunk, double& out_value) const {
    const auto result = Evaluate(chunk);
    if (result.status != LuaEvaluationStatus::Succeeded || !result.number) {
        return false;
    }
    out_value = *result.number;
    return true;
}

} // namespace Luminumbra::scripting
