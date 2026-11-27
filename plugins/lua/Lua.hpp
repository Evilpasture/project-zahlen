// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// ZHLN::Lua -- reflection-driven Lua state evaluation, transactional patching,
// and first-class stateless script layers. One namespace, one error model, one
// implementation. Stateful Roblox-style interop is opt-in via <lua/Host.hpp>.

#include <Zahlen/COM/IEngine.hpp>
#include <lua/Plugin.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/ILogger.hpp>
#include <Zahlen/COM/LoggerView.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <Zahlen/Core/Reflection/Callable.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/ECS/ComponentPolicy.hpp>
#include <algorithm>
#include <concepts>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <Zahlen/Common.h>
#include <Zahlen/Core/FunctionRef.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <ranges>
#include <tuple>

namespace ZHLN {

namespace Lua {




// Opt-in trait for aggregate coordinate/color structs (e.g. JPH::Float2/3/4)
// so Lua authors can write either `{ 1.0, 2.0, 3.0 }` or `{ x = 1.0, y = 2.0, z = 3.0 }`.
template <typename T>
struct Vector: std::false_type {};

// Customization point for non-aggregate value types with private/SIMD storage
// (e.g. FVec3, FVec4, Quat in Scene.hpp).
template <typename T>
struct ValueTrait {
    static constexpr bool specialized = false;
};

class Value;
class Builder;
class CallFrame;

// Every userdata payload stored inside the Lua GC heap must be pure POD:
// trivially copyable and trivially destructible. Smart pointers (`Ref<T>`,
// `std::shared_ptr<T>`), `std::function`, containers, and RAII handles are
// rejected at compile time so Lua GC and C++ lifetimes never entangle.
template <typename T>
concept TrivialUserdata = std::is_object_v<std::remove_cvref_t<T>> && !std::is_pointer_v<std::remove_cvref_t<T>> &&
                         std::is_trivially_copyable_v<std::remove_cvref_t<T>> && std::is_trivially_destructible_v<std::remove_cvref_t<T>>;

// Unique per-type process-wide identity token (`const void*` address of an
// `inline constexpr char` variable template). Works for named and TU-local
// anonymous-namespace types alike with O(1) pointer comparison.
using TypeID = const void*;

namespace TemplatedDetail {

template <typename T>
inline constexpr char kLuaTypeTagAnchor = '\0';

} // namespace TemplatedDetail

template <typename T>
[[nodiscard]] constexpr auto TypeIdOf() noexcept -> TypeID {
    using Clean = std::remove_cvref_t<T>;
    return &TemplatedDetail::kLuaTypeTagAnchor<Clean>;
}

// Monotonic handle into the VM-internal Lua listener table. Inert `uint64_t`:
// valid only while registered in the owning `VirtualMachine`, and safe to copy into POD
// userdata or C++ signal lambdas without pinning Lua references from C++ dtors.
using ListenerID                               = uint64_t;
inline constexpr ListenerID kInvalidListenerID = 0;

using HostCallback = auto (*)(CallFrame&) -> int;

struct MethodEntry {
    std::string_view name;
    HostCallback     callback = nullptr;
};

// Metamethod set for a `TrivialUserdata` type. Note the deliberate absence of
// `__gc`: POD userdata require no finalizer.
struct MetatableSpec {
    std::string_view             debugName;
    HostCallback                 index    = nullptr;
    HostCallback                 newIndex = nullptr;
    HostCallback                 toString = nullptr;
    HostCallback                 eq       = nullptr;
    HostCallback                 add      = nullptr;
    HostCallback                 sub      = nullptr;
    HostCallback                 mul      = nullptr;
    HostCallback                 div      = nullptr;
    HostCallback                 unm      = nullptr;
    std::span<const MethodEntry> methods {};
};

template <typename T>
concept SequenceContainer = std::ranges::range<T> && requires(T& c, typename T::value_type v) { c.push_back(std::move(v)); };

// Sandboxed RAII LuaJIT VM used for state evaluation, pure reducer calls, or
// isolated host adapters (such as ProjectLight's DataModelBindings).
class ZHLN_STATIC_API VirtualMachine {
  public:
    VirtualMachine() noexcept;
    ~VirtualMachine();

    VirtualMachine(const VirtualMachine&)                    = delete;
    auto operator=(const VirtualMachine&) -> VirtualMachine& = delete;
    VirtualMachine(VirtualMachine&& other) noexcept;
    auto operator=(VirtualMachine&& other) noexcept -> VirtualMachine&;

    [[nodiscard]] static auto Create(Budget budget = {}) noexcept -> std::expected<VirtualMachine, ErrorCode>;

    [[nodiscard]] auto IsValid() const noexcept -> bool;
    [[nodiscard]] auto GetBudget() const noexcept -> const Budget&;
    [[nodiscard]] auto UsedMemoryBytes() const noexcept -> size_t;

    // Executes a statement/module chunk that returns nothing (or discards return values),
    // subject to this VM's instruction and memory budgets.
    [[nodiscard]] auto ExecuteString(std::string_view code, std::string_view chunkName = "chunk") noexcept -> std::expected<void, ErrorCode>;
    // Detail for the most recent failure (Lua error string, chunk name,
    // budget hit); empty when the last call succeeded. The ErrorCode says
    // what kind of failure; this says where and why.
    [[nodiscard]] auto LastErrorDetail() const noexcept -> std::string_view;

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
        const void* consumeCtx,
        std::expected<void, ErrorCode> (*consume)(const void* ctx, Value result)
    ) noexcept -> std::expected<void, ErrorCode>;

    // --- Compiled, stateless-script API -------------------------------------
    // Used by ZHLN::Lua::Script. The chunk is parsed once with mode "t" and
    // retained as a registry function; every invocation gives it a brand-new
    // environment table, so locals/upvalues cannot survive across calls.
    using PushArgsFn  = int (*)(const void* ctx, Builder& builder);
    using ConsumeFn   = std::expected<void, ErrorCode> (*)(const void* ctx, Value result);

    [[nodiscard]] auto CompileChunk(std::string_view code, std::string_view chunkName) noexcept -> std::expected<void, ErrorCode>;
    [[nodiscard]] auto HasCompiledChunk() const noexcept -> bool;

    // Limits the globals callable from a compiled chunk. `allowedBits` uses the
    // Permission::Bit values declared by <lua/Lua.hpp>; this layer only reads
    // the raw word so the core plugin stays independent of the scripting API.
    [[nodiscard]] auto ApplyScriptPermissions(uint64_t allowedBits) noexcept -> std::expected<void, ErrorCode>;

    // Registers one host function in the VM-owned capability table used to
    // build each call environment. The callback must be pure with respect to
    // the host state unless the caller explicitly opts into stateful interop.
    [[nodiscard]] auto RegisterHostFunction(std::string_view name, HostCallback callback) noexcept -> std::expected<void, ErrorCode>;

    // Calls the compiled chunk with `pushArgs`, then either uses its returned
    // table directly or calls the returned function with the same arguments.
    [[nodiscard]] auto EvaluateCompiledCallTop(
        const void* pushCtx,
        PushArgsFn  pushArgs,
        const void* consumeCtx,
        ConsumeFn   consume,
        std::string_view chunkName = "script"
    ) noexcept -> std::expected<void, ErrorCode>;

    // Host interop API (`<lua/Host.hpp>`): binds non-owning C++ context
    // pointers, registers `TrivialUserdata` metatables, populates globals, and
    // dispatches Lua-side listeners under this VM's instruction/memory budgets.
    template <typename T>
    void SetHostContext(T* context) noexcept {
        using Clean = std::remove_cvref_t<T>;
        SetHostContextRaw(TypeIdOf<Clean>(), static_cast<void*>(context));
    }

    template <typename T>
    void ClearHostContext() noexcept {
        using Clean = std::remove_cvref_t<T>;
        SetHostContextRaw(TypeIdOf<Clean>(), nullptr);
    }

    template <typename T>
    [[nodiscard]] auto HostContext() const noexcept -> T* {
        using Clean = std::remove_cvref_t<T>;
        return static_cast<Clean*>(HostContextRaw(TypeIdOf<Clean>()));
    }

