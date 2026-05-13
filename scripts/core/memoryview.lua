local ffi = require("ffi")

local ok = pcall(ffi.typeof, "ZHLN_BufferView")
if not ok then
    ffi.cdef [[
        typedef struct ZHLN_BufferView {
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
        } ZHLN_BufferView;
        typedef struct ZHLN_Engine ZHLN_Engine;
        ZHLN_BufferView ZHLN_GetPhysicsPositions(ZHLN_Engine* engine);
        ZHLN_BufferView ZHLN_GetPhysicsLinearVelocities(ZHLN_Engine* engine);
        void ZHLN_ReleaseBuffer(void* owner);

        ZHLN_BufferView ZHLN_GetECSBuffer(ZHLN_Engine* engine, const char* componentName);
        ZHLN_BufferView ZHLN_GetECSEntities(ZHLN_Engine* engine, const char* componentName);
        int ZHLN_IsKeyDown(ZHLN_Engine* engine, uint8_t key);
        void ZHLN_GetMouseDelta(ZHLN_Engine* engine, float* outX, float* outY);
        void ZHLN_SetCharacterVelocity(ZHLN_Engine* engine, uint64_t physicsHandle, float x, float y, float z);
        int ZHLN_IsCharacterOnGround(ZHLN_Engine* engine, uint64_t physicsHandle);
        void ZHLN_SetLinearVelocity(ZHLN_Engine* engine, uint64_t physicsHandle, float x, float y, float z);
        float ZHLN_GetCameraYaw(struct ZHLN_Engine* engine);
        void ZHLN_AddImpulse(struct ZHLN_Engine* engine, uint64_t entityHandle, float x,
								 float y, float z);

        typedef struct ZHLN_ContactEventF {
            uint64_t body1;
            uint64_t body2;
            float px, py, pz;
            float nx, ny, nz;
            float impulse;
            uint32_t type;
            uint32_t flags;
            float slidingSpeed;
            float rvx, rvy, rvz;
            uint32_t mat1, mat2;
            uint32_t sub1, sub2;
        } __attribute__((aligned(128))) ZHLN_ContactEventF;

        typedef struct ZHLN_ContactEventD {
            uint64_t body1;
            uint64_t body2;
            double px, py, pz;
            float nx, ny, nz;
            float impulse;
            uint32_t type;
            uint32_t flags;
            float slidingSpeed;
            float rvx, rvy, rvz;
            uint32_t mat1, mat2;
            uint32_t sub1, sub2;
        } __attribute__((aligned(128))) ZHLN_ContactEventD;

        ZHLN_BufferView ZHLN_GetPhysicsContactEvents(ZHLN_Engine* engine);
    ]]
end

local CODE_TO_TYPE = { 
    f = "float", d = "double", i = "int32_t", I = "uint32_t", Q = "uint64_t",
    EvtF = "ZHLN_ContactEventF", EvtD = "ZHLN_ContactEventD" 
}
local BufferMT = {}
local TypeCache = {}

local function get_ctype(format_ptr)
    local fmt = ffi.string(format_ptr)
    local t = TypeCache[fmt]
    if not t then
        local real_type = CODE_TO_TYPE[fmt] or "char"
        t = ffi.typeof(real_type .. "*")
        TypeCache[fmt] = t
    end
    return t
end

function BufferMT:__index(i)
    -- 1. Named Component Mapping
    if type(i) == "string" then
        local map = { x = 0, y = 1, z = 2, w = 3, r = 0, g = 1, b = 2, a = 3 }
        local idx = map[i]
        if idx then return self[idx] end
        return BufferMT[i]
    end

    -- 2. Recursive Slicing (ndim > 1)
    if self.ndim > 1 then
        local sub = ffi.new("ZHLN_BufferView")
        sub.obj = nil -- Prevent GC from releasing the C++ buffer prematurely
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

    -- 3. Scalar Access (ndim == 1)
    local ptr = ffi.cast("char*", self.buf) + (i * self.strides[0])
    return ffi.cast(get_ctype(self.format), ptr)[0]
end

function BufferMT:__newindex(i, val)
    if self.readonly ~= 0 then error("Buffer is Read-Only") end

    -- 1. Named Component Write (view.y = 10)
    if type(i) == "string" then
        local map = { x = 0, y = 1, z = 2, w = 3 }
        local idx = map[i]
        if idx then self[idx] = val return end
        rawset(self, i, val)
        return
    end

    -- 2. Scalar Write (ndim == 1)
    if self.ndim == 1 then
        local ptr = ffi.cast("char*", self.buf) + (i * self.strides[0])
        ffi.cast(get_ctype(self.format), ptr)[0] = val
        return
    end

    -- 3. Bulk Write / Slice Assignment (ndim > 1)
    if self.ndim > 1 then
        local sub = self[i] -- The target row/slice
        
        -- Fix: LuaJIT FFI objects are type "cdata"
        local vtype = type(val)
        
        if vtype == "table" then
            -- Copy from table: view[i] = {x=1, y=2}
            if val.x ~= nil then sub.x = val.x end
            if val.y ~= nil then sub.y = val.y end
            if val.z ~= nil then sub.z = val.z end
            if val.w ~= nil then sub.w = val.w end
            for k = 1, #val do if k <= tonumber(sub.shape[0]) then sub[k-1] = val[k] end end
        elseif vtype == "cdata" and ffi.istype("ZHLN_BufferView", val) then
            -- Copy from another view: view[i] = other_view
            -- If user does `view[i] = view[i]`, buf pointers match, we do nothing.
            if sub.buf ~= val.buf then
                local bytes = math.min(tonumber(sub.shape[0] * sub.itemsize), tonumber(val.len))
                ffi.copy(sub.buf, val.buf, bytes)
            end
        else
            error("Cannot assign a scalar to a high-dimensional view. Use a table or index further.")
        end
        return
    end
end

function BufferMT:__len() return tonumber(self.shape[0]) end
function BufferMT:release() 
    if self.obj ~= nil then 
        ffi.C.ZHLN_ReleaseBuffer(self.obj) -- Pass the pointer stored in .obj
        self.obj = nil 
    end 
end
function BufferMT:__gc() self:release() end

if not ok then ffi.metatype("ZHLN_BufferView", BufferMT) end
return { C = ffi.C }