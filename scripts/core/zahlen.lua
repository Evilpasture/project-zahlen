-- Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
-- SPDX-License-Identifier: GPL-3.0-or-later

local ffi = require("scripts.core.ffi_cdef")
local mem = require("scripts.core.memoryview")
local ecs = require("scripts.core.ecs")

-- ============================================================================
-- Math Types (vec3)
-- ============================================================================
local vec3 = {}
vec3.__index = vec3

function vec3.new(x, y, z) return ffi.new("vec3", x or 0, y or 0, z or 0) end

function vec3.__add(a, b) return vec3.new(a.x + b.x, a.y + b.y, a.z + b.z) end

function vec3.__sub(a, b) return vec3.new(a.x - b.x, a.y - b.y, a.z - b.z) end

function vec3.__unm(a) return vec3.new(-a.x, -a.y, -a.z) end

function vec3.__mul(a, b)
    if type(a) == "number" then
        return vec3.new(a * b.x, a * b.y, a * b.z)
    elseif type(b) == "number" then
        return vec3.new(a.x * b, a.y * b, a.z * b)
    end
    return a.x * b.x + a.y * b.y + a.z * b.z -- Dot product
end

function vec3:length_sq() return self.x * self.x + self.y * self.y + self.z * self.z end

function vec3:length() return math.sqrt(self:length_sq()) end

function vec3:normalized()
    local len = self:length()
    if len < 0.0001 then return vec3.new(0, 0, 0) end
    return self * (1.0 / len)
end

function vec3:cross(b)
    return vec3.new(self.y * b.z - self.z * b.y, self.z * b.x - self.x * b.z, self.x * b.y - self.y * b.x)
end

function vec3:__tostring() return string.format("vec3(%.2f, %.2f, %.2f)", self.x, self.y, self.z) end

ffi.metatype("vec3", vec3)

-- ============================================================================
-- PhysicsWorld Subsystem
-- ============================================================================
local PhysicsWorld = {}
function PhysicsWorld.new(engine_raw)
    local self = { _raw = engine_raw }
    return setmetatable(self, {
        __index = function(t, key)
            -- Python-style dynamic memoryview properties
            if key == "positions" then
                return mem.C.ZHLN_GetPhysicsPositions(t._raw)
            elseif key == "velocities" then
                return mem.C.ZHLN_GetPhysicsLinearVelocities(t._raw)
            elseif key == "contacts" then
                return mem.C.ZHLN_GetPhysicsContactEvents(t._raw)
            else
                return PhysicsWorld[key]
            end
        end
    })
end

function PhysicsWorld:raycast(origin, direction, max_dist, ignore_entity)
    local res = ffi.C.ZHLN_Raycast(self._raw,
        origin.x, origin.y, origin.z,
        direction.x, direction.y, direction.z,
        max_dist or 1000.0, ignore_entity or 0)

    if res.hasHit == 1 then
        return {
            entity = res.entity,
            position = vec3.new(res.px, res.py, res.pz),
            normal = vec3.new(res.nx, res.ny, res.nz),
            fraction = res.fraction
        }
    end
    return nil
end

function PhysicsWorld:apply_impulse(entity, impulse)
    ffi.C.ZHLN_AddImpulse(self._raw, entity, impulse.x, impulse.y, impulse.z)
end

function PhysicsWorld:set_linear_velocity(entity, velocity)
    ffi.C.ZHLN_SetLinearVelocity(self._raw, entity, velocity.x, velocity.y, velocity.z)
end

function PhysicsWorld:set_character_velocity(entity, velocity)
    ffi.C.ZHLN_SetCharacterVelocity(self._raw, entity, velocity.x, velocity.y, velocity.z)
end

function PhysicsWorld:is_grounded(entity)
    return ffi.C.ZHLN_IsCharacterOnGround(self._raw, entity) == 1
end

function PhysicsWorld:setup_ragdoll(player_ent, parts_table)
    local count = #parts_table
    local parts_arr = ffi.new("uint64_t[?]", count)
    for i = 1, count do parts_arr[i - 1] = parts_table[i] end
    local args = ffi.new("SetupRagdollArgs", { player_ent, count, parts_arr })
    ffi.C.ZHLN_DispatchCommand(self._raw, "SetupRagdoll", args)
end

-- ============================================================================
-- Camera Subsystem
-- ============================================================================
local Camera = {}
function Camera.new(engine_raw)
    local self = { _raw = engine_raw }
    return setmetatable(self, {
        __index = function(t, key)
            if key == "yaw" then
                return ffi.C.ZHLN_GetCameraYaw(t._raw)
            elseif key == "fov" then
                return ffi.C.ZHLN_GetCameraFOV(t._raw)
            else
                return Camera[key]
            end
        end,
        __newindex = function(t, key, value)
            if key == "fov" then
                ffi.C.ZHLN_SetCameraFOV(t._raw, value)
            else
                rawset(t, key, value)
            end
        end
    })
