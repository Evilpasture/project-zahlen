-- scripts/core/zahlen.lua
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

function vec3.__unm(a) return ffi.new("vec3", -a.x, -a.y, -a.z) end

function vec3.__mul(a, b)
    if type(a) == "number" then
        return ffi.new("vec3", a * b.x, a * b.y, a * b.z)
    elseif type(b) == "number" then
        return ffi.new("vec3", a.x * b, a.y * b, a.z * b)
    end
    return a.x * b.x + a.y * b.y + a.z * b.z
end

function vec3:length_sq() return self.x * self.x + self.y * self.y + self.z * self.z end

function vec3:length() return math.sqrt(self:length_sq()) end

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

ffi.metatype("vec3", vec3)

local zahlen = {}
function zahlen.vec3(x, y, z) return ffi.new("vec3", x or 0, y or 0, z or 0) end

local tracked_views = {}

local function track(v)
    table.insert(tracked_views, v)
    return v
end

-- ============================================================================
-- System Scheduler (Supports Hot-Reload Swapping)
-- ============================================================================
zahlen.scheduler = {
    systems = {}
}

function zahlen.scheduler.register(name, priority, fn)
    -- Remove any old system of the same name to allow seamless hot-reloads
    for i = #zahlen.scheduler.systems, 1, -1 do
        if zahlen.scheduler.systems[i].name == name then
            table.remove(zahlen.scheduler.systems, i)
        end
    end

    table.insert(zahlen.scheduler.systems, {
        name = name,
        priority = priority or 100,
        fn = fn,
        enabled = true
    })
    table.sort(zahlen.scheduler.systems, function(a, b) return a.priority < b.priority end)
end

-- ============================================================================
-- Cooperative Task Scheduler
-- ============================================================================
zahlen.task = {}
local active_tasks = {}
local pending_values = {}

function zahlen.task.dispatch(fn)
    local co = coroutine.create(fn)
    table.insert(active_tasks, co)
end

function zahlen.task.update()
    local i = 1
    while i <= #active_tasks do
        local co = active_tasks[i]
        if coroutine.status(co) == "dead" then
            table.remove(active_tasks, i)
        else
            local val = pending_values[co]
            pending_values[co] = nil

            local ok, res = coroutine.resume(co, val)
            if not ok then
                _G.zahlen.log("Error in Task: " .. tostring(res))
                table.remove(active_tasks, i)
            elseif res == "WAIT_CHANNEL" then
                table.remove(active_tasks, i)
            else
                i = i + 1
            end
        end
    end
end

function zahlen.cleanup()
    for i = 1, #tracked_views do
        tracked_views[i]:release()
        tracked_views[i] = nil
    end
    tracked_views = {}
    zahlen.task.update()
end

-- ============================================================================
-- World Class
-- ============================================================================
local World = {}
World.__index = function(self, key)
    if key == "positions" then return track(mem.C.ZHLN_GetPhysicsPositions(self.engine)) end
    if key == "velocities" then return track(mem.C.ZHLN_GetPhysicsLinearVelocities(self.engine)) end
    if key == "contacts" then return track(mem.C.ZHLN_GetPhysicsContactEvents(self.engine)) end
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
    W = 1,
    w = 1,
    A = 2,
    a = 2,
    S = 3,
    s = 3,
    D = 4,
    d = 4,
    LSHIFT = 5,
    lshift = 5,
    SHIFT = 5,
    shift = 5,
    RBUTTON = 6,
    rbutton = 6,
    SPACE = 7,
    space = 7,
    R = 9,
    r = 9
}

local Engine = {}
Engine.__index = Engine

function Engine:world()
    return setmetatable({ engine = self.raw }, World)
end

function Engine:get_camera_yaw()
    return mem.C.ZHLN_GetCameraYaw(self.raw)
end

function Engine:get_camera_fov()
    return mem.C.ZHLN_GetCameraFOV(self.raw)
end

function Engine:set_camera_fov(fov)
    mem.C.ZHLN_SetCameraFOV(self.raw, fov)
end

function Engine:is_key_down(key)
    local code = KEY_MAP[key]
    if not code then return false end
    return mem.C.ZHLN_IsKeyDown(self.raw, code) == 1
end

function Engine:play_sound(filepath, volume)
    mem.C.ZHLN_PlayOneShot(self.raw, filepath, volume or 1.0)
end

function Engine:play_sound_3d(filepath, x, y, z, volume)
    mem.C.ZHLN_PlayOneShot3D(self.raw, filepath, x, y, z, volume or 1.0)
end

function Engine:beep(frequency, duration, volume)
    mem.C.ZHLN_PlayProceduralBeep(self.raw, frequency or 440.0, duration or 0.15, volume or 0.25)
end

function zahlen.wrap(ptr)
    return setmetatable({ raw = ptr }, Engine)
end

zahlen.log = _G.zahlen.log
zahlen.warn = _G.zahlen.warn

zahlen.ecs = ecs

-- ============================================================================
-- Message Channels (Pure Lua)
-- ============================================================================
local Channel = {}
Channel.__index = Channel

function zahlen.create_channel()
    return setmetatable({
        queue = {},
        waiters = {}
    }, Channel)
end

function Channel:destroy()
    self.queue = {}
    self.waiters = {}
end

function Channel:push(val)
    if #self.waiters > 0 then
        local co = table.remove(self.waiters, 1)
        pending_values[co] = val
        table.insert(active_tasks, co)
    else
        table.insert(self.queue, val)
    end
end

function Channel:pop()
    local co = coroutine.running()
    if not co then
        error("Channel:pop() must be run within a Dispatch task!")
    end

    if #self.queue > 0 then
        return table.remove(self.queue, 1)
    end

    table.insert(self.waiters, co)
    return coroutine.yield("WAIT_CHANNEL")
end

-- ============================================================================
-- Global Framework Entry Points (Launches & Orchestrates on first frame)
-- ============================================================================
_G.update = function(ptr, dt)
    -- 1. Lazy-initialize FFI wrappers on first frame
    if not _G.engine then
        _G.engine = zahlen.wrap(ptr)
        _G.world = _G.engine:world()
        _G.game_ecs = ecs.new(ptr)

        local InventoryShell = require("scripts.core.inventory")
        _G.inventory_shell = InventoryShell.new()

        zahlen.log("Core: Engine and ECS wraps successfully initialized.")
    end

    -- 2. Autodiscover and bind Player Entity dynamically
    if not _G.player_ent then
        for ent, movement in _G.game_ecs:view("MovementComponent") do
            _G.player_ent = ent
            break
        end

        if _G.player_ent then
            _G.game_ecs:add(_G.player_ent, "combat", { hp = 100, max_hp = 100, is_poisoned = false })
            _G.game_ecs:add(_G.player_ent, "inventory", { coins = 0, equipped = nil })
            zahlen.log("Core: Dynamically bound player state to entity " .. tostring(_G.player_ent))

            local eyes = _G.game_ecs:find("pomni_eyes")
            if eyes then
                zahlen.log("Core: Located eyes at entity " .. tostring(eyes))
            end
        end
    end

    -- 3. Execute all registered systems sequentially
    for i = 1, #zahlen.scheduler.systems do
        local sys = zahlen.scheduler.systems[i]
        if sys.enabled then
            sys.fn(dt)
        end
    end
end

_G.run_inventory_command = function(cmd)
    if _G.inventory_shell then
        local out = _G.inventory_shell:execute_command(cmd)
        if out ~= "" then
            ffi.C.ZHLN_LogInventoryShell(out)
        end
    end
end

return zahlen
