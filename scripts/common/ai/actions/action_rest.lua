--[[
    action_rest.lua
    An action for resting, which satisfies the fatigue need.
]]

local ActionRest = {
    name = "Rest",
    cost = 8.0, -- Resting is time-consuming
}

function ActionRest:get_preconditions(agent)
    -- An agent will only rest if it's tired AND feels safe.
    return { 
        isTired = true,
        isThreatened = false
    }
end

function ActionRest:get_effects(agent)
    return { isTired = false }
end

function ActionRest:on_start(agent)
    print("GroveStrider ".. tostring(agent.entity) .." finds a spot to rest.")
    agent.entity:play_animation("rest_start")
    agent.start_time = agent.entity:get_world_time()
end

function ActionRest:is_finished(agent)
    -- Rest for 15 seconds
    if agent.entity:get_world_time() - agent.start_time >= 15.0 then
        local needs = agent.entity:get_component("NeedsComponent")
        needs.fatigue = 0.0 -- Fully recover from fatigue
        print("GroveStrider ".. tostring(agent.entity) .." is well-rested.")
        return true
    end
    return false
end

-- This action is a good candidate for being interrupted. If a threat appears
-- while resting, the GOAP planner will see that the 'be_safe' goal now has a
-- much higher priority. It will abandon the current plan and create a new one,
-- which will likely start with the 'Flee' action. Your C++ Instinct Engine would
-- then call an 'on_interrupt' function if one existed on this action's script.

return ActionRest