end

-- ============================================================================
-- Input Subsystem
-- ============================================================================
local Input = {}
Input.__index = Input

local KEY_MAP = {
    W = 1,
    A = 2,
    S = 3,
    D = 4,
    LSHIFT = 5,
    SHIFT = 5,
    RBUTTON = 6,
    SPACE = 7,
    ESCAPE = 8,
    R = 9
}

function Input.new(engine_raw) return setmetatable({ _raw = engine_raw }, Input) end

function Input:is_key_down(key_name)
    local code = KEY_MAP[string.upper(key_name)]
    if not code then return false end
    return ffi.C.ZHLN_IsKeyDown(self._raw, code) == 1
end

function Input:get_mouse_delta()
    local x, y = ffi.new("float[1]"), ffi.new("float[1]")
    ffi.C.ZHLN_GetMouseDelta(self._raw, x, y)
    return x[0], y[0]
end

-- ============================================================================
-- Audio Subsystem
-- ============================================================================
local Audio = {}
Audio.__index = Audio
function Audio.new(engine_raw) return setmetatable({ _raw = engine_raw }, Audio) end

function Audio:play(filepath, volume)
    ffi.C.ZHLN_PlayOneShot(self._raw, filepath, volume or 1.0)
end

function Audio:play_3d(filepath, pos, volume)
    ffi.C.ZHLN_PlayOneShot3D(self._raw, filepath, pos.x, pos.y, pos.z, volume or 1.0)
end

function Audio:beep(frequency, duration, volume)
    ffi.C.ZHLN_PlayProceduralBeep(self._raw, frequency or 440.0, duration or 0.15, volume or 0.25)
end

-- ============================================================================
-- Engine Root Object
-- ============================================================================
local Engine = {}
Engine.__index = Engine

function Engine.new(raw_ptr)
    local self = setmetatable({
        _raw = raw_ptr,
        physics = PhysicsWorld.new(raw_ptr),
        camera = Camera.new(raw_ptr),
        input = Input.new(raw_ptr),
        audio = Audio.new(raw_ptr),
        ecs = ecs.new(raw_ptr),

        dialogue = require("scripts.core.dialogue"),

        -- Event system
        _events = {},
        _tracked_views = {},

        -- Logging
        log = _G.zahlen and _G.zahlen.log or print,
        warn = _G.zahlen and _G.zahlen.warn or print
    }, Engine)

    return self
end

function Engine:track_view(v)
    table.insert(self._tracked_views, v)
    return v
end

function Engine:on(event_name, callback)
    self._events[event_name] = self._events[event_name] or {}
    table.insert(self._events[event_name], callback)
end

function Engine:trigger(event_name, ...)
    local listeners = self._events[event_name]
    if listeners then
        for i = 1, #listeners do listeners[i](...) end
    end
end

-- Asset / Spawning API
function Engine:spawn(path, options)
    options = options or {}
    local pos = options.position or vec3.new(0, 0, 0)
    local create_physics = (options.physics == nil) and false or options.physics
    local is_static = (options.static == nil) and true or options.static
    local is_animated = (options.animated == nil) and false or options.animated

    local max_count = options.max_entities or 2048
    local ent_buffer = ffi.new("uint64_t[?]", max_count)
    local path_c = ffi.new("char[256]")
    ffi.copy(path_c, path)

    local args = ffi.new("SpawnPrefabArgs", {
        path_c, pos.x, pos.y, pos.z,
        create_physics and 1 or 0, is_static and 1 or 0, is_animated and 1 or 0,
        max_count, ent_buffer
    })

    local count = tonumber(ffi.C.ZHLN_DispatchCommand(self._raw, "SpawnPrefab", args))
    local entities = {}
    for i = 0, count - 1 do table.insert(entities, ent_buffer[i]) end
    return entities
end

local SHAPE_TYPES = {
    box = 0,
    sphere = 1,
    capsule = 2,
    cylinder = 3,
    plane = 4
}

