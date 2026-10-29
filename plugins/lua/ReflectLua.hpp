// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// plugins/lua/ReflectLua.hpp
//
// Reflection-driven Lua state evaluation and transactional patching.
//
// Same architectural contract as plugins/toml/TOML.hpp:
//   * C++26 reflected structs and enums ARE the schema -- no manual glue,
//     no FFI struct generators, and no Lua-owned canonical state.
//   * Missing keys leave C++ default member initializers (or existing live
//     fields during ApplyPatch*) intact.
//   * Unknown keys fail with LuaError::UnknownField by default (or log a
//     warning when LuaBudget::rejectUnknownFields is false) so typos in
//     hand-authored state files never drop silently.
//   * Every evaluation runs inside a fresh or isolated sandboxed VM with
//     no ffi/io/os/package/debug libraries, bounded by deterministic
//     instruction and memory ceilings.
//   * ApplyPatch* hydrates onto a staging copy first and commits to the
//     caller's struct only when the entire table validates cleanly.

#include <Zahlen/Common.h>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

struct lua_State;

namespace ZHLN {

enum class LuaError : uint8_t {
    AllocationFailed          ZHLN_ANNOTATION(ZHLN::Description<"Failed to allocate Lua state"> {}) = 1,
    FileReadFailed            ZHLN_ANNOTATION(ZHLN::Description<"Could not open or read Lua file"> {}),
    SyntaxError               ZHLN_ANNOTATION(ZHLN::Description<"Lua chunk failed syntax parsing"> {}),
    RuntimeError              ZHLN_ANNOTATION(ZHLN::Description<"Lua chunk raised a runtime error"> {}),
    InstructionBudgetExceeded ZHLN_ANNOTATION(ZHLN::Description<"Lua evaluation exceeded instruction budget"> {}),
    MemoryBudgetExceeded      ZHLN_ANNOTATION(ZHLN::Description<"Lua evaluation exceeded memory budget"> {}),
    NotATable                 ZHLN_ANNOTATION(ZHLN::Description<"Expected a Lua table value"> {}),
    NotAFunction              ZHLN_ANNOTATION(ZHLN::Description<"Expected a Lua function value"> {}),
    TypeMismatch              ZHLN_ANNOTATION(ZHLN::Description<"Lua value type did not match reflected C++ field"> {}),
    InvalidEnumString         ZHLN_ANNOTATION(ZHLN::Description<"Lua string did not match any enumerator of the reflected C++ enum"> {}),
    UnknownField              ZHLN_ANNOTATION(ZHLN::Description<"Lua table contained a key not present on the reflected C++ struct"> {}),
    OutOfRange                ZHLN_ANNOTATION(ZHLN::Description<"Lua numeric, string, or array value exceeded target C++ bounds"> {}),
};

struct LuaBudget {
    uint32_t maxInstructions     = 1'000'000;
    size_t   maxMemoryBytes      = 16 * 1024 * 1024;
    bool     rejectUnknownFields = true;
};

namespace ReflectLua {

// Opt-in trait for aggregate coordinate/color structs (e.g. JPH::Float2/3/4)
// so Lua authors can write either `{ 1.0, 2.0, 3.0 }` or `{ x = 1.0, y = 2.0, z = 3.0 }`.
template <typename T>
struct LuaVector : std::false_type {};

// Customization point for non-aggregate value types with private/SIMD storage
// (e.g. JPH::Vec3, JPH::Vec4, JPH::Quat in SceneLua.hpp).
template <typename T>
struct LuaValueTrait {
    static constexpr bool specialized = false;
};

class Value;
class Builder;

// Sandboxed RAII LuaJIT VM used for state evaluation, pure reducer calls, or
// isolated host adapters (such as ProjectLight's DataModelBindings).
class ZHLN_STATIC_API LuaVm {
  public:
    LuaVm() noexcept;
    ~LuaVm();

