--[[
    directive_lunar_bloom.lua
    A world directive that triggers on a full moon. It causes all
    Glimmercap mushrooms to enter a 'hyper-bloom' state, dramatically
    increasing their light emission.
]]

local LunarBloom = {
    duration = 480, -- Lasts for 8 minutes (480 seconds)
}

-- The C++ DirectiveSystem would check conditions and call on_start().
-- Condition for this could be: is_night() and is_full_moon().
function LunarBloom:on_start()
    print("DIRECTIVE: A Lunar Bloom has begun!")
    self.world_api = self.entity:get_world_api()
    self.start_time = self.world_api:get_world_time()
    
    self.affected_caps = self.world_api:find_entities_by_archetype("flora_glimmercap")
    
    for _, cap_entity in ipairs(self.affected_caps) do
        local aether = cap_entity:get_component("AethericFieldComponent")
        local render = cap_entity:get_component("RenderMeshComponent")

        -- Store original values so we can revert them later
        aether.original_strength = aether.emission.strength
        render.original_scale = cap_entity:get_component("TransformComponent").scale

        -- Enhance the effect
        aether.emission.strength = aether.original_strength * 3.0
        render.color_multiplier = {r=1.5, g=1.8, b=2.0}
        cap_entity:get_component("TransformComponent").scale = {x=1.2, y=1.2, z=1.2}
    end
    
    self.state = "active"
end

function LunarBloom:on_update(dt)
    if self.state ~= "active" then return end
    
    -- Check if the directive's time is up
    if self.world_api:get_world_time() - self.start_time >= self.duration then
        self:end_scenario()
    end
end

function LunarBloom:end_scenario()
    print("DIRECTIVE: The Lunar Bloom fades.")
    for _, cap_entity in ipairs(self.affected_caps) do
         local aether = cap_entity:get_component("AethericFieldComponent")
        local render = cap_entity:get_component("RenderMeshComponent")

        -- Revert to original values
        aether.emission.strength = aether.original_strength
        render.color_multiplier = {r=1.0, g=1.0, b=1.0}
        cap_entity:get_component("TransformComponent").scale = render.original_scale
    end
    self.state = "finished"
end

function LunarBloom:is_finished()
    return self.state == "finished"
end

return LunarBloom