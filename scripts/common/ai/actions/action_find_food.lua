--[[
    action_find_food.lua
    A planning action that finds the nearest known food source and moves to it.
    This action connects the state of 'isHungry' to the 'isNearFood' state required by Graze.
]]

local ActionFindFood = {
    name = "FindFood",
    cost = 3.0,
}

function ActionFindFood:get_preconditions(agent)
    -- This action is only valid if the agent is hungry BUT doesn't currently see food.
    return {
        isHungry = true,
        isNearFood = false 
    }
end

function ActionFindFood:get_effects(agent)
    -- The outcome of this action is that the agent will be near a food source.
    return { isNearFood = true }
end

function ActionFindFood:on_start(agent)
    -- Assumes an API call to the engine to query the world for specific entity types/tags.
    -- This simulates the agent using memory or long-range senses (smell).
    local food_source = agent.entity:find_nearest_with_tag("food_source")

    if food_source then
        print("GroveStrider ".. tostring(agent.entity) .." has located a food source and is moving towards it.")
        local target_pos = food_source:get_component("TransformComponent").position
        agent.entity:set_destination(target_pos)
        agent.target_food = food_source -- Store for later, if needed
    else
        -- If no food is found, the action fails immediately. The planner will try something else.
        print("GroveStrider ".. tostring(agent.entity) .." could not find any food.")
        agent:action_failed()
    end
end

function ActionFindFood:is_finished(agent)
    -- The action is complete when the physics system reports the destination has been reached.
    return agent.entity:has_reached_destination()
end

function ActionFindFood:on_end(agent)
    -- Clean up any state stored on the agent table for this action.
    agent.target_food = nil
end

return ActionFindFood