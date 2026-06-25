-- Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
-- SPDX-License-Identifier: GPL-3.0-or-later

local ffi = require("scripts.core.ffi_cdef")

local CODE_TO_TYPE = {
    f = "float",
    d = "double",
    i = "int32_t",
    I = "uint32_t",
    Q = "uint64_t",
    EvtF = "ZHLN_ContactEventF",
    EvtD = "ZHLN_ContactEventD"
}
local BufferMT = {}
local TypeCache = {}

local uint32_ptr = ffi.typeof("uint32_t*")

-- Register a duplicate, metatable-free raw structure to bypass the metatype index interceptor
local ok = pcall(ffi.typeof, "ZHLN_BufferView_Raw")
if not ok then
    ffi.cdef [[
        typedef struct ZHLN_BufferView_Raw {
            void*    buf;
            void*    obj;
            size_t   len;
            uint32_t itemsize;
            char     format[8];
            int      readonly;
            uint32_t ndim;
            size_t   shape[4];
            size_t   strides[4];
            uint32_t flags;
            uint32_t owner_type;
        } ZHLN_BufferView_Raw;
    ]]
end
local RawType = ffi.typeof("ZHLN_BufferView_Raw*")

local function get_ctype(format_ptr)
    local key = ffi.cast(uint32_ptr, format_ptr)[0]
    local t = TypeCache[key]
    if not t then
        local fmt = ffi.string(format_ptr)
        local real_type = CODE_TO_TYPE[fmt] or "char"
        t = ffi.typeof(real_type .. "*")
        TypeCache[key] = t
    end
    return t
end

function BufferMT:get(i, j, k, l)
    local offset = 0
    if i then offset = offset + i * self.strides[0] end
    if j then offset = offset + j * self.strides[1] end
    if k then offset = offset + k * self.strides[2] end
    if l then offset = offset + l * self.strides[3] end
    local ptr = ffi.cast("char*", self.buf) + offset
    return ffi.cast(get_ctype(self.format), ptr)[0]
end

function BufferMT:set(val, i, j, k, l)
    if self.readonly ~= 0 then error("Buffer is Read-Only") end
    local offset = 0
    if i then offset = offset + i * self.strides[0] end
    if j then offset = offset + j * self.strides[1] end
    if k then offset = offset + k * self.strides[2] end
    if l then offset = offset + l * self.strides[3] end
    local ptr = ffi.cast("char*", self.buf) + offset
    ffi.cast(get_ctype(self.format), ptr)[0] = val
end

function BufferMT:release()
    -- Cast to the metatable-free raw pointer to bypass __index / __newindex interception
    local raw = ffi.cast(RawType, self)
    if raw.obj ~= nil then
        local args = ffi.new("ReleaseBufferArgs", { raw.obj })
        ffi.C.ZHLN_DispatchCommand(nil, "ReleaseBuffer", args)
        raw.obj = nil -- Safely clears the C++ owner pointer with no Lua callbacks
    end
end

local COMPONENT_MAP = { x = 0, y = 1, z = 2, w = 3, r = 0, g = 1, b = 2, a = 3 }

function BufferMT:__index(i)
    local method = BufferMT[i]
    if method then return method end
    if type(i) == "string" then
        local idx = COMPONENT_MAP[i]
        if idx then return self[idx] end

        -- Safely query the raw, metatable-free pointer to read native fields (like .obj or .buf)
        local raw = ffi.cast(RawType, self)
        local success, val = pcall(function() return raw[i] end)
        if success then return val end

        -- Return nil instead of throwing an error. This prevents the catastrophic
        -- nested error loop that overflows the LuaJIT stack.
        return nil
    end
    if self.ndim > 1 then
        local sub = ffi.new("ZHLN_BufferView")
        sub.obj = nil
        sub.itemsize = self.itemsize
        sub.readonly = self.readonly
        ffi.copy(sub.format, self.format, 8)
        sub.buf = ffi.cast("char*", self.buf) + (i * self.strides[0])
        sub.ndim = self.ndim - 1
        for d = 0, sub.ndim - 1 do
            sub.shape[d] = self.shape[d + 1]
            sub.strides[d] = self.strides[d + 1]
        end
        return sub
    end
    local ptr = ffi.cast("char*", self.buf) + (i * self.strides[0])
    return ffi.cast(get_ctype(self.format), ptr)[0]
end

function BufferMT:__newindex(i, val)
    if self.readonly ~= 0 then error("Buffer is Read-Only") end
    if type(i) == "string" then
        local idx = COMPONENT_MAP[i]
        if idx then
            self[idx] = val
        else
            error("Cannot assign arbitrary property '" .. i .. "' to ZHLN_BufferView")
        end
        return
    end
    if self.ndim == 1 then
        local ptr = ffi.cast("char*", self.buf) + (i * self.strides[0])
        ffi.cast(get_ctype(self.format), ptr)[0] = val
        return
    end
    if self.ndim > 1 then
        local sub = self[i]
        local vtype = type(val)
        if vtype == "table" then
            if val.x ~= nil then sub.x = val.x end
            if val.y ~= nil then sub.y = val.y end
            if val.z ~= nil then sub.z = val.z end
            if val.w ~= nil then sub.w = val.w end
            for k = 1, #val do if k <= tonumber(sub.shape[0]) then sub[k - 1] = val[k] end end
        elseif vtype == "cdata" and ffi.istype("ZHLN_BufferView", val) then
            if sub.buf ~= val.buf then
                local bytes = math.min(tonumber(sub.shape[0] * sub.itemsize), tonumber(val.len))
                ffi.copy(sub.buf, val.buf, bytes)
            end
        else
            error("Cannot assign a scalar to a high-dimensional view.")
        end
        return
    end
end

function BufferMT:__len() return tonumber(self.shape[0]) end

function BufferMT:__gc() self:release() end

pcall(ffi.metatype, "ZHLN_BufferView", BufferMT)

return { C = ffi.C }
