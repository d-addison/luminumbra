--[[
    action_graze.lua
    An action for eating, which satisfies the hunger need.
]]

local ActionGraze = {
    name = "Graze",
    cost = 2.0,
}

function ActionGraze:get_preconditions(agent)
    return { isNearFood = true }
end

function ActionGraze:get_effects(agent)
    return { isHungry = false }
end

function ActionGraze:on_start(agent)
    print("GroveStrider ".. tostring(agent.entity) .." starts grazing.")
    agent.entity:play_animation("graze") -- Assumes an animation system API
    agent.start_time = agent.entity:get_world_time()
end

function ActionGraze:is_finished(agent)
    -- Graze for 5 seconds
    if agent.entity:get_world_time() - agent.start_time >= 5.0 then
        local needs = agent.entity:get_component("NeedsComponent")
        needs.hunger = 0.0 -- Fully satisfy hunger
        print("GroveStrider ".. tostring(agent.entity) .." is full.")
        return true
    end
    return false
end

return ActionGraze