    template <TrivialUserdata T>
    [[nodiscard]] auto RegisterUserdataType(const MetatableSpec& spec) noexcept -> std::expected<void, ErrorCode> {
        using Clean = std::remove_cvref_t<T>;
        return RegisterUserdataTypeRaw(TypeIdOf<Clean>(), spec);
    }

    // Runs `populate` with a fresh, stack-balanced `CallFrame` so host bindings
    // can build global tables/functions/constants without touching `lua_State*`.
    [[nodiscard]] auto WithFrame(FunctionRef<void(CallFrame&) const> populate) noexcept -> std::expected<void, ErrorCode>;

    // Listener lifecycle and budgeted protected callback invocation.
    void               UnregisterListener(ListenerID id) noexcept;
    [[nodiscard]] auto HasListener(ListenerID id) const noexcept -> bool;
    [[nodiscard]] auto InvokeListener(ListenerID id, FunctionRef<void(CallFrame&) const> pushArgs, std::string_view contextName = "callback") noexcept
        -> std::expected<void, ErrorCode>;

  private:
    friend class CallFrame;
    struct Impl;
    std::unique_ptr<Impl> _impl;

    void               SetHostContextRaw(TypeID typeId, void* context) noexcept;
    [[nodiscard]] auto HostContextRaw(TypeID typeId) const noexcept -> void*;
    [[nodiscard]] auto RegisterUserdataTypeRaw(TypeID typeId, const MetatableSpec& spec) noexcept -> std::expected<void, ErrorCode>;
};

// Non-owning view of a value on an active VirtualMachine stack frame.
// Keeps <lua.h> completely private to Lua.cpp.
// Thin bridge declared here and implemented in Lua.cpp where lua.h and the
// full CallFrame are visible. Keeps <lua.h> out of plain host-function callsites.
struct HostAccessBridge;

class ZHLN_STATIC_API Value {
  public:
    Value() noexcept = default;

    [[nodiscard]] auto IsValid() const noexcept -> bool {
        return _rawState != nullptr && _index > 0;
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
    [[nodiscard]] auto SequenceLength() const noexcept -> std::optional<size_t>;
    [[nodiscard]] auto GetTableStringKeys() const -> std::expected<std::vector<std::string>, ErrorCode>;

    // Calls `visit(childValue)` with the looked-up key or 1-based array element pushed on the stack,
    // popping the temporary stack slot automatically on return.
    [[nodiscard]] auto WithKey(std::string_view key, const void* ctx, std::expected<void, ErrorCode> (*visit)(const void* ctx, Value child)) const noexcept
        -> std::expected<bool, ErrorCode>;

    [[nodiscard]] auto
        WithElement(size_t zeroBasedIndex, const void* ctx, std::expected<void, ErrorCode> (*visit)(const void* ctx, Value element)) const noexcept
        -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto GetBudget() const noexcept -> const Budget& {
        static constexpr Budget kDefault {};
        return _budget != nullptr ? *_budget : kDefault;
    }
    [[nodiscard]] auto Path() const noexcept -> std::string_view {
        return _path;
    }

  private:
    friend class VirtualMachine;
    friend struct HostAccessBridge;
    Value(void* rawState, int absIndex, const Budget* budget, std::string_view path) noexcept:
        _rawState(rawState), _index(absIndex), _budget(budget), _path(path) {
    }

    void*            _rawState = nullptr;
    int              _index    = 0;
    const Budget* _budget   = nullptr;
    std::string_view _path;
};

// Stack-balanced value pusher for passing read-only C++ inputs into pure Lua functions.
class ZHLN_STATIC_API Builder {
  public:
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
    friend class VirtualMachine;
    friend struct HostAccessBridge;
    explicit Builder(void* rawState) noexcept: _rawState(rawState) {
    }

