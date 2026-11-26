// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Core/FunctionRef.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <lua.hpp>
#include <lua/LuaInterop.hpp>
#include <lua/ReflectLua.hpp>
#include <luajit.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN::ReflectLua {

namespace {

constexpr int kHookSliceInstructions = 4096;

// Registry-key sentinels whose unique static addresses partition the Lua
// registry without string-key collisions or cross-VM sharing.
char kListenerTableRegistryKey;

auto ToAbsIndex(lua_State* state, int idx) noexcept -> int {
    if (idx > 0 || idx <= LUA_REGISTRYINDEX) {
        return idx;
    }
    return lua_gettop(state) + idx + 1;
}

auto SafeLogPrint(lua_State* state) -> int {
    const int   nargs = lua_gettop(state);
    std::string line;
    for (int i = 1; i <= nargs; ++i) {
        if (i > 1) {
            line.push_back('\t');
        }
        size_t      len = 0;
        const char* str = nullptr;
        if (lua_type(state, i) == LUA_TSTRING) {
            str = lua_tolstring(state, i, &len);
        } else {
            lua_getglobal(state, "tostring");
            lua_pushvalue(state, i);
            if (lua_pcall(state, 1, 1, 0) == 0 && lua_type(state, -1) == LUA_TSTRING) {
                str = lua_tolstring(state, -1, &len);
                if (str != nullptr) {
                    line.append(str, len);
                }
                lua_pop(state, 1);
                continue;
            }
            lua_pop(state, 1);
        }
        if (str != nullptr) {
            line.append(str, len);
        }
    }
    ZHLN::Log<ZHLN::LogSeverity::Info>(ZHLN::LogChannel {"Script"}, "{}", line);
    return 0;
}

auto OpenSandboxLibsC(lua_State* state) -> int {
    lua_pushcfunction(state, luaopen_base);
    lua_pushliteral(state, "");
    lua_call(state, 1, 0);

    lua_pushcfunction(state, luaopen_table);
    lua_pushliteral(state, LUA_TABLIBNAME);
    lua_call(state, 1, 0);

    lua_pushcfunction(state, luaopen_string);
    lua_pushliteral(state, LUA_STRLIBNAME);
    lua_call(state, 1, 0);

    lua_pushcfunction(state, luaopen_math);
    lua_pushliteral(state, LUA_MATHLIBNAME);
    lua_call(state, 1, 0);

    lua_pushcfunction(state, luaopen_bit);
    lua_pushliteral(state, LUA_BITLIBNAME);
    lua_call(state, 1, 0);

    // Strip file/bytecode/metatable-escape entry points from base library;
    // io, os, package, debug, ffi, jit, and coroutine were never opened.
    constexpr std::array<const char*, 9> kForbiddenGlobals = {
        "dofile", "loadfile", "load", "loadstring", "collectgarbage", "newproxy", "getfenv", "setfenv", "gcinfo",
    };
    for (const char* name: kForbiddenGlobals) {
        lua_pushnil(state);
        lua_setglobal(state, name);
    }

    lua_pushcfunction(state, &SafeLogPrint);
    lua_setglobal(state, "print");

    // Pre-create the VM-internal Lua listener table (`registry[&kListenerTableRegistryKey]`).
    lua_pushlightuserdata(state, &kListenerTableRegistryKey);
    lua_newtable(state);
    lua_rawset(state, LUA_REGISTRYINDEX);
    return 0;
}

void EncodeUint64Key(uint64_t value, std::array<char, sizeof(uint64_t)>& out) noexcept {
    std::memcpy(out.data(), &value, sizeof(uint64_t));
}

auto AsLuaState(void* raw) noexcept -> lua_State* {
    return static_cast<lua_State*>(raw);
}

struct HostContextEntry {
    TypeID typeId {};
    void*  pointer = nullptr;
};

struct RegisteredTypeEntry {
    TypeID typeId {};
    int    metatableRegistryRef = LUA_NOREF;
    int    internTableRef       = LUA_NOREF;
};

} // namespace

struct CallFrame::Context {
    Vm::Impl*  vm     = nullptr;
    lua_State* state  = nullptr;
    int        argTop = 0;
    bool       oomHit = false;
};

struct Vm::Impl {
    lua_State*                       state                     = nullptr;
    lua_Alloc                        baseAlloc                 = nullptr;
    void*                            baseAllocUd               = nullptr;
    LuaBudget                        budget                    = {};
    size_t                           usedBytes                 = 0;
    uint32_t                         protectedCallDepth        = 0;
    int64_t                          remainingInstructions     = 0;
    bool                             instructionBudgetExceeded = false;
    bool                             memoryBudgetExceeded      = false;
    ListenerID                       nextListenerId            = 1;
    std::string                      lastErrorDetail;
    std::vector<HostContextEntry>    hostContexts;
    std::vector<RegisteredTypeEntry> registeredTypes;

    static auto AllocWrapper(void* ud, void* ptr, size_t osize, size_t nsize) noexcept -> void* {
        auto* self = static_cast<Impl*>(ud);
        // In Lua 5.1 / LuaJIT, when ptr == nullptr, `osize` carries the Lua GC object
        // type tag rather than a previous byte allocation size.
        const size_t oldBytes = (ptr != nullptr) ? osize : 0u;

        if (nsize == 0u) {
            void* res       = self->baseAlloc(self->baseAllocUd, ptr, osize, 0u);
            self->usedBytes = (self->usedBytes >= oldBytes) ? (self->usedBytes - oldBytes) : 0u;
            return res;
        }

        if (nsize > oldBytes) {
            const size_t growth = nsize - oldBytes;
            if (self->budget.maxMemoryBytes > 0u && (self->usedBytes + growth) > self->budget.maxMemoryBytes) {
                self->memoryBudgetExceeded = true;
                return nullptr;
            }
            void* res = self->baseAlloc(self->baseAllocUd, ptr, osize, nsize);
            if (res != nullptr) {
                self->usedBytes += growth;
            }
            return res;
        }

        void* res = self->baseAlloc(self->baseAllocUd, ptr, osize, nsize);
        if (res != nullptr) {
            const size_t shrink = oldBytes - nsize;
            self->usedBytes     = (self->usedBytes >= shrink) ? (self->usedBytes - shrink) : 0u;
        }
        return res;
    }

    static void InstructionHook(lua_State* state, lua_Debug* /*ar*/) {
        void* allocUd = nullptr;
        lua_getallocf(state, &allocUd);
        auto* self = static_cast<Impl*>(allocUd);
        if (self != nullptr) {
            self->remainingInstructions -= kHookSliceInstructions;
            if (self->remainingInstructions <= 0) {
                self->instructionBudgetExceeded = true;
                luaL_error(state, "Lua instruction budget exceeded");
            }
        }
    }

    [[nodiscard]] auto FindType(TypeID typeId) const noexcept -> const RegisteredTypeEntry* {
        for (const RegisteredTypeEntry& entry: registeredTypes) {
            if (entry.typeId == typeId) {
                return &entry;
            }
        }
        return nullptr;
    }

