// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <optional> // std::optional and its feature-test macro, when available

#if defined(__cpp_lib_optional) && __cpp_lib_optional >= 202506L

namespace ZHLN {

template <typename T>
using Optional = std::optional<T>;

}

#else

#include <cassert>
#include <compare>
#include <concepts>
#include <cstdlib>
#include <functional>
#include <memory>
#include <ranges>
#include <type_traits>
#include <utility>

namespace ZHLN::OptionalDetail {

template <typename T>
class Reference;

template <typename T>
struct Select {
    using type = std::optional<T>;
};

template <typename T>
struct Select<T&> {
    using type = Reference<T>;
};

template <typename T>
inline constexpr bool IsOptional = false;

template <typename T>
inline constexpr bool IsOptional<std::optional<T>> = true;

template <typename T>
inline constexpr bool IsOptional<Reference<T>> = true;

// Deliberately narrower than the standard's reference-conversion rules: only
// borrow pointer-compatible lvalues. In particular, an lvalue proxy converting
// to a value must not create a temporary that a const reference could bind to.
template <typename From, typename To>
concept Borrowable = std::is_lvalue_reference_v<From> && std::is_convertible_v<std::remove_reference_t<From>*, To*>;

template <typename T>
struct IteratorBase {};

template <typename T>
    requires(std::is_object_v<T> && !std::is_unbounded_array_v<T>)
struct IteratorBase<T> {
    using iterator = T*;
};

// Construct transform's value directly inside std::optional, including a
// non-copyable, non-movable result. The conversion's prvalue is guaranteed to
// be elided; materialising the result before emplace would require a move.
template <typename F, typename T>
struct TransformResult {
    F&& function;
    T&  target;

    constexpr operator std::remove_cv_t<std::invoke_result_t<F, T&>>() && {
        return std::invoke(std::forward<F>(function), target);
    }
};

} // namespace ZHLN::OptionalDetail

namespace ZHLN {

// Like FunctionRef, use the native type when available, not an extension to
// namespace std. Value optionals always retain the standard library's type.
template <typename T>
using Optional = typename OptionalDetail::Select<T>::type;

namespace OptionalDetail {

// Borrowed optional reference; never owns its target. Keep that target alive
// and, when borrowing an owning optional, engaged for every use of this view.
// Assignment/emplace rebind; constness of the wrapper does not propagate to T.
// This fallback accepts lvalues, reference_wrapper, other borrowed optionals,
// and lvalue owning optionals, but no user-defined reference conversions.
template <typename T>
class Reference: public IteratorBase<T> {
    static_assert(!std::is_reference_v<T> && !std::is_void_v<T>);
    static_assert(!std::same_as<std::remove_cv_t<T>, std::nullopt_t> && !std::same_as<std::remove_cv_t<T>, std::in_place_t>);

  public:
    using value_type = T;

    constexpr Reference() noexcept = default;
    constexpr Reference(std::nullopt_t) noexcept {
    }
    constexpr Reference(const Reference&) noexcept                    = default;
    constexpr auto operator=(const Reference&) noexcept -> Reference& = default;

    template <typename U>
        requires Borrowable<U, T>
    constexpr Reference(U&& target) noexcept: pointer_(std::addressof(target)) {
    }

    template <typename U>
        requires Borrowable<U, T>
    constexpr explicit Reference(std::in_place_t, U&& target) noexcept: Reference(std::forward<U>(target)) {
    }

    template <typename U>
        requires std::is_convertible_v<U*, T*>
    constexpr Reference(std::reference_wrapper<U> target) noexcept: pointer_(std::addressof(target.get())) {
    }

    template <typename U>
        requires std::is_convertible_v<U*, T*>
    constexpr explicit Reference(std::in_place_t, std::reference_wrapper<U> target) noexcept: Reference(target) {
    }

    template <typename U>
        requires std::is_convertible_v<U*, T*>
    constexpr Reference(const Reference<U>& other) noexcept: pointer_(other ? std::addressof(*other) : nullptr) {
    }

    template <typename U>
        requires(!std::is_reference_v<U> && std::is_convertible_v<U*, T*>)
    constexpr Reference(std::optional<U>& other) noexcept: pointer_(other ? std::addressof(*other) : nullptr) {
    }

    template <typename U>
        requires(!std::is_reference_v<U> && std::is_convertible_v<const U*, T*>)
    constexpr Reference(const std::optional<U>& other) noexcept: pointer_(other ? std::addressof(*other) : nullptr) {
    }

    // A const-lvalue overload alone would also accept a temporary owner.
    template <typename U>
    Reference(std::optional<U>&&) = delete;
    template <typename U>
    Reference(const std::optional<U>&&) = delete;

