// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// The reference compatibility contract, exercised through the same grouped
// suite/expectation API as the other core tests. Helpers and fixtures stay local.
#include "TestsFramework.hpp"
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>

namespace {

using ZHLN::Optional;
using Ref = Optional<int&>;

struct Base {
    int value = 1;
};
struct Derived: Base {};
struct ValueProxy {
    operator int() const {
        return 42;
    }
};
struct Unhashable {};
struct Immovable {
    int value;
    explicit Immovable(int initial): value(initial) {
    }
    Immovable(const Immovable&) = delete;
    Immovable(Immovable&&)      = delete;
};
struct AddressTrap {
    int  value = 1;
    auto operator&() -> AddressTrap* {
        return nullptr;
    }
};
struct NonAssignable {
    int  value                                             = 1;
    auto operator=(const NonAssignable&) -> NonAssignable& = delete;
};

using Function       = int(int);
using BoundedArray   = int[2];
using UnboundedArray = int[];

int Increment(int value) {
    return value + 1;
}

template <typename O>
concept HasBegin = requires(O& optional) { optional.begin(); };

template <typename O>
concept HasValueOr = requires(const O& optional) { optional.value_or(0); };

template <typename O, typename U>
concept CanEmplace = requires(O& optional, U&& value) { optional.emplace(std::forward<U>(value)); };

static_assert(std::same_as<Optional<int>, std::optional<int>>);
static_assert(std::same_as<typename Ref::value_type, int>);
static_assert(std::same_as<decltype(*std::declval<const Ref&>()), int&>);
static_assert(std::same_as<decltype(std::declval<const Ref&>().value()), int&>);
static_assert(std::same_as<decltype(*std::declval<Ref&&>()), int&>);
static_assert(std::same_as<decltype(*std::declval<const Optional<const int&>&>()), const int&>);
static_assert(std::is_nothrow_copy_constructible_v<Ref> && std::is_nothrow_copy_assignable_v<Ref>);
static_assert(std::is_constructible_v<Ref, int&>);
static_assert(!std::is_constructible_v<Ref, const int&>);
static_assert(std::is_constructible_v<Optional<const int&>, const int&>);
static_assert(!std::is_constructible_v<Ref, int&&>);
static_assert(!std::is_constructible_v<Optional<const int&>, int&&>);
static_assert(!std::is_constructible_v<Optional<const int&>, const int&&>);
static_assert(!std::is_constructible_v<Optional<const int&>, ValueProxy&>);
static_assert(!std::is_constructible_v<Optional<const int&>, ValueProxy&&>);
static_assert(std::is_constructible_v<Ref, std::reference_wrapper<int>>);
static_assert(std::is_constructible_v<Optional<Base&>, Derived&>);
static_assert(!std::is_constructible_v<Optional<Derived&>, Base&>);
static_assert(std::is_constructible_v<Optional<const int&>, std::optional<int>&>);
static_assert(std::is_constructible_v<Optional<const int&>, const std::optional<int>&>);
static_assert(!std::is_constructible_v<Optional<const int&>, std::optional<int>&&>);
static_assert(!std::is_constructible_v<Optional<const int&>, const std::optional<int>&&>);
static_assert(!std::is_assignable_v<Ref&, int&&>);
static_assert(!std::is_assignable_v<Optional<const int&>&, int&&>);
static_assert(CanEmplace<Ref, int&>);
static_assert(!CanEmplace<Optional<const int&>, int&&>);
static_assert(!CanEmplace<Optional<const int&>, ValueProxy&>);
static_assert(std::same_as<decltype(std::declval<Ref&>().emplace(std::declval<int&>())), int&>);
static_assert(std::same_as<decltype(std::declval<const Ref&>().value_or(0)), int>);
static_assert(std::ranges::view<Ref> && std::ranges::contiguous_range<Ref> && std::ranges::borrowed_range<Ref>);
static_assert(std::same_as<std::ranges::range_reference_t<const Ref>, int&>);
static_assert(!HasBegin<Optional<Function&>> && !HasValueOr<Optional<Function&>>);
static_assert(HasBegin<Optional<BoundedArray&>> && !HasValueOr<Optional<BoundedArray&>>);
static_assert(!HasBegin<Optional<UnboundedArray&>> && !HasValueOr<Optional<UnboundedArray&>>);
static_assert(!std::is_default_constructible_v<std::hash<Optional<Unhashable&>>>);

#if defined(__cpp_lib_optional) && __cpp_lib_optional >= 202506L
static_assert(std::same_as<Ref, std::optional<int&>>);
#else
static_assert(std::is_trivially_copyable_v<Ref>);
static_assert(sizeof(Ref) == sizeof(int*));
#endif

constexpr auto ConstexprSemantics() -> bool {
    int a = 10;
    int b = 20;
    Ref ref;
    if (ref || ref.begin() != ref.end()) {
        return false;
    }
    ref            = a;
    *ref           = 11;
    const Ref copy = ref;
    *copy          = 12;
    ref            = b;
    if (a != 12 || b != 20 || std::addressof(*ref) != std::addressof(b)) {
        return false;
    }
    ref.emplace(a);
    if (std::addressof(*ref) != std::addressof(a)) {
        return false;
    }
    ref = std::nullopt;
    if (ref.value_or(99) != 99) {
        return false;
    }
    ref.emplace(b);
    auto mapped = ref.transform([](int& value) { return value + 1; });
    auto alias  = ref.transform([](int& value) -> int& { return value; });
    if (!mapped || *mapped != 21 || std::addressof(*alias) != std::addressof(b)) {
        return false;
    }
    auto chained = ref.and_then([](int& value) -> Ref { return value; });
    if (std::addressof(*chained) != std::addressof(b)) {
        return false;
    }
    ref.reset();
    auto recovered = ref.or_else([&]() -> Ref { return a; });
    return !ref && std::addressof(*recovered) == std::addressof(a) && a == 12 && b == 20;
}

static_assert(ConstexprSemantics());

enum class OptionalTestError : uint8_t {
    RebindingFailed = 1,
    ConstAccessFailed,
    ConversionFailed,
    MonadicFailed,
    IterationFailed,
    ComparisonFailed,
    MissingException,
};

struct OptionalTestSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> empty_and_constexpr_reference_contract() {
            Ref empty;
            if (!ZHLN::Test::ExpectTrue(ConstexprSemantics()) || !ZHLN::Test::ExpectFalse(empty.has_value()) ||
                !ZHLN::Test::ExpectEq(empty.begin(), empty.end()) || !ZHLN::Test::ExpectEq(empty.value_or(99), 99)) {
                return std::unexpected(OptionalTestError::RebindingFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> assignment_copy_move_and_emplace_rebind() {
            int firstValue  = 1;
            int secondValue = 2;
            Ref first       = firstValue;
            Ref second      = secondValue;
            first           = second;
            if (!ZHLN::Test::ExpectEq(std::addressof(*first), std::addressof(secondValue)) || !ZHLN::Test::ExpectEq(firstValue, 1) ||
                !ZHLN::Test::ExpectEq(secondValue, 2)) {
                return std::unexpected(OptionalTestError::RebindingFailed);
            }
            Ref& self = first;
            first     = self;
            if (!ZHLN::Test::ExpectEq(std::addressof(*first), std::addressof(secondValue))) {
                return std::unexpected(OptionalTestError::RebindingFailed);
            }
            Ref moved = std::move(first);
            if (!ZHLN::Test::ExpectTrue(first.has_value()) || !ZHLN::Test::ExpectTrue(moved.has_value()) ||
                !ZHLN::Test::ExpectEq(std::addressof(*first), std::addressof(*moved)) ||
                !ZHLN::Test::ExpectEq(std::addressof(moved.emplace(firstValue)), std::addressof(firstValue))) {
                return std::unexpected(OptionalTestError::RebindingFailed);
            }
            NonAssignable            left;
            NonAssignable            right;
            Optional<NonAssignable&> nonassignable = left;
            nonassignable                          = right;
            if (!ZHLN::Test::ExpectEq(std::addressof(*nonassignable), std::addressof(right))) {
                return std::unexpected(OptionalTestError::RebindingFailed);
            }
            AddressTrap            target;
            Optional<AddressTrap&> trapped = target;
            if (!ZHLN::Test::ExpectEq(trapped.operator->(), std::addressof(target)) || !ZHLN::Test::ExpectEq(trapped->value, 1)) {
                return std::unexpected(OptionalTestError::RebindingFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> swap_and_reset_only_change_bindings() {
            int firstValue  = 1;
            int secondValue = 2;
            Ref first       = firstValue;
            Ref second      = secondValue;
            using std::swap;
            swap(first, second);
            if (!ZHLN::Test::ExpectEq(std::addressof(*first), std::addressof(secondValue)) ||
                !ZHLN::Test::ExpectEq(std::addressof(*second), std::addressof(firstValue)) || !ZHLN::Test::ExpectEq(firstValue, 1) ||
                !ZHLN::Test::ExpectEq(secondValue, 2)) {
                return std::unexpected(OptionalTestError::RebindingFailed);
            }
            second = std::nullopt;
            if (!ZHLN::Test::ExpectFalse(second.has_value()) || !ZHLN::Test::ExpectEq(firstValue, 1)) {
                return std::unexpected(OptionalTestError::RebindingFailed);
            }
            swap(first, second);
            if (!ZHLN::Test::ExpectFalse(first.has_value()) || !ZHLN::Test::ExpectTrue(second.has_value()) ||
                !ZHLN::Test::ExpectEq(std::addressof(*second), std::addressof(secondValue))) {
                return std::unexpected(OptionalTestError::RebindingFailed);
            }
            second.reset();
            if (!ZHLN::Test::ExpectFalse(second.has_value()) || !ZHLN::Test::ExpectEq(secondValue, 2)) {
                return std::unexpected(OptionalTestError::RebindingFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> const_wrapper_preserves_mutable_target_and_value_or_copies() {
            int       value               = 3;
            const Ref wrapper             = value;
            *wrapper                      = 7;
            Optional<const int&> readonly = wrapper;
            if (!ZHLN::Test::ExpectEq(value, 7) || !ZHLN::Test::ExpectEq(std::addressof(readonly.value()), std::addressof(value))) {
                return std::unexpected(OptionalTestError::ConstAccessFailed);
            }
            auto copy = readonly.value_or(0);
            copy      = 8;
            if (!ZHLN::Test::ExpectEq(copy, 8) || !ZHLN::Test::ExpectEq(value, 7)) {
                return std::unexpected(OptionalTestError::ConstAccessFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> reference_wrappers_and_derived_views_borrow_targets() {
            int                  value   = 4;
            Ref                  wrapped = std::ref(value);
            Optional<const int&> constWrapped(std::in_place, std::cref(value));
            if (!ZHLN::Test::ExpectEq(std::addressof(*wrapped), std::addressof(value)) ||
                !ZHLN::Test::ExpectEq(std::addressof(*constWrapped), std::addressof(value)) ||
                !ZHLN::Test::ExpectEq(std::addressof(wrapped.emplace(std::ref(value))), std::addressof(value))) {
                return std::unexpected(OptionalTestError::ConversionFailed);
            }
            Derived               derived;
            Optional<Derived&>    child         = derived;
            Optional<Base&>       parent        = child;
            Optional<const Base&> temporaryView = Optional<Derived&> {derived};
            if (!ZHLN::Test::ExpectEq(std::addressof(*parent), static_cast<Base*>(std::addressof(derived))) || !ZHLN::Test::ExpectEq(temporaryView->value, 1)) {
                return std::unexpected(OptionalTestError::ConversionFailed);
            }
            Optional<const int&> emptyView = Ref {};
            if (!ZHLN::Test::ExpectFalse(emptyView.has_value())) {
                return std::unexpected(OptionalTestError::ConversionFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> owning_optional_lvalues_can_be_borrowed() {
            std::optional<Derived> owner(std::in_place);
            Optional<Base&>        borrowed = owner;
            borrowed->value                 = 9;
            if (!ZHLN::Test::ExpectEq(owner->value, 9)) {
                return std::unexpected(OptionalTestError::ConversionFailed);
            }
            const std::optional<int> constOwner         = 5;
            Optional<const int&>     borrowedConstOwner = constOwner;
            if (!ZHLN::Test::ExpectEq(std::addressof(*borrowedConstOwner), std::addressof(*constOwner))) {
                return std::unexpected(OptionalTestError::ConversionFailed);
            }
            std::optional<int> emptyOwner;
            Ref                emptyBorrow = emptyOwner;
            if (!ZHLN::Test::ExpectFalse(emptyBorrow.has_value())) {
                return std::unexpected(OptionalTestError::ConversionFailed);
            }
            owner.reset(); // The borrowed reference must not be used after this.
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> transform_preserves_references_and_supports_immovable_results() {
            int       value = 3;
            const Ref ref   = value;
            auto      alias = ref.transform([](int& input) -> int& { return input; });
            if (!ZHLN::Test::ExpectEq(std::addressof(*alias), std::addressof(value))) {
                return std::unexpected(OptionalTestError::MonadicFailed);
            }
            *alias         = 8;
            auto immutable = ref.transform([](int& input) -> const int& { return input; });
            static_assert(std::same_as<decltype(immutable), Optional<const int&>>);
            auto immovable = ref.transform([](int& input) { return Immovable(input); });
            auto moveOnly  = ref.transform([owned = std::make_unique<int>(2)](int& input) { return input + *owned; });
            if (!ZHLN::Test::ExpectEq(value, 8) || !ZHLN::Test::ExpectEq(std::addressof(*immutable), std::addressof(value)) ||
                !ZHLN::Test::ExpectEq(immovable->value, 8) || !ZHLN::Test::ExpectEq(*moveOnly, 10)) {
                return std::unexpected(OptionalTestError::MonadicFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> monadic_callbacks_short_circuit_on_engagement() {
            int       value = 3;
            const Ref ref   = value;
            Ref       empty;
            int       calls     = 0;
            auto      increment = [&](int& input) {
                ++calls;
                return input + 1;
            };
            if (!ZHLN::Test::ExpectEq(*ref.transform(increment), 4) || !ZHLN::Test::ExpectEq(calls, 1) ||
                !ZHLN::Test::ExpectFalse(empty.transform(increment).has_value()) || !ZHLN::Test::ExpectEq(calls, 1)) {
                return std::unexpected(OptionalTestError::MonadicFailed);
            }
            auto chain = [&](int& input) -> Ref {
                ++calls;
                return input;
            };
            if (!ZHLN::Test::ExpectEq(std::addressof(*ref.and_then(chain)), std::addressof(value)) || !ZHLN::Test::ExpectEq(calls, 2) ||
                !ZHLN::Test::ExpectFalse(empty.and_then(chain).has_value()) || !ZHLN::Test::ExpectEq(calls, 2) ||
                !ZHLN::Test::ExpectEq(*ref.and_then([](int& input) { return std::optional<long> {input}; }), 3L)) {
                return std::unexpected(OptionalTestError::MonadicFailed);
            }
            auto recover = [&]() -> Ref {
                ++calls;
                return value;
            };
            if (!ZHLN::Test::ExpectEq(std::addressof(*ref.or_else(recover)), std::addressof(value)) || !ZHLN::Test::ExpectEq(calls, 2) ||
                !ZHLN::Test::ExpectEq(std::addressof(*empty.or_else(recover)), std::addressof(value)) || !ZHLN::Test::ExpectEq(calls, 3)) {
                return std::unexpected(OptionalTestError::MonadicFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> engaged_and_empty_views_iterate_at_most_once() {
            int       value  = 5;
            const Ref ref    = value;
            int       visits = 0;
            for (int& item: ref) {
                ++visits;
                item += 1;
            }
            if (!ZHLN::Test::ExpectEq(visits, 1) || !ZHLN::Test::ExpectEq(value, 6) || !ZHLN::Test::ExpectEq(std::ranges::distance(ref), 1)) {
                return std::unexpected(OptionalTestError::IterationFailed);
            }
            Ref empty;
            for ([[maybe_unused]] int& item: empty) {
                ++visits;
            }
            if (!ZHLN::Test::ExpectEq(visits, 1) || !ZHLN::Test::ExpectEq(empty.begin(), empty.end())) {
                return std::unexpected(OptionalTestError::IterationFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> function_and_array_references_need_no_returnable_value() {
            Optional<Function&> function = Increment;
            if (!ZHLN::Test::ExpectEq((*function)(4), 5) || !ZHLN::Test::ExpectEq(function.operator->(), std::addressof(Increment))) {
                return std::unexpected(OptionalTestError::ConversionFailed);
            }
            BoundedArray            array       = {1, 2};
            Optional<BoundedArray&> bounded     = array;
            (*bounded)[1]                       = 3;
            Optional<UnboundedArray&> unbounded = array;
            if (!ZHLN::Test::ExpectEq(array[1], 3) || !ZHLN::Test::ExpectEq(std::addressof(*bounded), std::addressof(array)) ||
                !ZHLN::Test::ExpectEq(bounded.end(), bounded.begin() + 1) || !ZHLN::Test::ExpectEq((*unbounded)[0], 1)) {
                return std::unexpected(OptionalTestError::ConversionFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> comparisons_follow_optional_value_semantics() {
            int firstValue   = 2;
            int sameValue    = 2;
            int greaterValue = 3;
            Ref first        = firstValue;
            Ref equal        = sameValue;
            Ref greater      = greaterValue;
            Ref empty;
            // Exercise the optional operators themselves, including mixed
            // std::optional overloads that must not treat a view as a value.
            if (!ZHLN::Test::ExpectTrue(first == equal) || !ZHLN::Test::ExpectTrue(first != greater) || !ZHLN::Test::ExpectTrue(empty == Ref {}) ||
                !ZHLN::Test::ExpectTrue(empty == std::nullopt) || !ZHLN::Test::ExpectTrue(std::nullopt == empty) ||
                !ZHLN::Test::ExpectTrue(first != std::nullopt) || !ZHLN::Test::ExpectTrue(std::nullopt != first) || !ZHLN::Test::ExpectTrue(empty < first) ||
                !ZHLN::Test::ExpectTrue(empty <= first) || !ZHLN::Test::ExpectTrue(first > empty) || !ZHLN::Test::ExpectTrue(first >= empty) ||
                !ZHLN::Test::ExpectTrue(std::nullopt < first) || !ZHLN::Test::ExpectTrue(first > std::nullopt) || !ZHLN::Test::ExpectTrue(first < greater) ||
                !ZHLN::Test::ExpectTrue(first <= equal) || !ZHLN::Test::ExpectTrue(greater >= first) || !ZHLN::Test::ExpectTrue(first == 2) ||
                !ZHLN::Test::ExpectTrue(2 == first) || !ZHLN::Test::ExpectTrue(first != 3) || !ZHLN::Test::ExpectTrue(3 != first) ||
                !ZHLN::Test::ExpectTrue(first < 3) || !ZHLN::Test::ExpectTrue(3 > first) || !ZHLN::Test::ExpectTrue(empty < 0) ||
                !ZHLN::Test::ExpectTrue(0 > empty) || !ZHLN::Test::ExpectTrue(first == std::optional<int> {2}) ||
                !ZHLN::Test::ExpectTrue(std::optional<int> {2} == first) || !ZHLN::Test::ExpectTrue(empty == std::optional<int> {}) ||
                !ZHLN::Test::ExpectTrue(std::optional<int> {} == empty) || !ZHLN::Test::ExpectFalse(empty != std::optional<int> {}) ||
                !ZHLN::Test::ExpectFalse(std::optional<int> {} != empty) || !ZHLN::Test::ExpectTrue(first > std::optional<int> {}) ||
                !ZHLN::Test::ExpectTrue(std::optional<int> {} < first) || !ZHLN::Test::ExpectTrue(empty <= std::optional<int> {}) ||
                !ZHLN::Test::ExpectTrue(std::optional<int> {} <= empty) || !ZHLN::Test::ExpectTrue(empty >= std::optional<int> {}) ||
                !ZHLN::Test::ExpectTrue(std::optional<int> {} >= empty) || !ZHLN::Test::ExpectTrue(std::optional<int> {3} > first) ||
                !ZHLN::Test::ExpectTrue(first < std::optional<int> {3})) {
                return std::unexpected(OptionalTestError::ComparisonFailed);
            }
            return {};
        }

#if defined(__cpp_exceptions)
        std::expected<void, ZHLN::ErrorCode> empty_value_throws_bad_optional_access() {
            try {
                (void) Ref {}.value();
            } catch (const std::bad_optional_access&) {
                return {};
            }
            return std::unexpected(OptionalTestError::MissingException);
        }
#endif
    };
};

} // namespace

// Exported for the existing CPU_Core group; this translation unit has no main.
auto RunOptionalSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<OptionalTestSuite>();
}