    LuaVm(const LuaVm&)                    = delete;
    auto operator=(const LuaVm&) -> LuaVm& = delete;
    LuaVm(LuaVm&& other) noexcept;
    auto operator=(LuaVm&& other) noexcept -> LuaVm&;

    [[nodiscard]] static auto Create(LuaBudget budget = {}) noexcept -> std::expected<LuaVm, ErrorCode>;

    [[nodiscard]] auto IsValid() const noexcept -> bool;
    [[nodiscard]] auto Budget() const noexcept -> const LuaBudget&;
    [[nodiscard]] auto UsedMemoryBytes() const noexcept -> size_t;

    // Executes a statement/module chunk that returns nothing (or discards return values),
    // subject to this VM's instruction and memory budgets.
    [[nodiscard]] auto ExecuteString(std::string_view code, std::string_view chunkName = "chunk") noexcept
        -> std::expected<void, ErrorCode>;
    [[nodiscard]] auto ExecuteFile(const std::filesystem::path& path) noexcept -> std::expected<void, ErrorCode>;

    // Evaluates `code` and passes the top-of-stack Value to `consume` while the stack frame lives.
    [[nodiscard]] auto EvaluateTop(
        std::string_view code,
        std::string_view chunkName,
        const void*      consumeCtx,
        std::expected<void, ErrorCode> (*consume)(const void* ctx, Value root)
    ) noexcept -> std::expected<void, ErrorCode>;

    // Evaluates `code` expecting a function `return function(...)`, pushes arguments via `pushArgs`,
    // invokes the function under budget, and passes its return Value to `consume`.
    [[nodiscard]] auto EvaluateCallTop(
        std::string_view code,
        std::string_view chunkName,
        const void*      pushCtx,
        int (*pushArgs)(const void* ctx, Builder& builder),
        const void*      consumeCtx,
        std::expected<void, ErrorCode> (*consume)(const void* ctx, Value result)
    ) noexcept -> std::expected<void, ErrorCode>;

    // Escape hatch for isolated C-API host bindings (e.g. ProjectLight DataModelBindings).
    [[nodiscard]] auto RawState() noexcept -> lua_State*;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

// Non-owning view of a value on an active LuaVm stack frame.
// Keeps <lua.h> completely private to ReflectLua.cpp.
class ZHLN_STATIC_API Value {
  public:
    Value() noexcept = default;
    Value(lua_State* state, int absIndex, const LuaBudget* budget, std::string_view path) noexcept
        : _state(state), _index(absIndex), _budget(budget), _path(path) {
    }

    [[nodiscard]] auto IsValid() const noexcept -> bool {
        return _state != nullptr && _index > 0;
    }
    [[nodiscard]] auto IsNil() const noexcept -> bool;
    [[nodiscard]] auto IsTable() const noexcept -> bool;

    [[nodiscard]] auto GetBool() const noexcept -> std::expected<bool, ErrorCode>;
    [[nodiscard]] auto GetInt() const noexcept -> std::expected<int64_t, ErrorCode>;
    [[nodiscard]] auto GetUInt() const noexcept -> std::expected<uint64_t, ErrorCode>;
    [[nodiscard]] auto GetDouble() const noexcept -> std::expected<double, ErrorCode>;
    [[nodiscard]] auto GetString() const noexcept -> std::expected<std::string_view, ErrorCode>;

    // Table operations. A Lua table `{ 10, 20, 30 }` is a pure sequence when all
    // keys are contiguous integers `1..N`; mixed tables report `IsSequence() == false`.
    [[nodiscard]] auto IsSequence(size_t& outLength) const noexcept -> bool;
    [[nodiscard]] auto GetTableStringKeys() const -> std::expected<std::vector<std::string>, ErrorCode>;

