#pragma once

// BatchResult<T, E>: the outcome of a batch operation, split into successes and errors.
//
// Design notes
//  * An empty batch is a complete success (by design). operator bool and
//    IsCompleteSuccess() agree with that, like std::expected's operator bool.
//  * Original interleaving of successes/errors is NOT preserved.
//  * Rvalue overloads move elements; lvalue overloads copy them.

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <expected>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN {

namespace TemplatedDetail {

template <class>
inline constexpr bool is_expected_v = false;
template <class T, class E>
inline constexpr bool is_expected_v<std::expected<T, E>> = true;

template <class>
inline constexpr bool is_optional_v = false;
template <class T>
inline constexpr bool is_optional_v<std::optional<T>> = true;

// Type of `std::forward_like<Self>(std::declval<X&>())`:
//   Self = const B&  -> const X&,  Self = B&  -> X&,  Self = B -> X&&
template <class Self, class X>
using like_t = decltype(std::forward_like<Self>(std::declval<X&>()));

// lvalue range -> view that copies elements; rvalue range -> view that moves elements.
// (Moving a container does not move its elements: append_range(std::move(vec)) still copies.)
template <class R>
constexpr auto ForwardElements(R&& r) {
    if constexpr (std::is_lvalue_reference_v<R>) {
        return std::views::all(r);
    } else {
        return std::views::as_rvalue(std::forward<R>(r));
    }
}

// Reference type produced by ForwardElements(R) (T& for lvalue ranges, T&& for rvalue ranges).
// Constraints use this so that move-only element types work with rvalue ranges.
template <class R>
using forwarded_ref_t = std::ranges::range_reference_t<decltype(ForwardElements(std::declval<R>()))>;

// vector::append_range where available (C++23 library), otherwise reserve + copy.
template <class V, class R>
constexpr void AppendRange(V& v, R&& r) {
    if constexpr (requires { v.append_range(std::forward<R>(r)); }) {
        v.append_range(std::forward<R>(r));
    } else {
        if constexpr (std::ranges::sized_range<R>) {
            v.reserve(v.size() + static_cast<std::size_t>(std::ranges::size(r)));
        }
        std::ranges::copy(std::forward<R>(r), std::back_inserter(v));
    }
}

} // namespace TemplatedDetail

/// Batch outcome. Empty batch is complete success (by design).
template <std::movable T, std::movable E>
struct BatchResult {
    using success_type = T;
    using error_type   = E;

    std::vector<T> successes;
    std::vector<E> errors;

    struct Capacity {
        std::size_t successes = 0;
        std::size_t errors    = 0;
    };

    // ── Construction ────────────────────────────────────────────────

    BatchResult() = default;

    /// From two ranges of successes and errors. Rvalue ranges are moved from.
    template <std::ranges::input_range R1, std::ranges::input_range R2>
        requires std::convertible_to<TemplatedDetail::forwarded_ref_t<R1>, T> && std::convertible_to<TemplatedDetail::forwarded_ref_t<R2>, E>
    explicit BatchResult(R1&& succ, R2&& err) {
        AddSuccesses(std::forward<R1>(succ));
        AddErrors(std::forward<R2>(err));
    }

    /// From a range of std::expected<T, E>. Rvalue ranges are moved from.
    /// Also enables:  results | std::ranges::to<BatchResult<T, E>>()
    template <std::ranges::input_range R>
        requires std::same_as<std::remove_cvref_t<std::ranges::range_reference_t<R>>, std::expected<T, E>>
    explicit BatchResult(R&& items) {
        // Only reserve when the size is known; never consume a single-pass range just to count it.
        if constexpr (std::ranges::sized_range<R>) {
            const auto n = static_cast<std::size_t>(std::ranges::size(items));
            successes.reserve(n);
            errors.reserve(n / 4); // heuristic: most items succeed
        }
        for (auto&& item: TemplatedDetail::ForwardElements(std::forward<R>(items))) {
            Add(std::forward<decltype(item)>(item));
        }
    }

    // ── Building ────────────────────────────────────────────────────

    constexpr void AddSuccess(T value) {
        successes.push_back(std::move(value));
    }

    constexpr void AddError(E error) {
        errors.push_back(std::move(error));
    }

    template <typename... Args>
        requires std::constructible_from<T, Args...>
    constexpr void EmplaceSuccess(Args&&... args) {
        successes.emplace_back(std::forward<Args>(args)...);
    }

    template <typename... Args>
        requires std::constructible_from<E, Args...>
    constexpr void EmplaceError(Args&&... args) {
        errors.emplace_back(std::forward<Args>(args)...);
    }

    // bulk (rvalue ranges are moved from)
    template <std::ranges::input_range R>
        requires std::convertible_to<TemplatedDetail::forwarded_ref_t<R>, T>
    constexpr void AddSuccesses(R&& r) {
        TemplatedDetail::AppendRange(successes, TemplatedDetail::ForwardElements(std::forward<R>(r)));
    }

    template <std::ranges::input_range R>
        requires std::convertible_to<TemplatedDetail::forwarded_ref_t<R>, E>
    constexpr void AddErrors(R&& r) {
        TemplatedDetail::AppendRange(errors, TemplatedDetail::ForwardElements(std::forward<R>(r)));
    }

    // ── std::expected / std::optional in ────────────────────────────

    /// expected<T,E> -> success or error, value-category aware (rvalue moves).
    template <typename X>
        requires std::same_as<std::remove_cvref_t<X>, std::expected<T, E>>
    constexpr void Add(X&& result) {
        if (result) {
            successes.push_back(*std::forward<X>(result));
        } else {
            errors.push_back(std::forward<X>(result).error());
        }
    }

    /// std::unexpected -> error.
    template <typename G>
        requires std::constructible_from<E, G>
    constexpr void Add(std::unexpected<G> u) {
        errors.emplace_back(std::move(u).error());
    }

    /// optional<T> -> success if engaged. Returns whether it was added.
    constexpr bool AddSuccessIfPresent(std::optional<T> v) {
        if (!v) {
            return false;
        }
        successes.push_back(std::move(*v));
        return true;
    }

    /// optional<E> -> error if engaged. Returns whether it was added.
    constexpr bool AddErrorIfPresent(std::optional<E> e) {
        if (!e) {
            return false;
        }
        errors.push_back(std::move(*e));
        return true;
    }

    /// optional<T> -> success if engaged, otherwise the given error.
    template <typename G>
        requires std::constructible_from<E, G>
    constexpr void AddOrError(std::optional<T> v, G&& err) {
        if (v) {
            successes.push_back(std::move(*v));
        } else {
            errors.emplace_back(std::forward<G>(err));
        }
    }

    // ── Capacity ────────────────────────────────────────────────────

    constexpr void Reserve(Capacity cap) {
        successes.reserve(cap.successes);
        errors.reserve(cap.errors);
    }

    constexpr void Reserve(std::integral auto s, std::integral auto e = 0) {
        successes.reserve(static_cast<std::size_t>(s));
        errors.reserve(static_cast<std::size_t>(e));
    }

    // ── Merge ───────────────────────────────────────────────────────

    /// Consuming merge: elements are moved, `other` is left empty.
    constexpr void Merge(BatchResult&& other) {
        if (this == &other) {
            return;
        }
        AddSuccesses(std::move(other.successes));
        AddErrors(std::move(other.errors));
        other.Clear();
    }

    /// Non-consuming merge: elements are copied. Self-merge appends a copy of itself.
    constexpr void Merge(const BatchResult& other) {
        if (this == &other) {
            BatchResult copy = other;
            Merge(std::move(copy));
            return;
        }
        AddSuccesses(other.successes);
        AddErrors(other.errors);
    }

    // ── Queries ─────────────────────────────────────────────────────

    [[nodiscard]] constexpr std::size_t SuccessCount() const noexcept {
        return successes.size();
    }
    [[nodiscard]] constexpr std::size_t ErrorCount() const noexcept {
        return errors.size();
    }
    [[nodiscard]] constexpr std::size_t TotalCount() const noexcept {
        return successes.size() + errors.size();
    }

    [[nodiscard]] constexpr bool HasErrors() const noexcept {
        return !errors.empty();
    }
    [[nodiscard]] constexpr bool HasSuccesses() const noexcept {
        return !successes.empty();
    }
    [[nodiscard]] constexpr bool IsEmpty() const noexcept {
        return successes.empty() && errors.empty();
    }
    [[nodiscard]] constexpr bool IsCompleteSuccess() const noexcept {
        return errors.empty();
    } // empty == success
    [[nodiscard]] constexpr bool IsPartialSuccess() const noexcept {
        return !successes.empty() && !errors.empty();
    }
    [[nodiscard]] constexpr bool IsTotalFailure() const noexcept {
        return successes.empty() && !errors.empty();
    }

    /// Mirrors std::expected: true means "no errors" (an empty batch is true).
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return IsCompleteSuccess();
    }

    /// Pointer to the first success/error, or nullptr. (Swap for optional<const T&> in C++26.)
    [[nodiscard]] constexpr const T* FirstSuccess() const noexcept {
        return successes.empty() ? nullptr : successes.data();
    }
    [[nodiscard]] constexpr const E* FirstError() const noexcept {
        return errors.empty() ? nullptr : errors.data();
    }

    // ── Views (zero-cost) ───────────────────────────────────────────

    [[nodiscard]] constexpr std::span<const T> SuccessView() const noexcept {
        return successes;
    }
    [[nodiscard]] constexpr std::span<const E> ErrorView() const noexcept {
        return errors;
    }

    // ── Functional helpers ──────────────────────────────────────────

    template <std::invocable<const T&> F>
    constexpr void ForEachSuccess(F&& f) const {
        std::ranges::for_each(successes, std::forward<F>(f));
    }

    template <std::invocable<const E&> F>
    constexpr void ForEachError(F&& f) const {
        std::ranges::for_each(errors, std::forward<F>(f));
    }

    /// Map successes -> BatchResult<U, E>. On an rvalue, elements are moved instead of copied.
    template <class Self, class F>
        requires std::invocable<F&, TemplatedDetail::like_t<Self, T>>
    [[nodiscard]] constexpr auto MapSuccesses(this Self&& self, F&& f) {
        using U = std::remove_cvref_t<std::invoke_result_t<F&, TemplatedDetail::like_t<Self, T>>>;
        BatchResult<U, E> out;
        out.successes.reserve(self.successes.size());
        for (auto& s: self.successes) {
            out.successes.push_back(std::invoke(f, std::forward_like<Self>(s)));
        }
        out.errors = std::forward_like<Self>(self.errors);
        return out;
    }

    /// Map errors -> BatchResult<T, G>.
    template <class Self, class F>
        requires std::invocable<F&, TemplatedDetail::like_t<Self, E>>
    [[nodiscard]] constexpr auto MapErrors(this Self&& self, F&& f) {
        using G = std::remove_cvref_t<std::invoke_result_t<F&, TemplatedDetail::like_t<Self, E>>>;
        BatchResult<T, G> out;
        out.errors.reserve(self.errors.size());
        for (auto& e: self.errors) {
            out.errors.push_back(std::invoke(f, std::forward_like<Self>(e)));
        }
        out.successes = std::forward_like<Self>(self.successes);
        return out;
    }

    /// Fallible step over successes: f(T) -> std::expected<U, E>.
    /// Failures of f move into `errors` (appended after the existing ones).
    template <class Self, class F>
        requires std::invocable<F&, TemplatedDetail::like_t<Self, T>> &&
                 TemplatedDetail::is_expected_v<std::remove_cvref_t<std::invoke_result_t<F&, TemplatedDetail::like_t<Self, T>>>>
    [[nodiscard]] constexpr auto AndThenSuccesses(this Self&& self, F&& f) {
        using R = std::remove_cvref_t<std::invoke_result_t<F&, TemplatedDetail::like_t<Self, T>>>;
        static_assert(std::same_as<typename R::error_type, E>, "f must return std::expected<U, E> with the same error type");
        static_assert(!std::is_void_v<typename R::value_type>, "expected<void, E> is not supported");
        BatchResult<typename R::value_type, E> out;
        out.errors = std::forward_like<Self>(self.errors);
        out.successes.reserve(self.successes.size());
        for (auto& s: self.successes) {
            out.Add(std::invoke(f, std::forward_like<Self>(s)));
        }
        return out;
    }

    /// Optional step over successes: f(T) -> std::optional<U>. nullopt drops the item.
    template <class Self, class F>
        requires std::invocable<F&, TemplatedDetail::like_t<Self, T>> &&
                 TemplatedDetail::is_optional_v<std::remove_cvref_t<std::invoke_result_t<F&, TemplatedDetail::like_t<Self, T>>>>
    [[nodiscard]] constexpr auto FilterMapSuccesses(this Self&& self, F&& f) {
        using U = typename std::remove_cvref_t<std::invoke_result_t<F&, TemplatedDetail::like_t<Self, T>>>::value_type;
        BatchResult<U, E> out;
        out.errors = std::forward_like<Self>(self.errors);
        for (auto& s: self.successes) {
            if (auto r = std::invoke(f, std::forward_like<Self>(s))) {
                out.successes.push_back(std::move(*r));
            }
        }
        return out;
    }

    // ── std::expected / std::optional out ───────────────────────────
    // All of these copy on lvalues and move on rvalues:  std::move(batch).ToExpected()

    /// All-or-nothing: expected<vector<T>, vector<E>>.
    template <class Self>
    [[nodiscard]] constexpr std::expected<std::vector<T>, std::vector<E>> ToExpected(this Self&& self) {
        if (self.errors.empty()) {
            return std::forward_like<Self>(self.successes);
        }
        return std::unexpected(std::forward_like<Self>(self.errors));
    }

    /// Fail-fast style: expected<vector<T>, E> carrying the first error.
    template <class Self>
    [[nodiscard]] constexpr std::expected<std::vector<T>, E> ToExpectedFirstError(this Self&& self) {
        if (self.errors.empty()) {
            return std::forward_like<Self>(self.successes);
        }
        return std::unexpected(std::forward_like<Self>(self.errors.front()));
    }

    /// Engaged only on complete success.
    template <class Self>
    [[nodiscard]] constexpr std::optional<std::vector<T>> ToOptional(this Self&& self) {
        if (!self.errors.empty()) {
            return std::nullopt;
        }
        return std::forward_like<Self>(self.successes);
    }

    /// Flatten back to expecteds (successes first, then errors; original order is not recoverable).
    template <class Self>
    [[nodiscard]] constexpr std::vector<std::expected<T, E>> ToExpecteds(this Self&& self) {
        std::vector<std::expected<T, E>> out;
        out.reserve(self.successes.size() + self.errors.size());
        for (auto& s: self.successes) {
            out.emplace_back(std::in_place, std::forward_like<Self>(s));
        }
        for (auto& e: self.errors) {
            out.emplace_back(std::unexpect, std::forward_like<Self>(e));
        }
        return out;
    }

    // ── Cleanup ─────────────────────────────────────────────────────

    constexpr void Clear() noexcept {
        successes.clear();
        errors.clear();
    }

    // ── Equality ────────────────────────────────────────────────────

    friend constexpr bool operator==(const BatchResult&, const BatchResult&) = default;
};
} // namespace ZHLN
