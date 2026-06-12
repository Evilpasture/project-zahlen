-- Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
-- SPDX-License-Identifier: GPL-3.0-or-later


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

local Engine = {}
Engine.__index = Engine

function Engine:world()
    return setmetatable({ engine = self.raw }, {
        __index = function(t, k)
            if k == "positions" then return track(mem.C.ZHLN_GetPhysicsPositions(t.engine)) end
            if k == "velocities" then return track(mem.C.ZHLN_GetPhysicsLinearVelocities(t.engine)) end
            if k == "contacts" then return track(mem.C.ZHLN_GetPhysicsContactEvents(t.engine)) end
        end
    })
end

function Engine:get_camera_yaw() return mem.C.ZHLN_GetCameraYaw(self.raw) end

function Engine:get_camera_fov() return mem.C.ZHLN_GetCameraFOV(self.raw) end

function Engine:set_camera_fov(fov) mem.C.ZHLN_SetCameraFOV(self.raw, fov) end

function Engine:is_key_down(key)
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
    local code = KEY_MAP[key]
    if not code then return false end
    return mem.C.ZHLN_IsKeyDown(self.raw, code) == 1
end

function Engine:play_sound(filepath, volume) mem.C.ZHLN_PlayOneShot(self.raw, filepath, volume or 1.0) end

function Engine:play_sound_3d(filepath, x, y, z, volume)
    mem.C.ZHLN_PlayOneShot3D(self.raw, filepath, x, y, z,
        volume or 1.0)
end

function Engine:beep(frequency, duration, volume)
    mem.C.ZHLN_PlayProceduralBeep(self.raw, frequency or 440.0,
        duration or 0.15, volume or 0.25)
end

local function wrap(ptr)
    return setmetatable({ raw = ptr }, Engine)
end

local tracked_views = {}
local function track(v)
    table.insert(tracked_views, v)
    return v
end

-- ============================================================================
-- Modular zahlen Scheduler
-- ============================================================================
local scheduler = { systems = {} }
function scheduler.register(name, priority, fn)
    for i = #scheduler.systems, 1, -1 do
        if scheduler.systems[i].name == name then
            table.remove(scheduler.systems, i)
        end
    end
    table.insert(scheduler.systems, { name = name, priority = priority or 100, fn = fn, enabled = true })
    table.sort(scheduler.systems, function(a, b) return a.priority < b.priority end)
end

-- ============================================================================
-- The Declarative "zh" Namespace (Hyprland style)
-- ============================================================================
local zh = {}
local event_registry = {}

zh.scheduler = scheduler
zh.vec3 = function(x, y, z) return ffi.new("vec3", x or 0, y or 0, z or 0) end

-- Bind logging utilities directly to FFI C-functions
zh.log = _G.zahlen.log
zh.warn = _G.zahlen.warn

function zh.config(cfg)
    if not _G.game_state then return end

    local function apply(src, dest)
        for k, v in pairs(src) do
            if type(v) == "table" then
                if type(dest[k]) == "cdata" and ffi.istype("float[3]", dest[k]) then
                    dest[k][0] = v[1] or v.x or 0
                    dest[k][1] = v[2] or v.y or 0
                    dest[k][2] = v[3] or v.z or 0
                else
                    apply(v, dest[k])
                end
            else
                dest[k] = v
            end
        end
    end
    apply(cfg, _G.game_state)
end

function zh.on(event, fn)
    event_registry[event] = event_registry[event] or {}
    table.insert(event_registry[event], fn)
end

function zh.trigger(event, ...)
    local listeners = event_registry[event]
    if listeners then
        for i = 1, #listeners do
            listeners[i](...)
        end
    end
end

-- --- dsp Action Generators ---
zh.dsp = {
    sound = {
        play = function(path, volume) return { type = "PlaySound", path = path, volume = volume or 1.0 } end,
        play_3d = function(path, x, y, z, volume)
            return {
                type = "PlaySound3D",
                path = path,
                x = x,
                y = y,
                z = z,
                volume =
                    volume or 1.0
            }
        end,
        beep = function(freq, duration, volume)
            return {
                type = "Beep",
                freq = freq,
                duration = duration,
                volume = volume or
                    0.25
            }
        end
    },
    physics = {
        add_impulse = function(ent, x, y, z) return { type = "AddImpulse", ent = ent, x = x, y = y, z = z } end,
        set_velocity = function(ent, x, y, z) return { type = "SetVelocity", ent = ent, x = x, y = y, z = z } end
    },
    ragdoll = {
        setup = function(player, parts) return { type = "SetupRagdoll", player = player, parts = parts } end
    }
}