    void* _rawState = nullptr;
};

namespace TemplatedDetail {

template <typename T>
struct IsFixedString: std::false_type {};
template <size_t N>
struct IsFixedString<ZHLN::FixedString<N>>: std::true_type {};

template <typename T>
concept FixedStringLike = IsFixedString<std::remove_cvref_t<T>>::value;

template <typename T>
concept FixedCharArray =
    std::is_bounded_array_v<std::remove_reference_t<T>> && std::is_same_v<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<T>>>, char>;

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
                       (std::is_same_v<T, std::optional<typename T::value_type>> || std::is_same_v<T, ZHLN::Optional<typename T::value_type>>);

template <typename T>
concept VectorLike = Vector<T>::value && (ZHLN::Reflect::FieldCount<T>() > 0);

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
auto PatchObject(T& target, Value table, std::string* detail = nullptr) -> std::expected<void, ErrorCode>;

template <typename T>
auto PatchVector(T& target, Value table, std::string* detail = nullptr) -> std::expected<void, ErrorCode>;

// Reads `reader` into dst. On failure, when `detail` is non-null it
// receives what failed (field path composed by PatchObject, string
// lengths, enum tokens, array overflow) -- pure data, so this layer
// never needs a logger to explain itself.
template <typename T>
auto ReadInto(T& dst, Value reader, std::string* detail = nullptr) -> std::expected<void, ErrorCode> {
    using Decayed = std::remove_cvref_t<T>;

    const auto fail = [detail](ErrorCode code, std::string why) -> std::unexpected<ErrorCode> {
        if (detail != nullptr && detail->empty()) {
            *detail = std::move(why);
        }
        return std::unexpected(code);
    };

    if constexpr (FixedCharArray<T>) {
        constexpr size_t kCap = std::extent_v<std::remove_reference_t<T>>;
        static_assert(kCap > 0);
        auto str = reader.GetString();
        if (!str) {
            return std::unexpected(str.error());
        }
        if (str->size() >= kCap) {
            return fail(Error::OutOfRange, std::format("string of {} chars does not fit char[{}]", str->size(), kCap));
        }
        std::memcpy(dst, str->data(), str->size());
        dst[str->size()] = '\0';
        return {};
    } else if constexpr (std::is_bounded_array_v<std::remove_reference_t<T>>) {
        const auto seqLenOpt = reader.SequenceLength();
        if (!seqLenOpt) {
            return std::unexpected(Error::TypeMismatch);
        }
        const size_t seqLen   = *seqLenOpt;
        using RawArr          = std::remove_reference_t<T>;
        using ElemT           = std::remove_extent_t<RawArr>;
        constexpr size_t kCap = std::extent_v<RawArr>;
        if (seqLen > kCap) {
            return fail(Error::OutOfRange, std::format("sequence of {} elements exceeds capacity {}", seqLen, kCap));
        }
        for (size_t i = 0; i < seqLen; ++i) {
            struct ElemCtx {
                ElemT*       dstElem;
                std::string* detail;
            } ctx {&dst[i], detail};
            auto res = reader.WithElement(i, &ctx, [](const void* rawCtx, Value elem) -> std::expected<void, ErrorCode> {
                const auto* c = static_cast<const ElemCtx*>(rawCtx);
                return ReadInto(*c->dstElem, elem, c->detail);
            });
            if (!res) {
                return std::unexpected(res.error());
            }
        }
        return {};
    } else if constexpr (ValueTrait<Decayed>::specialized) {
        auto parsed = ValueTrait<Decayed>::Read(reader);
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
            return fail(Error::OutOfRange, std::format("string of {} chars exceeds the {}-char limit", parsed->size(), Decayed::kMaxTextLength));
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
            return fail(Error::InvalidEnumString, std::format("invalid enum token '{}'", *raw));
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
                return fail(Error::OutOfRange, std::format("value {} exceeds the target integer range", *raw));
            }
            dst = static_cast<Decayed>(*raw);
            return {};
        } else {
            auto raw = reader.GetInt();
            if (!raw) {
                return std::unexpected(raw.error());
            }
            if (*raw < static_cast<int64_t>(std::numeric_limits<Decayed>::min()) || *raw > static_cast<int64_t>(std::numeric_limits<Decayed>::max())) {
                return std::unexpected(Error::OutOfRange);
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
        if (auto res = ReadInto(inner, reader, detail); !res) {
            return std::unexpected(res.error());
        }
        dst = std::move(inner);
        return {};
    } else if constexpr (VectorLike<Decayed>) {
        return PatchVector(dst, reader);
    } else if constexpr (MapLike<Decayed>) {
        if (!reader.IsTable()) {
            return std::unexpected(Error::TypeMismatch);
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
                MappedT*     dstVal;
                std::string* detail;
            } ctx {&mapped, detail};
            auto found = reader.WithKey(key, &ctx, [](const void* rawCtx, Value child) -> std::expected<void, ErrorCode> {
                const auto* c = static_cast<const KeyCtx*>(rawCtx);
                return ReadInto(*c->dstVal, child, c->detail);
            });
            if (!found) {
                return std::unexpected(found.error());
            }
            out.emplace(key, std::move(mapped));
        }
        dst = std::move(out);
        return {};
    } else if constexpr (FixedStdArray<Decayed>) {
        const auto seqLenOpt = reader.SequenceLength();
        if (!seqLenOpt) {
            return std::unexpected(Error::TypeMismatch);
        }
        const size_t     seqLen = *seqLenOpt;
        constexpr size_t kCap   = std::tuple_size<Decayed>::value;
        if (seqLen > kCap) {
            return fail(Error::OutOfRange, std::format("sequence of {} elements exceeds capacity {}", seqLen, kCap));
        }
        for (size_t i = 0; i < seqLen; ++i) {
            using ElemT = typename Decayed::value_type;
            struct ElemCtx {
                ElemT*       dstElem;
                std::string* detail;
            } ctx {&dst[i], detail};
            auto res = reader.WithElement(i, &ctx, [](const void* rawCtx, Value elem) -> std::expected<void, ErrorCode> {
                const auto* c = static_cast<const ElemCtx*>(rawCtx);
                return ReadInto(*c->dstElem, elem, c->detail);
            });
            if (!res) {
                return std::unexpected(res.error());
            }
        }
        return {};
    } else if constexpr (std::ranges::range<Decayed>) {
        const auto seqLenOpt = reader.SequenceLength();
        if (!seqLenOpt) {
            return std::unexpected(Error::TypeMismatch);
        }
        const size_t seqLen = *seqLenOpt;
        Decayed      container;
        if constexpr (requires { container.reserve(seqLen); }) {
            container.reserve(seqLen);
        }
        for (size_t i = 0; i < seqLen; ++i) {
            using ElemT = typename Decayed::value_type;
            ElemT elemVal {};
            struct ElemCtx {
                ElemT*       dstElem;
                std::string* detail;
            } ctx {&elemVal, detail};
            auto res = reader.WithElement(i, &ctx, [](const void* rawCtx, Value elem) -> std::expected<void, ErrorCode> {
                const auto* c = static_cast<const ElemCtx*>(rawCtx);
                return ReadInto(*c->dstElem, elem, c->detail);
            });
            if (!res) {
                return std::unexpected(res.error());
            }
            container.push_back(std::move(elemVal));
        }
        dst = std::move(container);
        return {};
    } else if constexpr (ZHLN::Reflect::FieldCount<Decayed>() > 0) {
        return PatchObject(dst, reader, detail);
    } else {
        return std::unexpected(Error::TypeMismatch);
    }
}

// Value traits construct a fresh value, which is right for evaluation but
// loses untouched coordinates during a patch. Patch vector fields against
// their existing value and commit only after every supplied lane succeeds.
template <typename T>
auto PatchVector(T& target, Value table, std::string* detail) -> std::expected<void, ErrorCode> {
    if (!table.IsTable())
        return std::unexpected(Error::TypeMismatch);

    T          staging   = target;
    const auto seqLenOpt = table.SequenceLength();
    if (!seqLenOpt) {
        if (auto res = PatchObject(staging, table, detail); !res)
            return std::unexpected(res.error());
    } else {
        const size_t     seqLen = *seqLenOpt;
        constexpr size_t kArity = ZHLN::Reflect::FieldCount<T>();
        if (seqLen > kArity) {
            return std::unexpected(Error::OutOfRange);
        }
        std::optional<ErrorCode> err;
        size_t                   index = 0;
        ZHLN::Reflect::ForEachField(staging, [&](auto& comp) -> void {
            const size_t at = index++;
            if (err || at >= seqLen)
                return;
            using CompT = std::remove_cvref_t<decltype(comp)>;
            struct ElemCtx {
                CompT* dstComp;
            } ctx {&comp};
            auto result = table.WithElement(at, &ctx, [](const void* rawCtx, Value elem) -> std::expected<void, ErrorCode> {
                const auto* c = static_cast<const ElemCtx*>(rawCtx);
                return ReadInto(*c->dstComp, elem);
            });
            if (!result)
                err = result.error();
        });
        if (err)
            return std::unexpected(*err);
    }
    target = std::move(staging);
    return {};
}

template <typename T>
auto PatchField(T& target, Value value, std::string* detail = nullptr) -> std::expected<void, ErrorCode> {
    if constexpr (VectorLike<std::remove_cvref_t<T>>) {
        return PatchVector(target, value, detail);
    } else {
        return ReadInto(target, value, detail);
    }
}

template <typename T>
auto PatchObject(T& target, Value table, std::string* detail) -> std::expected<void, ErrorCode> {
    if (!table.IsTable()) {
        return std::unexpected(Error::NotATable);
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
        std::string fieldDetail;
        struct FieldCtxEx {
            std::remove_reference_t<FieldRefT>* ptr;
            std::string*                        detail;
        } ctxEx {&fieldRef, &fieldDetail};
        auto res = table.WithKey(fieldName, &ctxEx, [](const void* rawCtx, Value child) -> std::expected<void, ErrorCode> {
            const auto* c = static_cast<const FieldCtxEx*>(rawCtx);
            return PatchField(*c->ptr, child, c->detail);
        });
        if (!res) {
            err = res.error();
            if (detail != nullptr) {
                *detail = fieldDetail.empty() ? std::format("field '{}': {}", fieldName, res.error()) : std::format("field '{}': {}", fieldName, fieldDetail);
            }
        }
    });

    if (err) {
        return std::unexpected(*err);
    }

