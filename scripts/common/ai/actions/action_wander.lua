--[[
    action_wander.lua
    A simple action for moving to a random nearby point.
    This serves as the default "idle" behavior.
]]

local ActionWander = {
    name = "Wander",
    cost = 5.0, -- A moderately costly action to encourage goal-oriented behavior
}

-- The planner checks this to see if the action is possible.
function ActionWander:get_preconditions(agent)
    return {} -- No preconditions, can always wander.
end

-- This tells the planner what will change after the action is done.
function ActionWander:get_effects(agent)
    return { hasExplored = true } -- Satisfies the low-priority "explore" goal.
end

-- Called once by the C++ Instinct Engine when the action begins.
function ActionWander:on_start(agent)
    print("GroveStrider ".. tostring(agent.entity) .." starts wandering.")
    local current_pos = agent.entity:get_component("TransformComponent").position
    
    -- Find a random point within a 20-unit radius to wander to.
    -- A real implementation would use a navmesh or ground-sampling function.
    local random_offset = {x = math.random(-20, 20), y = 0, z = math.random(-20, 20)}
    local target_pos = {
        x = current_pos.x + random_offset.x,
        y = current_pos.y, -- Assume ground level for now
        z = current_pos.z + random_offset.z,
    }

    agent.entity:set_destination(target_pos)
end

-- Called every tick while the action is running.
function ActionWander:is_finished(agent)
    -- The action is finished once the entity's physics body has reached its destination.
    return agent.entity:has_reached_destination()
end

return ActionWander