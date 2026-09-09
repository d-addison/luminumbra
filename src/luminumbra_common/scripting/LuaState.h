#pragma once

#include <memory>
#include <optional>
#include <string>

namespace luminumbra::fields {
class EnergyFieldState;
}

namespace Luminumbra::scripting {

struct LuaApiManifest;

enum class LuaEvaluationStatus {
    Succeeded,
    SyntaxError,
    RuntimeError,
    OtherError
};

struct LuaEvaluationResult {
    LuaEvaluationStatus status = LuaEvaluationStatus::Succeeded;
    // The first return value, when convertible by the existing numeric seam.
    // An empty optional does not imply that execution failed.
    std::optional<double> number;
    std::string diagnostic;
};

// The engine's Lua host exposes only the read-only energy-field sampler through
// `sample_energy_field` and `world.sample_energy_field`. No standard libraries
// are opened. The manifest describes these two initially installed entry points.
// This test/dev host has no instruction or memory budget, per-chunk global
// isolation, or general behavior execution contract.
//
// SIM-PATH RULE (the  bridge rule): script sampling is one-way
// read-only. Bindings CONSUME sim state through const pointers wired by the
// host; nothing registered here may write back — gameplay writes go through
// components (FieldEmitterComponent), never through Lua.
class LuaState {
public:
    LuaState();
    ~LuaState();

    LuaState(const LuaState&) = delete;
    LuaState& operator=(const LuaState&) = delete;

    static const LuaApiManifest& api_manifest();

    // session-context seam. The host (GameSession owner)
    // points the sampler at the stateful energy layer — or nullptr when the
    // session is absent or sim.aether_state is OFF, in which case the binding
    // reads 0.0 everywhere. The layer is BORROWED: the caller keeps it alive
    // for the duration (or re-seats/clears the pointer when it goes away).
    void set_energy_field(const luminumbra::fields::EnergyFieldState* field);

    // Host-side implementation of the `sample_energy_field` binding: quantize
    // the world position by the shared 24 m cell size (std::floor — the same
    // semantics as GameSession's anchor quantization), read channel 0, return
    // GAMEPLAY units (raw / kEnergyRawPerUnit). The field is columnar (2.5D):
    // y is accepted for API symmetry and ignored. 0.0 when no layer is wired.
    [[nodiscard]] double sample_energy_field(double x, double y, double z) const;

    // Execute a chunk and distinguish success (including no/nonnumeric return)
    // from syntax/runtime failures. Diagnostics describe Lua errors; they are
    // not yet graph source maps. The VM's globals persist between evaluations.
    [[nodiscard]] LuaEvaluationResult Evaluate(const std::string& chunk) const;

    // Run a Lua chunk in the sandboxed VM and extract its first return value
    // as a number. Returns false (out_value untouched) on a parse/runtime
    // error or a non-numeric result — the test/dev evaluation seam.
    [[nodiscard]] bool EvalNumber(const std::string& chunk, double& out_value) const;

private:
    struct Impl; // holds the sol2 VM and keeps its headers out of the public surface
    std::unique_ptr<Impl> m_impl;
    const luminumbra::fields::EnergyFieldState* m_energy_field = nullptr;
};

} // namespace Luminumbra::scripting