    // Calls `visit(childValue)` with the looked-up key or 1-based array element pushed on the stack,
    // popping the temporary stack slot automatically on return.
    [[nodiscard]] auto WithKey(
        std::string_view key,
        const void*      ctx,
        std::expected<void, ErrorCode> (*visit)(const void* ctx, Value child)
    ) const noexcept -> std::expected<bool, ErrorCode>;

    [[nodiscard]] auto WithElement(
        size_t      zeroBasedIndex,
        const void* ctx,
        std::expected<void, ErrorCode> (*visit)(const void* ctx, Value element)
    ) const noexcept -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto Budget() const noexcept -> const LuaBudget& {
        static constexpr LuaBudget kDefault {};
        return _budget != nullptr ? *_budget : kDefault;
    }
    [[nodiscard]] auto Path() const noexcept -> std::string_view {
        return _path;
    }

  private:
    lua_State*       _state  = nullptr;
    int              _index  = 0;
    const LuaBudget* _budget = nullptr;
    std::string_view _path;
};

// Stack-balanced value pusher for passing read-only C++ inputs into pure Lua functions.
class ZHLN_STATIC_API Builder {
  public:
    explicit Builder(lua_State* state) noexcept: _state(state) {
    }

    void PushNil() noexcept;
    void PushBool(bool value) noexcept;
    void PushInt(int64_t value) noexcept;
    void PushUInt(uint64_t value) noexcept;
    void PushDouble(double value) noexcept;
    void PushString(std::string_view value) noexcept;

    void BeginTable(int arrayHint = 0, int hashHint = 0) noexcept;
    void SetFieldTop(std::string_view key) noexcept;
    void SetElementTop(size_t zeroBasedIndex) noexcept;

  private:
    lua_State* _state = nullptr;
};

[[nodiscard]] ZHLN_STATIC_API auto ReadLuaFileText(const std::filesystem::path& path) noexcept
    -> std::expected<std::string, ErrorCode>;

namespace TemplatedDetail {

    template <typename T>
    struct IsFixedString : std::false_type {};
    template <size_t N>
    struct IsFixedString<ZHLN::FixedString<N>> : std::true_type {};

    template <typename T>
    concept FixedStringLike = IsFixedString<std::remove_cvref_t<T>>::value;

    template <typename T>
    concept FixedCharArray = std::is_bounded_array_v<std::remove_reference_t<T>> &&
                             std::is_same_v<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<T>>>, char>;

    template <typename T>
    concept FixedStdArray = requires {
        typename T::value_type;
        { std::tuple_size<T>::value } -> std::convertible_to<size_t>;
    };

    template <typename T>
    concept StringLike = std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>;

    template <typename T>
    concept MapLike = requires {
        typename T::key_type;
        typename T::mapped_type;
    } && StringLike<typename T::key_type>;

    template <typename T>
    concept OptionalLike = requires { typename T::value_type; } &&
                           (std::is_same_v<T, std::optional<typename T::value_type>> ||
                            std::is_same_v<T, ZHLN::Optional<typename T::value_type>>);

    template <typename T>
    concept VectorLike = LuaVector<T>::value && (ZHLN::Reflect::FieldCount<T>() > 0);

    inline auto EqualAsciiIgnoreCase(std::string_view a, std::string_view b) noexcept -> bool {
        if (a.size() != b.size()) {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            const auto ca = static_cast<unsigned char>(a[i]);
            const auto cb = static_cast<unsigned char>(b[i]);
            const auto la = (ca >= 'A' && ca <= 'Z') ? static_cast<unsigned char>(ca + ('a' - 'A')) : ca;
            const auto lb = (cb >= 'A' && cb <= 'Z') ? static_cast<unsigned char>(cb + ('a' - 'A')) : cb;
            if (la != lb) {
                return false;
            }
        }
        return true;
    }

