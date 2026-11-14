// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Core/COM/Types.hpp
//
// The vocabulary that crosses a shared-library boundary. Each type is a fixed
// layout of scalar words: no allocator, no CRT state, no standard-library
// container object, and nothing whose layout a second standard library could
// disagree about. Ordinary Zahlen code keeps using std::string_view, std::span
// and std::expected -- the conversion lives at the boundary declaration.

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/ErrorCode.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ZHLN::COM {

// Failures the COM protocol or surface itself produces, distinct from the
// subsystem ErrorCode categories forwarded from the engine.
enum class ComError : uint8_t {
    AllocationFailed     ZHLN_ANNOTATION(ZHLN::Description<"COM object allocation failed."> {}) = 1,
    AssetNotCached       ZHLN_ANNOTATION(ZHLN::Description<"Requested asset is not present in the engine cache."> {}) = 2,
    NullArgument         ZHLN_ANNOTATION(ZHLN::Description<"Required COM pointer or chain node payload is null."> {}) = 3,
    LayoutMismatch       ZHLN_ANNOTATION(ZHLN::Description<"COM chain node size or reflected layout hash disagrees with the library."> {}) = 4,
    UnsupportedExtension ZHLN_ANNOTATION(ZHLN::Description<"COM chain contains an unrecognized structure type."> {}) = 5,
    DuplicateExtension   ZHLN_ANNOTATION(ZHLN::Description<"COM chain contains duplicate structure types or a cycle."> {}) = 6,
    InvalidArgument      ZHLN_ANNOTATION(ZHLN::Description<"COM descriptor arguments are incomplete or invalid."> {}) = 7,
};

struct StringView {
    const char* data = nullptr;
    size_t      size = 0;

    constexpr StringView() noexcept = default;
    constexpr StringView(const char* text, size_t length) noexcept: data(text), size(length) {
    }
    constexpr StringView(std::string_view view) noexcept: data(view.data()), size(view.size()) {
    }
    constexpr StringView(const char* text) noexcept: StringView(std::string_view(text)) {
    }

    [[nodiscard]] constexpr operator std::string_view() const noexcept {
        return {data, size};
    }
};

static_assert(std::is_standard_layout_v<StringView> && std::is_trivially_copyable_v<StringView>);

template <typename T>
struct Span {
    T*     data = nullptr;
    size_t size = 0;

    constexpr Span() noexcept = default;
    constexpr Span(T* pointer, size_t count) noexcept: data(pointer), size(count) {
    }
    constexpr Span(std::span<T> view) noexcept: data(view.data()), size(view.size()) {
    }

    // A contiguous range converts in one step. Building the std::span here is
    // *inside* the constructor, not a second user-defined conversion at the
    // call site, so Span<const std::byte> accepts a std::array<std::byte, N>
    // directly.
    template <typename Range>
        requires (!std::is_same_v<std::remove_cvref_t<Range>, Span> &&
                  !std::is_same_v<std::remove_cvref_t<Range>, std::span<T>> &&
                  requires(Range&& range) { std::span<T>(std::forward<Range>(range)); })
    constexpr Span(Range&& range) noexcept: Span(std::span<T>(std::forward<Range>(range))) {
    }

    [[nodiscard]] constexpr operator std::span<T>() const noexcept {
        return {data, size};
    }

    [[nodiscard]] constexpr operator std::span<const T>() const noexcept requires (!std::is_const_v<T>) {
        return {data, size};
    }
};

static_assert(std::is_standard_layout_v<Span<const std::byte>> && std::is_trivially_copyable_v<Span<const std::byte>>);

// Presence as a value, not as a protocol. A `bool` return plus a `T*`
// out-parameter is a C habit that survives into C++ only because the ABI
// forbids std::optional: it makes the caller declare storage before it knows
// whether anything will be put there, it cannot be returned or chained, and
// it leaves the out-parameter's state on a miss as a thing you have to
// document rather than a thing the type says. Option<T> is a fixed layout of
// T plus a flag, so it crosses as an ordinary return value and the ABI audit
// accepts it on the wire.
//
// The conversion to std::optional is the same bargain Result makes with
// std::expected: the wire type is what the vtable traffics in, the standard
// type is one implicit conversion away in the caller's own translation unit.
template <typename T>
struct Option {
    static_assert(std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>,
                  "COM::Option<T> carries T by value across the boundary; T must be a fixed-layout value.");

    T    value {};
    bool engaged = false;

    constexpr Option() noexcept = default;
    constexpr Option(T val) noexcept: value(val), engaged(true) {
    }
    constexpr Option(std::nullopt_t) noexcept {
    }

    [[nodiscard]] constexpr bool HasValue() const noexcept {
        return engaged;
    }

    [[nodiscard]] constexpr T ValueOr(T fallback) const noexcept {
        return engaged ? value : fallback;
    }

    [[nodiscard]] constexpr operator std::optional<T>() const noexcept {
        if (engaged) return value;
        return std::nullopt;
    }
};

static_assert(std::is_standard_layout_v<Option<uint32_t>> && std::is_trivially_copyable_v<Option<uint32_t>>);

template <typename T>
struct Result {
    static_assert(std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>,
                  "COM::Result<T> carries T by value across the boundary; T must be a fixed-layout value.");

    T         value {};
    ErrorCode error {};
    bool      ok = true;

    constexpr Result() noexcept = default;
    constexpr Result(T val) noexcept: value(val), ok(true) {
    }
    constexpr Result(ErrorCode err) noexcept: error(err), ok(false) {
    }

    template <typename E>
        requires (std::is_enum_v<E> && !std::is_same_v<E, T>)
    constexpr Result(E err) noexcept: error(ErrorCode(err)), ok(false) {
    }

    [[nodiscard]] constexpr bool HasValue() const noexcept {
        return ok;
    }

    [[nodiscard]] constexpr operator std::expected<T, ErrorCode>() const {
        if (ok) return value;
        return std::unexpected(error);
    }
};

static_assert(std::is_standard_layout_v<Result<uint32_t>> && std::is_trivially_copyable_v<Result<uint32_t>>);

template <>
struct Result<void> {
    ErrorCode error {};
    bool      ok = true;

    constexpr Result() noexcept = default;
    constexpr Result(ErrorCode err) noexcept: error(err), ok(false) {
    }

    template <typename E>
        requires std::is_enum_v<E>
    constexpr Result(E err) noexcept: error(ErrorCode(err)), ok(false) {
    }

    [[nodiscard]] constexpr bool HasValue() const noexcept {
        return ok;
    }

    [[nodiscard]] constexpr operator std::expected<void, ErrorCode>() const {
        if (ok) return {};
        return std::unexpected(error);
    }
};

static_assert(std::is_standard_layout_v<Result<void>> && std::is_trivially_copyable_v<Result<void>>);

} // namespace ZHLN::COM
