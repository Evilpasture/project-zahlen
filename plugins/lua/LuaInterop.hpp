// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Type-safe, lifetime-decoupled C++ <-> LuaJIT C API boundary.
//
// Architectural invariants:
//   1. Classic Lua C API (`lua_*` / `luaL_*`) is strictly quarantined inside
//      `plugins/lua/*.cpp`; downstream layers never include `<lua.h>` or touch
//      a raw `lua_State*`.
//   2. Zero cross-heap GC co-ownership (`LuaPodUserdata`): every userdata slot
//      allocated on the Lua heap is trivially copyable and trivially
//      destructible (value types and opaque integer IDs such as `InstanceId`),
//      so Lua GC sweeps plain memory with zero C++ `__gc` destructors while C++
//      owns its canonical object graph and connection tokens independently.
//   3. Lua closures stay on the Lua heap inside a VM-owned listener table keyed
//      by monotonic `ListenerID`s, and every C++ -> Lua callback enters through
//      the same deterministic instruction- and memory-budgeted protected call
//      path as `Vm::ExecuteString`.

#include <Zahlen/Common.h>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <lua/ReflectLua.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace ZHLN::ReflectLua {

namespace TemplatedDetail {

template <typename E>
struct ReflectedEnumTable {
    std::array<E, ZHLN::Reflect::EnumCount<E>()> values {};
    size_t                                       count = 0;
};

template <typename E>
consteval auto BuildReflectedEnumTable() -> ReflectedEnumTable<E> {
    ReflectedEnumTable<E> result;
    constexpr auto        names = ZHLN::Reflect::EnumNames<E>();
    for (const std::string_view name: names) {
        const auto value = ZHLN::Reflect::StringToEnum<E>(name);
        if (!value) {
            continue;
        }
        bool seen = false;
        for (size_t i = 0; i < result.count; ++i) {
            if (result.values[i] == *value) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            result.values[result.count++] = *value;
        }
    }
    return result;
}

template <typename E>
[[nodiscard]] constexpr auto CanonicalEnumName(E value) noexcept -> std::string_view {
    constexpr auto names = ZHLN::Reflect::EnumNames<E>();
    for (const std::string_view name: names) {
        const auto candidate = ZHLN::Reflect::StringToEnum<E>(name);
        if (candidate && *candidate == value) {
            return name;
        }
    }
    return ZHLN::Reflect::EnumToString(value);
}

} // namespace TemplatedDetail

// Non-owning view over an active Lua call frame (arguments `1..ArgCount()` plus
// value/userdata/table construction on top of the stack).
class ZHLN_STATIC_API CallFrame {
  public:
    CallFrame(const CallFrame&)                    = delete;
    auto operator=(const CallFrame&) -> CallFrame& = delete;

    [[nodiscard]] auto ArgCount() const noexcept -> int;

    // 1-based argument queries (`1 <= index <= ArgCount()`).
    [[nodiscard]] auto IsNilOrNone(int index) const noexcept -> bool;
    [[nodiscard]] auto IsBool(int index) const noexcept -> bool;
    [[nodiscard]] auto IsNumber(int index) const noexcept -> bool;
    [[nodiscard]] auto IsString(int index) const noexcept -> bool;
    [[nodiscard]] auto IsTable(int index) const noexcept -> bool;
    [[nodiscard]] auto IsFunction(int index) const noexcept -> bool;

    // Lua truthiness (`false` and `nil`/none are false; everything else true).
    [[nodiscard]] auto ToBool(int index) const noexcept -> bool;
    [[nodiscard]] auto ReadBool(int index) const noexcept -> std::optional<bool>;
    [[nodiscard]] auto ReadDouble(int index) const noexcept -> std::optional<double>;
    [[nodiscard]] auto ReadFloat(int index) const noexcept -> std::optional<float>;
    [[nodiscard]] auto ReadInt64(int index) const noexcept -> std::optional<int64_t>;
    [[nodiscard]] auto ReadUInt64(int index) const noexcept -> std::optional<uint64_t>;
    [[nodiscard]] auto ReadStringView(int index) const noexcept -> std::optional<std::string_view>;
    [[nodiscard]] auto ReadString(int index) const -> std::optional<std::string>;

