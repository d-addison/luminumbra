// AetherScriptBinding provides proving signals for the Lua energy-field sampler.
// Three contracts are pinned here:
//   1. the manifest gate stays green WITH the new entry (adding a binding
//      REQUIRES a manifest entry — the surface is manifest-enumerated);
//   2. the binding is absent-safe: no wired layer -> 0.0, never an error;
//   3. the binding reads QUANTIZED TRUTH: a seeded EnergyFieldState sampled
//      through the live sol2 VM returns at_cell raw / kEnergyRawPerUnit at the
//      std::floor-quantized 24 m cell — including the emitter-gather path
//      (FieldEmitterComponent -> GatherFieldEmitterDeposits -> Tick -> Lua).
//
// Registered into frontier_gates_test (mirrors lua_sandbox_escape_test.cpp).
// The game-side alias (scripts/common/api/aetheric_field.lua) is a pure Lua
// delegation to the same global; its resolution is covered by driving the
// identical chunk body here.
#include "gtest/gtest.h"

#include "scripting/LuaApiManifest.h"
#include "scripting/LuaState.h"

#include "components/FieldEmitterComponents.h"
#include "fields/EnergyFieldState.h"
#include "systems/FieldEmitterSystem.h"

#include "entt/entt.hpp"

#include <string>

namespace {

using luminumbra::fields::EnergyFieldState;
using luminumbra::fields::kEnergyRawPerUnit;
using Luminumbra::scripting::GetLuaApiManifest;
using Luminumbra::scripting::LuaApiManifest;
using Luminumbra::scripting::LuaApiManifestMeetsBaseline;
using Luminumbra::scripting::LuaState;
using Luminumbra::scripting::SerializeLuaApiManifestJson;

bool ManifestExposes(const LuaApiManifest& manifest,
                     const std::string& module,
                     const std::string& name) {
    for (const auto& entry : manifest.entries) {
        if (entry.module == module && entry.name == name) {
            return true;
        }
    }
    return false;
}

// Evaluate a chunk expected to yield a number; ADD_FAILURE (returning 0.0) on
// an eval error so a broken registration reads as a loud test failure, not a
// silent zero-compare.
double EvalOrFail(const LuaState& lua, const std::string& chunk) {
    double value = 0.0;
    if (!lua.EvalNumber(chunk, value)) {
        ADD_FAILURE() << "Lua eval failed for chunk: " << chunk;
        return 0.0;
    }
    return value;
}

} // namespace

// Contract 1: the manifest gate stays green with the sampler entry present —
// baseline met, entry listed under its module home, serialized JSON carries it.
TEST(AetherScriptBinding, ManifestCarriesTheSamplerEntry) {
    const LuaApiManifest& manifest = GetLuaApiManifest();
    EXPECT_TRUE(LuaApiManifestMeetsBaseline(manifest))
        << "manifest baseline must stay green WITH the sample_energy_field entry";
    EXPECT_TRUE(ManifestExposes(manifest, "world", "sample_energy_field"))
        << "-5: the binding requires a manifest entry (world.sample_energy_field)";

    const std::string json = SerializeLuaApiManifestJson(manifest);
    EXPECT_NE(json.find("\"name\": \"sample_energy_field\""), std::string::npos);
    EXPECT_NE(json.find("world.sample_energy_field(x: number, y: number, z: number) -> number"),
              std::string::npos);
}