    [[nodiscard]] auto FindMutableType(TypeID typeId) noexcept -> RegisteredTypeEntry* {
        for (RegisteredTypeEntry& entry: registeredTypes) {
            if (entry.typeId == typeId) {
                return &entry;
            }
        }
        return nullptr;
    }

    [[nodiscard]] auto FindHostContext(TypeID typeId) const noexcept -> void* {
        for (const HostContextEntry& entry: hostContexts) {
            if (entry.typeId == typeId) {
                return entry.pointer;
            }
        }
        return nullptr;
    }

    void SetHostContext(TypeID typeId, void* pointer) noexcept {
        for (HostContextEntry& entry: hostContexts) {
            if (entry.typeId == typeId) {
                entry.pointer = pointer;
                return;
            }
        }
        if (pointer != nullptr) {
            hostContexts.push_back(HostContextEntry {.typeId = typeId, .pointer = pointer});
        }
    }

    static auto CallbackThunk(lua_State* state) -> int {
        auto* callbackPtr = lua_touserdata(state, lua_upvalueindex(1));
        if (callbackPtr == nullptr) {
            return 0;
        }
        const auto callback = std::bit_cast<HostCallback>(callbackPtr);
        void*      allocUd  = nullptr;
        lua_getallocf(state, &allocUd);
        auto* self = static_cast<Impl*>(allocUd);
        if (self == nullptr || callback == nullptr) {
            return 0;
        }

        const int          argTop = lua_gettop(state);
        CallFrame::Context frameCtx {
            .vm     = self,
            .state  = state,
            .argTop = argTop,
            .oomHit = false,
        };
        CallFrame frame(frameCtx);
        const int nresults = callback(frame);
        if (frameCtx.oomHit || self->memoryBudgetExceeded) {
            return luaL_error(state, "Lua memory budget exceeded in host callback");
        }
        return std::max(0, nresults);
    }

    static auto MethodDispatchIndexThunk(lua_State* state) -> int {
        // Fallback `__index` when only `methods` were supplied on `MetatableSpec`:
        // upvalue(1) holds the method table.
        if (lua_type(state, 2) != LUA_TSTRING) {
            lua_pushnil(state);
            return 1;
        }
        lua_pushvalue(state, 2);
        lua_rawget(state, lua_upvalueindex(1));
        return 1;
    }

    static auto CombinedIndexThunk(lua_State* state) -> int {
        // upvalue(1) = method table, upvalue(2) = lightuserdata HostCallback `index`.
        if (lua_type(state, 2) == LUA_TSTRING) {
            lua_pushvalue(state, 2);
            lua_rawget(state, lua_upvalueindex(1));
            if (!lua_isnil(state, -1)) {
                return 1;
            }
            lua_pop(state, 1);
        }

        auto* callbackPtr = lua_touserdata(state, lua_upvalueindex(2));
        if (callbackPtr == nullptr) {
            lua_pushnil(state);
            return 1;
        }
        const auto callback = std::bit_cast<HostCallback>(callbackPtr);
        void*      allocUd  = nullptr;
        lua_getallocf(state, &allocUd);
        auto* self = static_cast<Impl*>(allocUd);
        if (self == nullptr || callback == nullptr) {
            lua_pushnil(state);
            return 1;
        }
        const int          argTop = lua_gettop(state);
        CallFrame::Context frameCtx {
            .vm     = self,
            .state  = state,
            .argTop = argTop,
            .oomHit = false,
        };
        CallFrame frame(frameCtx);
        const int nresults = callback(frame);
        if (frameCtx.oomHit || self->memoryBudgetExceeded) {
            return luaL_error(state, "Lua memory budget exceeded in __index");
        }
        return std::max(0, nresults);
    }

    void PushWrappedCallback(HostCallback callback) const noexcept {
        static_assert(sizeof(HostCallback) == sizeof(void*));
        if (callback == nullptr) {
            lua_pushnil(state);
            return;
        }
        lua_pushlightuserdata(state, std::bit_cast<void*>(callback));
        lua_pushcclosure(state, &Impl::CallbackThunk, 1);
    }

    struct ProtectedBudgetScope {
        Impl*   self              = nullptr;
        bool    outermost         = false;
        int64_t savedInstructions = 0;
        bool    savedInstExceeded = false;
        bool    savedMemExceeded  = false;

        explicit ProtectedBudgetScope(Impl* impl) noexcept: self(impl) {
            outermost = (self->protectedCallDepth == 0u);
            ++self->protectedCallDepth;
            if (outermost) {
                self->instructionBudgetExceeded = false;
                self->memoryBudgetExceeded      = false;
                if (self->budget.maxInstructions > 0u) {
                    self->remainingInstructions = static_cast<int64_t>(self->budget.maxInstructions);
                    const int slice = static_cast<int>(std::min<uint32_t>(self->budget.maxInstructions, static_cast<uint32_t>(kHookSliceInstructions)));
                    lua_sethook(self->state, &Impl::InstructionHook, LUA_MASKCOUNT, slice);
                } else {
                    lua_sethook(self->state, nullptr, 0, 0);
                }
            } else {
                // Re-entrant C++ -> Lua callback inside an active Lua execution
                // shares the active instruction counter when one is armed, or
                // arms a fresh slice if the outer call had no hook.
                savedInstructions = self->remainingInstructions;
                savedInstExceeded = self->instructionBudgetExceeded;
                savedMemExceeded  = self->memoryBudgetExceeded;
                if (self->budget.maxInstructions > 0u && lua_gethook(self->state) == nullptr) {
                    const int slice = static_cast<int>(std::min<uint32_t>(self->budget.maxInstructions, static_cast<uint32_t>(kHookSliceInstructions)));
                    lua_sethook(self->state, &Impl::InstructionHook, LUA_MASKCOUNT, slice);
                }
            }
        }

        ~ProtectedBudgetScope() {
            --self->protectedCallDepth;
            if (outermost && self->state != nullptr) {
                lua_sethook(self->state, nullptr, 0, 0);
            }
        }

        ProtectedBudgetScope(const ProtectedBudgetScope&)                    = delete;
        auto operator=(const ProtectedBudgetScope&) -> ProtectedBudgetScope& = delete;
    };

    [[nodiscard]] auto LoadTextChunk(std::string_view code, std::string_view chunkName) noexcept -> std::expected<void, ErrorCode> {
        memoryBudgetExceeded = false;
        const std::string nameBuf(chunkName);
        // Mode "t" permits plain-text Lua source only and rejects binary bytecode.
        const int rc = luaL_loadbufferx(state, code.data(), code.size(), nameBuf.c_str(), "t");
        if (rc == 0) {
            return {};
        }
        const char* msg = (lua_type(state, -1) == LUA_TSTRING) ? lua_tostring(state, -1) : "syntax error";
        if (rc == LUA_ERRMEM || memoryBudgetExceeded) {
            lastErrorDetail = std::format("memory budget exceeded while parsing '{}': {}", chunkName, msg);
            lua_pop(state, 1);
            return std::unexpected(LuaError::MemoryBudgetExceeded);
        }
        lastErrorDetail = std::format("syntax error in '{}': {}", chunkName, msg);
        lua_pop(state, 1);
        return std::unexpected(LuaError::SyntaxError);
    }