    // Reads a numeric field (`table[field]`) from a Lua table at `tableIndex`
    // (1-based argument index) without disturbing the stack.
    [[nodiscard]] auto ReadTableFloatField(int tableIndex, std::string_view field) const noexcept -> std::optional<float>;

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] auto ReadEnum(int index) const noexcept -> std::optional<E> {
        if (const auto str = ReadStringView(index)) {
            if (str->empty()) {
                return std::nullopt;
            }
            constexpr auto names = ZHLN::Reflect::EnumNames<E>();
            for (const std::string_view candidate: names) {
                if (!TemplatedDetail::EqualAsciiIgnoreCase(*str, candidate)) {
                    continue;
                }
                if (const auto parsed = ZHLN::Reflect::StringToEnum<E>(candidate)) {
                    return *parsed;
                }
                return std::nullopt;
            }
            return std::nullopt;
        }
        if (const auto numberOpt = ReadDouble(index)) {
            const double   number = *numberOpt;
            constexpr auto values = TemplatedDetail::BuildReflectedEnumTable<E>();
            if (!std::isfinite(number) || number < 0.0 ||
                number >= static_cast<double>(values.count) || std::floor(number) != number) {
                return std::nullopt;
            }
            return values.values[static_cast<size_t>(number)];
        }
        return std::nullopt;
    }

    // Typed POD userdata inspection (`nullptr` on type or metatable mismatch).
    template <LuaPodUserdata T>
    [[nodiscard]] auto TestUserdata(int index) const noexcept -> const std::remove_cvref_t<T>* {
        using Clean = std::remove_cvref_t<T>;
        return static_cast<const Clean*>(TestUserdataRaw(index, TypeIdOf<Clean>()));
    }

    template <LuaPodUserdata T>
    [[nodiscard]] auto TestMutableUserdata(int index) noexcept -> std::remove_cvref_t<T>* {
        using Clean = std::remove_cvref_t<T>;
        return static_cast<Clean*>(TestMutableUserdataRaw(index, TypeIdOf<Clean>()));
    }

    // Non-owning host context bound onto the owning `Vm` via `Vm::SetHostContext<T>`.
    template <typename T>
    [[nodiscard]] auto HostContext() const noexcept -> T* {
        using Clean = std::remove_cvref_t<T>;
        return static_cast<Clean*>(HostContextRaw(TypeIdOf<Clean>()));
    }

    // Stack pushers (push 1 value onto the Lua return/construction stack).
    void PushNil() noexcept;
    void PushBool(bool value) noexcept;
    void PushInt64(int64_t value) noexcept;
    void PushUInt64(uint64_t value) noexcept;
    void PushDouble(double value) noexcept;
    void PushFloat(float value) noexcept {
        PushDouble(static_cast<double>(value));
    }
    void PushString(std::string_view value) noexcept;
    void PushFunction(HostCallback callback) noexcept;

    template <typename E>
        requires std::is_enum_v<E>
    void PushEnum(E value) noexcept {
        PushString(TemplatedDetail::CanonicalEnumName(value));
    }

    template <LuaPodUserdata T>
    void PushUserdata(const T& value) noexcept {
        using Clean = std::remove_cvref_t<T>;
        PushUserdataRaw(TypeIdOf<Clean>(), &value, sizeof(Clean), alignof(Clean));
    }

    // Interned POD userdata: maps `(TypeID, cacheKey)` in a VM-owned weak-value
    // table so repeated pushes of the same live `cacheKey` yield the exact same
    // Lua userdata identity (`rawequal`, `==`, and table-key identity), while
    // allowing Lua GC to collect unreferenced userdata freely with zero C++
    // finalizers.
    template <LuaPodUserdata T>
    void PushInternedUserdata(uint64_t cacheKey, const T& value) noexcept {
        using Clean = std::remove_cvref_t<T>;
        PushInternedUserdataRaw(TypeIdOf<Clean>(), cacheKey, &value, sizeof(Clean), alignof(Clean));
    }

    // Table construction on top of the stack (mirrors `Builder`).
    void BeginTable(int arrayHint = 0, int hashHint = 0) noexcept;
    // Pops the value on top of the stack and assigns `table[key] = top`.
    void SetFieldTop(std::string_view key) noexcept;
    // Pops the value on top of the stack and assigns `table[oneBasedIndex] = top`.
    void SetArrayElementTop(int oneBasedIndex) noexcept;
    // Pops the value on top of the stack and assigns `_G[name] = top`.
    void SetGlobalTop(std::string_view name) noexcept;

    // Stores the Lua function at 1-based argument `funcIndex` into the VM's
    // internal Lua-side listener table and returns its `ListenerID`
    // (`kInvalidListenerID` if `funcIndex` is not a function or OOM).
    [[nodiscard]] auto RegisterListener(int funcIndex) noexcept -> ListenerID;
    void               UnregisterListener(ListenerID id) noexcept;
    [[nodiscard]] auto HasListener(ListenerID id) const noexcept -> bool;

  private:
    friend class Vm;
    struct Context;

    explicit CallFrame(Context& ctx) noexcept: _ctx(ctx) {
    }

    [[nodiscard]] auto TestUserdataRaw(int index, TypeID typeId) const noexcept -> const void*;
    [[nodiscard]] auto TestMutableUserdataRaw(int index, TypeID typeId) noexcept -> void*;
    [[nodiscard]] auto HostContextRaw(TypeID typeId) const noexcept -> void*;
    void               PushUserdataRaw(TypeID typeId, const void* bytes, size_t size, size_t alignment) noexcept;
    void               PushInternedUserdataRaw(
                      TypeID      typeId,
                      uint64_t    cacheKey,
                      const void* bytes,
                      size_t      size,
                      size_t      alignment
                  ) noexcept;

    Context& _ctx;
};

} // namespace ZHLN::ReflectLua
