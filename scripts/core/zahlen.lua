local mem = require("scripts.core.memoryview")
local ffi = require("scripts.core.ffi_cdef")
local ecs = require("scripts.core.ecs")

-- ============================================================================
-- Vector3 Math Implementation (Zero-Allocation)
-- ============================================================================
local vec3 = {}
vec3.__index = vec3

function vec3.__add(a, b) return ffi.new("vec3", a.x + b.x, a.y + b.y, a.z + b.z) end
function vec3.__sub(a, b) return ffi.new("vec3", a.x - b.x, a.y - b.y, a.z - b.z) end
function vec3.__unm(a)    return ffi.new("vec3", -a.x, -a.y, -a.z) end

function vec3.__mul(a, b)
    if type(a) == "number" then
        return ffi.new("vec3", a * b.x, a * b.y, a * b.z)
    elseif type(b) == "number" then
        return ffi.new("vec3", a.x * b, a.y * b, a.z * b)
    end
    -- Dot product if both are vectors
    return a.x * b.x + a.y * b.y + a.z * b.z
end

function vec3:length_sq() return self.x*self.x + self.y*self.y + self.z*self.z end
function vec3:length()    return math.sqrt(self:length_sq()) end

function vec3:normalized()
    local len = self:length()
    if len < 0.0001 then return ffi.new("vec3", 0, 0, 0) end
    return self * (1.0 / len)
end

function vec3:cross(b)
    return ffi.new("vec3", 
        self.y * b.z - self.z * b.y,
        self.z * b.x - self.x * b.z,
        self.x * b.y - self.y * b.x
    )
end

-- IN-PLACE MUTATORS (High Performance, avoids JIT aborts)
function vec3:add_inplace(b)
    self.x = self.x + b.x
    self.y = self.y + b.y
    self.z = self.z + b.z
    return self
end

function vec3:sub_inplace(b)
    self.x = self.x - b.x
    self.y = self.y - b.y
    self.z = self.z - b.z
    return self
end

function vec3:scale_inplace(scalar)
    self.x = self.x * scalar
    self.y = self.y * scalar
    self.z = self.z * scalar
    return self
end

function vec3:__tostring()
    return string.format("vec3(%.2f, %.2f, %.2f)", self.x, self.y, self.z)
end

-- Register the type with LuaJIT
ffi.metatype("vec3", vec3)

-- Helper for the user to create new vectors
local zahlen = {}
function zahlen.vec3(x, y, z) return ffi.new("vec3", x or 0, y or 0, z or 0) end


local tracked_views = {}

-- Internal: Tracks a view so it can be forced-released at end of frame
local function track(v)
    table.insert(tracked_views, v)
    return v
end

-- Force-releases all buffers tracked this frame
function zahlen.cleanup()
    for i = 1, #tracked_views do
        tracked_views[i]:release()
        tracked_views[i] = nil
    end
    tracked_views = {}
end

-- ============================================================================
-- World Class
-- ============================================================================
local World = {}
World.__index = function(self, key)
    if key == "positions"  then return track(mem.C.ZHLN_GetPhysicsPositions(self.engine)) end
    if key == "velocities" then return track(mem.C.ZHLN_GetPhysicsLinearVelocities(self.engine)) end
    if key == "contacts"   then return track(mem.C.ZHLN_GetPhysicsContactEvents(self.engine)) end
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

function World:apply_impulse(handle, x_or_v, y, z)
    if type(x_or_v) == "cdata" then
        mem.C.ZHLN_AddImpulse(self.engine, handle, x_or_v.x, x_or_v.y, x_or_v.z)
    else
        mem.C.ZHLN_AddImpulse(self.engine, handle, x_or_v, y, z)
    end
end

function World:raycast(ox, oy, oz, dx, dy, dz, max_dist, ignore_handle)
    max_dist = max_dist or 1000.0
    ignore_handle = ignore_handle or 0ULL
    
    local res = mem.C.ZHLN_Raycast(self.engine, ox, oy, oz, dx, dy, dz, max_dist, ignore_handle)
    
    if res.hasHit == 1 then
        return {
            entity = res.entity,
            p = ffi.new("vec3", res.px, res.py, res.pz),
            n = ffi.new("vec3", res.nx, res.ny, res.nz),
            fraction = res.fraction
        }
    end
    return nil
end

function World:set_movement_input(handle, x, z)
    mem.C.ZHLN_SetMovementInput(self.engine, handle, x, z)
end

function World:set_jump_intent(handle)
    mem.C.ZHLN_SetJumpIntent(self.engine, handle)
end

-- ============================================================================
-- Engine Class
-- ============================================================================

local KEY_MAP = {
    W = 1, w = 1,
    A = 2, a = 2,
    S = 3, s = 3,
    D = 4, d = 4,
    LSHIFT = 5, lshift = 5, SHIFT = 5, shift = 5,
    RBUTTON = 6, rbutton = 6,
    SPACE = 7, space = 7
}

local Engine = {}
Engine.__index = Engine

function Engine:world()
    return setmetatable({ engine = self.raw }, World)
end

function Engine:get_camera_yaw()
    return mem.C.ZHLN_GetCameraYaw(self.raw)
end

function Engine:is_key_down(key)
    local code = KEY_MAP[key]
    if not code then return false end
    return mem.C.ZHLN_IsKeyDown(self.raw, code) == 1
end

function zahlen.wrap(ptr)
    return setmetatable({ raw = ptr }, Engine)
end

-- Redefine standard logging to feel like Python
function zahlen.log(...)
---@diagnostic disable-next-line: undefined-field
    _G.zahlen.log(...)
end

zahlen.ecs = ecs

return zahlen