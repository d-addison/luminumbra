#include "LuaState.h"

#include "LuaApiManifest.h"

#include <Luau/Compiler.h>
#include <lua.h>
#include <lualib.h>

#include <cmath>
#include <memory>
#include <new>
#include <optional>
#include <string>

#include "../fields/EnergyFieldState.h"
#include "../systems/AetherFieldSystem.h" // kAetherCellSizeM — the shared 24 m grid identity

namespace Luminumbra::scripting {

struct LuaState::Impl {
    std::unique_ptr<lua_State, decltype(&lua_close)> main{luaL_newstate(), lua_close};
    lua_State* thread = nullptr;

    explicit Impl(LuaState& owner) {
        if (!main) {
            throw std::bad_alloc();
        }
        // Keep the manifest's two entry points as the entire initial surface.
        // No standard libraries, loaders, environment access or random source.
        lua_pushlightuserdata(main.get(), &owner);
        lua_pushcclosure(main.get(), Sample, "sample_energy_field", 1);
        lua_setglobal(main.get(), "sample_energy_field");
        lua_newtable(main.get());
        lua_getglobal(main.get(), "sample_energy_field");
        lua_setfield(main.get(), -2, "sample_energy_field");
        lua_setglobal(main.get(), "world");
        luaL_sandbox(main.get());

        // The main stack roots this one thread for the lifetime of the VM.
        // Its private globals persist across every evaluation of this host.
        thread = lua_newthread(main.get());
        luaL_sandboxthread(thread);
    }

    static int Sample(lua_State* state) {
        const auto* owner =
            static_cast<const LuaState*>(lua_touserdata(state, lua_upvalueindex(1)));
        luaL_checktype(state, 1, LUA_TNUMBER);
        luaL_checktype(state, 2, LUA_TNUMBER);
        luaL_checktype(state, 3, LUA_TNUMBER);
        const double x = lua_tonumber(state, 1);
        const double y = lua_tonumber(state, 2);
        const double z = lua_tonumber(state, 3);
        lua_pushnumber(state, owner->sample_energy_field(x, y, z));
        return 1;
    }
};

LuaState::LuaState()
    : m_impl(std::make_unique<Impl>(*this)) {}

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
    const std::string bytecode = Luau::compile(chunk);
    auto* state = m_impl->thread;
    lua_settop(state, 0);
    // Repeated loads must not cache imports from this mutable global table.
    lua_setsafeenv(state, LUA_GLOBALSINDEX, false);
    int status = luau_load(state, "=LuaState", bytecode.data(), bytecode.size(), 0);
    if (status == LUA_OK) {
        status = lua_pcall(state, 0, 1, 0);
    }
    LuaEvaluationResult evaluation;
    if (status != LUA_OK) {
        evaluation.status = LuaEvaluationStatus::OtherError;
        if (!bytecode.empty() && bytecode.front() == '\0') {
            evaluation.status = LuaEvaluationStatus::SyntaxError;
        } else if (status == LUA_ERRRUN) {
            evaluation.status = LuaEvaluationStatus::RuntimeError;
        }
        const char* diagnostic = lua_tostring(state, -1);
        evaluation.diagnostic = diagnostic != nullptr ? diagnostic : "Luau evaluation failed";
    } else if (lua_type(state, -1) == LUA_TNUMBER) {
        evaluation.number = lua_tonumber(state, -1);
    }
    lua_settop(state, 0);
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