// Contract 2: absent layer -> 0.0 everywhere, on the host seam AND through the
// live VM (both the bare global and its world.* manifest home), never an error.
TEST(AetherScriptBinding, SamplerReadsZeroWithNoLayer) {
    LuaState lua;
    EXPECT_DOUBLE_EQ(lua.sample_energy_field(0.0, 0.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(lua.sample_energy_field(-1000.0, 50.0, 1000.0), 0.0);
    EXPECT_DOUBLE_EQ(EvalOrFail(lua, "return sample_energy_field(0, 0, 0)"), 0.0);
    EXPECT_DOUBLE_EQ(EvalOrFail(lua, "return world.sample_energy_field(12.5, 3.0, -80.0)"), 0.0);

    // Re-seating to null (session teardown) restores the absent contract.
    EnergyFieldState field;
    lua.set_energy_field(&field);
    lua.set_energy_field(nullptr);
    EXPECT_DOUBLE_EQ(EvalOrFail(lua, "return sample_energy_field(0, 0, 0)"), 0.0);
}

// Contract 3a: the binding reads quantized truth from a seeded layer — raw /
// kEnergyRawPerUnit at the std::floor 24 m cell, negative coordinates included.
TEST(AetherScriptBinding, SamplerReadsSeededLayerThroughLua) {
    EnergyFieldState field;
    field.SetAnchorCell(0, 0);
    field.QueueDeposit(/*emitter=*/1, /*cx=*/2, /*cz=*/3, /*channel=*/0, 512);
    field.QueueDeposit(/*emitter=*/1, /*cx=*/-1, /*cz=*/-1, /*channel=*/0, 256);
    ASSERT_EQ(field.Tick(1), 0u) << "test invariant: nothing clips";
    ASSERT_EQ(field.at_cell(2, 3), 512u);

    LuaState lua;
    lua.set_energy_field(&field);

    // Cell (2, 3) spans x in [48, 72), z in [72, 96): 512 raw = 2.0 units.
    EXPECT_DOUBLE_EQ(lua.sample_energy_field(60.0, 5.0, 80.0), 2.0);
    EXPECT_DOUBLE_EQ(EvalOrFail(lua, "return sample_energy_field(60.0, 5.0, 80.0)"), 2.0);
    EXPECT_DOUBLE_EQ(EvalOrFail(lua, "return world.sample_energy_field(48.0, 0.0, 95.9)"), 2.0);

    // Floor (not truncate-toward-zero) quantization: (-0.5, -0.5) is cell
    // (-1, -1), matching GameSession's anchor semantics. 256 raw = 1.0 unit.
    EXPECT_DOUBLE_EQ(EvalOrFail(lua, "return sample_energy_field(-0.5, 0.0, -0.5)"), 1.0);
    // The neighbouring unseeded cell (0, 0) still reads 0.
    EXPECT_DOUBLE_EQ(EvalOrFail(lua, "return sample_energy_field(0.5, 0.0, 0.5)"), 0.0);
}

// Contract 3b: the whole emitter path end-to-end — FieldEmitterComponent
// carriers gathered id-sorted into the layer, ticked, then sampled from Lua.
// A zero-rate emitter is inert (the additive opt-in contract).
TEST(AetherScriptBinding, EmitterGatherFeedsTheSampler) {
    entt::registry registry;

    // Emitter at world (30, 0, 30) -> cell (1, 1): rate 1024 raw, radius 1 ->
    // centre 1024, each Chebyshev-1 ring cell 1024 >> 1 == 512.
    const auto emitter = registry.create();
    {
        auto& tf = registry.emplace<Luminumbra::Components::TransformComponent>(emitter);
        tf.position.x = 30.0f;
        tf.position.z = 30.0f;
        auto& em = registry.emplace<Luminumbra::Components::FieldEmitterComponent>(emitter);
        em.rate_raw_per_tick = 1024;
        em.radius_cells = 1;
    }
    // Zero-default carrier: participates in nothing (rate 0 -> skipped).
    const auto inert = registry.create();
    registry.emplace<Luminumbra::Components::TransformComponent>(inert);
    registry.emplace<Luminumbra::Components::FieldEmitterComponent>(inert);

    EnergyFieldState field;
    field.SetAnchorCell(0, 0);
    const auto stats = Luminumbra::Systems::GatherFieldEmitterDeposits(registry, field);
    EXPECT_EQ(stats.emitters, 1u) << "the rate-0 carrier must be inert";
    EXPECT_EQ(stats.deposits, 9u) << "centre + 8 Chebyshev-1 ring cells";
    ASSERT_EQ(field.Tick(1), 0u) << "test invariant: nothing clips";

    LuaState lua;
    lua.set_energy_field(&field);
    // Centre cell (1, 1): 1024 raw = 4.0 units at x, z in [24, 48).
    EXPECT_DOUBLE_EQ(EvalOrFail(lua, "return sample_energy_field(36.0, 0.0, 30.0)"), 4.0);
    // Ring cell (0, 1): 512 raw = 2.0 units at x in [0, 24).
    EXPECT_DOUBLE_EQ(EvalOrFail(lua, "return sample_energy_field(12.0, 0.0, 30.0)"), 2.0);
    // Two rings out is untouched.
    EXPECT_DOUBLE_EQ(EvalOrFail(lua, "return sample_energy_field(-30.0, 0.0, 30.0)"), 0.0);

    const double raw_per_unit = static_cast<double>(kEnergyRawPerUnit);
    EXPECT_DOUBLE_EQ(1024.0 / raw_per_unit, 4.0) << "pinned unit scale drifted";
}

// The eval seam's failure contract: parse errors, runtime errors, and
// non-numeric results report false (no throw, no abort) — and the VM is bare
// (no stdlib): escape-shaped globals like os/load simply do not exist.
TEST(AetherScriptBinding, EvalNumberRejectsErrorsAndNonNumbers) {
    LuaState lua;
    double value = 123.0;
    EXPECT_FALSE(lua.EvalNumber("this is not lua", value));
    EXPECT_FALSE(lua.EvalNumber("error('boom')", value));
    EXPECT_FALSE(lua.EvalNumber("return 'a string'", value));
    EXPECT_FALSE(lua.EvalNumber("return nil", value));
    EXPECT_EQ(value, 123.0) << "a failed eval must leave out_value untouched";

    // Sandbox spot-checks: no stdlib was opened, so the escape corpus's
    // favourite globals are nil (full corpus: lua_sandbox_escape_test.cpp).
    EXPECT_DOUBLE_EQ(EvalOrFail(lua, "return (os == nil) and 1 or 0"), 1.0);
    EXPECT_DOUBLE_EQ(EvalOrFail(lua, "return (io == nil) and 1 or 0"), 1.0);
    EXPECT_DOUBLE_EQ(EvalOrFail(lua, "return (load == nil) and 1 or 0"), 1.0);
    EXPECT_DOUBLE_EQ(EvalOrFail(lua, "return (dofile == nil) and 1 or 0"), 1.0);
}
