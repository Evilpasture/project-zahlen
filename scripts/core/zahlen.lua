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
            if key == "positions" then
                local view = ffi.new("ZHLN_BufferView[1]")
                local args = ffi.new("GetBufferArgs", { view })
                ffi.C.ZHLN_DispatchCommand(t._raw, "GetPhysicsPositions", args)
                return view[0]
            elseif key == "velocities" then
                local view = ffi.new("ZHLN_BufferView[1]")
                local args = ffi.new("GetBufferArgs", { view })
                ffi.C.ZHLN_DispatchCommand(t._raw, "GetPhysicsLinearVelocities", args)
                return view[0]
            elseif key == "contacts" then
                local view = ffi.new("ZHLN_BufferView[1]")
                local args = ffi.new("GetBufferArgs", { view })
                ffi.C.ZHLN_DispatchCommand(t._raw, "GetPhysicsContactEvents", args)
                return view[0]
            else
                return PhysicsWorld[key]
            end
        end
    })
end

-- (Fix applied to raycast coordinates mapping)
function PhysicsWorld:raycast(origin, direction, max_dist, ignore_entity)
    local res = ffi.new("ZHLN_RaycastResult[1]")
    local args = ffi.new("RaycastArgs", {
        origin.x, origin.y, origin.z,
        direction.x, direction.y, direction.z,
        max_dist or 1000.0, ignore_entity or 0, res
    })
    ffi.C.ZHLN_DispatchCommand(self._raw, "Raycast", args)

    if res[0].hasHit == 1 then
        return {
            entity = res[0].entity,
            position = vec3.new(res[0].px, res[0].py, res[0].pz),
            normal = vec3.new(res[0].nx, res[0].ny, res[0].nz),
            fraction = res[0].fraction
        }
    end
    return nil
end

function PhysicsWorld:apply_impulse(entity, impulse)
    local args = ffi.new("SetCharVelArgs", { entity, impulse.x, impulse.y, impulse.z })
    ffi.C.ZHLN_DispatchCommand(self._raw, "AddImpulse", args)
end

function PhysicsWorld:set_linear_velocity(entity, velocity)
    local args = ffi.new("SetCharVelArgs", { entity, velocity.x, velocity.y, velocity.z })
    ffi.C.ZHLN_DispatchCommand(self._raw, "SetLinearVelocity", args)
end

function PhysicsWorld:set_character_velocity(entity, velocity)
    local args = ffi.new("SetCharVelArgs", { entity, velocity.x, velocity.y, velocity.z })
    ffi.C.ZHLN_DispatchCommand(self._raw, "SetCharacterVelocity", args)
end

function PhysicsWorld:is_grounded(entity)
    local args = ffi.new("EntityOnlyArgs", { entity })
    return ffi.C.ZHLN_DispatchCommand(self._raw, "IsCharacterOnGround", args) == 1
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
                local fargs = ffi.new("float[1]")
                local args = ffi.new("CameraFloatArgs", { fargs })
                ffi.C.ZHLN_DispatchCommand(t._raw, "GetCameraYaw", args)
                return fargs[0]
            elseif key == "fov" then
                local fargs = ffi.new("float[1]")
                local args = ffi.new("CameraFloatArgs", { fargs })
                ffi.C.ZHLN_DispatchCommand(t._raw, "GetCameraFOV", args)
                return fargs[0]
            else
                return Camera[key]
            end
        end,
        __newindex = function(t, key, value)
            if key == "fov" then
                local args = ffi.new("SetCameraFOVArgs", { value })
                ffi.C.ZHLN_DispatchCommand(t._raw, "SetCameraFOV", args)
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
    R = 9,
    E = 10,
    LBUTTON = 11
}

function Input.new(engine_raw) return setmetatable({ _raw = engine_raw }, Input) end

function Input:is_key_down(key_name)
    local code = KEY_MAP[string.upper(key_name)]
    if not code then return false end
    local args = ffi.new("IsKeyDownArgs", { code })
    return ffi.C.ZHLN_DispatchCommand(self._raw, "IsKeyDown", args) == 1
end

function Input:get_mouse_delta()
    local x = ffi.new("float[1]")
    local y = ffi.new("float[1]")
    local args = ffi.new("GetMouseDeltaArgs", { x, y })
    ffi.C.ZHLN_DispatchCommand(self._raw, "GetMouseDelta", args)
    return x[0], y[0]
end

-- ============================================================================
-- Audio Subsystem
-- ============================================================================
local Audio = {}
Audio.__index = Audio
function Audio.new(engine_raw) return setmetatable({ _raw = engine_raw }, Audio) end

function Audio:play(filepath, volume)
    local args = ffi.new("PlayOneShotArgs", { filepath, volume or 1.0 })
    ffi.C.ZHLN_DispatchCommand(self._raw, "PlayOneShot", args)
end

function Audio:play_3d(filepath, pos, volume)
    local args = ffi.new("PlayOneShot3DArgs", { filepath, pos.x, pos.y, pos.z, volume or 1.0 })
    ffi.C.ZHLN_DispatchCommand(self._raw, "PlayOneShot3D", args)
end

function Audio:beep(frequency, duration, volume)
    local args = ffi.new("PlayProceduralBeepArgs", { frequency or 440.0, duration or 0.15, volume or 0.25 })
    ffi.C.ZHLN_DispatchCommand(self._raw, "PlayProceduralBeep", args)
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
        settings = { pp = nil, aa = nil },

        dialogue = require("scripts.core.dialogue"),

        _events = {},
        _tracked_views = {},

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

local SHAPE_TYPES = { box = 0, sphere = 1, capsule = 2, cylinder = 3, plane = 4 }