    [[nodiscard]] auto RunProtected(int nargs, int nresults, std::string_view chunkName) noexcept -> std::expected<void, ErrorCode> {
        const ProtectedBudgetScope scope(this);
        const int                  rc = lua_pcall(state, nargs, nresults, 0);
        if (rc == 0) {
            lastErrorDetail.clear();
            return {};
        }

        const char* msg = (lua_type(state, -1) == LUA_TSTRING) ? lua_tostring(state, -1) : "runtime error";
        if (instructionBudgetExceeded) {
            lastErrorDetail = std::format("instruction budget exceeded in '{}': {}", chunkName, msg);
            lua_pop(state, 1);
            return std::unexpected(LuaError::InstructionBudgetExceeded);
        }
        if (rc == LUA_ERRMEM || memoryBudgetExceeded) {
            lastErrorDetail = std::format("memory budget exceeded in '{}': {}", chunkName, msg);
            lua_pop(state, 1);
            return std::unexpected(LuaError::MemoryBudgetExceeded);
        }
        lastErrorDetail = std::format("runtime error in '{}': {}", chunkName, msg);
        lua_pop(state, 1);
        return std::unexpected(LuaError::RuntimeError);
    }
};

Vm::Vm() noexcept = default;

Vm::~Vm() {
    if (_impl && _impl->state != nullptr) {
        lua_sethook(_impl->state, nullptr, 0, 0);
        if (_impl->baseAlloc != nullptr) {
            lua_setallocf(_impl->state, _impl->baseAlloc, _impl->baseAllocUd);
        }
        lua_close(_impl->state);
        _impl->state = nullptr;
    }
}

Vm::Vm(Vm&& other) noexcept                    = default;
auto Vm::operator=(Vm&& other) noexcept -> Vm& = default;

auto Vm::Create(LuaBudget budget) noexcept -> std::expected<Vm, ErrorCode> {
    lua_State* raw = luaL_newstate();
    if (raw == nullptr) {
        return std::unexpected(LuaError::AllocationFailed);
    }

    // Run in LuaJIT's fast interpreter rather than compiling traces for one-shot
    // state tables so instruction hooks trigger deterministically.
    luaJIT_setmode(raw, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);

    auto impl       = std::make_unique<Impl>();
    impl->state     = raw;
    impl->budget    = budget;
    impl->baseAlloc = lua_getallocf(raw, &impl->baseAllocUd);
    impl->usedBytes = static_cast<size_t>(lua_gc(raw, LUA_GCCOUNT, 0)) * 1024u + static_cast<size_t>(lua_gc(raw, LUA_GCCOUNTB, 0));

    if (budget.maxMemoryBytes > 0u && impl->usedBytes > budget.maxMemoryBytes) {
        lua_close(raw);
        return std::unexpected(LuaError::MemoryBudgetExceeded);
    }

    lua_setallocf(raw, &Impl::AllocWrapper, impl.get());

    if (lua_cpcall(raw, &OpenSandboxLibsC, nullptr) != 0) {
        const bool oom = impl->memoryBudgetExceeded;
        lua_setallocf(raw, impl->baseAlloc, impl->baseAllocUd);
        lua_close(raw);
        return std::unexpected(oom ? LuaError::MemoryBudgetExceeded : LuaError::AllocationFailed);
    }

    Vm vm;
    vm._impl = std::move(impl);
    return vm;
}

auto Vm::IsValid() const noexcept -> bool {
    return _impl != nullptr && _impl->state != nullptr;
}

auto Vm::Budget() const noexcept -> const LuaBudget& {
    static constexpr LuaBudget kDefault {};
    return _impl != nullptr ? _impl->budget : kDefault;
}

auto Vm::UsedMemoryBytes() const noexcept -> size_t {
    return _impl != nullptr ? _impl->usedBytes : 0u;
}

auto Vm::ExecuteString(std::string_view code, std::string_view chunkName) noexcept -> std::expected<void, ErrorCode> {
    if (!IsValid()) {
        return std::unexpected(LuaError::AllocationFailed);
    }
    const int baseTop = lua_gettop(_impl->state);
    if (auto loadRes = _impl->LoadTextChunk(code, chunkName); !loadRes) {
        lua_settop(_impl->state, baseTop);
        return loadRes;
    }
    auto runRes = _impl->RunProtected(0, 0, chunkName);
    lua_settop(_impl->state, baseTop);
    return runRes;
}

auto Vm::LastErrorDetail() const noexcept -> std::string_view {
    return _impl != nullptr ? std::string_view(_impl->lastErrorDetail) : std::string_view {};
}

auto Vm::EvaluateTop(
    std::string_view code,
    std::string_view chunkName,
    const void*      consumeCtx,
    std::expected<void, ErrorCode> (*consume)(const void* ctx, Value root)
) noexcept -> std::expected<void, ErrorCode> {
    if (!IsValid()) {
        return std::unexpected(LuaError::AllocationFailed);
    }
    lua_State* state   = _impl->state;
    const int  baseTop = lua_gettop(state);

    if (auto loadRes = _impl->LoadTextChunk(code, chunkName); !loadRes) {
        lua_settop(state, baseTop);
        return loadRes;
    }
    if (auto runRes = _impl->RunProtected(0, 1, chunkName); !runRes) {
        lua_settop(state, baseTop);
        return runRes;
    }

    const int   valIdx = lua_gettop(state);
    const Value root(state, valIdx, &_impl->budget, chunkName);
    auto        res = consume(consumeCtx, root);
    lua_settop(state, baseTop);
    return res;
}

auto Vm::EvaluateCallTop(
    std::string_view code,
    std::string_view chunkName,
    const void*      pushCtx,
    int (*pushArgs)(const void* ctx, Builder& builder),
    const void* consumeCtx,
    std::expected<void, ErrorCode> (*consume)(const void* ctx, Value result)
) noexcept -> std::expected<void, ErrorCode> {
    if (!IsValid()) {
        return std::unexpected(LuaError::AllocationFailed);
    }
    lua_State* state   = _impl->state;
    const int  baseTop = lua_gettop(state);

    if (auto loadRes = _impl->LoadTextChunk(code, chunkName); !loadRes) {
        lua_settop(state, baseTop);
        return loadRes;
    }
    if (auto fnRes = _impl->RunProtected(0, 1, chunkName); !fnRes) {
        lua_settop(state, baseTop);
        return fnRes;
    }

    if (lua_type(state, -1) != LUA_TFUNCTION) {
        lua_settop(state, baseTop);
        return std::unexpected(LuaError::NotAFunction);
    }

    Builder   builder(state);
    const int argc = (pushArgs != nullptr) ? pushArgs(pushCtx, builder) : 0;

    if (auto callRes = _impl->RunProtected(argc, 1, chunkName); !callRes) {
        lua_settop(state, baseTop);
        return callRes;
    }

    if (_impl->memoryBudgetExceeded) {
        lua_settop(state, baseTop);
        return std::unexpected(LuaError::MemoryBudgetExceeded);
    }

    const int   resIdx = lua_gettop(state);
    const Value outVal(state, resIdx, &_impl->budget, chunkName);
    auto        res = consume(consumeCtx, outVal);
    lua_settop(state, baseTop);
    return res;
}

