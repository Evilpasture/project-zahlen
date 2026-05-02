local ffi = require("ffi")

ffi.cdef[[
    typedef struct {
        void*    buf;
        size_t   len;
        uint32_t itemsize;
        char     format[4];
        uint32_t ndim;
        size_t   shape[4];
        size_t   strides[4];
    } ZHLN_BufferView;
]]

local M = {}

-- Create a specialized accessor based on format
local function get_cast_type(format)
    local char = string.char(format[0])
    if char == 'f' then return ffi.typeof("float*") end
    if char == 'd' then return ffi.typeof("double*") end
    if char == 'I' then return ffi.typeof("uint32_t*") end
    return ffi.typeof("char*")
end

local BufferMT = {}
BufferMT.__index = {
    -- Optional: add slicing, .shape, etc
}

-- The Magic: Multi-dimensional / Strided Indexing
function BufferMT:__getitem(i)
    -- Pointer math: base_addr + (index * stride)
    -- LuaJIT is smart enough to hoist the cast out of loops
    local byte_ptr = ffi.cast("char*", self.buf)
    local element_ptr = ffi.cast(self.cast_type, byte_ptr + (i * self.strides[0]))
    return element_ptr
end

-- Use __index for array-style access: view[i]
function BufferMT:__index(i)
    if type(i) == "number" then
        local byte_ptr = ffi.cast("char*", self.buf)
        return ffi.cast(self.cast_type, byte_ptr + (i * self.strides[0]))
    end
    return BufferMT.__index[i]
end

-- Create the metatype once
M.View = ffi.metatype("ZHLN_BufferView", {
    __index = BufferMT.__index,
    __newindex = function(self, i, val) 
        -- Support writing: view[i].x = 10
        local byte_ptr = ffi.cast("char*", self.buf)
        local target = ffi.cast(self.cast_type, byte_ptr + (i * self.strides[0]))
        target[0] = val
    end
})

return M