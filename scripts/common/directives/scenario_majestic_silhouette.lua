--[[
    scenario_majestic_silhouette.lua
    A world directive that attempts to create a specific, aesthetically
    pleasing scene at sunset. It finds a GroveStrider and guides it to a
    scenic viewpoint.
]]

local Scenario = {}

-- Called by the C++ DirectiveSystem when it's time to run this scenario.
function Scenario:on_start()
    print("Directive Started: Majestic Silhouette")
    
    self.world_api = self.entity:get_world_api() -- Assume a way to get world context
    self.grove_strider = self.world_api:find_entity_by_archetype("creature_grovestrider")
    self.scenic_point = self.world_api:find_location_with_tag("scenic_vista_west")

    if not self.grove_strider or not self.scenic_point then
        print("Directive failed: Could not find required entity or location.")
        self:end_early()
        return
    end

    -- Give the GroveStrider a temporary, high-priority, scripted goal.
    -- This will override its normal AI until the directive is complete.
    self.grove_strider:override_ai_goal({
        name = "Reach Silhouette Point",
        destination = self.scenic_point.position
    })

    self.state = "moving_to_point"
end

-- Called every tick by the DirectiveSystem.
function Scenario:on_update(dt)
    if self.state == "moving_to_point" then
        if self.grove_strider:has_reached_destination() then
            print("GroveStrider has reached the scenic point.")
            self.grove_strider:play_animation("idle_look_out")
            self.state = "waiting_for_sunset"
        end
    elseif self.state == "waiting_for_sunset" then
        local time_of_day = self.world_api:get_time_of_day()
        if time_of_day > 0.75 and time_of_day < 0.8 then -- Assuming 0.75 is sunset
            print("The perfect moment! Capturing silhouette.")
            self.world_api:focus_scenic_camera(self.scenic_point, self.grove_strider)
            self:end_scenario()
        end
    end
end

function Scenario:end_early()
    print("Directive ending prematurely.")
    self.state = "finished"
end

function Scenario:end_scenario()
    print("Directive 'Majestic Silhouette' complete.")
    if self.grove_strider then
        self.grove_strider:release_ai_override()
    end
    self.state = "finished"
end

function Scenario:is_finished()
    return self.state == "finished"
end

return Scenario