-- --- Unified Opaque Dispatcher ---
function zh.dispatch(act)
    if not act then return end

    if act.type == "PlaySound" then
        _G.engine:play_sound(act.path, act.volume)
    elseif act.type == "PlaySound3D" then
        _G.engine:play_sound_3d(act.path, act.x, act.y, act.z, act.volume)
    elseif act.type == "Beep" then
        _G.engine:beep(act.freq, act.duration, act.volume)
    elseif act.type == "AddImpulse" then
        local raw_v = _G.game_ecs:get(act.ent, "PhysicsComponent")
        if raw_v then
            ffi.C.ZHLN_AddImpulse(_G.engine.raw, raw_v.physicsHandle, act.x, act.y, act.z)
        end
    elseif act.type == "SetVelocity" then
        local raw_v = _G.game_ecs:get(act.ent, "PhysicsComponent")
        if raw_v then
            ffi.C.ZHLN_SetCharacterVelocity(_G.engine.raw, raw_v.physicsHandle, act.x, act.y, act.z)
        end
    elseif act.type == "SetupRagdoll" then
        local count = #act.parts
        local parts_arr = ffi.new("uint64_t[?]", count)
        for i = 1, count do parts_arr[i - 1] = act.parts[i] end
        local args = ffi.new("SetupRagdollArgs", { act.player, count, parts_arr })
        ffi.C.ZHLN_DispatchCommand(_G.engine.raw, "SetupRagdoll", args)
    end
end

function zh.spawn(path, options)
    options = options or {}
    local pos = options.position or { 0, 0, 0 }
    local create_physics = (options.physics == nil) and false or options.physics
    local is_static = (options.static == nil) and true or options.static
    local is_animated = (options.animated == nil) and false or options.animated

    local max_count = options.max_entities or 2048
    local ent_buffer = ffi.new("uint64_t[?]", max_count)

    local path_c = ffi.new("char[256]")
    ffi.copy(path_c, path)

    local args = ffi.new("SpawnPrefabArgs", {
        path_c,
        pos[1] or pos.x or 0,
        pos[2] or pos.y or 0,
        pos[3] or pos.z or 0,
        create_physics and 1 or 0,
        is_static and 1 or 0,
        is_animated and 1 or 0,
        max_count,
        ent_buffer
    })

    local count = tonumber(ffi.C.ZHLN_DispatchCommand(_G.engine.raw, "SpawnPrefab", args))
    if count == 0 then return {} end

    local entities = {}
    for i = 0, count - 1 do table.insert(entities, ent_buffer[i]) end
    return entities
end

function zh.find(name_query)
    if not _G.game_ecs then return nil end
    local query = name_query:lower()
    for ent, name_comp in _G.game_ecs:view("NameComponent") do
        local name = ffi.string(name_comp.name):lower()
        if name:find(query) then return ent end
    end
    return nil
end

function zh.create_box(hx, hy, hz, r, g, b, a)
    local args = ffi.new("CreateBoxArgs", { hx, hy, hz, r or 1, g or 1, b or 1, a or 1 })
    return ffi.C.ZHLN_DispatchCommand(_G.engine.raw, "CreateBox", args)
end

function zh.create_material(r, g, b, a)
    local out_pipeline = ffi.new("uint64_t[1]")
    local out_albedo = ffi.new("uint32_t[1]")
    local args = ffi.new("CreateMaterialArgs", { r or 1, g or 1, b or 1, a or 1, out_pipeline, out_albedo })
    ffi.C.ZHLN_DispatchCommand(_G.engine.raw, "CreateBasicMaterial", args)
    return out_pipeline[0], out_albedo[0]
end

