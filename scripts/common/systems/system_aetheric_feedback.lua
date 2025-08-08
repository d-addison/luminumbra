--[[
    system_aetheric_feedback.lua
    A common system that runs periodically to apply the effects of the
    Aetheric Field (Lumin and Umbra) to entities.
]]

local AethericFeedback = {}

-- This function would be called by the C++ ScriptSystem on all entities
-- that have both an AethericFieldComponent and a NeedsComponent.
function AethericFeedback:update(entity, dt)
    local aether = entity:get_component("AethericFieldComponent")
    local needs = entity:get_component("NeedsComponent")
    if not aether or not needs then return end

    local current_pos = entity:get_component("TransformComponent").position
    -- Assume API to sample the world's persistent energy field
    local field_value = entity:get_world_api():get_aetheric_value(current_pos) -- Returns e.g., { lumin = 0.8, umbra = 0.2 }

    if aether.attunement == "Lumin" then
        -- GroveStriders are stressed by deep shadow
        if field_value.umbra > aether.tolerance.umbra then
            needs.safety = needs.safety - (0.1 * dt) -- Feel unsafe in the dark
            needs.fatigue = needs.fatigue + (aether.fatigue_rate * 0.5 * dt) -- Tire faster
        end
    elseif aether.attunement == "Umbra" then
        -- ShadowStalkers are weakened and exposed by bright light
        if field_value.lumin > aether.tolerance.lumin then
            entity:apply_damage(1 * dt) -- Take slow burn damage in light
            local stealth = entity:get_component("StealthComponent")
            if stealth then
                stealth.visibility_multiplier = 5.0 -- Become highly visible
            end
        else
            local stealth = entity:get_component("StealthComponent")
            if stealth then
                -- The deeper the shadow, the harder to see
                stealth.visibility_multiplier = 1.0 - (field_value.umbra * (1.0 - stealth.umbra_multiplier))
            end
        end
    end
end

return AethericFeedback