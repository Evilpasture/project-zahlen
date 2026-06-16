-- Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
-- SPDX-License-Identifier: GPL-3.0-or-later


-- scripts/core/ecs.lua
-- Cross-version Lua compatibility shim for unpack
table.unpack = table.unpack or _G.unpack or unpack
local ffi = require("scripts.core.ffi_cdef")
local mem = require("scripts.core.memoryview")

local Registry = {}
Registry.__index = Registry

-- List of components managed on the C++ side
local NATIVE_COMPONENTS = {
    TransformComponent = true,
    HierarchyComponent = true,
    MovementComponent = true,
    MeshComponent = true,
    PhysicsComponent = true,
    ALifeComponent = true,
    RagdollComponent = true,
    NameComponent = true,
    TargetCameraComponent = true,
    PhysicsStateComponent = true,
}

-- Stable key converter for cdata / uint64_t table indexing
local function to_key(ent)
    if type(ent) == "cdata" then
        return tostring(ent)
    end
    return ent
end

function Registry.new(engine_raw)
    return setmetatable({
        engine = engine_raw,
        pools = {},    -- componentName -> { entity_id -> component_data } (Lua-only)
        sizes = {},    -- componentName -> count
        entities = {}, -- entity_id -> true
        next_id = 1,   -- Fallback for purely Lua-created entities
    }, Registry)
end

function Registry:is_native(comp_name)
    return NATIVE_COMPONENTS[comp_name] == true
end

-- ============================================================================
-- Entity Management
-- ============================================================================

function Registry:create(ent)
    -- If a C++ packed uint64_t handle is passed, use it. Otherwise, generate a Lua ID.
    local id = ent or self.next_id
    if not ent then
        self.next_id = self.next_id + 1
    end

    self.entities[to_key(id)] = true
    return id
end

function Registry:destroy(ent)
    local key = to_key(ent)
    if not self.entities[key] then return end

    -- Remove from all component pools and update size counters
    for name, pool in pairs(self.pools) do
        if pool[key] ~= nil then
            pool[key] = nil
            self.sizes[name] = self.sizes[name] - 1
        end
    end

    self.entities[key] = nil
end

function Registry:is_alive(ent)
    return self.entities[to_key(ent)] == true
end

-- ============================================================================
-- Component Management
-- ============================================================================

function Registry:add(ent, comp_name, data)
    self.entities[to_key(ent)] = true

    if self:is_native(comp_name) then
        -- Native components are allocated via C++ if missing, then populated from Lua
        local ptr = ffi.C.ZHLN_GetComponent(self.engine, ent, comp_name)
        if ptr == nil then
            ptr = ffi.C.ZHLN_AddComponent(self.engine, ent, comp_name)
        end
        if ptr == nil then
            error("Cannot add native component '" .. comp_name .. "' from Lua.")
        end
        local comp = ffi.cast(comp_name .. "*", ptr)
        if data then
            for k, v in pairs(data) do comp[k] = v end
        end
        return comp
    else
        -- Fallback to standard Lua table allocation
        local pool = self.pools[comp_name]
        if not pool then
            pool = {}
            self.pools[comp_name] = pool
            self.sizes[comp_name] = 0
        end

        local key = to_key(ent)
        if pool[key] == nil then
            self.sizes[comp_name] = self.sizes[comp_name] + 1
        end

        if type(data) == "function" then
            pool[key] = data()
        else
            pool[key] = data or {}
        end

        return pool[key]
    end
end

function Registry:get(ent, comp_name)
    if self:is_native(comp_name) then
        local ptr = ffi.C.ZHLN_GetComponent(self.engine, ent, comp_name)
        if ptr == nil then return nil end
        return ffi.cast(comp_name .. "*", ptr)
    else
        local pool = self.pools[comp_name]
        return pool and pool[to_key(ent)] or nil
    end
end

function Registry:has(ent, comp_name)
    if self:is_native(comp_name) then
        return ffi.C.ZHLN_GetComponent(self.engine, ent, comp_name) ~= nil
    else
        local pool = self.pools[comp_name]
        return (pool and pool[to_key(ent)] ~= nil) or false
    end
end

function Registry:remove(ent, comp_name)
    if self:is_native(comp_name) then
        -- Native component lifecycles are controlled by C++ structural arrays.
        -- Removing them from memory is handled at the engine level.
    else
        local pool = self.pools[comp_name]
        local key = to_key(ent)
        if pool and pool[key] ~= nil then
            pool[key] = nil
            self.sizes[comp_name] = self.sizes[comp_name] - 1
        end
    end
end

-- ============================================================================
-- Unified View System (JIT-Friendly)
-- ============================================================================