    template <typename E>
    auto ParseReflectedEnum(std::string_view raw) noexcept -> std::optional<E> {
        if (const auto exact = ZHLN::Reflect::StringToEnum<E>(raw)) {
            return exact;
        }
        constexpr auto kNames = ZHLN::Reflect::EnumNames<E>();
        for (const std::string_view candidate: kNames) {
            if (EqualAsciiIgnoreCase(raw, candidate)) {
                return ZHLN::Reflect::StringToEnum<E>(candidate);
            }
        }
        return std::nullopt;
    }

    template <typename T>
    auto PatchObject(T& target, Value table) -> std::expected<void, ErrorCode>;

    template <typename T>
    auto ReadInto(T& dst, Value reader) -> std::expected<void, ErrorCode> {
        using Decayed = std::remove_cvref_t<T>;

        if constexpr (FixedCharArray<T>) {
            constexpr size_t kCap = std::extent_v<std::remove_reference_t<T>>;
            static_assert(kCap > 0);
            auto str = reader.GetString();
            if (!str) {
                return std::unexpected(str.error());
            }
            if (str->size() >= kCap) {
                ZHLN::Log("[Lua] string of length {} exceeds char[{}] at '{}'", str->size(), kCap, reader.Path());
                return std::unexpected(LuaError::OutOfRange);
            }
            std::memcpy(dst, str->data(), str->size());
            dst[str->size()] = '\0';
            return {};
        } else if constexpr (std::is_bounded_array_v<std::remove_reference_t<T>>) {
            size_t seqLen = 0;
            if (!reader.IsSequence(seqLen)) {
                return std::unexpected(LuaError::TypeMismatch);
            }
            using RawArr          = std::remove_reference_t<T>;
            using ElemT           = std::remove_extent_t<RawArr>;
            constexpr size_t kCap = std::extent_v<RawArr>;
            if (seqLen > kCap) {
                ZHLN::Log("[Lua] array of {} elements exceeds array[{}] at '{}'", seqLen, kCap, reader.Path());
                return std::unexpected(LuaError::OutOfRange);
            }
            for (size_t i = 0; i < seqLen; ++i) {
                struct ElemCtx {
                    ElemT* dstElem;
                } ctx {&dst[i]};
                auto res = reader.WithElement(i, &ctx, [](const void* rawCtx, Value elem) -> std::expected<void, ErrorCode> {
                    const auto* c = static_cast<const ElemCtx*>(rawCtx);
                    return ReadInto(*c->dstElem, elem);
                });
                if (!res) {
                    return std::unexpected(res.error());
                }
            }
            return {};
        } else if constexpr (LuaValueTrait<Decayed>::specialized) {
            auto parsed = LuaValueTrait<Decayed>::Read(reader);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            dst = std::move(*parsed);
            return {};
        } else if constexpr (std::is_same_v<Decayed, bool>) {
            auto parsed = reader.GetBool();
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            dst = *parsed;
            return {};
        } else if constexpr (std::is_same_v<Decayed, std::string>) {
            auto parsed = reader.GetString();
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            dst.assign(parsed->data(), parsed->size());
            return {};
        } else if constexpr (FixedStringLike<Decayed>) {
            auto parsed = reader.GetString();
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            if (parsed->size() > Decayed::kMaxTextLength) {
                ZHLN::Log(
                    "[Lua] string of length {} exceeds FixedString<{}> capacity at '{}'",
                    parsed->size(),
                    Decayed::kMaxTextLength + 1,
                    reader.Path()
                );
                return std::unexpected(LuaError::OutOfRange);
            }
            dst.assign(*parsed);
            return {};
        } else if constexpr (std::is_enum_v<Decayed>) {
            auto raw = reader.GetString();
            if (!raw) {
                return std::unexpected(raw.error());
            }
            const auto parsed = ParseReflectedEnum<Decayed>(*raw);
            if (!parsed) {
                ZHLN::Log("[Lua] '{}' is not a valid enumerator of {} at '{}'", *raw, ZHLN::Reflect::TypeName<Decayed>(), reader.Path());
                return std::unexpected(LuaError::InvalidEnumString);
            }
            dst = *parsed;
            return {};
        } else if constexpr (std::is_integral_v<Decayed>) {
            if constexpr (std::is_unsigned_v<Decayed>) {
                auto raw = reader.GetUInt();
                if (!raw) {
                    return std::unexpected(raw.error());
                }
                if (*raw > static_cast<uint64_t>(std::numeric_limits<Decayed>::max())) {
                    return std::unexpected(LuaError::OutOfRange);
                }
                dst = static_cast<Decayed>(*raw);
                return {};
            } else {
                auto raw = reader.GetInt();
                if (!raw) {
                    return std::unexpected(raw.error());
                }
                if (*raw < static_cast<int64_t>(std::numeric_limits<Decayed>::min()) ||
                    *raw > static_cast<int64_t>(std::numeric_limits<Decayed>::max())) {
                    return std::unexpected(LuaError::OutOfRange);
                }
                dst = static_cast<Decayed>(*raw);
                return {};
            }
        } else if constexpr (std::is_floating_point_v<Decayed>) {
            auto raw = reader.GetDouble();
            if (!raw) {
                return std::unexpected(raw.error());
            }
            dst = static_cast<Decayed>(*raw);
            return {};
        } else if constexpr (OptionalLike<Decayed>) {
            if (reader.IsNil()) {
                dst = std::nullopt;
                return {};
            }
            typename Decayed::value_type inner {};
            if (dst.has_value()) {
                inner = *dst;
            }
            if (auto res = ReadInto(inner, reader); !res) {
                return std::unexpected(res.error());
            }
            dst = std::move(inner);
            return {};
        } else if constexpr (VectorLike<Decayed>) {
            if (!reader.IsTable()) {
                return std::unexpected(LuaError::TypeMismatch);
            }
            constexpr size_t kArity = ZHLN::Reflect::FieldCount<Decayed>();
            size_t           seqLen = 0;
            if (reader.IsSequence(seqLen)) {
                if (seqLen > kArity) {
                    ZHLN::Log("[Lua] array of {} elements exceeds {}[{}] at '{}'", seqLen, ZHLN::Reflect::TypeName<Decayed>(), kArity, reader.Path());
                    return std::unexpected(LuaError::OutOfRange);
                }
                std::optional<ErrorCode> err;
                size_t                   index = 0;
                ZHLN::Reflect::ForEachField(dst, [&](auto& comp) -> void {
                    const size_t at = index++;
                    if (err || at >= seqLen) {
                        return;
                    }
                    using CompT = std::remove_cvref_t<decltype(comp)>;
                    struct ElemCtx {
                        CompT* dstComp;
                    } ctx {&comp};
                    auto elemRes = reader.WithElement(at, &ctx, [](const void* rawCtx, Value elem) -> std::expected<void, ErrorCode> {
                        const auto* c = static_cast<const ElemCtx*>(rawCtx);
                        return ReadInto(*c->dstComp, elem);
                    });
                    if (!elemRes) {
                        err = elemRes.error();
                    }
                });
                if (err) {
                    return std::unexpected(*err);
                }
                return {};
            }
            return PatchObject(dst, reader);
        } else if constexpr (MapLike<Decayed>) {
            if (!reader.IsTable()) {
                return std::unexpected(LuaError::TypeMismatch);
            }
            auto keys = reader.GetTableStringKeys();
            if (!keys) {
                return std::unexpected(keys.error());
            }
            Decayed out;
            for (const std::string& key: *keys) {
                using MappedT = typename Decayed::mapped_type;
                MappedT mapped {};
                struct KeyCtx {
                    MappedT* dstVal;
                } ctx {&mapped};
                auto found = reader.WithKey(key, &ctx, [](const void* rawCtx, Value child) -> std::expected<void, ErrorCode> {
                    const auto* c = static_cast<const KeyCtx*>(rawCtx);
                    return ReadInto(*c->dstVal, child);
                });
                if (!found) {
                    return std::unexpected(found.error());
                }
                out.emplace(key, std::move(mapped));
            }
            dst = std::move(out);
            return {};
        } else if constexpr (FixedStdArray<Decayed>) {
            size_t seqLen = 0;
            if (!reader.IsSequence(seqLen)) {
                return std::unexpected(LuaError::TypeMismatch);
            }
            constexpr size_t kCap = std::tuple_size<Decayed>::value;
            if (seqLen > kCap) {
                ZHLN::Log("[Lua] array of {} elements exceeds array[{}] at '{}'", seqLen, kCap, reader.Path());
                return std::unexpected(LuaError::OutOfRange);
            }
            for (size_t i = 0; i < seqLen; ++i) {
                using ElemT = typename Decayed::value_type;
                struct ElemCtx {
                    ElemT* dstElem;
                } ctx {&dst[i]};
                auto res = reader.WithElement(i, &ctx, [](const void* rawCtx, Value elem) -> std::expected<void, ErrorCode> {
                    const auto* c = static_cast<const ElemCtx*>(rawCtx);
                    return ReadInto(*c->dstElem, elem);
                });
                if (!res) {
                    return std::unexpected(res.error());
                }
            }
            return {};
        } else if constexpr (std::ranges::range<Decayed>) {
            size_t seqLen = 0;
            if (!reader.IsSequence(seqLen)) {
                return std::unexpected(LuaError::TypeMismatch);
            }
            Decayed container;
            if constexpr (requires { container.reserve(seqLen); }) {
                container.reserve(seqLen);
            }
            for (size_t i = 0; i < seqLen; ++i) {
                using ElemT = typename Decayed::value_type;
                ElemT elemVal {};
                struct ElemCtx {
                    ElemT* dstElem;
                } ctx {&elemVal};
                auto res = reader.WithElement(i, &ctx, [](const void* rawCtx, Value elem) -> std::expected<void, ErrorCode> {
                    const auto* c = static_cast<const ElemCtx*>(rawCtx);
                    return ReadInto(*c->dstElem, elem);
                });
                if (!res) {
                    return std::unexpected(res.error());
                }
                container.push_back(std::move(elemVal));
            }
            dst = std::move(container);
            return {};
        } else if constexpr (ZHLN::Reflect::FieldCount<Decayed>() > 0) {
            return PatchObject(dst, reader);
        } else {
            return std::unexpected(LuaError::TypeMismatch);
        }
    }