void Vm::SetHostContextRaw(TypeID typeId, void* context) noexcept {
    if (IsValid()) {
        _impl->SetHostContext(typeId, context);
    }
}

auto Vm::HostContextRaw(TypeID typeId) const noexcept -> void* {
    return IsValid() ? _impl->FindHostContext(typeId) : nullptr;
}

auto Vm::RegisterUserdataTypeRaw(TypeID typeId, const MetatableSpec& spec) noexcept -> std::expected<void, ErrorCode> {
    if (!IsValid()) {
        return std::unexpected(LuaError::AllocationFailed);
    }
    lua_State* state   = _impl->state;
    const int  baseTop = lua_gettop(state);
    if (!lua_checkstack(state, 8)) {
        return std::unexpected(LuaError::MemoryBudgetExceeded);
    }

    _impl->memoryBudgetExceeded = false;

    RegisteredTypeEntry* existing = _impl->FindMutableType(typeId);
    if (existing != nullptr && existing->metatableRegistryRef != LUA_NOREF) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, existing->metatableRegistryRef);
    } else {
        lua_newtable(state);
    }
    const int mtIdx = ToAbsIndex(state, -1);

    if (!spec.debugName.empty()) {
        lua_pushlstring(state, spec.debugName.data(), spec.debugName.size());
        lua_setfield(state, mtIdx, "__name");
    }
    // Hide the metatable from Lua scripts (`getmetatable(u)` returns false) so
    // sandboxed chunks cannot mutate shared metamethod tables.
    lua_pushboolean(state, 0);
    lua_setfield(state, mtIdx, "__metatable");

    const auto bindSlot = [&](const char* field, HostCallback fn) {
        if (fn != nullptr) {
            _impl->PushWrappedCallback(fn);
            lua_setfield(state, mtIdx, field);
        }
    };

    if (!spec.methods.empty()) {
        lua_createtable(state, 0, static_cast<int>(spec.methods.size()));
        const int methodsIdx = ToAbsIndex(state, -1);
        for (const MethodEntry& method: spec.methods) {
            if (!method.name.empty() && method.callback != nullptr) {
                lua_pushlstring(state, method.name.data(), method.name.size());
                _impl->PushWrappedCallback(method.callback);
                lua_rawset(state, methodsIdx);
            }
        }
        if (spec.index != nullptr) {
            static_assert(sizeof(HostCallback) == sizeof(void*));
            lua_pushlightuserdata(state, std::bit_cast<void*>(spec.index));
            lua_pushcclosure(state, &Impl::CombinedIndexThunk, 2);
            lua_setfield(state, mtIdx, "__index");
        } else {
            lua_pushcclosure(state, &Impl::MethodDispatchIndexThunk, 1);
            lua_setfield(state, mtIdx, "__index");
        }
    } else {
        bindSlot("__index", spec.index);
    }

    bindSlot("__newindex", spec.newIndex);
    bindSlot("__tostring", spec.toString);
    bindSlot("__eq", spec.eq);
    bindSlot("__add", spec.add);
    bindSlot("__sub", spec.sub);
    bindSlot("__mul", spec.mul);
    bindSlot("__div", spec.div);
    bindSlot("__unm", spec.unm);

    if (existing == nullptr) {
        lua_pushvalue(state, mtIdx);
        const int mtRef = luaL_ref(state, LUA_REGISTRYINDEX);

        // Weak-value intern cache `{ [binaryUint64Key] = userdata }` for
        // `CallFrame::PushInternedUserdata`. Because values are weak (`__mode = "v"`),
        // Lua GC reclaims unreferenced POD userdata automatically.
        lua_newtable(state);
        lua_newtable(state);
        lua_pushliteral(state, "v");
        lua_setfield(state, -2, "__mode");
        lua_setmetatable(state, -2);
        const int internRef = luaL_ref(state, LUA_REGISTRYINDEX);

        _impl->registeredTypes.push_back(
            RegisteredTypeEntry {
                .typeId               = typeId,
                .metatableRegistryRef = mtRef,
                .internTableRef       = internRef,
            }
        );
    }

    lua_settop(state, baseTop);
    if (_impl->memoryBudgetExceeded) {
        return std::unexpected(LuaError::MemoryBudgetExceeded);
    }
    return {};
}

auto Vm::WithFrame(FunctionRef<void(CallFrame&)> populate) noexcept -> std::expected<void, ErrorCode> {
    if (!IsValid()) {
        return std::unexpected(LuaError::AllocationFailed);
    }
    lua_State* state   = _impl->state;
    const int  baseTop = lua_gettop(state);
    if (!lua_checkstack(state, 8)) {
        return std::unexpected(LuaError::MemoryBudgetExceeded);
    }

    _impl->memoryBudgetExceeded = false;
    CallFrame::Context frameCtx {
        .vm     = _impl.get(),
        .state  = state,
        .argTop = 0,
        .oomHit = false,
    };
    CallFrame frame(frameCtx);
    populate(frame);
    lua_settop(state, baseTop);
    if (frameCtx.oomHit || _impl->memoryBudgetExceeded) {
        return std::unexpected(LuaError::MemoryBudgetExceeded);
    }
    return {};
}

void Vm::UnregisterListener(ListenerID id) noexcept {
    if (!IsValid() || id == kInvalidListenerID) {
        return;
    }
    lua_State* state   = _impl->state;
    const int  baseTop = lua_gettop(state);
    if (!lua_checkstack(state, 4)) {
        return;
    }
    lua_pushlightuserdata(state, &kListenerTableRegistryKey);
    lua_rawget(state, LUA_REGISTRYINDEX);
    if (lua_istable(state, -1)) {
        std::array<char, sizeof(uint64_t)> keyBytes {};
        EncodeUint64Key(id, keyBytes);
        lua_pushlstring(state, keyBytes.data(), keyBytes.size());
        lua_pushnil(state);
        lua_rawset(state, -3);
    }
    lua_settop(state, baseTop);
}

