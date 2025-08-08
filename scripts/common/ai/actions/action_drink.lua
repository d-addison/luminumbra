--[[
    action_drink.lua
    An action for drinking water, which satisfies the thirst need.
]]

local ActionDrink = {
    name = "Drink",
    cost = 2.0,
}

function ActionDrink:get_preconditions(agent)
    return { isNearWater = true }
end

function ActionDrink:get_effects(agent)
    return { isThirsty = false }
end

function ActionDrink:on_start(agent)
    print("GroveStrider ".. tostring(agent.entity) .." begins to drink.")
    agent.entity:play_animation("drink")
    agent.start_time = agent.entity:get_world_time()
end

function ActionDrink:is_finished(agent)
    -- Drink for 4 seconds
    if agent.entity:get_world_time() - agent.start_time >= 4.0 then
        local needs = agent.entity:get_component("NeedsComponent")
        needs.thirst = 0.0 -- Fully satisfy thirst
        print("GroveStrider ".. tostring(agent.entity) .." is no longer thirsty.")
        return true
    end
    return false
end

return ActionDrink