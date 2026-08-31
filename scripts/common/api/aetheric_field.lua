--[[
    aetheric_field.lua
    Game-side alias over the engine's generic energy-field sampler (
    -5). The engine keeps generic naming ("energy field" — the split-lint
    bans game-flavored names under src/); the Aetheric flavor lives HERE, in
    scripts/ only.
]]

local AethericField = {}

-- Read the aetheric energy at a world position, in gameplay units (raw / 256).
-- Delegates to the engine binding registered by LuaState: the bare global
-- sample_energy_field (also reachable as world.sample_energy_field, its
-- manifest home). Returns 0 when the stateful layer is absent (session gone or
-- sim.aether_state OFF) — callers need no nil-guard. Read-only by contract:
-- scripts sample the field; they never write it (emitters are components).
function AethericField.get_aetheric_value(x, y, z)
    return sample_energy_field(x, y, z)
end

return AethericField