    template <typename T>
    auto PatchObject(T& target, Value table) -> std::expected<void, ErrorCode> {
        if (!table.IsTable()) {
            return std::unexpected(LuaError::NotATable);
        }

        auto keys = table.GetTableStringKeys();
        if (!keys) {
            return std::unexpected(keys.error());
        }

        T                        staging = target;
        std::vector<uint8_t>     consumed(keys->size(), 0u);
        std::optional<ErrorCode> err;

        ZHLN::Reflect::ForEachFieldWithName(staging, [&](std::string_view fieldName, auto& fieldRef) -> void {
            if (err) {
                return;
            }
            bool present = false;
            for (size_t i = 0; i < keys->size(); ++i) {
                if ((*keys)[i] == fieldName) {
                    consumed[i] = 1u;
                    present     = true;
                }
            }
            if (!present) {
                return;
            }

            using FieldRefT = decltype(fieldRef);
            struct FieldCtx {
                std::remove_reference_t<FieldRefT>* ptr;
            } ctx {&fieldRef};

            auto res = table.WithKey(fieldName, &ctx, [](const void* rawCtx, Value child) -> std::expected<void, ErrorCode> {
                const auto* c = static_cast<const FieldCtx*>(rawCtx);
                return ReadInto(*c->ptr, child);
            });
            if (!res) {
                ZHLN::Log("[Lua] field '{}' of {} rejected value", fieldName, ZHLN::Reflect::TypeName<T>());
                err = res.error();
            }
        });

        if (err) {
            return std::unexpected(*err);
        }

        for (size_t i = 0; i < keys->size(); ++i) {
            if (!consumed[i]) {
                ZHLN::Log("[Lua] {} has no field named '{}' at '{}'", ZHLN::Reflect::TypeName<T>(), (*keys)[i], table.Path());
                if (table.Budget().rejectUnknownFields) {
                    return std::unexpected(LuaError::UnknownField);
                }
            }
        }

        target = std::move(staging);
        return {};
    }

