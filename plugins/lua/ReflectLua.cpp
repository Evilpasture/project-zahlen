// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <lua/ReflectLua.hpp>

#include <lua.hpp>
#include <luajit.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace ZHLN::ReflectLua {

namespace {

    constexpr int kHookSliceInstructions = 4096;

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
        ZHLN::Log("[Lua] {}", line);
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
            "dofile",
            "loadfile",
            "load",
            "loadstring",
            "collectgarbage",
            "newproxy",
            "getfenv",
            "setfenv",
            "gcinfo",
        };
        for (const char* name: kForbiddenGlobals) {
            lua_pushnil(state);
            lua_setglobal(state, name);
        }

        lua_pushcfunction(state, &SafeLogPrint);
        lua_setglobal(state, "print");
        return 0;
    }

} // namespace

struct LuaVm::Impl {
    lua_State* state                     = nullptr;
    lua_Alloc  baseAlloc                 = nullptr;
    void*      baseAllocUd               = nullptr;
    LuaBudget  budget                    = {};
    size_t     usedBytes                 = 0;
    int64_t    remainingInstructions     = 0;
    bool       instructionBudgetExceeded = false;
    bool       memoryBudgetExceeded      = false;

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

    void ArmBudgets() noexcept {
        instructionBudgetExceeded = false;
        memoryBudgetExceeded      = false;
        if (budget.maxInstructions > 0u) {
            remainingInstructions = static_cast<int64_t>(budget.maxInstructions);
            const int slice       = static_cast<int>(
                std::min<uint32_t>(budget.maxInstructions, static_cast<uint32_t>(kHookSliceInstructions))
            );
            lua_sethook(state, &Impl::InstructionHook, LUA_MASKCOUNT, slice);
        } else {
            lua_sethook(state, nullptr, 0, 0);
        }
    }

    void DisarmBudgets() noexcept {
        if (state != nullptr) {
            lua_sethook(state, nullptr, 0, 0);
        }
    }

    [[nodiscard]] auto LoadTextChunk(std::string_view code, std::string_view chunkName) noexcept
        -> std::expected<void, ErrorCode> {
        memoryBudgetExceeded = false;
        const std::string nameBuf(chunkName);
        // Mode "t" permits plain-text Lua source only and rejects binary bytecode.
        const int rc = luaL_loadbufferx(state, code.data(), code.size(), nameBuf.c_str(), "t");
        if (rc == 0) {
            return {};
        }
        const char* msg = (lua_type(state, -1) == LUA_TSTRING) ? lua_tostring(state, -1) : "syntax error";
        if (rc == LUA_ERRMEM || memoryBudgetExceeded) {
            ZHLN::Log("[Lua] memory budget exceeded while parsing '{}': {}", chunkName, msg);
            lua_pop(state, 1);
            return std::unexpected(LuaError::MemoryBudgetExceeded);
        }
        ZHLN::Log("[Lua] syntax error in '{}': {}", chunkName, msg);
        lua_pop(state, 1);
        return std::unexpected(LuaError::SyntaxError);
    }

    [[nodiscard]] auto RunProtected(int nargs, int nresults, std::string_view chunkName) noexcept
        -> std::expected<void, ErrorCode> {
        ArmBudgets();
        const int rc = lua_pcall(state, nargs, nresults, 0);
        DisarmBudgets();
        if (rc == 0) {
            return {};
        }

        const char* msg = (lua_type(state, -1) == LUA_TSTRING) ? lua_tostring(state, -1) : "runtime error";
        if (instructionBudgetExceeded) {
            ZHLN::Log("[Lua] instruction budget ({}) exceeded in '{}'", budget.maxInstructions, chunkName);
            lua_pop(state, 1);
            return std::unexpected(LuaError::InstructionBudgetExceeded);
        }
        if (rc == LUA_ERRMEM || memoryBudgetExceeded) {
            ZHLN::Log("[Lua] memory budget ({} B) exceeded in '{}'", budget.maxMemoryBytes, chunkName);
            lua_pop(state, 1);
            return std::unexpected(LuaError::MemoryBudgetExceeded);
        }
        ZHLN::Log("[Lua] runtime error in '{}': {}", chunkName, msg);
        lua_pop(state, 1);
        return std::unexpected(LuaError::RuntimeError);
    }
};

