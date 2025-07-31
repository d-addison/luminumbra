-- grovestrider.lua
-- Defines the behavior for the GroveStrider agent.

local GroveStrider = {}

function GroveStrider:new(entity_id)
  local agent = {
    id = entity_id,
    state = "Idle"
  }
  
  -- Define the available GOAP actions for this creature
  agent.actions = {
    "action_graze",
    "action_flee",
    "action_wander"
  }
  
  -- Define the primary goals
  agent.goals = {
    { name = "satisfy_hunger", priority = function(needs) return needs.hunger end },
    { name = "be_safe", priority = function(needs) return 1.0 - needs.safety end }
  }
  
  print("GroveStrider agent created for entity: " .. tostring(entity_id))
  return agent
end

return GroveStrider
