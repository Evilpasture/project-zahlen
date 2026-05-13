local mem = require("scripts.core.memoryview")
local ffi = require("ffi")

local zhln = {}
local tracked_views = {}

-- Internal: Tracks a view so it can be forced-released at end of frame
local function track(v)
    table.insert(tracked_views, v)
    return v
end

-- Force-releases all buffers tracked this frame
function zhln.cleanup()
    for i = 1, #tracked_views do
        tracked_views[i]:release()
        tracked_views[i] = nil
    end
    tracked_views = {}
end

-- --- World Class ---
local World = {}
World.__index = function(self, key)
    if key == "positions"  then return track(mem.C.ZHLN_GetPhysicsPositions(self.engine)) end
    if key == "velocities" then return track(mem.C.ZHLN_GetPhysicsLinearVelocities(self.engine)) end
    return World[key]
end

function World:set_linear_velocity(handle, x, y, z)
    mem.C.ZHLN_SetLinearVelocity(self.engine, handle, x, y, z)
end

function World:set_character_velocity(handle, x, y, z)
    mem.C.ZHLN_SetCharacterVelocity(self.engine, handle, x, y, z)
end

function World:is_on_ground(handle)
    return mem.C.ZHLN_IsCharacterOnGround(self.engine, handle) == 1
end

function World:get_entities(comp)
    return track(mem.C.ZHLN_GetECSEntities(self.engine, comp or "PhysicsComponent"))
end

function World:get_component_buffer(comp)
    return track(mem.C.ZHLN_GetECSBuffer(self.engine, comp or "PhysicsComponent"))
end

-- --- Engine Class ---
local Engine = {}
Engine.__index = Engine

function Engine:world()
    return setmetatable({ engine = self.raw }, World)
end

function Engine:get_camera_yaw()
    return mem.C.ZHLN_GetCameraYaw(self.raw)
end

function Engine:is_key_down(key)
    local map = { W=1, A=2, S=3, D=4, SHIFT=5 }
    return mem.C.ZHLN_IsKeyDown(self.raw, map[key:upper()] or 0) == 1
end

function zhln.wrap(ptr)
    return setmetatable({ raw = ptr }, Engine)
end

-- Redefine standard logging to feel like Python
function zhln.log(...)
    -- Calling the C-function registered in Step 2
    -- It will automatically find the file/line of the person calling zhln.log
---@diagnostic disable-next-line: undefined-field
    _G.zhln.log(...)
end

function World:apply_impulse(handle, x, y, z)
    mem.C.ZHLN_AddImpulse(self.engine, handle, x, y, z)
end

return zhln