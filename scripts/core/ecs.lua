-- Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
-- SPDX-License-Identifier: GPL-3.0-or-later

table.unpack = table.unpack or _G.unpack or unpack
local ffi = require("scripts.core.ffi_cdef")

local Registry = {}
Registry.__index = Registry

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
    PBRComponent = true,
    TextComponent = true,
    UISettingsComponent = true,
    SunTagComponent = true,
}

local DYNAMIC_COMPONENTS = {} -- Tracks active dynamic types registered via Lua

-- Register dynamic FFI structures with C++ sparse sets
function Registry:register_dynamic(comp_name)
    if DYNAMIC_COMPONENTS[comp_name] then return end

    -- Extract sizing properties directly from active FFI definitions
    local size = ffi.sizeof(comp_name)
    local align = ffi.alignof(comp_name)

    local args = ffi.new("RegisterDynamicComponentArgs", { comp_name, size, align })
    local family_id = ffi.C.ZHLN_DispatchCommand(self.engine, "RegisterDynamicComponent", args)

    if family_id == 0xFFFFFFFF then
        error("Failed to register dynamic component: " .. comp_name)
    end

    DYNAMIC_COMPONENTS[comp_name] = true
end

local function to_key(ent)
    if type(ent) == "number" then
        return tostring(ffi.cast("uint64_t", ent))
    elseif type(ent) == "cdata" then
        return tostring(ent)
    end
    return tostring(ent)
end

function Registry.new(engine_raw)
    return setmetatable({
        engine = engine_raw, pools = {}, sizes = {}, entities = {}, next_id = 1,
    }, Registry)
end

function Registry:is_native(comp_name)
    return NATIVE_COMPONENTS[comp_name] == true or DYNAMIC_COMPONENTS[comp_name] == true
end

function Registry:create(ent)
    local id = ent
    if not ent then
        id = ffi.C.ZHLN_DispatchCommand(self.engine, "CreateEntity", nil)
    end
    self.entities[to_key(id)] = true
    return id
end

function Registry:destroy(ent)
    local key = to_key(ent)
    if not self.entities[key] then return end

    local args = ffi.new("EntityOnlyArgs", { ent })
    ffi.C.ZHLN_DispatchCommand(self.engine, "DestroyEntity", args)

    for name, pool in pairs(self.pools) do
        if pool[key] ~= nil then
            pool[key] = nil
            self.sizes[name] = self.sizes[name] - 1
        end
    end
    self.entities[key] = nil
end

function Registry:is_alive(ent) return self.entities[to_key(ent)] == true end

function Registry:add(ent, comp_name, data)
    self.entities[to_key(ent)] = true

    if self:is_native(comp_name) then
        local args = ffi.new("GetComponentArgs", { ent, comp_name })
        local ptr_int = ffi.C.ZHLN_DispatchCommand(self.engine, "GetComponent", args)
        if ptr_int == 0ULL then
            ptr_int = ffi.C.ZHLN_DispatchCommand(self.engine, "AddComponent", args)
        end
        if ptr_int == 0ULL then error("Cannot add native component '" .. comp_name .. "'.") end

        local comp = ffi.cast(comp_name .. "*", ptr_int)
        if data then for k, v in pairs(data) do comp[k] = v end end
        return comp
    else
        local pool = self.pools[comp_name]
        if not pool then
            pool = {}
            self.pools[comp_name] = pool
            self.sizes[comp_name] = 0
        end
        local key = to_key(ent)
        if pool[key] == nil then self.sizes[comp_name] = self.sizes[comp_name] + 1 end
        if type(data) == "function" then pool[key] = data() else pool[key] = data or {} end
        return pool[key]
    end
end

function Registry:get(ent, comp_name)
    if self:is_native(comp_name) then
        local args = ffi.new("GetComponentArgs", { ent, comp_name })
        local ptr_int = ffi.C.ZHLN_DispatchCommand(self.engine, "GetComponent", args)
        if ptr_int == 0ULL then return nil end
        return ffi.cast(comp_name .. "*", ptr_int)
    else
        local pool = self.pools[comp_name]
        return pool and pool[to_key(ent)] or nil
    end
end

function Registry:has(ent, comp_name)
    if self:is_native(comp_name) then
        local args = ffi.new("GetComponentArgs", { ent, comp_name })
        return ffi.C.ZHLN_DispatchCommand(self.engine, "GetComponent", args) ~= 0ULL
    else
        local pool = self.pools[comp_name]
        return (pool and pool[to_key(ent)] ~= nil) or false
    end
end

function Registry:remove(ent, comp_name)
    if not self:is_native(comp_name) then
        local pool = self.pools[comp_name]
        local key = to_key(ent)
        if pool and pool[key] ~= nil then
            pool[key] = nil
            self.sizes[comp_name] = self.sizes[comp_name] - 1
        end
    end
end

function Registry:view(...)
    local comps = { ... }
    local n = #comps
    if n == 0 then return function() end end

    local has_native, primary_native = false, nil
    for i = 1, n do
        if self:is_native(comps[i]) then
            has_native = true
            primary_native = comps[i]
            break
        end
    end

    if not has_native then return self:pure_lua_view(table.unpack(comps)) end

    local view_buf = ffi.new("ZHLN_BufferView[1]")
    local args = ffi.new("GetECSBufferArgs", { primary_native, view_buf })
    ffi.C.ZHLN_DispatchCommand(self.engine, "GetECSEntities", args)
    local entities_view = view_buf[0]

    local count = tonumber(entities_view.shape[0])
    local entity_array = ffi.cast("uint64_t*", entities_view.buf)

    local entities = {}
    for i = 0, count - 1 do entities[i + 1] = entity_array[i] end

    local rel_args = ffi.new("ReleaseBufferArgs", { entities_view.obj })
    ffi.C.ZHLN_DispatchCommand(nil, "ReleaseBuffer", rel_args)

    local index = 1
    return function()
        while index <= count do
            local ent = entities[index]
            index = index + 1
            local results, matches = {}, true
            for i = 1, n do
                local comp = self:get(ent, comps[i])
                if comp == nil then
                    matches = false; break
                end
                results[i] = comp
            end
            if matches then return ent, table.unpack(results, 1, n) end
        end
    end
end

function Registry:pure_lua_view(...)
    -- Optimized inner-Lua pure view unchanged
    local comps = { ... }
    local n = #comps
    if n == 0 then return function() end end

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
        if name ~= smallest_name then table.insert(other_pools, self.pools[name] or {}) end
    end

    local ent, val = nil, nil
    return function()
        while true do
            ent, val = next(smallest_pool, ent)
            if not ent then return nil end
            local matches = true
            for p = 1, #other_pools do
                if other_pools[p][ent] == nil then
                    matches = false; break
                end
            end
            if matches then
                local results = {}
                for i = 1, n do results[i] = self.pools[comps[i]][ent] end
                return ent, table.unpack(results, 1, n)
            end
        end
    end
end

function Registry:find(name)
    for ent, name_comp in self:view("NameComponent") do
        if ffi.string(name_comp.name) == name then return ent end
    end
    return nil
end

return Registry