    constexpr auto operator=(std::nullopt_t) noexcept -> Reference& {
        reset();
        return *this;
    }

    template <typename U>
        requires Borrowable<U, T>
    constexpr auto emplace(U&& target) noexcept -> T& {
        pointer_ = std::addressof(target);
        return *pointer_;
    }

    template <typename U>
        requires std::is_convertible_v<U*, T*>
    constexpr auto emplace(std::reference_wrapper<U> target) noexcept -> T& {
        pointer_ = std::addressof(target.get());
        return *pointer_;
    }

    constexpr void swap(Reference& other) noexcept {
        std::swap(pointer_, other.pointer_);
    }

    friend constexpr void swap(Reference& left, Reference& right) noexcept {
        left.swap(right);
    }

    [[nodiscard]] constexpr auto begin() const noexcept -> T*
        requires(std::is_object_v<T> && !std::is_unbounded_array_v<T>)
    {
        return pointer_;
    }

    [[nodiscard]] constexpr auto end() const noexcept -> T*
        requires(std::is_object_v<T> && !std::is_unbounded_array_v<T>)
    {
        // Do not perform pointer arithmetic on a disengaged null pointer.
        return pointer_ ? pointer_ + 1 : pointer_;
    }

    [[nodiscard]] constexpr auto operator->() const noexcept -> T* {
        assert(pointer_ != nullptr);
        return pointer_;
    }

    [[nodiscard]] constexpr auto operator*() const noexcept -> T& {
        assert(pointer_ != nullptr);
        return *pointer_;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] constexpr auto has_value() const noexcept -> bool {
        return pointer_ != nullptr;
    }

    [[nodiscard]] constexpr auto value() const -> T& {
        if (!pointer_) {
#if defined(__cpp_exceptions)
            throw std::bad_optional_access {};
#else
            // Match the project's -fno-exceptions builds without depending on
            // a library-private throw helper or pulling in engine/reflection.
            std::abort();
#endif
        }
        return *pointer_;
    }

    template <typename U = std::remove_cv_t<T>>
        requires(std::is_copy_constructible_v<std::remove_cv_t<T>> && std::is_convertible_v<U&&, std::remove_cv_t<T>>)
    [[nodiscard]] constexpr auto value_or(U&& fallback) const {
        using Value = std::remove_cv_t<T>;
        if (pointer_) {
            return Value(*pointer_);
        }
        return static_cast<Value>(std::forward<U>(fallback));
    }

    template <typename F>
    constexpr auto and_then(F&& function) const {
        using Result = std::remove_cvref_t<std::invoke_result_t<F, T&>>;
        static_assert(IsOptional<Result>, "Optional<T&>::and_then must return an Optional");
        if (pointer_) {
            return std::invoke(std::forward<F>(function), *pointer_);
        }
        return Result {};
    }

    template <typename F>
    constexpr auto transform(F&& function) const {
        using Result = std::remove_cv_t<std::invoke_result_t<F, T&>>;
        if (pointer_) {
            if constexpr (std::is_reference_v<Result>) {
                return Optional<Result> {std::invoke(std::forward<F>(function), *pointer_)};
            } else {
                return Optional<Result> {std::in_place, TransformResult<F, T> {std::forward<F>(function), *pointer_}};
            }
        }
        return Optional<Result> {};
    }

    template <typename F>
        requires std::invocable<F>
    constexpr auto or_else(F&& function) const -> Reference {
        static_assert(std::same_as<std::remove_cvref_t<std::invoke_result_t<F>>, Reference>, "Optional<T&>::or_else must return the same Optional type");
        if (pointer_) {
            return *this;
        }
        return std::invoke(std::forward<F>(function));
    }

    constexpr void reset() noexcept {
        pointer_ = nullptr;
    }

    template <typename U>
        requires requires(const T& left, const U& right) {
            { left == right } -> std::convertible_to<bool>;
        }
    [[nodiscard]] constexpr auto operator==(const Reference<U>& other) const -> bool {
        return has_value() == other.has_value() && (!pointer_ || *pointer_ == *other);
    }

    template <typename U>
        requires std::three_way_comparable_with<T, U>
    [[nodiscard]] constexpr auto operator<=>(const Reference<U>& other) const -> std::compare_three_way_result_t<T, U> {
        if (pointer_ && other) {
            return *pointer_ <=> *other;
        }
        return has_value() <=> other.has_value();
    }

    [[nodiscard]] constexpr auto operator==(std::nullopt_t) const noexcept -> bool {
        return !pointer_;
    }

    [[nodiscard]] constexpr auto operator<=>(std::nullopt_t) const noexcept -> std::strong_ordering {
        return has_value() <=> false;
    }