function Engine:spawn_entity(options)
    options = options or {}

    local shape_str = string.lower(options.type or "box")
    local shape_type = SHAPE_TYPES[shape_str] or 0

    local p1, p2, p3 = 1, 1, 1

    -- Dynamically route keyword arguments into standard parameters
    if shape_str == "box" then
        local size = options.size or options.extents or vec3.new(1, 1, 1)
        p1, p2, p3 = size.x, size.y, size.z
    elseif shape_str == "sphere" then
        p1 = options.radius or 0.5
    elseif shape_str == "capsule" or shape_str == "cylinder" then
        p1 = options.radius or 0.5
        p2 = options.half_height or 1.0
    elseif shape_str == "plane" then
        p1 = options.extent or 10.0
    end

    local pos = options.position or vec3.new(0, 0, 0)
    local rot = options.rotation or { 0, 0, 0, 1 } -- Identity quaternion
    local col = options.color or { 1, 1, 1, 1 }
    local is_static = (options.static == nil) and true or options.static

    local args = ffi.new("SpawnEntityArgs", {
        shape_type,
        p1, p2, p3,
        pos.x, pos.y, pos.z,
        rot[1], rot[2], rot[3], rot[4],
        col[1], col[2], col[3], col[4],
        is_static and 1 or 0
    })

    local entity_raw = ffi.C.ZHLN_DispatchCommand(self._raw, "SpawnEntity", args)
    return tonumber(entity_raw)
end

function Engine:create_material(color)
    color = color or { 1, 1, 1, 1 }
    local out_pipeline = ffi.new("uint64_t[1]")
    local out_albedo = ffi.new("uint32_t[1]")
    local args = ffi.new("CreateMaterialArgs", { color[1], color[2], color[3], color[4], out_pipeline, out_albedo })
    ffi.C.ZHLN_DispatchCommand(self._raw, "CreateBasicMaterial", args)
    return out_pipeline[0], out_albedo[0]
end

function Engine:config(cfg)
    if not _G.game_state then return end
    for k, v in pairs(cfg) do
        if type(v) == "table" and type(_G.game_state[k]) == "cdata" then
            _G.game_state[k][0] = v[1] or v.x or 0
            _G.game_state[k][1] = v[2] or v.y or 0
            _G.game_state[k][2] = v[3] or v.z or 0
        else
            _G.game_state[k] = v
        end
    end
end

-- ============================================================================
-- Threading Task Scheduler
-- ============================================================================
local scheduler = { systems = {} }
function scheduler.register(name, priority, fn)
    for i = #scheduler.systems, 1, -1 do
        if scheduler.systems[i].name == name then table.remove(scheduler.systems, i) end
    end
    table.insert(scheduler.systems, { name = name, priority = priority or 100, fn = fn, enabled = true })
    table.sort(scheduler.systems, function(a, b) return a.priority < b.priority end)
end

-- ============================================================================
-- Global Host Hooks & Initialization
-- ============================================================================
local engine_ptr = ffi.C.ZHLN_GetEngineContext()

-- Expose the new Python-like root object unconditionally so `require` always works
_G.zh = Engine.new(engine_ptr)
_G.zh.vec3 = vec3.new
_G.zh.scheduler = scheduler

if engine_ptr ~= nil then
    -- Expose global backward compatibility aliases if needed
    _G.engine = _G.zh
    _G.game_ecs = _G.zh.ecs
    _G.world = _G.zh.physics

    -- Load Dialogue Trees Database
    local db = require("scripts.dialogue_db")
    for id, tree in pairs(db) do
        _G.zh.dialogue:register(id, tree)
    end

    local InventoryShell = require("scripts.core.inventory")
    _G.inventory_shell = InventoryShell.new()

    local raw_ptr = ffi.C.ZHLN_GetGameState(engine_ptr)
    if raw_ptr ~= nil then
        _G.game_state = ffi.cast("ZHLN_GameState*", raw_ptr)
    else
        -- Fallback default state
        _G.game_state = ffi.new("ZHLN_GameState")
        _G.game_state.giMode = 1
        -- ... default initializations ...
        ffi.C.ZHLN_RegisterGameState(engine_ptr, _G.game_state)
    end

    for ent, _ in _G.zh.ecs:view("MovementComponent") do
        _G.player_ent = ent
        break
    end
end

_G.update = function(ptr, dt)
    if not _G.engine_started then
        _G.engine_started = true
        _G.zh:trigger("engine.start")
    end

    _G.zh:trigger("engine.tick", dt)

    _G.zh.dialogue:update(dt)

    for i = 1, #scheduler.systems do
        local sys = scheduler.systems[i]
        if sys.enabled then sys.fn(dt) end
    end

    -- Cleanup views automatically each frame
    for i = 1, #_G.zh._tracked_views do
        _G.zh._tracked_views[i]:release()
        _G.zh._tracked_views[i] = nil
    end
    _G.zh._tracked_views = {}
end

_G.run_inventory_command = function(cmd)
    if _G.inventory_shell then
        local out = _G.inventory_shell:execute_command(cmd)
        if out ~= "" then ffi.C.ZHLN_LogInventoryShell(out) end
    end
end

return _G.zh