LuaVm::LuaVm() noexcept = default;

LuaVm::~LuaVm() {
    if (_impl && _impl->state != nullptr) {
        lua_sethook(_impl->state, nullptr, 0, 0);
        if (_impl->baseAlloc != nullptr) {
            lua_setallocf(_impl->state, _impl->baseAlloc, _impl->baseAllocUd);
        }
        lua_close(_impl->state);
        _impl->state = nullptr;
    }
}

LuaVm::LuaVm(LuaVm&& other) noexcept = default;
auto LuaVm::operator=(LuaVm&& other) noexcept -> LuaVm& = default;

auto LuaVm::Create(LuaBudget budget) noexcept -> std::expected<LuaVm, ErrorCode> {
    lua_State* raw = luaL_newstate();
    if (raw == nullptr) {
        return std::unexpected(LuaError::AllocationFailed);
    }

    // Run in LuaJIT's fast interpreter rather than compiling traces for one-shot
    // state tables so instruction hooks trigger deterministically.
    luaJIT_setmode(raw, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);

    auto impl    = std::make_unique<Impl>();
    impl->state  = raw;
    impl->budget = budget;
    impl->baseAlloc = lua_getallocf(raw, &impl->baseAllocUd);
    impl->usedBytes = static_cast<size_t>(lua_gc(raw, LUA_GCCOUNT, 0)) * 1024u +
                      static_cast<size_t>(lua_gc(raw, LUA_GCCOUNTB, 0));

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

    LuaVm vm;
    vm._impl = std::move(impl);
    return vm;
}

auto LuaVm::IsValid() const noexcept -> bool {
    return _impl != nullptr && _impl->state != nullptr;
}

auto LuaVm::Budget() const noexcept -> const LuaBudget& {
    static constexpr LuaBudget kDefault {};
    return _impl != nullptr ? _impl->budget : kDefault;
}

auto LuaVm::UsedMemoryBytes() const noexcept -> size_t {
    return _impl != nullptr ? _impl->usedBytes : 0u;
}

auto LuaVm::RawState() noexcept -> lua_State* {
    return _impl != nullptr ? _impl->state : nullptr;
}