    template <typename T>
    void PushValue(Builder& builder, const T& value) {
        using Decayed = std::remove_cvref_t<T>;

        if constexpr (FixedCharArray<T>) {
            builder.PushString(std::string_view(value));
        } else if constexpr (LuaValueTrait<Decayed>::specialized) {
            LuaValueTrait<Decayed>::Push(builder, value);
        } else if constexpr (std::is_same_v<Decayed, bool>) {
            builder.PushBool(value);
        } else if constexpr (StringLike<Decayed>) {
            builder.PushString(std::string_view(value));
        } else if constexpr (FixedStringLike<Decayed>) {
            builder.PushString(std::string_view(value.data(), value.size()));
        } else if constexpr (std::is_enum_v<Decayed>) {
            builder.PushString(ZHLN::Reflect::EnumToString(value));
        } else if constexpr (std::is_integral_v<Decayed>) {
            if constexpr (std::is_unsigned_v<Decayed>) {
                builder.PushUInt(static_cast<uint64_t>(value));
            } else {
                builder.PushInt(static_cast<int64_t>(value));
            }
        } else if constexpr (std::is_floating_point_v<Decayed>) {
            builder.PushDouble(static_cast<double>(value));
        } else if constexpr (OptionalLike<Decayed>) {
            if (value.has_value()) {
                PushValue(builder, *value);
            } else {
                builder.PushNil();
            }
        } else if constexpr (VectorLike<Decayed>) {
            constexpr size_t kArity = ZHLN::Reflect::FieldCount<Decayed>();
            builder.BeginTable(static_cast<int>(kArity), 0);
            size_t idx = 0;
            ZHLN::Reflect::ForEachField(value, [&](const auto& comp) -> void {
                PushValue(builder, comp);
                builder.SetElementTop(idx++);
            });
        } else if constexpr (MapLike<Decayed>) {
            builder.BeginTable(0, static_cast<int>(value.size()));
            for (const auto& [k, v]: value) {
                PushValue(builder, v);
                builder.SetFieldTop(k);
            }
        } else if constexpr (std::ranges::range<Decayed>) {
            builder.BeginTable();
            size_t idx = 0;
            for (const auto& elem: value) {
                PushValue(builder, elem);
                builder.SetElementTop(idx++);
            }
        } else if constexpr (ZHLN::Reflect::FieldCount<Decayed>() > 0) {
            constexpr size_t kFields = ZHLN::Reflect::FieldCount<Decayed>();
            builder.BeginTable(0, static_cast<int>(kFields));
            ZHLN::Reflect::ForEachFieldWithName(value, [&](std::string_view fieldName, const auto& fieldVal) -> void {
                PushValue(builder, fieldVal);
                builder.SetFieldTop(fieldName);
            });
        } else {
            builder.PushNil();
        }
    }

} // namespace TemplatedDetail

// ----------------------------------------------------------------------------
// Public Monadic Evaluation & Transactional Patching API
// ----------------------------------------------------------------------------

template <typename T>
[[nodiscard]] auto ApplyPatchString(T& target, std::string_view chunk, std::string_view chunkName = "patch", LuaBudget budget = {})
    -> std::expected<void, ErrorCode> {
    auto vm = LuaVm::Create(budget);
    if (!vm) {
        return std::unexpected(vm.error());
    }

    struct PatchCtx {
        T* targetPtr;
    } ctx {&target};

    return vm->EvaluateTop(chunk, chunkName, &ctx, [](const void* rawCtx, Value root) -> std::expected<void, ErrorCode> {
        const auto* c = static_cast<const PatchCtx*>(rawCtx);
        return TemplatedDetail::ReadInto(*c->targetPtr, root);
    });
}

template <typename T>
[[nodiscard]] auto ApplyPatchFile(T& target, const std::filesystem::path& path, LuaBudget budget = {})
    -> std::expected<void, ErrorCode> {
    auto source = ReadLuaFileText(path);
    if (!source) {
        return std::unexpected(source.error());
    }
    const std::string chunkName = "@" + path.generic_string();
    return ApplyPatchString(target, *source, chunkName, budget);
}

template <typename T>
[[nodiscard]] auto EvaluateString(std::string_view chunk, std::string_view chunkName = "chunk", LuaBudget budget = {})
    -> std::expected<T, ErrorCode> {
    T out {};
    if (auto res = ApplyPatchString(out, chunk, chunkName, budget); !res) {
        return std::unexpected(res.error());
    }
    return out;
}

template <typename T>
[[nodiscard]] auto EvaluateFile(const std::filesystem::path& path, LuaBudget budget = {}) -> std::expected<T, ErrorCode> {
    T out {};
    if (auto res = ApplyPatchFile(out, path, budget); !res) {
        return std::unexpected(res.error());
    }
    return out;
}

// Evaluates a pure Lua function `return function(arg1, arg2, ...) -> table`
// and patches `target` transactionally with its returned table.
template <typename T, typename... InArgs>
[[nodiscard]] auto ApplyCallPatchString(
    T&               target,
    std::string_view chunk,
    std::string_view chunkName,
    LuaBudget        budget,
    const InArgs&... inputs
) -> std::expected<void, ErrorCode> {
    auto vm = LuaVm::Create(budget);
    if (!vm) {
        return std::unexpected(vm.error());
    }

    using TupleT = std::tuple<const InArgs&...>;
    TupleT argsTuple {inputs...};

    struct PatchCtx {
        T* targetPtr;
    } patchCtx {&target};

    return vm->EvaluateCallTop(
        chunk,
        chunkName,
        &argsTuple,
        [](const void* rawArgs, Builder& builder) -> int {
            const auto& tup = *static_cast<const TupleT*>(rawArgs);
            std::apply([&](const auto&... elems) { (TemplatedDetail::PushValue(builder, elems), ...); }, tup);
            return static_cast<int>(sizeof...(InArgs));
        },
        &patchCtx,
        [](const void* rawCtx, Value result) -> std::expected<void, ErrorCode> {
            const auto* c = static_cast<const PatchCtx*>(rawCtx);
            return TemplatedDetail::ReadInto(*c->targetPtr, result);
        }
    );
}

template <typename OutT, typename... InArgs>
[[nodiscard]] auto EvaluateCallString(
    std::string_view chunk,
    std::string_view chunkName,
    LuaBudget        budget,
    const InArgs&... inputs
) -> std::expected<OutT, ErrorCode> {
    OutT out {};
    if (auto res = ApplyCallPatchString(out, chunk, chunkName, budget, inputs...); !res) {
        return std::unexpected(res.error());
    }
    return out;
}

template <typename OutT, typename... InArgs>
[[nodiscard]] auto EvaluateCallFile(const std::filesystem::path& path, LuaBudget budget, const InArgs&... inputs)
    -> std::expected<OutT, ErrorCode> {
    auto source = ReadLuaFileText(path);
    if (!source) {
        return std::unexpected(source.error());
    }
    const std::string chunkName = "@" + path.generic_string();
    return EvaluateCallString<OutT>(*source, chunkName, budget, inputs...);
}

} // namespace ReflectLua
} // namespace ZHLN