    for (size_t i = 0; i < keys->size(); ++i) {
        if (!consumed[i]) {
            if (table.GetBudget().rejectUnknownFields) {
                if (detail != nullptr) {
                    *detail = std::format("unknown field '{}'", (*keys)[i]);
                }
                return std::unexpected(Error::UnknownField);
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
    } else if constexpr (ValueTrait<Decayed>::specialized) {
        ValueTrait<Decayed>::Push(builder, value);
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

// Declared above, defined in Lua.cpp where CallFrame and lua.h are visible.
// HostFnThunk never needs the stateful CallFrame definition, so plain C++ host
// functions require only <lua/Lua.hpp>.
struct ZHLN_STATIC_API HostAccessBridge {
    [[nodiscard]] static auto ArgCount(const CallFrame& frame) noexcept -> int;
    [[nodiscard]] static auto ArgValue(const CallFrame& frame, int oneBased) noexcept -> Value;
    // luaL_error unwinds through LuaJIT; marking this noexcept lets the C++
    // runtime terminate before lua_pcall can catch the error. It transfers
    // control by longjmp (not a C++ return), so no [[noreturn]] attribute:
    // Clang cannot see luaL_error's noreturn contract through the C API.
    static void RaiseError(const CallFrame& frame, std::string_view message);
    static void PushWith(CallFrame& frame, void* value, void (*pusher)(Builder&, void*)) noexcept;
};

namespace TemplatedDetail {

template <typename T>
void HostPushErased(Builder& builder, void* value) noexcept {
    PushValue(builder, *static_cast<const T*>(value));
}

template <typename T>
auto ReadHostArg(const CallFrame& frame, int oneBased) -> T {
    using Clean = std::remove_cvref_t<T>;
    static_assert(!std::is_reference_v<T> && !std::is_pointer_v<Clean>,
                  "ZHLN::Lua: HostFn arguments must be values (no pointers, IEngine*, or references).");
    static_assert(std::is_default_constructible_v<Clean>,
                  "ZHLN::Lua: HostFn arguments must be default-constructible value types.");
    Clean            out {};
    std::string      detail;
    auto             res = ReadInto(out, HostAccessBridge::ArgValue(frame, oneBased), &detail);
    if (!res) {
        HostAccessBridge::RaiseError(frame, detail.empty() ? "host function argument conversion failed" : detail);
    }
    return out;
}

// A HostFn can be either a raw `int(CallFrame&)` binding (opt-in advanced API)
// or an ordinary C++ function. Ordinary functions are unwrapped with static
// reflection: arguments are read as Values, results are pushed with PushValue.
template <auto Fn>
auto HostFnThunk(CallFrame& frame) -> int {
    if constexpr (std::is_invocable_v<decltype(Fn), CallFrame&>) {
        return static_cast<int>(std::invoke(Fn, frame));
    } else {
        using Traits = ZHLN::Reflect::FunctionTraits<Fn>;
        static_assert(!Traits::isCVariadic, "ZHLN::Lua: variadic HostFn functions are not supported.");
        using Args = typename Traits::ParameterTypes;
        if (HostAccessBridge::ArgCount(frame) != static_cast<int>(Traits::parameterCount)) {
            HostAccessBridge::RaiseError(frame, "wrong number of arguments to host function");
        }
        return [&]<size_t... I>(std::index_sequence<I...>) -> int {
            auto args = std::make_tuple(ReadHostArg<std::tuple_element_t<I, Args>>(frame, static_cast<int>(I) + 1)...);
            if constexpr (std::is_void_v<typename Traits::ReturnType>) {
                std::apply(Fn, args);
                return 0;
            } else {
                auto result = std::apply(Fn, args);
                HostAccessBridge::PushWith(frame, static_cast<void*>(&result), &HostPushErased<typename Traits::ReturnType>);
                return 1;
            }
        }(std::make_index_sequence<Traits::parameterCount> {});
    }
}

} // namespace TemplatedDetail

// ----------------------------------------------------------------------------
// Public Monadic Evaluation & Transactional Patching API
// ----------------------------------------------------------------------------

template <typename T>
[[nodiscard]] auto
    ApplyPatchString(T& target, std::string_view chunk, std::string_view chunkName = "patch", Budget budget = {}, std::string* outDetail = nullptr)
        -> std::expected<void, ErrorCode> {
    auto vm = VirtualMachine::Create(budget);
    if (!vm) {
        return std::unexpected(vm.error());
    }

    struct PatchCtx {
        T*           targetPtr;
        std::string* detail;
    } ctx {&target, outDetail};

    auto res = vm->EvaluateTop(chunk, chunkName, &ctx, [](const void* rawCtx, Value root) -> std::expected<void, ErrorCode> {
        const auto* c = static_cast<const PatchCtx*>(rawCtx);
        return TemplatedDetail::PatchField(*c->targetPtr, root, c->detail);
    });
    if (!res && outDetail != nullptr && outDetail->empty()) {
        // Chunk-level failures (syntax/runtime/budget) are recorded by the
        // VirtualMachine itself; surface them through the same detail channel.
        *outDetail = std::string(vm->LastErrorDetail());
    }
    return res;
}

template <typename T>
[[nodiscard]] auto EvaluateString(std::string_view chunk, std::string_view chunkName = "chunk", Budget budget = {}, std::string* outDetail = nullptr)
    -> std::expected<T, ErrorCode> {
    T out {};
    if constexpr (TemplatedDetail::VectorLike<std::remove_cvref_t<T>> && ValueTrait<std::remove_cvref_t<T>>::specialized) {
        // A value conversion constructs a vector according to its trait;
        // unlike patching, it must not treat a short sequence as an update.
        auto vm = VirtualMachine::Create(budget);
        if (!vm)
            return std::unexpected(vm.error());
        struct ValueCtx {
            T*           target;
            std::string* detail;
        } ctx {&out, outDetail};
        auto res = vm->EvaluateTop(chunk, chunkName, &ctx, [](const void* rawCtx, Value root) -> std::expected<void, ErrorCode> {
            const auto* c = static_cast<const ValueCtx*>(rawCtx);
            return TemplatedDetail::ReadInto(*c->target, root, c->detail);
        });
        if (!res) {
            if (outDetail != nullptr && outDetail->empty()) {
                *outDetail = std::string(vm->LastErrorDetail());
            }
            return std::unexpected(res.error());
        }
    } else if (auto res = ApplyPatchString(out, chunk, chunkName, budget, outDetail); !res) {
        return std::unexpected(res.error());
    }
    return out;
}

// Evaluates a pure Lua function `return function(arg1, arg2, ...) -> table`
// and patches `target` transactionally with its returned table.
template <typename T, typename... InArgs>
[[nodiscard]] auto ApplyCallPatchString(T& target, std::string_view chunk, std::string_view chunkName, Budget budget, const InArgs&... inputs)
    -> std::expected<void, ErrorCode> {
    auto vm = VirtualMachine::Create(budget);
    if (!vm) {
        return std::unexpected(vm.error());
    }

    using TupleT = std::tuple<const InArgs&...>;
    TupleT argsTuple {inputs...};

    struct PatchCtx {
        T* targetPtr;
    } patchCtx {&target};

    return vm->EvaluateCallTop(
        chunk, chunkName, &argsTuple,
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
[[nodiscard]] auto
    EvaluateCallString(std::string_view chunk, std::string_view chunkName, Budget budget, const InArgs&... inputs) -> std::expected<OutT, ErrorCode> {
    OutT out {};
    if (auto res = ApplyCallPatchString(out, chunk, chunkName, budget, inputs...); !res) {
        return std::unexpected(res.error());
    }
    return out;
}

enum class ErrorPolicy : uint8_t {
    Fail,       // propagate std::unexpected(Error) to ZHLN::App
    LogAndSkip, // log, leave state unchanged, keep running
};

// C++26 field annotations. The underlying C++ type is untouched for
// ConfigCli/TOML/JSON; only the Lua bridge interprets these tags.
struct ReadOnly {};
struct Hidden {};
struct OneShot {};

// --- Permissions -----------------------------------------------------------

namespace Permission {

enum class Bit : uint64_t {
    None            = 0,
    Math            = 1ULL << 0,
    Table           = 1ULL << 1,
    String          = 1ULL << 2,
    BitOp           = 1ULL << 3,
    Print           = 1ULL << 4,
    Random          = 1ULL << 5,
    GlobalsWrite    = 1ULL << 6,
    UnknownFields   = 1ULL << 7,
    Start           = 1ULL << 8,
    Tick            = 1ULL << 9,
    BeforeRender    = 1ULL << 10,
    AfterTick       = 1ULL << 11,
    HotReload       = 1ULL << 12,
    Settings        = 1ULL << 13,
    PersistentState = 1ULL << 62,
    HostIO          = 1ULL << 63,
};

template <Bit B>
struct Mask {
    static constexpr Bit bits = B;

    template <Bit Other>
    [[nodiscard]] constexpr auto operator|(Mask<Other>) const noexcept -> Mask<static_cast<Bit>(static_cast<uint64_t>(B) | static_cast<uint64_t>(Other))> {
        return {};
    }
};

inline constexpr Mask<Bit::Math>            Math {};
inline constexpr Mask<Bit::Table>           Table {};
inline constexpr Mask<Bit::String>          String {};
inline constexpr Mask<Bit::BitOp>           BitOp {};
inline constexpr Mask<Bit::Print>           Print {};
inline constexpr Mask<Bit::Random>          Random {};
inline constexpr Mask<Bit::GlobalsWrite>    GlobalsWrite {};
inline constexpr Mask<Bit::UnknownFields>   UnknownFields {};
inline constexpr Mask<Bit::Start>           Start {};
inline constexpr Mask<Bit::Tick>            Tick {};
inline constexpr Mask<Bit::BeforeRender>    BeforeRender {};
inline constexpr Mask<Bit::AfterTick>       AfterTick {};
inline constexpr Mask<Bit::HotReload>       HotReload {};
inline constexpr Mask<Bit::Settings>        Settings {};
inline constexpr Mask<Bit::PersistentState> PersistentState {};
inline constexpr Mask<Bit::HostIO>          HostIO {};

inline constexpr uint64_t kDefaultYes =
    static_cast<uint64_t>(Bit::Math) | static_cast<uint64_t>(Bit::Table) | static_cast<uint64_t>(Bit::String) |
    static_cast<uint64_t>(Bit::BitOp) | static_cast<uint64_t>(Bit::Print) | static_cast<uint64_t>(Bit::Start);

inline constexpr uint64_t kForbiddenAlways =
    static_cast<uint64_t>(Bit::PersistentState) | static_cast<uint64_t>(Bit::HostIO);

template <typename... Ts>
struct TypeList {};

template <Bit... Bs>
inline constexpr uint64_t BitsOf = (static_cast<uint64_t>(Bs) | ... | 0u);

template <typename... Ts>
struct WriteSet {
    static_assert((!ZHLN::SceneResources::ReadOnlyReflective<Ts> && ...),
                  "ZHLN::Lua: cannot grant write permission to a SceneResources::ReadOnlyReflective<T> type.");
};

template <typename... Ts>
inline constexpr WriteSet<Ts...> Write {};

template <ZHLN::StringLiteral Name, auto Fn>
struct HostFnTag {
    static constexpr auto name = Name;
    static constexpr auto fn   = Fn;
};

template <ZHLN::StringLiteral Name, auto Fn>
inline constexpr HostFnTag<Name, Fn> HostFn {};

// Registration is header-only and works for plain C++ functions without
// including <lua/Host.hpp>; raw CallFrame bindings remain opt-in.
template <typename HostFns>
struct HostRegistrar;

// YesBits is the effective mask; the two explicit masks exist solely so that
// contradictory chains (.No(X).Yes(X) or .Yes(X).No(X)) fail at compile time.
template <uint64_t YesBits, uint64_t ExplicitYesBits, uint64_t ExplicitNoBits, typename Writes_, typename HostFns_>
struct Policy {
    static constexpr uint64_t yes          = YesBits;
    static constexpr uint64_t explicitYes  = ExplicitYesBits;
    static constexpr uint64_t explicitNo   = ExplicitNoBits;
    using Writes  = Writes_;
    using HostFns = HostFns_;

    template <Bit... Bs>
    [[nodiscard]] constexpr auto Yes(Mask<Bs>...) const noexcept {
        static_assert((BitsOf<Bs...> & kForbiddenAlways) == 0u,
                      "ZHLN::Lua: PersistentState and HostIO are never grantable.");
        static_assert((ExplicitNoBits & BitsOf<Bs...>) == 0u,
                      "ZHLN::Lua: contradictory policy -- .No() was called before .Yes() on this bit.");
        return Policy<YesBits | BitsOf<Bs...>, ExplicitYesBits | BitsOf<Bs...>, ExplicitNoBits, Writes_, HostFns_> {};
    }

    template <Bit... Bs>
    [[nodiscard]] constexpr auto No(Mask<Bs>...) const noexcept {
        static_assert((ExplicitYesBits & BitsOf<Bs...>) == 0u,
                      "ZHLN::Lua: contradictory policy -- .Yes() was called before .No() on this bit.");
        return Policy<YesBits & ~BitsOf<Bs...>, ExplicitYesBits, ExplicitNoBits | BitsOf<Bs...>, Writes_, HostFns_> {};
    }

    template <typename... Ts>
    [[nodiscard]] constexpr auto Yes(WriteSet<Ts...>) const noexcept {
        return Policy<YesBits, ExplicitYesBits, ExplicitNoBits, TypeList<Ts...>, HostFns_> {};
    }

    template <ZHLN::StringLiteral Name, auto Fn>
    [[nodiscard]] constexpr auto Host(HostFnTag<Name, Fn>) const noexcept {
        using NextHosts = typename AppendHosts<HostFns_, HostFnTag<Name, Fn>>::type;
        return Policy<YesBits, ExplicitYesBits, ExplicitNoBits, Writes_, NextHosts> {};
    }

  private:
    template <typename List, typename Tag>
    struct AppendHosts;
    template <typename... Tags, typename Tag>
    struct AppendHosts<TypeList<Tags...>, Tag> {
        using type = TypeList<Tags..., Tag>;
    };
};

using DefaultPolicy = Policy<kDefaultYes, 0u, 0u, TypeList<>, TypeList<>>;

template <typename... Tags>
struct HostRegistrar<TypeList<Tags...>> {
    [[nodiscard]] static auto Register(VirtualMachine& vm) noexcept -> std::expected<void, ErrorCode> {
        std::optional<ErrorCode> failure;
        auto                     registerOne = [&]<typename Tag>() -> void {
            if (failure) {
                return;
            }
            if (auto res = vm.RegisterHostFunction(std::string_view(Tag::name), &TemplatedDetail::HostFnThunk<Tag::fn>); !res) {
                failure = res.error();
            }
        };
        (registerOne.template operator()<Tags>(), ...);
        if (failure) {
            return std::unexpected(*failure);
        }
        return {};
    }
};

} // namespace Permission

// --- Runtime ---------------------------------------------------------------

template <typename Policy = Permission::DefaultPolicy>
class LayerBuilder;

template <typename T, typename Policy>
class Layer;
template <typename T, typename Policy>
class ScriptBuilder;
template <typename T>
class Script;

class Runtime {
  public:
    struct ErrorDetail {
        Error       code {};
        std::string message;
    };

    [[nodiscard]] auto LastError() const noexcept -> const std::optional<ErrorDetail>& {
        return _lastError;
    }

  protected:
    Runtime() noexcept  = default;
    ~Runtime() noexcept = default;

    Runtime(const Runtime&)                    = delete;
    auto operator=(const Runtime&) -> Runtime& = delete;
    Runtime(Runtime&&) noexcept                = default;
    auto operator=(Runtime&&) noexcept -> Runtime& = default;

  private:
    template <typename, typename>
    friend class Layer;
    template <typename>
    friend class LayerBuilder;

    void SetLastError(Error code, std::string message) {
        _lastError = ErrorDetail {.code = code, .message = std::move(message)};
    }
    void ClearLastError() noexcept {
        _lastError.reset();
    }

    std::optional<ErrorDetail> _lastError;
};

// --- Reflected state marshalling and transactional patching ----------------

namespace TemplatedDetail {

struct PatchOptions {
    bool allowUnknownFields = false;
};

using Commit = std::function<void()>;

template <typename T>
concept StructuredAggregate =
    std::is_class_v<std::remove_cvref_t<T>> && ZHLN::Reflect::FieldCount<std::remove_cvref_t<T>>() > 0 &&
    !ValueTrait<std::remove_cvref_t<T>>::specialized && !Vector<std::remove_cvref_t<T>>::value &&
    !TemplatedDetail::FixedStringLike<std::remove_cvref_t<T>> &&
    !TemplatedDetail::StringLike<std::remove_cvref_t<T>> &&
    !TemplatedDetail::MapLike<std::remove_cvref_t<T>> &&
    !TemplatedDetail::OptionalLike<std::remove_cvref_t<T>> && !std::ranges::range<std::remove_cvref_t<T>>;

template <typename T>
void PushState(const T& state, Builder& builder) {
    using Clean = std::remove_cvref_t<T>;
    builder.BeginTable(0, static_cast<int>(ZHLN::Reflect::FieldCount<Clean>()));
    ZHLN::Reflect::ForEachDataMemberWithMeta(state, [&]<auto member, typename MemberType>(std::string_view name, const auto& value) -> void {
        if constexpr (ZHLN::Reflect::HasAnnotation<Hidden, member>()) {
            return;
        }
        TemplatedDetail::PushValue(builder, value);
        builder.SetFieldTop(name);
    });
}

template <typename T>
void ResetOneShots(T& state) {
    ZHLN::Reflect::ForEachDataMemberWithMeta(state, [&]<auto member, typename MemberType>(std::string_view, auto& value) -> void {
        using Field = std::remove_reference_t<decltype(value)>;
        if constexpr ((ZHLN::Reflect::HasAnnotation<OneShot, member>() ||
                       std::is_base_of_v<OneShot, std::remove_cvref_t<Field>>) &&
                      !std::is_reference_v<MemberType> && !std::is_const_v<Field> && std::default_initializable<Field>) {
            value = Field {};
        }
    });
}

template <typename T>
auto StageValue(T& target, Value value, const PatchOptions& options, std::string* detail, std::vector<Commit>& commits)
    -> std::expected<void, ErrorCode>;

template <typename T>
auto StageObject(T& target, Value table, const PatchOptions& options, std::string* detail, std::vector<Commit>& commits)
    -> std::expected<void, ErrorCode> {
    if (!table.IsTable()) {
        return std::unexpected(Error::NotATable);
    }
    auto keys = table.GetTableStringKeys();
    if (!keys) {
        return std::unexpected(keys.error());
    }

    std::vector<uint8_t> consumed(keys->size(), 0u);
    std::optional<Error> err;

    struct FieldContext {
        void*                       fieldPtr = nullptr;
        const PatchOptions*         options  = nullptr;
        std::string*                detail   = nullptr;
        std::vector<Commit>*        commits  = nullptr;
    };

    ZHLN::Reflect::ForEachDataMemberWithMeta(target, [&]<auto member, typename MemberType>(std::string_view name, auto& field) -> void {
        if (err) {
            return;
        }
        size_t keyIndex = keys->size();
        for (size_t i = 0; i < keys->size(); ++i) {
            if ((*keys)[i] == name) {
                keyIndex = i;
                break;
            }
        }
        if (keyIndex == keys->size()) {
            return;
        }
        consumed[keyIndex] = 1u;

        constexpr bool kHidden    = ZHLN::Reflect::HasAnnotation<Hidden, member>();
        constexpr bool kReference = std::is_reference_v<MemberType>;
        constexpr bool kReadOnly  = ZHLN::Reflect::HasAnnotation<ReadOnly, member>() ||
                                    std::is_base_of_v<ReadOnly, std::remove_cvref_t<MemberType>>;

        // Keep StageValue out of the instantiation set for fields that can
        // never be written, including non-copyable handles annotated Hidden.
        if constexpr (kHidden || kReference) {
            err = kHidden ? Error::UnknownField : Error::ReadOnlyField;
        } else if constexpr (kReadOnly) {
            err = Error::ReadOnlyField;
        } else {
            using Field = std::remove_reference_t<decltype(field)>;
            FieldContext ctx {
                .fieldPtr = static_cast<void*>(&field),
                .options  = &options,
                .detail   = detail,
                .commits  = &commits,
            };
            struct Visitor {
                static auto Apply(const void* rawCtx, Value child) -> std::expected<void, ErrorCode> {
                    const auto* c = static_cast<const FieldContext*>(rawCtx);
                    return StageValue(*static_cast<Field*>(c->fieldPtr), child, *c->options, c->detail, *c->commits);
                }
            };
            auto res = table.WithKey(name, &ctx, &Visitor::Apply);
            if (!res) {
                err = res.error().template get<Error>().value_or(Error::RuntimeError);
            }
        }
    });

    if (err) {
        return std::unexpected(*err);
    }
    if (!options.allowUnknownFields) {
        for (size_t i = 0; i < keys->size(); ++i) {
            if (consumed[i] == 0u) {
                if (detail != nullptr) {
                    *detail = std::format("unknown field '{}'", (*keys)[i]);
                }
                return std::unexpected(Error::UnknownField);
            }
        }
    }
    return {};
}

template <typename T>
auto StageValue(T& target, Value value, const PatchOptions& options, std::string* detail, std::vector<Commit>& commits)
    -> std::expected<void, ErrorCode> {
    using Clean = std::remove_cvref_t<T>;
    if constexpr (StructuredAggregate<T>) {
        return StageObject(target, value, options, detail, commits);
    } else {
        Clean staged = target;
        if (auto res = TemplatedDetail::ReadInto(staged, value, detail); !res) {
            return res;
        }
        commits.emplace_back([&target, staged = std::move(staged)]() mutable { target = std::move(staged); });
        return {};
    }
}

template <typename T>
auto PatchState(T& target, Value value, const PatchOptions& options, std::string* detail = nullptr)
    -> std::expected<void, ErrorCode> {
    std::vector<Commit> commits;
    if (auto res = StageValue(target, value, options, detail, commits); !res) {
        // No commit closure has run: the canonical state is untouched.
        return res;
    }
    for (auto& commit: commits) {
        commit();
    }
    return {};
}

} // namespace TemplatedDetail

// --- Script ----------------------------------------------------------------

template <typename T>
class Script {
  public:
    Script() noexcept = default;

    template <typename Policy = Permission::DefaultPolicy>
    [[nodiscard]] static auto Compile(
        std::string_view code,
        std::string_view name,
        Budget           budget = {},
        Policy           policy = {}
    ) noexcept -> std::expected<Script, ErrorCode> {
        (void) policy;
        Script result;
        result._allowUnknownFields = (Policy::yes & static_cast<uint64_t>(Permission::Bit::UnknownFields)) != 0u;
        Budget coreBudget {
            .maxInstructions     = budget.maxInstructions,
            .maxMemoryBytes      = budget.maxMemoryBytes,
            .rejectUnknownFields = (Policy::yes & static_cast<uint64_t>(Permission::Bit::UnknownFields)) == 0u,
        };
        auto vm = VirtualMachine::Create(coreBudget);
        if (!vm) {
            return std::unexpected(vm.error());
        }
        if (auto perms = vm->ApplyScriptPermissions(Policy::yes); !perms) {
            return std::unexpected(perms.error());
        }
        if constexpr (!std::same_as<typename Policy::HostFns, Permission::TypeList<>>) {
            if (auto registered = Permission::HostRegistrar<typename Policy::HostFns>::Register(*vm); !registered) {
                return std::unexpected(registered.error());
            }
        }
        if (auto compiled = vm->CompileChunk(code, name); !compiled) {
            return std::unexpected(compiled.error());
        }
        result._vm   = std::move(*vm);
        result._name = std::string(name);
        return result;
    }

    [[nodiscard]] auto operator()(T& state) noexcept -> std::expected<void, ErrorCode> {
        if (!_vm.IsValid() || !_vm.HasCompiledChunk()) {
            return std::unexpected(Error::ScriptLoadFailed);
        }
        struct CallContext {
            T*           state              = nullptr;
            std::string* detail             = nullptr;
            bool         allowUnknownFields = false;
        } ctx {&state, &_lastDetail, _allowUnknownFields};

        struct Invoker {
            static auto Push(const void* rawCtx, Builder& builder) -> int {
                const auto* c = static_cast<const CallContext*>(rawCtx);
                TemplatedDetail::PushState(*c->state, builder);
                return 1;
            }
            static auto Consume(const void* rawCtx, Value result) -> std::expected<void, ErrorCode> {
                const auto* c = static_cast<const CallContext*>(rawCtx);
                return TemplatedDetail::PatchState(
                    *c->state, result,
                    TemplatedDetail::PatchOptions {
                        .allowUnknownFields = c->allowUnknownFields,
                    },
                    c->detail
                );
            }
        };

        _lastDetail.clear();
        auto res = _vm.EvaluateCompiledCallTop(&ctx, &Invoker::Push, &ctx, &Invoker::Consume, _name);
        if (!res) {
            _lastError = res.error().template get<Error>();
        }
        return res;
    }

    [[nodiscard]] auto LastError() const noexcept -> std::optional<Error> {
        return _lastError;
    }
    [[nodiscard]] auto LastDetail() const noexcept -> std::string_view {
        return _lastDetail;
    }
    [[nodiscard]] auto Machine() noexcept -> VirtualMachine& {
        return _vm;
    }

  private:
    VirtualMachine       _vm;
    std::string          _name;
    std::string          _lastDetail;
    std::optional<Error> _lastError;
    bool                 _allowUnknownFields = false;

    template <typename, typename>
    friend class ScriptBuilder;
};

// Type-erased compiled-script builder: policy is checked at compile time here
// and then erased, so callsites never spell `decltype(Policy{}.Yes(...))`.
template <typename T, typename Policy = Permission::DefaultPolicy>
class ScriptBuilder {
  public:
    ScriptBuilder(std::string code, std::string name, Budget budget = {}) noexcept:
        _code(std::move(code)), _name(std::move(name)), _budget(budget) {
    }

    template <Permission::Bit... Bs>
    [[nodiscard]] auto Yes(Permission::Mask<Bs>... masks) const noexcept
        -> ScriptBuilder<T, decltype(Policy {}.Yes(masks...))> {
        using Next = decltype(Policy {}.Yes(masks...));
        return ScriptBuilder<T, Next>(_code, _name, _budget);
    }
    template <Permission::Bit... Bs>
    [[nodiscard]] auto No(Permission::Mask<Bs>... masks) const noexcept
        -> ScriptBuilder<T, decltype(Policy {}.No(masks...))> {
        using Next = decltype(Policy {}.No(masks...));
        return ScriptBuilder<T, Next>(_code, _name, _budget);
    }
    template <ZHLN::StringLiteral Name, auto Fn>
    [[nodiscard]] auto Host(Permission::HostFnTag<Name, Fn> fn) const noexcept
        -> ScriptBuilder<T, decltype(Policy {}.Host(fn))> {
        using Next = decltype(Policy {}.Host(fn));
        return ScriptBuilder<T, Next>(_code, _name, _budget);
    }
    [[nodiscard]] auto Budget(Lua::Budget value) const noexcept -> ScriptBuilder {
        auto copy = *this;
        copy._budget = value;
        return copy;
    }

    [[nodiscard]] auto Build() const noexcept -> std::expected<Script<T>, ErrorCode> {
        return Script<T>::template Compile<Policy>(_code, _name, _budget);
    }

  private:
    std::string _code;
    std::string _name;
    Lua::Budget _budget {};
};

template <typename T>
[[nodiscard]] auto Compile(std::string_view code, std::string_view name) noexcept -> ScriptBuilder<T> {
    return ScriptBuilder<T>(std::string(code), std::string(name));
}
template <typename T, typename Bytes>
    requires std::same_as<std::remove_cvref_t<Bytes>, std::span<const char>>
[[nodiscard]] auto Compile(Bytes&& bytes, std::string_view name) noexcept -> ScriptBuilder<T> {
    return Compile<T>(std::string_view(bytes.data(), bytes.size()), name);
}
template <typename T, typename Bytes>
    requires std::same_as<std::remove_cvref_t<Bytes>, std::span<const std::byte>>
[[nodiscard]] auto Compile(Bytes&& bytes, std::string_view name) noexcept -> ScriptBuilder<T> {
    return Compile<T>(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), name);
}

// --- App layer -------------------------------------------------------------

// Script sources enter Lua through exactly two doors: FromAsset reads text
// through the engine VFS (COM::IAssetLibrary), and FromString takes in-memory
// characters or bytes. There is deliberately no OS-filesystem open path.
template <typename Policy>
class LayerBuilder {
  public:
    [[nodiscard]] static auto FromAsset(std::string_view virtualPath) noexcept -> LayerBuilder {
        LayerBuilder builder;
        builder._fromAsset = true;
        builder._source    = virtualPath;
        builder._name      = virtualPath;
        return builder;
    }
    [[nodiscard]] static auto FromString(std::string_view code, std::string_view name = "inline.lua") noexcept -> LayerBuilder {
        LayerBuilder builder;
        builder._source = code;
        builder._name   = name;
        return builder;
    }
    template <typename Bytes>
        requires std::same_as<std::remove_cvref_t<Bytes>, std::span<const char>>
    [[nodiscard]] static auto FromString(Bytes&& bytes, std::string_view name = "inline.lua") noexcept -> LayerBuilder {
        return FromString(std::string_view(bytes.data(), bytes.size()), name);
    }
    template <typename Bytes>
        requires std::same_as<std::remove_cvref_t<Bytes>, std::span<const std::byte>>
    [[nodiscard]] static auto FromString(Bytes&& bytes, std::string_view name = "inline.lua") noexcept -> LayerBuilder {
        return FromString(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), name);
    }

    template <Permission::Bit... Bs>
    [[nodiscard]] auto Yes(Permission::Mask<Bs>... masks) const noexcept -> LayerBuilder<decltype(Policy {}.Yes(masks...))> {
        return Clone<decltype(Policy {}.Yes(masks...))>();
    }
    template <Permission::Bit... Bs>
    [[nodiscard]] auto No(Permission::Mask<Bs>... masks) const noexcept -> LayerBuilder<decltype(Policy {}.No(masks...))> {
        return Clone<decltype(Policy {}.No(masks...))>();
    }
    template <typename... Ts>
    [[nodiscard]] auto Write() const noexcept -> LayerBuilder<decltype(Policy {}.Yes(Permission::Write<Ts...>))> {
        return Clone<decltype(Policy {}.Yes(Permission::Write<Ts...>))>();
    }
    template <ZHLN::StringLiteral Name, auto Fn>
    [[nodiscard]] auto Host(Permission::HostFnTag<Name, Fn> fn) const noexcept -> LayerBuilder<decltype(Policy {}.Host(fn))> {
        return Clone<decltype(Policy {}.Host(fn))>();
    }

    [[nodiscard]] auto Budget(Lua::Budget value) const noexcept -> LayerBuilder {
        auto copy = *this;
        copy._budget = value;
        return copy;
    }
    [[nodiscard]] auto OnError(ErrorPolicy policy) const noexcept -> LayerBuilder {
        auto copy = *this;
        copy._errorPolicy = policy;
        return copy;
    }

    template <typename T>
    [[nodiscard]] auto State(T& state) const noexcept -> Layer<T, Policy> {
        return Layer<T, Policy>(*this, state);
    }

  private:
    template <typename NextPolicy>
    [[nodiscard]] auto Clone() const noexcept -> LayerBuilder<NextPolicy> {
        LayerBuilder<NextPolicy> next;
        next._source      = _source;
        next._name        = _name;
        next._fromAsset   = _fromAsset;
        next._loadError   = _loadError;
        next._budget      = _budget;
        next._errorPolicy = _errorPolicy;
        return next;
    }

    std::string  _source;
    std::string  _name;
    bool                     _fromAsset   = false;
    std::optional<ErrorCode> _loadError;
    Lua::Budget              _budget {};
    ErrorPolicy              _errorPolicy = ErrorPolicy::Fail;

    template <typename, typename>
    friend class Layer;
};

template <typename T, typename Policy = Permission::DefaultPolicy>
class Layer {
  public:
    Layer(const LayerBuilder<Policy>& builder, T& state):
        _source(builder._source), _name(builder._name), _fromAsset(builder._fromAsset), _loadError(builder._loadError),
        _budget(builder._budget), _errorPolicy(builder._errorPolicy), _state(&state) {
    }

    [[nodiscard]] auto StatePtr() noexcept -> T* {
        return _state;
    }

    template <Permission::Bit... Bs>
    [[nodiscard]] auto Yes(Permission::Mask<Bs>... masks) const noexcept -> Layer<T, decltype(Policy {}.Yes(masks...))> {
        using NextPolicy = decltype(Policy {}.Yes(masks...));
        return Layer<T, NextPolicy>(ToBuilder<NextPolicy>(), *_state);
    }
    template <Permission::Bit... Bs>
    [[nodiscard]] auto No(Permission::Mask<Bs>... masks) const noexcept -> Layer<T, decltype(Policy {}.No(masks...))> {
        using NextPolicy = decltype(Policy {}.No(masks...));
        return Layer<T, NextPolicy>(ToBuilder<NextPolicy>(), *_state);
    }
    template <typename... Ts>
    [[nodiscard]] auto Write() const noexcept -> Layer<T, decltype(Policy {}.Yes(Permission::Write<Ts...>))> {
        using NextPolicy = decltype(Policy {}.Yes(Permission::Write<Ts...>));
        return Layer<T, NextPolicy>(ToBuilder<NextPolicy>(), *_state);
    }
    template <ZHLN::StringLiteral Name, auto Fn>
    [[nodiscard]] auto Host(Permission::HostFnTag<Name, Fn> fn) const noexcept -> Layer<T, decltype(Policy {}.Host(fn))> {
        using NextPolicy = decltype(Policy {}.Host(fn));
        return Layer<T, NextPolicy>(ToBuilder<NextPolicy>(), *_state);
    }
    [[nodiscard]] auto Budget(Lua::Budget value) const noexcept -> Layer {
        auto builder = ToBuilder<Policy>();
        builder._budget = value;
        return Layer(builder, *_state);
    }
    [[nodiscard]] auto OnError(ErrorPolicy value) const noexcept -> Layer {
        auto builder = ToBuilder<Policy>();
        builder._errorPolicy = value;
        return Layer(builder, *_state);
    }

    [[nodiscard]] auto Start(COM::IEngine& engine) -> std::expected<void, ErrorCode> {
        if (_loadError) {
            return Fail(engine, "Start", *_loadError, "script file could not be read");
        }
        std::string source = _source;
        if (_fromAsset) {
            const auto assets = engine.Assets();
            if (assets == nullptr) {
                return Fail(engine, "Start", Error::FileReadFailed, "no asset library");
            }
            auto text = assets->ReadVirtualText(ZHLN::COM::StringView(_source));
            if (!text) {
                return Fail(engine, "Start", Error::FileReadFailed, "script asset was not found");
            }
            source = std::move(*text);
        }

        auto compiled = Script<T>::template Compile<Policy>(source, _name, _budget);
        if (!compiled) {
            return Fail(engine, "Start", compiled.error(), "compile failed");
        }
        _script.emplace(std::move(*compiled));

        // A Lua startup patch must land before C++ consumes the state it sets.
        if constexpr ((Policy::yes & static_cast<uint64_t>(Permission::Bit::Start)) != 0u) {
            if (auto res = Invoke(*_state); !res) {
                return Fail(engine, "Start", res.error(), _script->LastDetail());
            }
        }
        if constexpr (requires(T& s, COM::IEngine& e) { s.OnStart(e); }) {
            _state->OnStart(engine);
        }
        ClearRuntimeError();
        return {};
    }

    [[nodiscard]] auto Tick(COM::IEngine& engine, float dt) -> std::expected<void, ErrorCode>
        requires((Policy::yes & static_cast<uint64_t>(Permission::Bit::Tick)) != 0u)
    {
        return RunFramePhase(engine, dt, "Tick");
    }

    [[nodiscard]] auto BeforeRender(COM::IEngine& engine, float dt) -> std::expected<void, ErrorCode>
        requires((Policy::yes & static_cast<uint64_t>(Permission::Bit::BeforeRender)) != 0u)
    {
        return RunFramePhase(engine, dt, "BeforeRender");
    }

    [[nodiscard]] auto AfterTick(COM::IEngine& engine) -> std::expected<void, ErrorCode>
        requires((Policy::yes & static_cast<uint64_t>(Permission::Bit::AfterTick)) != 0u)
    {
        return RunFramePhase(engine, 0.0f, "AfterTick");
    }

    [[nodiscard]] auto Reload(COM::IEngine& engine) -> std::expected<void, ErrorCode>
        requires((Policy::yes & static_cast<uint64_t>(Permission::Bit::HotReload)) != 0u)
    {
        return Start(engine);
    }

    auto Stop(COM::IEngine& engine) -> void {
        if constexpr (requires(T& s, COM::IEngine& e) { s.OnStop(e); }) {
            _state->OnStop(engine);
        }
        _script.reset();
    }

    // Explicitly rebuild the compiled chunk from newly loaded source.
    [[nodiscard]] auto HotReloadSource(COM::IEngine& engine, std::string_view source) -> std::expected<void, ErrorCode> {
        auto compiled = Script<T>::template Compile<Policy>(source, _name, _budget);
        if (!compiled) {
            return Fail(engine, "HotReload", compiled.error(), "compile failed");
        }
        _script.emplace(std::move(*compiled));
        return {};
    }

  private:
    template <typename NextPolicy>
    [[nodiscard]] auto ToBuilder() const noexcept -> LayerBuilder<NextPolicy> {
        LayerBuilder<NextPolicy> builder;
        builder._source      = _source;
        builder._name        = _name;
        builder._fromAsset   = _fromAsset;
        builder._loadError   = _loadError;
        builder._budget      = _budget;
        builder._errorPolicy = _errorPolicy;
        return builder;
    }

    [[nodiscard]] auto Invoke(T& state) noexcept -> std::expected<void, ErrorCode> {
        if (!_script.has_value()) {
            return std::unexpected(Error::ScriptLoadFailed);
        }
        return (*_script)(state);
    }

    [[nodiscard]] auto RunFramePhase(COM::IEngine& engine, float dt, std::string_view phase) -> std::expected<void, ErrorCode> {
        // Exactly one dispatched phase per frame owns the C++ state boundary:
        // Tick if granted, otherwise the first available frame hook.
        static constexpr std::string_view kFrameBoundaryPhase = [] {
            if constexpr ((Policy::yes & static_cast<uint64_t>(Permission::Bit::Tick)) != 0u) {
                return std::string_view {"Tick"};
            } else if constexpr ((Policy::yes & static_cast<uint64_t>(Permission::Bit::BeforeRender)) != 0u) {
                return std::string_view {"BeforeRender"};
            } else {
                return std::string_view {"AfterTick"};
            }
        }();
        const bool frameBoundary = (phase == kFrameBoundaryPhase);
        if (frameBoundary) {
            TemplatedDetail::ResetOneShots(*_state);
            if constexpr (requires(T& s, COM::IEngine& e, float d) { s.BeforeLua(e, d); }) {
                _state->BeforeLua(engine, dt);
            }
        }
        if (auto res = Invoke(*_state); !res) {
            return Fail(engine, phase, res.error(), _script.has_value() ? _script->LastDetail() : std::string_view {});
        }
        if (frameBoundary) {
            if constexpr (requires(T& s, COM::IEngine& e, float d) { s.AfterLua(e, d); }) {
                _state->AfterLua(engine, dt);
            } else if constexpr (requires(T& s, COM::IEngine& e) { s.AfterLua(e); }) {
                _state->AfterLua(engine);
            }
            ClearRuntimeError();
        }
        return {};
    }

    [[nodiscard]] auto Fail(COM::IEngine& engine, std::string_view phase, ErrorCode code, std::string_view detail) const -> std::expected<void, ErrorCode> {
        const auto  error = code.get<Error>().value_or(Error::RuntimeError);
        const auto  text  = ZHLN::Reflect::EnumMessageOf(error);
        SetRuntimeError(error, std::format("{}: {}: {}", phase, text, detail));
        if (_errorPolicy == ErrorPolicy::LogAndSkip) {
            COM::LoggerView {engine.Logger(), "Lua"}.Warn("{}: {} ({})", phase, text, detail);
            return {};
        }
        return std::unexpected(code);
    }

    void ClearRuntimeError() const noexcept {
        if constexpr (std::derived_from<T, Runtime>) {
            _state->ClearLastError();
        }
    }
    void SetRuntimeError(Error error, std::string message) const {
        if constexpr (std::derived_from<T, Runtime>) {
            _state->SetLastError(error, std::move(message));
        }
    }

    std::string  _source;
    std::string  _name;
    bool                     _fromAsset  = false;
    std::optional<ErrorCode> _loadError;
    Lua::Budget              _budget {};
    ErrorPolicy              _errorPolicy = ErrorPolicy::Fail;
    T*                       _state = nullptr;
    std::optional<Script<T>> _script;
};

inline auto FromAsset(std::string_view path) noexcept -> LayerBuilder<> {
    return LayerBuilder<>::FromAsset(path);
}
inline auto FromString(std::string_view code, std::string_view name = "inline.lua") noexcept -> LayerBuilder<> {
    return LayerBuilder<>::FromString(code, name);
}
template <typename Bytes>
    requires std::same_as<std::remove_cvref_t<Bytes>, std::span<const char>>
[[nodiscard]] auto FromString(Bytes&& bytes, std::string_view name = "inline.lua") noexcept -> LayerBuilder<> {
    return LayerBuilder<>::FromString(bytes, name);
}
template <typename Bytes>
    requires std::same_as<std::remove_cvref_t<Bytes>, std::span<const std::byte>>
[[nodiscard]] auto FromString(Bytes&& bytes, std::string_view name = "inline.lua") noexcept -> LayerBuilder<> {
    return LayerBuilder<>::FromString(bytes, name);
}

} // namespace Lua
} // namespace ZHLN