auto Vm::HasListener(ListenerID id) const noexcept -> bool {
    if (!IsValid() || id == kInvalidListenerID) {
        return false;
    }
    lua_State* state   = _impl->state;
    const int  baseTop = lua_gettop(state);
    if (!lua_checkstack(state, 4)) {
        return false;
    }
    lua_pushlightuserdata(state, &kListenerTableRegistryKey);
    lua_rawget(state, LUA_REGISTRYINDEX);
    bool present = false;
    if (lua_istable(state, -1)) {
        std::array<char, sizeof(uint64_t)> keyBytes {};
        EncodeUint64Key(id, keyBytes);
        lua_pushlstring(state, keyBytes.data(), keyBytes.size());
        lua_rawget(state, -2);
        present = (lua_type(state, -1) == LUA_TFUNCTION);
    }
    lua_settop(state, baseTop);
    return present;
}

auto Vm::InvokeListener(ListenerID id, FunctionRef<void(CallFrame&)> pushArgs, std::string_view contextName) noexcept -> std::expected<void, ErrorCode> {
    if (!IsValid()) {
        return std::unexpected(LuaError::AllocationFailed);
    }
    if (id == kInvalidListenerID) {
        return std::unexpected(LuaError::NotAFunction);
    }

    lua_State* state   = _impl->state;
    const int  baseTop = lua_gettop(state);
    if (!lua_checkstack(state, 8)) {
        return std::unexpected(LuaError::MemoryBudgetExceeded);
    }

    lua_pushlightuserdata(state, &kListenerTableRegistryKey);
    lua_rawget(state, LUA_REGISTRYINDEX);
    if (!lua_istable(state, -1)) {
        lua_settop(state, baseTop);
        return std::unexpected(LuaError::NotAFunction);
    }

    std::array<char, sizeof(uint64_t)> keyBytes {};
    EncodeUint64Key(id, keyBytes);
    lua_pushlstring(state, keyBytes.data(), keyBytes.size());
    lua_rawget(state, -2);
    lua_remove(state, -2); // remove listener table, leaving function on stack

    if (lua_type(state, -1) != LUA_TFUNCTION) {
        lua_settop(state, baseTop);
        return std::unexpected(LuaError::NotAFunction);
    }

    const int          fnIndex = lua_gettop(state);
    CallFrame::Context frameCtx {
        .vm     = _impl.get(),
        .state  = state,
        .argTop = 0,
        .oomHit = false,
    };
    CallFrame frame(frameCtx);
    pushArgs(frame);
    if (frameCtx.oomHit || _impl->memoryBudgetExceeded) {
        lua_settop(state, baseTop);
        return std::unexpected(LuaError::MemoryBudgetExceeded);
    }

    const int nargs  = std::max(0, lua_gettop(state) - fnIndex);
    auto      runRes = _impl->RunProtected(nargs, 0, contextName);
    lua_settop(state, baseTop);
    return runRes;
}

// ----------------------------------------------------------------------------
// CallFrame
// ----------------------------------------------------------------------------

auto CallFrame::ArgCount() const noexcept -> int {
    return _ctx.argTop;
}

auto CallFrame::IsNilOrNone(int index) const noexcept -> bool {
    if (_ctx.state == nullptr || index < 1 || index > _ctx.argTop) {
        return true;
    }
    return lua_isnoneornil(_ctx.state, index);
}

auto CallFrame::IsBool(int index) const noexcept -> bool {
    return _ctx.state != nullptr && index >= 1 && index <= _ctx.argTop && lua_type(_ctx.state, index) == LUA_TBOOLEAN;
}

auto CallFrame::IsNumber(int index) const noexcept -> bool {
    return _ctx.state != nullptr && index >= 1 && index <= _ctx.argTop && lua_type(_ctx.state, index) == LUA_TNUMBER;
}

auto CallFrame::IsString(int index) const noexcept -> bool {
    return _ctx.state != nullptr && index >= 1 && index <= _ctx.argTop && lua_type(_ctx.state, index) == LUA_TSTRING;
}

auto CallFrame::IsTable(int index) const noexcept -> bool {
    return _ctx.state != nullptr && index >= 1 && index <= _ctx.argTop && lua_type(_ctx.state, index) == LUA_TTABLE;
}

auto CallFrame::IsFunction(int index) const noexcept -> bool {
    return _ctx.state != nullptr && index >= 1 && index <= _ctx.argTop && lua_type(_ctx.state, index) == LUA_TFUNCTION;
}

auto CallFrame::ToBool(int index) const noexcept -> bool {
    if (_ctx.state == nullptr || index < 1 || index > _ctx.argTop) {
        return false;
    }
    return lua_toboolean(_ctx.state, index) != 0;
}

auto CallFrame::ReadBool(int index) const noexcept -> std::optional<bool> {
    if (!IsBool(index)) {
        return std::nullopt;
    }
    return lua_toboolean(_ctx.state, index) != 0;
}

auto CallFrame::ReadDouble(int index) const noexcept -> std::optional<double> {
    if (!IsNumber(index)) {
        return std::nullopt;
    }
    const double val = lua_tonumber(_ctx.state, index);
    if (!std::isfinite(val)) {
        return std::nullopt;
    }
    return val;
}

auto CallFrame::ReadFloat(int index) const noexcept -> std::optional<float> {
    const auto val = ReadDouble(index);
    if (!val) {
        return std::nullopt;
    }
    return static_cast<float>(*val);
}

auto CallFrame::ReadInt64(int index) const noexcept -> std::optional<int64_t> {
    const auto val = ReadDouble(index);
    if (!val || std::floor(*val) != *val) {
        return std::nullopt;
    }
    constexpr double kMinInt64 = -9223372036854775808.0;
    constexpr double kMaxExcl  = 9223372036854775808.0;
    if (*val < kMinInt64 || *val >= kMaxExcl) {
        return std::nullopt;
    }
    return static_cast<int64_t>(*val);
}

auto CallFrame::ReadUInt64(int index) const noexcept -> std::optional<uint64_t> {
    const auto val = ReadDouble(index);
    if (!val || *val < 0.0 || std::floor(*val) != *val) {
        return std::nullopt;
    }
    constexpr double kMaxExcl = 18446744073709551616.0;
    if (*val >= kMaxExcl) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(*val);
}

auto CallFrame::ReadStringView(int index) const noexcept -> std::optional<std::string_view> {
    if (!IsString(index)) {
        return std::nullopt;
    }
    size_t      len = 0;
    const char* ptr = lua_tolstring(_ctx.state, index, &len);
    return std::string_view(ptr != nullptr ? ptr : "", len);
}

auto CallFrame::ReadString(int index) const -> std::optional<std::string> {
    const auto sv = ReadStringView(index);
    if (!sv) {
        return std::nullopt;
    }
    return std::string(*sv);
}

auto CallFrame::ReadTableFloatField(int tableIndex, std::string_view field) const noexcept -> std::optional<float> {
    if (!IsTable(tableIndex) || !lua_checkstack(_ctx.state, 3)) {
        return std::nullopt;
    }
    lua_pushlstring(_ctx.state, field.data(), field.size());
    lua_gettable(_ctx.state, tableIndex);
    std::optional<float> out;
    if (lua_type(_ctx.state, -1) == LUA_TNUMBER) {
        const double num = lua_tonumber(_ctx.state, -1);
        if (std::isfinite(num)) {
            out = static_cast<float>(num);
        }
    }
    lua_pop(_ctx.state, 1);
    return out;
}