function zh.register_debug_line(mesh_vbo, pipeline, albedo_idx)
    if _G.game_state then
        _G.game_state.debugLineVbo = mesh_vbo
        _G.game_state.debugLinePipeline = pipeline
        _G.game_state.debugLineAlbedo = albedo_idx
    end
end

function zh.register_player_parts(visual_parts)
    if not visual_parts or not _G.game_state then return end
    local count = math.min(#visual_parts, 128)
    _G.game_state.playerPartsCount = count
    for i = 1, count do
        _G.game_state.playerParts[i - 1] = visual_parts[i]
    end
end

function zh.is_key_down(key)
    if not _G.engine then return false end
    return _G.engine:is_key_down(key)
end

-- ============================================================================
-- Threading Task Scheduler
-- ============================================================================
zahlen = {
    task = {},
    create_channel = function()
        return setmetatable({ queue = {}, waiters = {} }, {
            __index = function(t, k)
                if k == "push" then
                    return function(self, val)
                        if #self.waiters > 0 then
                            local co = table.remove(self.waiters, 1)
                            pending_values[co] = val
                            table.insert(active_tasks, co)
                        else
                            table.insert(self.queue, val)
                        end
                    end
                elseif k == "pop" then
                    return function(self)
                        local co = coroutine.running()
                        if not co then error("Channel:pop() must be run within a Dispatch task!") end
                        if #self.queue > 0 then return table.remove(self.queue, 1) end
                        table.insert(self.waiters, co)
                        return coroutine.yield("WAIT_CHANNEL")
                    end
                end
            end
        })
    end
}

zahlen.task.dispatch = function(fn)
    local co = coroutine.create(fn)
    table.insert(active_tasks, co)
end

zahlen.task.update = function()
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
                zh.warn("Error in Task: " .. tostring(res))
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

-- --- Compile-time Immediate Context Binding ---
local engine_ptr = ffi.C.ZHLN_GetEngineContext()
if engine_ptr ~= nil then
    _G.engine = wrap(engine_ptr)
    _G.world = _G.engine:world()
    _G.game_ecs = ecs.new(engine_ptr)

    local InventoryShell = require("scripts.core.inventory")
    _G.inventory_shell = InventoryShell.new()

    local raw_ptr = ffi.C.ZHLN_GetGameState(engine_ptr)
    if raw_ptr ~= nil then
        _G.game_state = ffi.cast("ZHLN_GameState*", raw_ptr)
    else
        _G.game_state = ffi.new("ZHLN_GameState")
        _G.game_state.giMode = 1
        _G.game_state.aoRadius = 0.5
        _G.game_state.aoBias = 0.05
        _G.game_state.aoPower = 1.8
        _G.game_state.giIntensity = 1.2
        _G.game_state.giSamples = 8
        _G.game_state.useLocalProbe = 1
        _G.game_state.probeMin = { -22.0, 0.0, -22.0 }
        _G.game_state.probeMax = { 22.0, 12.0, 22.0 }
        _G.game_state.probePos = { 0.0, 4.0, 0.0 }
        _G.game_state.vignetteIntensity = 1.10
        _G.game_state.vignettePower = 1.50
        _G.game_state.enableSSR = 1
        _G.game_state.floorRoughness = 0.15
        _G.game_state.floorMetallic = 0.95
        _G.game_state.sphereLightRadius = 1.5
        _G.game_state.light1Intensity = 180.0
        _G.game_state.light2Intensity = 180.0
        _G.game_state.enableTAA = 1
        _G.game_state.taaFeedback = 0.95
        ffi.C.ZHLN_RegisterGameState(engine_ptr, _G.game_state)
    end

    for ent, movement in _G.game_ecs:view("MovementComponent") do
        _G.player_ent = ent
        break
    end

    if _G.player_ent then
        _G.game_ecs:add(_G.player_ent, "combat", { hp = 100, max_hp = 100, is_poisoned = false })
        _G.game_ecs:add(_G.player_ent, "inventory", { coins = 0, equipped = nil })
    end
end

-- ============================================================================
-- Global Host Hooks
-- ============================================================================
_G.update = function(ptr, dt)
    zh.trigger("engine.tick", dt)

    for i = 1, #scheduler.systems do
        local sys = scheduler.systems[i]
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



return zh