    template <typename U>
        requires requires(const T& left, const U& right) {
            { left == right } -> std::convertible_to<bool>;
        }
    [[nodiscard]] constexpr auto operator==(const std::optional<U>& other) const -> bool {
        return has_value() == other.has_value() && (!pointer_ || *pointer_ == *other);
    }

    template <typename U>
        requires std::three_way_comparable_with<T, U>
    [[nodiscard]] constexpr auto operator<=>(const std::optional<U>& other) const -> std::compare_three_way_result_t<T, U> {
        if (pointer_ && other) {
            return *pointer_ <=> *other;
        }
        return has_value() <=> other.has_value();
    }

    // Older libstdc++'s optional-vs-value operators do not recognise our
    // user-defined reference wrapper. Explicit mixed overloads beat those
    // legacy candidates and preserve disengaged == disengaged in both orders.
    template <typename U>
        requires requires(const Reference& ref, const std::optional<U>& owner) { ref.operator==(owner); }
    friend constexpr auto operator==(const std::optional<U>& owner, const Reference& ref) -> bool {
        return ref.operator==(owner);
    }

    template <typename U>
        requires requires(const Reference& ref, const std::optional<U>& owner) { ref.operator==(owner); }
    friend constexpr auto operator!=(const Reference& ref, const std::optional<U>& owner) -> bool {
        return !ref.operator==(owner);
    }

    template <typename U>
        requires requires(const Reference& ref, const std::optional<U>& owner) { ref.operator==(owner); }
    friend constexpr auto operator!=(const std::optional<U>& owner, const Reference& ref) -> bool {
        return !ref.operator==(owner);
    }

    template <typename U>
        requires std::three_way_comparable_with<T, U>
    friend constexpr auto operator<(const Reference& ref, const std::optional<U>& owner) -> bool {
        return ref.operator<=>(owner) < 0;
    }

    template <typename U>
        requires std::three_way_comparable_with<T, U>
    friend constexpr auto operator<(const std::optional<U>& owner, const Reference& ref) -> bool {
        return ref.operator<=>(owner) > 0;
    }

    template <typename U>
        requires std::three_way_comparable_with<T, U>
    friend constexpr auto operator>(const Reference& ref, const std::optional<U>& owner) -> bool {
        return ref.operator<=>(owner) > 0;
    }

    template <typename U>
        requires std::three_way_comparable_with<T, U>
    friend constexpr auto operator>(const std::optional<U>& owner, const Reference& ref) -> bool {
        return ref.operator<=>(owner) < 0;
    }

    template <typename U>
        requires std::three_way_comparable_with<T, U>
    friend constexpr auto operator<=(const Reference& ref, const std::optional<U>& owner) -> bool {
        return ref.operator<=>(owner) <= 0;
    }

    template <typename U>
        requires std::three_way_comparable_with<T, U>
    friend constexpr auto operator<=(const std::optional<U>& owner, const Reference& ref) -> bool {
        return ref.operator<=>(owner) >= 0;
    }

    template <typename U>
        requires std::three_way_comparable_with<T, U>
    friend constexpr auto operator>=(const Reference& ref, const std::optional<U>& owner) -> bool {
        return ref.operator<=>(owner) >= 0;
    }

    template <typename U>
        requires std::three_way_comparable_with<T, U>
    friend constexpr auto operator>=(const std::optional<U>& owner, const Reference& ref) -> bool {
        return ref.operator<=>(owner) <= 0;
    }

    template <typename U>
        requires(
            !IsOptional<std::remove_cvref_t<U>> && !std::same_as<std::remove_cvref_t<U>, std::nullopt_t> &&
            requires(const T& left, const U& right) {
                { left == right } -> std::convertible_to<bool>;
            }
        )
    [[nodiscard]] constexpr auto operator==(const U& other) const -> bool {
        return pointer_ && *pointer_ == other;
    }

    template <typename U>
        requires(!IsOptional<std::remove_cvref_t<U>> && !std::same_as<std::remove_cvref_t<U>, std::nullopt_t> && std::three_way_comparable_with<T, U>)
    [[nodiscard]] constexpr auto operator<=>(const U& other) const -> std::compare_three_way_result_t<T, U> {
        if (pointer_) {
            return *pointer_ <=> other;
        }
        return std::strong_ordering::less;
    }

  private:
    T* pointer_ = nullptr;
};

} // namespace OptionalDetail

} // namespace ZHLN

// These are the standard's permitted customisations for a user-defined type,
// not a replacement/specialisation of std::optional or a forged feature macro.
namespace std::ranges {

template <typename T>
inline constexpr bool enable_view<ZHLN::OptionalDetail::Reference<T>> = true;

template <typename T>
inline constexpr bool enable_borrowed_range<ZHLN::OptionalDetail::Reference<T>> = true;

} // namespace std::ranges

#endif