auto CallFrame::TestUserdataRaw(int index, TypeID typeId) const noexcept -> const void* {
    if (_ctx.state == nullptr || _ctx.vm == nullptr || index < 1 || index > _ctx.argTop) {
        return nullptr;
    }
    if (lua_type(_ctx.state, index) != LUA_TUSERDATA) {
        return nullptr;
    }
    const RegisteredTypeEntry* entry = _ctx.vm->FindType(typeId);
    if (entry == nullptr || entry->metatableRegistryRef == LUA_NOREF) {
        return nullptr;
    }
    if (!lua_checkstack(_ctx.state, 3) || !lua_getmetatable(_ctx.state, index)) {
        return nullptr;
    }
    lua_rawgeti(_ctx.state, LUA_REGISTRYINDEX, entry->metatableRegistryRef);
    const bool matches = (lua_rawequal(_ctx.state, -1, -2) != 0);
    lua_pop(_ctx.state, 2);
    return matches ? lua_touserdata(_ctx.state, index) : nullptr;
}

auto CallFrame::TestMutableUserdataRaw(int index, TypeID typeId) noexcept -> void* {
    return const_cast<void*>(TestUserdataRaw(index, typeId));
}

auto CallFrame::HostContextRaw(TypeID typeId) const noexcept -> void* {
    return _ctx.vm != nullptr ? _ctx.vm->FindHostContext(typeId) : nullptr;
}

void CallFrame::PushNil() noexcept {
    if (_ctx.state == nullptr || !lua_checkstack(_ctx.state, 2)) {
        _ctx.oomHit = true;
        return;
    }
    lua_pushnil(_ctx.state);
}

void CallFrame::PushBool(bool value) noexcept {
    if (_ctx.state == nullptr || !lua_checkstack(_ctx.state, 2)) {
        _ctx.oomHit = true;
        return;
    }
    lua_pushboolean(_ctx.state, value ? 1 : 0);
}

void CallFrame::PushInt64(int64_t value) noexcept {
    if (_ctx.state == nullptr || !lua_checkstack(_ctx.state, 2)) {
        _ctx.oomHit = true;
        return;
    }
    lua_pushnumber(_ctx.state, static_cast<lua_Number>(value));
}

void CallFrame::PushUInt64(uint64_t value) noexcept {
    if (_ctx.state == nullptr || !lua_checkstack(_ctx.state, 2)) {
        _ctx.oomHit = true;
        return;
    }
    lua_pushnumber(_ctx.state, static_cast<lua_Number>(value));
}

void CallFrame::PushDouble(double value) noexcept {
    if (_ctx.state == nullptr || !lua_checkstack(_ctx.state, 2)) {
        _ctx.oomHit = true;
        return;
    }
    lua_pushnumber(_ctx.state, static_cast<lua_Number>(value));
}

void CallFrame::PushString(std::string_view value) noexcept {
    if (_ctx.state == nullptr || !lua_checkstack(_ctx.state, 2)) {
        _ctx.oomHit = true;
        return;
    }
    lua_pushlstring(_ctx.state, value.data(), value.size());
}

void CallFrame::PushFunction(HostCallback callback) noexcept {
    if (_ctx.state == nullptr || _ctx.vm == nullptr || !lua_checkstack(_ctx.state, 3)) {
        _ctx.oomHit = true;
        return;
    }
    _ctx.vm->PushWrappedCallback(callback);
}

void CallFrame::PushUserdataRaw(TypeID typeId, const void* bytes, size_t size, [[maybe_unused]] size_t alignment) noexcept {
    if (_ctx.state == nullptr || _ctx.vm == nullptr || !lua_checkstack(_ctx.state, 3)) {
        _ctx.oomHit = true;
        return;
    }
    const RegisteredTypeEntry* entry = _ctx.vm->FindType(typeId);
    if (entry == nullptr || entry->metatableRegistryRef == LUA_NOREF) {
        lua_pushnil(_ctx.state);
        return;
    }
    void* slot = lua_newuserdata(_ctx.state, size);
    if (slot == nullptr) {
        _ctx.oomHit = true;
        return;
    }
    if (size > 0u && bytes != nullptr) {
        std::memcpy(slot, bytes, size);
    }
    lua_rawgeti(_ctx.state, LUA_REGISTRYINDEX, entry->metatableRegistryRef);
    lua_setmetatable(_ctx.state, -2);
}

void CallFrame::PushInternedUserdataRaw(TypeID typeId, uint64_t cacheKey, const void* bytes, size_t size, [[maybe_unused]] size_t alignment) noexcept {
    if (_ctx.state == nullptr || _ctx.vm == nullptr || !lua_checkstack(_ctx.state, 5)) {
        _ctx.oomHit = true;
        return;
    }
    const RegisteredTypeEntry* entry = _ctx.vm->FindType(typeId);
    if (entry == nullptr || entry->metatableRegistryRef == LUA_NOREF || entry->internTableRef == LUA_NOREF) {
        lua_pushnil(_ctx.state);
        return;
    }

    std::array<char, sizeof(uint64_t)> keyBytes {};
    EncodeUint64Key(cacheKey, keyBytes);

    lua_rawgeti(_ctx.state, LUA_REGISTRYINDEX, entry->internTableRef);
    lua_pushlstring(_ctx.state, keyBytes.data(), keyBytes.size());
    lua_rawget(_ctx.state, -2);
    if (lua_type(_ctx.state, -1) == LUA_TUSERDATA) {
        lua_remove(_ctx.state, -2); // remove intern table, keep cached userdata on top
        return;
    }
    lua_pop(_ctx.state, 1); // pop nil

    void* slot = lua_newuserdata(_ctx.state, size);
    if (slot == nullptr) {
        lua_pop(_ctx.state, 1);
        _ctx.oomHit = true;
        return;
    }
    if (size > 0u && bytes != nullptr) {
        std::memcpy(slot, bytes, size);
    }
    lua_rawgeti(_ctx.state, LUA_REGISTRYINDEX, entry->metatableRegistryRef);
    lua_setmetatable(_ctx.state, -2);

    // Store `internTable[keyBytes] = userdata` while keeping `userdata` on top.
    lua_pushlstring(_ctx.state, keyBytes.data(), keyBytes.size());
    lua_pushvalue(_ctx.state, -2);
    lua_rawset(_ctx.state, -4);
    lua_remove(_ctx.state, -2); // remove intern table
}

void CallFrame::BeginTable(int arrayHint, int hashHint) noexcept {
    if (_ctx.state == nullptr || !lua_checkstack(_ctx.state, 3)) {
        _ctx.oomHit = true;
        return;
    }
    lua_createtable(_ctx.state, arrayHint, hashHint);
}

