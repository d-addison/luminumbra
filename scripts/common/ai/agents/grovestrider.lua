--[[
    grovestrider.lua
    Agent Definition for the GroveStrider creature.

    This script defines the core goals and capabilities of the agent for the
    Instinct Engine's GOAP planner. It does not contain the logic for the
    actions themselves, but rather tells the planner what the agent *wants*
    to achieve and what it *can* do.
]]

local GroveStrider = {}
GroveStrider.__index = GroveStrider

function GroveStrider:new(entity_handle)
    local agent = setmetatable({}, GroveStrider)
    agent.entity = entity_handle -- The C++ handle to the entity in the ECS
    return agent
end

-- This function is called by the GOAP planner to understand the agent's current state.
-- It reads from the entity's components to create a simple, boolean view of the world.
function GroveStrider:get_world_state()
    local needs = self.entity:get_component("NeedsComponent")
    local senses = self.entity:get_component("SensesComponent")

    local state = {
        isHungry = (needs.hunger > 0.7),
        isThirsty = (needs.thirst > 0.7),
        isTired = (needs.fatigue > 0.8),
        isThreatened = (senses.nearest_threat ~= nil),
        isNearFood = (senses.nearest_food_source ~= nil),
        isNearWater = (senses.nearest_water_source ~= nil),
    }

    return state
end

-- This function defines the agent's desires. The GOAP planner will attempt
-- to find a sequence of actions to satisfy the highest-priority goal.
function GroveStrider:get_goals()
    local needs = self.entity:get_component("NeedsComponent")

    -- Goals are defined as a desired world state.
    -- The priority determines how important this goal is right now.
    local goals = {
        {
            name = "be_safe",
            priority = needs.safety < 0.5 and 100 or 0, -- Highest priority: drop everything and run
            state = { isThreatened = false }
        },
        {
            name = "satisfy_hunger",
            priority = needs.hunger * 10, -- Priority increases as hunger does
            state = { isHungry = false }
        },
        {
            name = "satisfy_thirst",
            priority = needs.thirst * 10,
            state = { isThirsty = false }
        },
        {
            name = "rest",
            priority = needs.fatigue * 8,
            state = { isTired = false }
        },
        {
            name = "explore",
            priority = 1.0, -- Low-priority default behavior
            state = { hasExplored = true }
        }
    }

    return goals
end

-- This tells the planner which action scripts are available to this agent.
function GroveStrider:get_available_actions()
    return {
        "action_flee",
        "action_graze",
        "action_drink",
        "action_rest",
        "action_wander",
        "action_find_food",
        "action_find_water"
    }
end

return GroveStrider