auto LuaVm::ExecuteString(std::string_view code, std::string_view chunkName) noexcept
    -> std::expected<void, ErrorCode> {
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

auto LuaVm::ExecuteFile(const std::filesystem::path& path) noexcept -> std::expected<void, ErrorCode> {
    auto source = ReadLuaFileText(path);
    if (!source) {
        return std::unexpected(source.error());
    }
    const std::string chunkName = "@" + path.generic_string();
    return ExecuteString(*source, chunkName);
}

auto LuaVm::EvaluateTop(
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

auto LuaVm::EvaluateCallTop(
    std::string_view code,
    std::string_view chunkName,
    const void*      pushCtx,
    int (*pushArgs)(const void* ctx, Builder& builder),
    const void*      consumeCtx,
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

// ----------------------------------------------------------------------------
// Value cursor
// ----------------------------------------------------------------------------

auto Value::IsNil() const noexcept -> bool {
    return !IsValid() || lua_type(_state, _index) == LUA_TNIL;
}

auto Value::IsTable() const noexcept -> bool {
    return IsValid() && lua_type(_state, _index) == LUA_TTABLE;
}

auto Value::GetBool() const noexcept -> std::expected<bool, ErrorCode> {
    if (!IsValid() || lua_type(_state, _index) != LUA_TBOOLEAN) {
        return std::unexpected(LuaError::TypeMismatch);
    }
    return lua_toboolean(_state, _index) != 0;
}

auto Value::GetInt() const noexcept -> std::expected<int64_t, ErrorCode> {
    if (!IsValid() || lua_type(_state, _index) != LUA_TNUMBER) {
        return std::unexpected(LuaError::TypeMismatch);
    }
    const double num = lua_tonumber(_state, _index);
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
    if (!IsValid() || lua_type(_state, _index) != LUA_TNUMBER) {
        return std::unexpected(LuaError::TypeMismatch);
    }
    const double num = lua_tonumber(_state, _index);
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
    if (!IsValid() || lua_type(_state, _index) != LUA_TNUMBER) {
        return std::unexpected(LuaError::TypeMismatch);
    }
    const double num = lua_tonumber(_state, _index);
    if (!std::isfinite(num)) {
        return std::unexpected(LuaError::OutOfRange);
    }
    return num;
}

auto Value::GetString() const noexcept -> std::expected<std::string_view, ErrorCode> {
    // Strictly check LUA_TSTRING first so Lua never mutates numbers in-place on the stack.
    if (!IsValid() || lua_type(_state, _index) != LUA_TSTRING) {
        return std::unexpected(LuaError::TypeMismatch);
    }
    size_t      len = 0;
    const char* ptr = lua_tolstring(_state, _index, &len);
    if (ptr == nullptr) {
        return std::unexpected(LuaError::TypeMismatch);
    }
    return std::string_view(ptr, len);
}

auto Value::IsSequence(size_t& outLength) const noexcept -> bool {
    outLength = 0;
    if (!IsTable()) {
        return false;
    }
    if (!lua_checkstack(_state, 4)) {
        return false;
    }

    size_t count  = 0;
    size_t maxKey = 0;

    lua_pushnil(_state);
    while (lua_next(_state, _index) != 0) {
        // Pop value immediately; keep key at -1 for next lua_next iteration.
        lua_pop(_state, 1);

        if (lua_type(_state, -1) != LUA_TNUMBER) {
            lua_pop(_state, 1);
            return false;
        }
        const double k = lua_tonumber(_state, -1);
        if (!std::isfinite(k) || k < 1.0 || std::floor(k) != k) {
            lua_pop(_state, 1);
            return false;
        }
        const auto idx = static_cast<size_t>(k);
        ++count;
        maxKey = std::max(maxKey, idx);
    }

    if (count != maxKey) {
        return false;
    }
    outLength = count;
    return true;
}

auto Value::GetTableStringKeys() const -> std::expected<std::vector<std::string>, ErrorCode> {
    if (!IsTable()) {
        return std::unexpected(LuaError::NotATable);
    }
    if (!lua_checkstack(_state, 4)) {
        return std::unexpected(LuaError::MemoryBudgetExceeded);
    }

    std::vector<std::string> keys;
    lua_pushnil(_state);
    while (lua_next(_state, _index) != 0) {
        lua_pop(_state, 1); // pop value, keep key on top
        if (lua_type(_state, -1) != LUA_TSTRING) {
            lua_pop(_state, 1);
            return std::unexpected(LuaError::TypeMismatch);
        }
        size_t      len = 0;
        const char* str = lua_tolstring(_state, -1, &len);
        keys.emplace_back(str, len);
    }
    return keys;
}

auto Value::WithKey(
    std::string_view key,
    const void*      ctx,
    std::expected<void, ErrorCode> (*visit)(const void* ctx, Value child)
) const noexcept -> std::expected<bool, ErrorCode> {
    if (!IsTable()) {
        return std::unexpected(LuaError::NotATable);
    }
    if (!lua_checkstack(_state, 4)) {
        return std::unexpected(LuaError::MemoryBudgetExceeded);
    }

    lua_pushlstring(_state, key.data(), key.size());
    lua_rawget(_state, _index);
    const int childIdx = ToAbsIndex(_state, -1);
    if (lua_type(_state, childIdx) == LUA_TNIL) {
        lua_pop(_state, 1);
        return false;
    }

    std::string childPath;
    childPath.reserve(_path.size() + 1u + key.size());
    if (!_path.empty()) {
        childPath.append(_path);
        childPath.push_back('.');
    }
    childPath.append(key);

    const Value child(_state, childIdx, _budget, childPath);
    auto        res = visit(ctx, child);
    lua_pop(_state, 1);
    if (!res) {
        return std::unexpected(res.error());
    }
    return true;
}

auto Value::WithElement(
    size_t      zeroBasedIndex,
    const void* ctx,
    std::expected<void, ErrorCode> (*visit)(const void* ctx, Value element)
) const noexcept -> std::expected<void, ErrorCode> {
    if (!IsTable()) {
        return std::unexpected(LuaError::NotATable);
    }
    if (!lua_checkstack(_state, 4)) {
        return std::unexpected(LuaError::MemoryBudgetExceeded);
    }

    lua_rawgeti(_state, _index, static_cast<int>(zeroBasedIndex + 1u));
    const int elemIdx = ToAbsIndex(_state, -1);

    std::string elemPath(_path);
    elemPath.push_back('[');
    elemPath.append(std::to_string(zeroBasedIndex + 1u));
    elemPath.push_back(']');

    const Value elem(_state, elemIdx, _budget, elemPath);
    auto        res = visit(ctx, elem);
    lua_pop(_state, 1);
    return res;
}

// ----------------------------------------------------------------------------
// Builder
// ----------------------------------------------------------------------------

void Builder::PushNil() noexcept {
    if (_state != nullptr && lua_checkstack(_state, 2)) {
        lua_pushnil(_state);
    }
}

void Builder::PushBool(bool value) noexcept {
    if (_state != nullptr && lua_checkstack(_state, 2)) {
        lua_pushboolean(_state, value ? 1 : 0);
    }
}

void Builder::PushInt(int64_t value) noexcept {
    if (_state != nullptr && lua_checkstack(_state, 2)) {
        lua_pushnumber(_state, static_cast<lua_Number>(value));
    }
}

void Builder::PushUInt(uint64_t value) noexcept {
    if (_state != nullptr && lua_checkstack(_state, 2)) {
        lua_pushnumber(_state, static_cast<lua_Number>(value));
    }
}

void Builder::PushDouble(double value) noexcept {
    if (_state != nullptr && lua_checkstack(_state, 2)) {
        lua_pushnumber(_state, static_cast<lua_Number>(value));
    }
}

void Builder::PushString(std::string_view value) noexcept {
    if (_state != nullptr && lua_checkstack(_state, 2)) {
        lua_pushlstring(_state, value.data(), value.size());
    }
}

void Builder::BeginTable(int arrayHint, int hashHint) noexcept {
    if (_state != nullptr && lua_checkstack(_state, 3)) {
        lua_createtable(_state, arrayHint, hashHint);
    }
}

void Builder::SetFieldTop(std::string_view key) noexcept {
    if (_state == nullptr || lua_gettop(_state) < 2 || !lua_checkstack(_state, 2)) {
        return;
    }
    // Stack: ... table(-2), val(-1) -> push key -> swap -> rawset
    lua_pushlstring(_state, key.data(), key.size());
    lua_insert(_state, -2);
    lua_rawset(_state, -3);
}

void Builder::SetElementTop(size_t zeroBasedIndex) noexcept {
    if (_state == nullptr || lua_gettop(_state) < 2) {
        return;
    }
    lua_rawseti(_state, -2, static_cast<int>(zeroBasedIndex + 1u));
}

// ----------------------------------------------------------------------------
// File helper
// ----------------------------------------------------------------------------

auto ReadLuaFileText(const std::filesystem::path& path) noexcept -> std::expected<std::string, ErrorCode> {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        ZHLN::Log("[Lua] failed to open '{}'", path.string());
        return std::unexpected(LuaError::FileReadFailed);
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        ZHLN::Log("[Lua] failed while reading '{}'", path.string());
        return std::unexpected(LuaError::FileReadFailed);
    }
    return buffer.str();
}

} // namespace ZHLN::ReflectLua