void CallFrame::SetFieldTop(std::string_view key) noexcept {
    if (_ctx.state == nullptr || lua_gettop(_ctx.state) <= _ctx.argTop + 1 || !lua_checkstack(_ctx.state, 2)) {
        return;
    }
    lua_pushlstring(_ctx.state, key.data(), key.size());
    lua_insert(_ctx.state, -2);
    lua_rawset(_ctx.state, -3);
}

void CallFrame::SetArrayElementTop(int oneBasedIndex) noexcept {
    if (_ctx.state == nullptr || lua_gettop(_ctx.state) <= _ctx.argTop + 1) {
        return;
    }
    lua_rawseti(_ctx.state, -2, oneBasedIndex);
}

void CallFrame::SetGlobalTop(std::string_view name) noexcept {
    if (_ctx.state == nullptr || lua_gettop(_ctx.state) <= _ctx.argTop || !lua_checkstack(_ctx.state, 2)) {
        return;
    }
    const std::string key(name);
    lua_setglobal(_ctx.state, key.c_str());
}

auto CallFrame::RegisterListener(int funcIndex) noexcept -> ListenerID {
    if (!IsFunction(funcIndex) || _ctx.vm == nullptr || !lua_checkstack(_ctx.state, 4)) {
        return kInvalidListenerID;
    }
    const ListenerID id = _ctx.vm->nextListenerId++;
    if (id == kInvalidListenerID) {
        return kInvalidListenerID;
    }

    lua_pushlightuserdata(_ctx.state, &kListenerTableRegistryKey);
    lua_rawget(_ctx.state, LUA_REGISTRYINDEX);
    if (!lua_istable(_ctx.state, -1)) {
        lua_pop(_ctx.state, 1);
        return kInvalidListenerID;
    }

    std::array<char, sizeof(uint64_t)> keyBytes {};
    EncodeUint64Key(id, keyBytes);
    lua_pushlstring(_ctx.state, keyBytes.data(), keyBytes.size());
    lua_pushvalue(_ctx.state, funcIndex);
    lua_rawset(_ctx.state, -3);
    lua_pop(_ctx.state, 1);

    if (_ctx.vm->memoryBudgetExceeded) {
        _ctx.oomHit = true;
        return kInvalidListenerID;
    }
    return id;
}

void CallFrame::UnregisterListener(ListenerID id) noexcept {
    if (_ctx.vm == nullptr || _ctx.state == nullptr || id == kInvalidListenerID || !lua_checkstack(_ctx.state, 4)) {
        return;
    }
    lua_pushlightuserdata(_ctx.state, &kListenerTableRegistryKey);
    lua_rawget(_ctx.state, LUA_REGISTRYINDEX);
    if (lua_istable(_ctx.state, -1)) {
        std::array<char, sizeof(uint64_t)> keyBytes {};
        EncodeUint64Key(id, keyBytes);
        lua_pushlstring(_ctx.state, keyBytes.data(), keyBytes.size());
        lua_pushnil(_ctx.state);
        lua_rawset(_ctx.state, -3);
    }
    lua_pop(_ctx.state, 1);
}

auto CallFrame::HasListener(ListenerID id) const noexcept -> bool {
    if (_ctx.vm == nullptr || _ctx.state == nullptr || id == kInvalidListenerID || !lua_checkstack(_ctx.state, 4)) {
        return false;
    }
    lua_pushlightuserdata(_ctx.state, &kListenerTableRegistryKey);
    lua_rawget(_ctx.state, LUA_REGISTRYINDEX);
    bool present = false;
    if (lua_istable(_ctx.state, -1)) {
        std::array<char, sizeof(uint64_t)> keyBytes {};
        EncodeUint64Key(id, keyBytes);
        lua_pushlstring(_ctx.state, keyBytes.data(), keyBytes.size());
        lua_rawget(_ctx.state, -2);
        present = (lua_type(_ctx.state, -1) == LUA_TFUNCTION);
        lua_pop(_ctx.state, 1);
    }
    lua_pop(_ctx.state, 1);
    return present;
}

// ----------------------------------------------------------------------------
// Value cursor
// ----------------------------------------------------------------------------

auto Value::IsNil() const noexcept -> bool {
    lua_State* state = AsLuaState(_rawState);
    return !IsValid() || lua_type(state, _index) == LUA_TNIL;
}

auto Value::IsTable() const noexcept -> bool {
    lua_State* state = AsLuaState(_rawState);
    return IsValid() && lua_type(state, _index) == LUA_TTABLE;
}

auto Value::GetBool() const noexcept -> std::expected<bool, ErrorCode> {
    lua_State* state = AsLuaState(_rawState);
    if (!IsValid() || lua_type(state, _index) != LUA_TBOOLEAN) {
        return std::unexpected(LuaError::TypeMismatch);
    }
    return lua_toboolean(state, _index) != 0;
}

auto Value::GetInt() const noexcept -> std::expected<int64_t, ErrorCode> {
    lua_State* state = AsLuaState(_rawState);
    if (!IsValid() || lua_type(state, _index) != LUA_TNUMBER) {
        return std::unexpected(LuaError::TypeMismatch);
    }
    const double num = lua_tonumber(state, _index);
    if (!std::isfinite(num) || std::floor(num) != num) {
        return std::unexpected(LuaError::TypeMismatch);
    }
    // Exact integer range check against double -> int64 conversion bounds.
    constexpr double kMinInt64 = -9223372036854775808.0; // -2^63 (exact in IEEE-754 double)
    constexpr double kMaxExcl  = 9223372036854775808.0;  // +2^63 (first out-of-range power of 2)
    if (num < kMinInt64 || num >= kMaxExcl) {
        return std::unexpected(LuaError::OutOfRange);
    }
    return static_cast<int64_t>(num);
}

auto Value::GetUInt() const noexcept -> std::expected<uint64_t, ErrorCode> {
    lua_State* state = AsLuaState(_rawState);
    if (!IsValid() || lua_type(state, _index) != LUA_TNUMBER) {
        return std::unexpected(LuaError::TypeMismatch);
    }
    const double num = lua_tonumber(state, _index);
    if (!std::isfinite(num) || std::floor(num) != num || num < 0.0) {
        return std::unexpected(num < 0.0 ? LuaError::OutOfRange : LuaError::TypeMismatch);
    }
    constexpr double kMaxExcl = 18446744073709551616.0; // +2^64
    if (num >= kMaxExcl) {
        return std::unexpected(LuaError::OutOfRange);
    }
    return static_cast<uint64_t>(num);
}

auto Value::GetDouble() const noexcept -> std::expected<double, ErrorCode> {
    lua_State* state = AsLuaState(_rawState);
    if (!IsValid() || lua_type(state, _index) != LUA_TNUMBER) {
        return std::unexpected(LuaError::TypeMismatch);
    }
    const double num = lua_tonumber(state, _index);
    if (!std::isfinite(num)) {
        return std::unexpected(LuaError::OutOfRange);
    }
    return num;
}