function Registry:view(...)
    local comps = { ... }
    local n = #comps
    if n == 0 then return function() end end

    local has_native = false
    local primary_native = nil
    for i = 1, n do
        if self:is_native(comps[i]) then
            has_native = true
            primary_native = comps[i]
            break
        end
    end

    if not has_native then
        return self:pure_lua_view(table.unpack(comps))
    end

    local entities_view = ffi.C.ZHLN_GetECSEntities(self.engine, primary_native)
    local count = tonumber(entities_view.shape[0])
    local entity_array = ffi.cast("uint64_t*", entities_view.buf)

    -- Copy entities to a Lua table to avoid holding the C++ Mutex during arbitrary Lua execution
    local entities = {}
    for i = 0, count - 1 do
        entities[i + 1] = entity_array[i]
    end
    entities_view:release()

    local index = 1

    return function()
        while index <= count do
            local ent = entities[index]
            index = index + 1

            local results = {}
            local matches = true

            for i = 1, n do
                local comp_name = comps[i]
                local comp = self:get(ent, comp_name)
                if comp == nil then
                    matches = false
                    break
                end
                results[i] = comp
            end

            if matches then
                return ent, table.unpack(results, 1, n)
            end
        end
    end
end

-- ============================================================================
-- Pure Lua Optimized View
-- ============================================================================

function Registry:pure_lua_view(...)
    local comps = { ... }
    local n = #comps
    if n == 0 then
        return function() end
    end

    if n == 1 then
        local name1 = comps[1]
        local p1 = self.pools[name1] or {}
        local ent = nil
        return function()
            local val
            ent, val = next(p1, ent)
            if ent then return ent, val end
        end
    elseif n == 2 then
        local name1, name2 = comps[1], comps[2]
        local p1, p2 = self.pools[name1] or {}, self.pools[name2] or {}
        local swap = (self.sizes[name1] or 0) > (self.sizes[name2] or 0)

        local iter_pool, check_pool = p1, p2
        if swap then iter_pool, check_pool = p2, p1 end

        local ent = nil
        return function()
            while true do
                local val
                ent, val = next(iter_pool, ent)
                if not ent then return nil end

                local other_val = check_pool[ent]
                if other_val ~= nil then
                    if swap then
                        return ent, other_val, val
                    else
                        return ent, val, other_val
                    end
                end
            end
        end
    elseif n == 3 then
        local name1, name2, name3 = comps[1], comps[2], comps[3]
        local p1 = self.pools[name1] or {}
        local p2 = self.pools[name2] or {}
        local p3 = self.pools[name3] or {}

        local s1 = self.sizes[name1] or 0
        local s2 = self.sizes[name2] or 0
        local s3 = self.sizes[name3] or 0

        local ent = nil
        if s1 <= s2 and s1 <= s3 then
            return function()
                while true do
                    local val1
                    ent, val1 = next(p1, ent)
                    if not ent then return nil end
                    local val2, val3 = p2[ent], p3[ent]
                    if val2 ~= nil and val3 ~= nil then
                        return ent, val1, val2, val3
                    end
                end
            end
        elseif s2 <= s1 and s2 <= s3 then
            return function()
                while true do
                    local val2
                    ent, val2 = next(p2, ent)
                    if not ent then return nil end
                    local val1, val3 = p1[ent], p3[ent]
                    if val1 ~= nil and val3 ~= nil then
                        return ent, val1, val2, val3
                    end
                end
            end
        else
            return function()
                while true do
                    local val3
                    ent, val3 = next(p3, ent)
                    if not ent then return nil end
                    local val1, val2 = p1[ent], p2[ent]
                    if val1 ~= nil and val2 ~= nil then
                        return ent, val1, val2, val3
                    end
                end
            end
        end
    end

    local smallest_name = comps[1]
    local smallest_size = self.sizes[smallest_name] or 0

    for i = 2, n do
        local name = comps[i]
        local size = self.sizes[name] or 0
        if size < smallest_size then
            smallest_size = size
            smallest_name = name
        end
    end

    if smallest_size == 0 then return function() end end

    local smallest_pool = self.pools[smallest_name]
    local other_pools = {}
    for i = 1, n do
        local name = comps[i]
        if name ~= smallest_name then
            table.insert(other_pools, self.pools[name] or {})
        end
    end

    local ent, val = nil, nil
    return function()
        while true do
            ent, val = next(smallest_pool, ent)
            if not ent then return nil end

            local matches = true
            for p = 1, #other_pools do
                if other_pools[p][ent] == nil then
                    matches = false
                    break
                end
            end

            if matches then
                local results = {}
                for i = 1, n do
                    results[i] = self.pools[comps[i]][ent]
                end
                return ent, table.unpack(results, 1, n)
            end
        end
    end
end

-- ============================================================================
-- Utility Helper: Find Entity by Name
-- ============================================================================
function Registry:find(name)
    for ent, name_comp in self:view("NameComponent") do
        if ffi.string(name_comp.name) == name then
            return ent
        end
    end
    return nil
end

return Registry