function Engine:spawn_entity(options)
    options = options or {}
    local shape_str = string.lower(options.type or "box")
    local shape_type = SHAPE_TYPES[shape_str] or 0
    local p1, p2, p3 = 1, 1, 1

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
    local rot = options.rotation or { 0, 0, 0, 1 }
    local col = options.color or { 1, 1, 1, 1 }
    local is_static = (options.static == nil) and true or options.static

    local args = ffi.new("SpawnEntityArgs", {
        shape_type, p1, p2, p3, pos.x, pos.y, pos.z,
        rot[1], rot[2], rot[3], rot[4], col[1], col[2], col[3], col[4],
        is_static and 1 or 0
    })

    return tonumber(ffi.C.ZHLN_DispatchCommand(self._raw, "SpawnEntity", args))
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
    local pp = self.settings.pp
    local aa = self.settings.aa

    if cfg.giMode and pp then pp[0].giMode = cfg.giMode end
    if cfg.aoRadius and pp then pp[0].aoRadius = cfg.aoRadius end
    if cfg.aoBias and pp then pp[0].aoBias = cfg.aoBias end
    if cfg.aoPower and pp then pp[0].aoPower = cfg.aoPower end
    if cfg.giIntensity and pp then pp[0].giIntensity = cfg.giIntensity end
    if cfg.giSamples and pp then pp[0].giSamples = cfg.giSamples end
    if cfg.useLocalProbe and pp then pp[0].useLocalProbe = cfg.useLocalProbe end

    if cfg.probeMin and pp then
        pp[0].probeMin[0] = cfg.probeMin[1] or cfg.probeMin.x or 0
        pp[0].probeMin[1] = cfg.probeMin[2] or cfg.probeMin.y or 0
        pp[0].probeMin[2] = cfg.probeMin[3] or cfg.probeMin.z or 0
    end
    if cfg.probeMax and pp then
        pp[0].probeMax[0] = cfg.probeMax[1] or cfg.probeMax.x or 0
        pp[0].probeMax[1] = cfg.probeMax[2] or cfg.probeMax.y or 0
        pp[0].probeMax[2] = cfg.probeMax[3] or cfg.probeMax.z or 0
    end
    if cfg.probePos and pp then
        pp[0].probePos[0] = cfg.probePos[1] or cfg.probePos.x or 0
        pp[0].probePos[1] = cfg.probePos[2] or cfg.probePos.y or 0
        pp[0].probePos[2] = cfg.probePos[3] or cfg.probePos.z or 0
    end

    if cfg.vignetteIntensity and pp then pp[0].vignetteIntensity = cfg.vignetteIntensity end
    if cfg.vignettePower and pp then pp[0].vignettePower = cfg.vignettePower end
    if cfg.enableSSR ~= nil and pp then pp[0].enableSSR = cfg.enableSSR end
    if cfg.enableRTR ~= nil and pp then pp[0].enableRTR = cfg.enableRTR end

    if cfg.enableTAA ~= nil and aa then
        aa[0].state.mode = cfg.enableTAA == 1 and 2 or 0
    end
    if cfg.taaFeedback and aa then aa[0].state.taaFeedback = cfg.taaFeedback end
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

_G.zh = Engine.new(engine_ptr)
_G.zh.vec3 = vec3.new
_G.zh.scheduler = scheduler

if engine_ptr ~= nil then
    _G.engine = _G.zh
    _G.game_ecs = _G.zh.ecs
    _G.world = _G.zh.physics

    local db = require("scripts.dialogue_db")
    for id, tree in pairs(db) do
        _G.zh.dialogue:register(id, tree)
    end

    local InventoryShell = require("scripts.core.inventory")
    _G.inventory_shell = InventoryShell.new()

    -- Pull Singleton Settings Components directly from ECS
    local pp_view = ffi.new("ZHLN_BufferView[1]")
    ffi.C.ZHLN_DispatchCommand(engine_ptr, "GetECSBuffer",
        ffi.new("GetECSBufferArgs", { "PostProcessSettingsComponent", pp_view }))
    if pp_view[0].buf ~= nil then
        _G.zh.settings.pp = ffi.cast("PostProcessSettingsComponent*", pp_view[0].buf)
    end
    -- Immediately release the Registry's structural shadow lock!
    if pp_view[0].obj ~= nil then
        local rel_args = ffi.new("ReleaseBufferArgs", { pp_view[0].obj })
        ffi.C.ZHLN_DispatchCommand(nil, "ReleaseBuffer", rel_args)
    end

    local aa_view = ffi.new("ZHLN_BufferView[1]")
    ffi.C.ZHLN_DispatchCommand(engine_ptr, "GetECSBuffer",
        ffi.new("GetECSBufferArgs", { "AASettingsComponent", aa_view }))
    if aa_view[0].buf ~= nil then
        _G.zh.settings.aa = ffi.cast("AASettingsComponent*", aa_view[0].buf)
    end
    -- Immediately release the Registry's structural shadow lock!
    if aa_view[0].obj ~= nil then
        local rel_args = ffi.new("ReleaseBufferArgs", { aa_view[0].obj })
        ffi.C.ZHLN_DispatchCommand(nil, "ReleaseBuffer", rel_args)
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

    for i = 1, #_G.zh._tracked_views do
        _G.zh._tracked_views[i]:release()
        _G.zh._tracked_views[i] = nil
    end
    _G.zh._tracked_views = {}
end

_G.run_inventory_command = function(cmd)
    if _G.inventory_shell then
        local out = _G.inventory_shell:execute_command(cmd)
        if out ~= "" then
            local args = ffi.new("LogInventoryArgs", { out })
            ffi.C.ZHLN_DispatchCommand(engine_ptr, "LogInventoryShell", args)
        end
    end
end

return _G.zh