auto Value::GetString() const noexcept -> std::expected<std::string_view, ErrorCode> {
    lua_State* state = AsLuaState(_rawState);
    // Strictly check LUA_TSTRING first so Lua never mutates numbers in-place on the stack.
    if (!IsValid() || lua_type(state, _index) != LUA_TSTRING) {
        return std::unexpected(LuaError::TypeMismatch);
    }
    size_t      len = 0;
    const char* ptr = lua_tolstring(state, _index, &len);
    if (ptr == nullptr) {
        return std::unexpected(LuaError::TypeMismatch);
    }
    return std::string_view(ptr, len);
}

auto Value::SequenceLength() const noexcept -> std::optional<size_t> {
    if (!IsTable()) {
        return std::nullopt;
    }
    lua_State* state = AsLuaState(_rawState);
    if (!lua_checkstack(state, 4)) {
        return std::nullopt;
    }

    size_t count  = 0;
    size_t maxKey = 0;

    lua_pushnil(state);
    while (lua_next(state, _index) != 0) {
        // Pop value immediately; keep key at -1 for next lua_next iteration.
        lua_pop(state, 1);

        if (lua_type(state, -1) != LUA_TNUMBER) {
            lua_pop(state, 1);
            return std::nullopt;
        }
        const double k = lua_tonumber(state, -1);
        if (!std::isfinite(k) || k < 1.0 || std::floor(k) != k) {
            lua_pop(state, 1);
            return std::nullopt;
        }
        const auto idx = static_cast<size_t>(k);
        ++count;
        maxKey = std::max(maxKey, idx);
    }

    if (count != maxKey) {
        return std::nullopt;
    }
    return count;
}

auto Value::GetTableStringKeys() const -> std::expected<std::vector<std::string>, ErrorCode> {
    if (!IsTable()) {
        return std::unexpected(LuaError::NotATable);
    }
    lua_State* state = AsLuaState(_rawState);
    if (!lua_checkstack(state, 4)) {
        return std::unexpected(LuaError::MemoryBudgetExceeded);
    }

    std::vector<std::string> keys;
    lua_pushnil(state);
    while (lua_next(state, _index) != 0) {
        lua_pop(state, 1); // pop value, keep key on top
        if (lua_type(state, -1) != LUA_TSTRING) {
            lua_pop(state, 1);
            return std::unexpected(LuaError::TypeMismatch);
        }
        size_t      len = 0;
        const char* str = lua_tolstring(state, -1, &len);
        keys.emplace_back(str, len);
    }
    return keys;
}

auto Value::WithKey(std::string_view key, const void* ctx, std::expected<void, ErrorCode> (*visit)(const void* ctx, Value child)) const noexcept
    -> std::expected<bool, ErrorCode> {
    if (!IsTable()) {
        return std::unexpected(LuaError::NotATable);
    }
    lua_State* state = AsLuaState(_rawState);
    if (!lua_checkstack(state, 4)) {
        return std::unexpected(LuaError::MemoryBudgetExceeded);
    }

    lua_pushlstring(state, key.data(), key.size());
    lua_rawget(state, _index);
    const int childIdx = ToAbsIndex(state, -1);
    if (lua_type(state, childIdx) == LUA_TNIL) {
        lua_pop(state, 1);
        return false;
    }

    std::string childPath;
    childPath.reserve(_path.size() + 1u + key.size());
    if (!_path.empty()) {
        childPath.append(_path);
        childPath.push_back('.');
    }
    childPath.append(key);

    const Value child(state, childIdx, _budget, childPath);
    auto        res = visit(ctx, child);
    lua_pop(state, 1);
    if (!res) {
        return std::unexpected(res.error());
    }
    return true;
}

auto Value::WithElement(size_t zeroBasedIndex, const void* ctx, std::expected<void, ErrorCode> (*visit)(const void* ctx, Value element)) const noexcept
    -> std::expected<void, ErrorCode> {
    if (!IsTable()) {
        return std::unexpected(LuaError::NotATable);
    }
    lua_State* state = AsLuaState(_rawState);
    if (!lua_checkstack(state, 4)) {
        return std::unexpected(LuaError::MemoryBudgetExceeded);
    }

    lua_rawgeti(state, _index, static_cast<int>(zeroBasedIndex + 1u));
    const int elemIdx = ToAbsIndex(state, -1);

    std::string elemPath(_path);
    elemPath.push_back('[');
    elemPath.append(std::to_string(zeroBasedIndex + 1u));
    elemPath.push_back(']');

    const Value elem(state, elemIdx, _budget, elemPath);
    auto        res = visit(ctx, elem);
    lua_pop(state, 1);
    return res;
}

void Builder::PushNil() noexcept {
    lua_State* state = AsLuaState(_rawState);
    if (state != nullptr && lua_checkstack(state, 2)) {
        lua_pushnil(state);
    }
}

void Builder::PushBool(bool value) noexcept {
    lua_State* state = AsLuaState(_rawState);
    if (state != nullptr && lua_checkstack(state, 2)) {
        lua_pushboolean(state, value ? 1 : 0);
    }
}

void Builder::PushInt(int64_t value) noexcept {
    lua_State* state = AsLuaState(_rawState);
    if (state != nullptr && lua_checkstack(state, 2)) {
        lua_pushnumber(state, static_cast<lua_Number>(value));
    }
}

void Builder::PushUInt(uint64_t value) noexcept {
    lua_State* state = AsLuaState(_rawState);
    if (state != nullptr && lua_checkstack(state, 2)) {
        lua_pushnumber(state, static_cast<lua_Number>(value));
    }
}

void Builder::PushDouble(double value) noexcept {
    lua_State* state = AsLuaState(_rawState);
    if (state != nullptr && lua_checkstack(state, 2)) {
        lua_pushnumber(state, static_cast<lua_Number>(value));
    }
}

void Builder::PushString(std::string_view value) noexcept {
    lua_State* state = AsLuaState(_rawState);
    if (state != nullptr && lua_checkstack(state, 2)) {
        lua_pushlstring(state, value.data(), value.size());
    }
}

void Builder::BeginTable(int arrayHint, int hashHint) noexcept {
    lua_State* state = AsLuaState(_rawState);
    if (state != nullptr && lua_checkstack(state, 3)) {
        lua_createtable(state, arrayHint, hashHint);
    }
}

void Builder::SetFieldTop(std::string_view key) noexcept {
    lua_State* state = AsLuaState(_rawState);
    if (state == nullptr || lua_gettop(state) < 2 || !lua_checkstack(state, 2)) {
        return;
    }
    // Stack: ... table(-2), val(-1) -> push key -> swap -> rawset
    lua_pushlstring(state, key.data(), key.size());
    lua_insert(state, -2);
    lua_rawset(state, -3);
}

void Builder::SetElementTop(size_t zeroBasedIndex) noexcept {
    lua_State* state = AsLuaState(_rawState);
    if (state == nullptr || lua_gettop(state) < 2) {
        return;
    }
    lua_rawseti(state, -2, static_cast<int>(zeroBasedIndex + 1u));
}

} // namespace ZHLN::ReflectLua
