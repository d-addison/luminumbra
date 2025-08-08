--[[
    action_flee.lua
    A high-priority action to run away from a perceived threat.
]]

local ActionFlee = {
    name = "Flee",
    cost = 1.0, -- Low cost so it's always preferred when preconditions are met.
}

function ActionFlee:get_preconditions(agent)
    return { isThreatened = true }
end

-- The effect is not guaranteed, but it's what the agent *hopes* to achieve.
function ActionFlee:get_effects(agent)
    return { isThreatened = false }
end

function ActionFlee:on_start(agent)
    print("GroveStrider ".. tostring(agent.entity) .." is fleeing!")
    local senses = agent.entity:get_component("SensesComponent")
    if not senses.nearest_threat then return end

    local my_pos = agent.entity:get_component("TransformComponent").position
    local threat_pos = senses.nearest_threat:get_component("TransformComponent").position

    -- Calculate a vector pointing away from the threat
    local flee_vector = {
        x = my_pos.x - threat_pos.x,
        y = 0,
        z = my_pos.z - threat_pos.z,
    }
    
    -- Normalize and scale to find a destination 50 units away
    local len = math.sqrt(flee_vector.x^2 + flee_vector.z^2)
    local flee_destination = {
        x = my_pos.x + (flee_vector.x / len) * 50,
        y = my_pos.y,
        z = my_pos.z + (flee_vector.z / len) * 50,
    }

    agent.entity:set_destination(flee_destination)
    agent.entity:set_movement_speed_multiplier(2.0) -- Run faster
end

function ActionFlee:on_end(agent)
    -- Always reset speed multiplier when the action finishes or is interrupted
    agent.entity:set_movement_speed_multiplier(1.0)
end

function ActionFlee:is_finished(agent)
    -- Fleeing is "finished" once the threat is no longer sensed.
    -- The GOAP planner will re-evaluate every few seconds. If the threat is still
    -- present, it will likely plan a new 'Flee' action.
    local senses = agent.entity:get_component("SensesComponent")
    return senses.nearest_threat == nil
end

return ActionFlee