// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/detail/Ranges.hpp
#pragma once

#include <array>
#include <iterator>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ZHLN::Ranges {

namespace detail {

// Helper to deduce the weakest iterator_category among a set of categories
template <typename... Categories>
struct MinCategory;

template <typename Cat>
struct MinCategory<Cat> {
    using type = Cat;
};

template <typename Cat1, typename Cat2, typename... Rest>
struct MinCategory<Cat1, Cat2, Rest...> {
    using lower = std::conditional_t<std::is_base_of_v<Cat1, Cat2>, Cat2, std::conditional_t<std::is_base_of_v<Cat2, Cat1>, Cat1, std::input_iterator_tag>>;
    using type  = typename MinCategory<lower, Rest...>::type;
};

// Maps raw iterator categories to standard tags
template <typename Category>
using NormalizeCategory = std::conditional_t<
    std::is_base_of_v<std::random_access_iterator_tag, Category>,
    std::random_access_iterator_tag,
    std::conditional_t<
        std::is_base_of_v<std::bidirectional_iterator_tag, Category>,
        std::bidirectional_iterator_tag,
        std::conditional_t<std::is_base_of_v<std::forward_iterator_tag, Category>, std::forward_iterator_tag, std::input_iterator_tag>>>;

} // namespace detail

// ============================================================================
// Core Zip View
// ============================================================================

template <typename... Iterators>
class ZipIterator {
  public:
    using value_type        = std::tuple<typename std::iterator_traits<Iterators>::reference...>;
    using reference         = value_type;
    using pointer           = void;
    using difference_type   = std::ptrdiff_t;
    using iterator_category = typename detail::MinCategory<detail::NormalizeCategory<typename std::iterator_traits<Iterators>::iterator_category>...>::type;

    constexpr ZipIterator() = default;
    constexpr explicit ZipIterator(Iterators... iters): _iters(iters...) {
    }

    constexpr ZipIterator& operator++() {
        std::apply([](auto&... it) { (++it, ...); }, _iters);
        return *this;
    }

    constexpr ZipIterator operator++(int) {
        ZipIterator temp = *this;
        ++(*this);
        return temp;
    }

    [[nodiscard]] constexpr reference operator*() const {
        return std::apply([](auto&... it) { return reference(*it...); }, _iters);
    }

    [[nodiscard]] constexpr bool operator==(const ZipIterator& other) const {
        return any_equal(other, std::index_sequence_for<Iterators...> {});
    }

    [[nodiscard]] constexpr bool operator!=(const ZipIterator& other) const {
        return !(*this == other);
    }

  private:
    template <size_t... Is>
    [[nodiscard]] constexpr bool any_equal(const ZipIterator& other, std::index_sequence<Is...> /*unused*/) const {
        return ((std::get<Is>(_iters) == std::get<Is>(other._iters)) || ...);
    }

    std::tuple<Iterators...> _iters;
};

template <typename... Ranges>
class ZipRange {
  public:
    constexpr explicit ZipRange(Ranges&&... ranges): _ranges(std::forward<Ranges>(ranges)...) {
    }

    [[nodiscard]] constexpr auto begin() {
        return std::apply(
            [](auto&&... r) {
                using std::begin;
                return ZipIterator(begin(r)...);
            },
            _ranges
        );
    }

    [[nodiscard]] constexpr auto end() {
        return std::apply(
            [](auto&&... r) {
                using std::end;
                return ZipIterator(end(r)...);
            },
            _ranges
        );
    }

  private:
    std::tuple<Ranges...> _ranges;
};

// ============================================================================
// Transform View
// ============================================================================

template <typename Iterator, typename Func>
class TransformIterator {
  public:
    using value_type        = std::invoke_result_t<Func, typename std::iterator_traits<Iterator>::reference>;
    using reference         = value_type;
    using pointer           = void;
    using difference_type   = typename std::iterator_traits<Iterator>::difference_type;
    using iterator_category = detail::NormalizeCategory<typename std::iterator_traits<Iterator>::iterator_category>;

    constexpr TransformIterator() = default;
    constexpr TransformIterator(Iterator it, Func func): _it(it), _func(func) {
    }

    constexpr TransformIterator& operator++() {
        ++_it;
        return *this;
    }

    constexpr TransformIterator operator++(int) {
        TransformIterator temp = *this;
        ++(*this);
        return temp;
    }

    [[nodiscard]] constexpr reference operator*() const {
        return _func(*_it);
    }

    [[nodiscard]] constexpr bool operator==(const TransformIterator& other) const {
        return _it == other._it;
    }
    [[nodiscard]] constexpr bool operator!=(const TransformIterator& other) const {
        return _it != other._it;
    }

  private:
    Iterator     _it;
    mutable Func _func; // FIXED: mutable allows stateful / non-const operator() lambdas
};

template <typename Range, typename Func>
class TransformRange {
  public:
    constexpr explicit TransformRange(Range&& range, Func func): _range(std::forward<Range>(range)), _func(func) {
    }

    [[nodiscard]] constexpr auto begin() {
        using std::begin;
        return TransformIterator(begin(_range), _func);
    }

    [[nodiscard]] constexpr auto end() {
        using std::end;
        return TransformIterator(end(_range), _func);
    }

  private:
    Range _range;
    Func  _func;
};

// ============================================================================
// Filter View
// ============================================================================

template <typename Iterator, typename Pred>
class FilterIterator {
  private:
    using base_category = typename std::iterator_traits<Iterator>::iterator_category;

  public:
    using value_type      = typename std::iterator_traits<Iterator>::value_type;
    using reference       = typename std::iterator_traits<Iterator>::reference;
    using pointer         = typename std::iterator_traits<Iterator>::pointer;
    using difference_type = typename std::iterator_traits<Iterator>::difference_type;
    // Filtering inherently requires forward scanning, capping category at forward_iterator_tag
    using iterator_category =
        std::conditional_t<std::is_base_of_v<std::forward_iterator_tag, base_category>, std::forward_iterator_tag, std::input_iterator_tag>;

    constexpr FilterIterator() = default;
    constexpr FilterIterator(Iterator it, Iterator end, Pred pred): _it(it), _end(end), _pred(pred) {
        SatisfyPredicate();
    }

    constexpr FilterIterator& operator++() {
        ++_it;
        SatisfyPredicate();
        return *this;
    }

    constexpr FilterIterator operator++(int) {
        FilterIterator temp = *this;
        ++(*this);
        return temp;
    }

    [[nodiscard]] constexpr reference operator*() const {
        return *_it;
    }

    [[nodiscard]] constexpr bool operator==(const FilterIterator& other) const {
        return _it == other._it;
    }
    [[nodiscard]] constexpr bool operator!=(const FilterIterator& other) const {
        return _it != other._it;
    }

  private:
    constexpr void SatisfyPredicate() {
        while (_it != _end && !_pred(*_it)) {
            ++_it;
        }
    }

    Iterator _it;
    Iterator _end;
    Pred     _pred;
};

template <typename Range, typename Pred>
class FilterRange {
  public:
    constexpr explicit FilterRange(Range&& range, Pred pred): _range(std::forward<Range>(range)), _pred(pred) {
    }

    [[nodiscard]] constexpr auto begin() {
        using std::begin;
        using std::end;
        return FilterIterator(begin(_range), end(_range), _pred);
    }

    [[nodiscard]] constexpr auto end() {
        using std::end;
        return FilterIterator(end(_range), end(_range), _pred);
    }

  private:
    Range _range;
    Pred  _pred;
};

// ============================================================================
// Stride View
// ============================================================================

template <typename Iterator>
class StrideIterator {
  public:
    using value_type        = typename std::iterator_traits<Iterator>::value_type;
    using reference         = typename std::iterator_traits<Iterator>::reference;
    using pointer           = typename std::iterator_traits<Iterator>::pointer;
    using difference_type   = typename std::iterator_traits<Iterator>::difference_type;
    using iterator_category = detail::NormalizeCategory<typename std::iterator_traits<Iterator>::iterator_category>;

    constexpr StrideIterator() = default;
    constexpr StrideIterator(Iterator it, Iterator end, size_t stride): _it(it), _end(end), _stride(stride) {
    }

    constexpr StrideIterator& operator++() {
        for (size_t i = 0; i < _stride && _it != _end; ++i) {
            ++_it;
        }
        return *this;
    }

    constexpr StrideIterator operator++(int) {
        StrideIterator temp = *this;
        ++(*this);
        return temp;
    }

    [[nodiscard]] constexpr reference operator*() const {
        return *_it;
    }

    [[nodiscard]] constexpr bool operator==(const StrideIterator& other) const {
        return _it == other._it;
    }
    [[nodiscard]] constexpr bool operator!=(const StrideIterator& other) const {
        return _it != other._it;
    }

  private:
    Iterator _it;
    Iterator _end;
    size_t   _stride;
};

template <typename Range>
class StrideRange {
  public:
    constexpr explicit StrideRange(Range&& range, size_t stride): _range(std::forward<Range>(range)), _stride(stride) {
    }

    [[nodiscard]] constexpr auto begin() {
        using std::begin;
        using std::end;
        return StrideIterator(begin(_range), end(_range), _stride);
    }

    [[nodiscard]] constexpr auto end() {
        using std::end;
        return StrideIterator(end(_range), end(_range), _stride);
    }

  private:
    Range  _range;
    size_t _stride;
};

// ============================================================================
// Take View (Limits to first N elements)
// ============================================================================

template <typename Iterator>
class TakeIterator {
  public:
    using value_type        = typename std::iterator_traits<Iterator>::value_type;
    using reference         = typename std::iterator_traits<Iterator>::reference;
    using pointer           = typename std::iterator_traits<Iterator>::pointer;
    using difference_type   = typename std::iterator_traits<Iterator>::difference_type;
    using iterator_category = detail::NormalizeCategory<typename std::iterator_traits<Iterator>::iterator_category>;

    constexpr TakeIterator() = default;
    constexpr TakeIterator(Iterator it, size_t count): _it(it), _count(count) {
    }

    constexpr TakeIterator& operator++() {
        if (_count > 0) {
            ++_it;
            --_count;
        }
        return *this;
    }

    constexpr TakeIterator operator++(int) {
        TakeIterator temp = *this;
        ++(*this);
        return temp;
    }

    [[nodiscard]] constexpr reference operator*() const {
        return *_it;
    }

    [[nodiscard]] constexpr bool operator==(const TakeIterator& other) const {
        return (_count == 0 && other._count == 0) || (_it == other._it);
    }
    [[nodiscard]] constexpr bool operator!=(const TakeIterator& other) const {
        return !(*this == other);
    }

  private:
    Iterator _it;
    size_t   _count;
};

template <typename Range>
class TakeRange {
  public:
    constexpr explicit TakeRange(Range&& range, size_t count): _range(std::forward<Range>(range)), _count(count) {
    }

    [[nodiscard]] constexpr auto begin() {
        using std::begin;
        return TakeIterator(begin(_range), _count);
    }

    [[nodiscard]] constexpr auto end() {
        using std::end;
        return TakeIterator(end(_range), 0);
    }

  private:
    Range  _range;
    size_t _count;
};

// ============================================================================
// Drop View (Skips first N elements - Advanced at zero runtime cost)
// ============================================================================

template <typename Range>
class DropRange {
  public:
    constexpr explicit DropRange(Range&& range, size_t count): _range(std::forward<Range>(range)), _count(count) {
    }

    [[nodiscard]] constexpr auto begin() {
        using std::begin;
        using std::end;
        auto it   = begin(_range);
        auto last = end(_range);
        for (size_t i = 0; i < _count && it != last; ++i) {
            ++it;
        }
        return it;
    }

    [[nodiscard]] constexpr auto end() {
        using std::end;
        return end(_range);
    }

  private:
    Range  _range;
    size_t _count;
};

// ============================================================================
// EraseIf Action (Terminal mutation)
// ============================================================================

template <typename Container, typename Pred>
constexpr auto EraseIf(Container& c, Pred pred) {
    for (auto it = c.begin(); it != c.end();) {
        if (pred(*it)) {
            it = c.erase(it);
        } else {
            ++it;
        }
    }
    return c;
}

// ============================================================================
// FindIf Action (Linear Search)
// ============================================================================

template <typename Container, typename Pred>
constexpr auto FindIf(Container& c, Pred pred) {
    using std::begin;
    using std::end;
    auto it   = begin(c);
    auto last = end(c);
    for (; it != last; ++it) {
        if (pred(*it)) {
            return it;
        }
    }
    return last;
}

// ============================================================================
// Functional Adapters for Pipe `|` Syntax
// ============================================================================

template <typename Func>
struct TransformAdapter {
    Func func;
    template <typename Range>
    constexpr auto operator()(Range&& r) const {
        return TransformRange<Range, Func>(std::forward<Range>(r), func);
    }
};

template <typename Pred>
struct FilterAdapter {
    Pred pred;
    template <typename Range>
    constexpr auto operator()(Range&& r) const {
        return FilterRange<Range, Pred>(std::forward<Range>(r), pred);
    }
};

struct StrideAdapter {
    size_t stride;
    template <typename Range>
    constexpr auto operator()(Range&& r) const {
        return StrideRange<Range>(std::forward<Range>(r), stride);
    }
};

struct TakeAdapter {
    size_t count;
    template <typename Range>
    constexpr auto operator()(Range&& r) const {
        return TakeRange<Range>(std::forward<Range>(r), count);
    }
};

struct DropAdapter {
    size_t count;
    template <typename Range>
    constexpr auto operator()(Range&& r) const {
        return DropRange<Range>(std::forward<Range>(r), count);
    }
};

template <typename Pred>
struct EraseIfAdapter {
    Pred pred;

    // FIXED: Now delegates directly to ZHLN::Ranges::EraseIf(c, pred)
    template <typename Container>
    constexpr auto operator()(Container& c) const {
        return ZHLN::Ranges::EraseIf(c, pred);
    }
};

template <typename Pred>
struct FindIfAdapter {
    Pred pred;

    template <typename Container>
    constexpr auto operator()(Container& c) const {
        return ZHLN::Ranges::FindIf(c, pred);
    }
};

struct DefaultKeySelector {
    template <typename Element>
    constexpr decltype(auto) operator()(Element&& element) const noexcept {
        return element.first;
    }
};

template <typename Key, typename Factory, typename KeySelector>
struct FindOrInsertAdapter {
    Key         key;
    Factory     factory;
    KeySelector key_selector;

    template <typename Container>
    constexpr auto operator()(Container& c) const -> auto& {
        auto it = ZHLN::Ranges::FindIf(c, [this](const auto& element) {
            if constexpr (std::is_invocable_v<KeySelector, decltype(element)>) {
                return key_selector(element) == key;
            } else {
                return element.first == key;
            }
        });

        if (it != c.end()) {
            if constexpr (requires { it->second; }) {
                return it->second;
            } else {
                return *it;
            }
        }

        if constexpr (requires { c.emplace_back(); }) {
            if constexpr (std::is_invocable_v<Factory>) {
                if constexpr (requires { c.push_back(typename Container::value_type {key, factory()}); }) {
                    c.push_back({key, factory()});
                } else {
                    c.emplace_back(key, factory());
                }
            } else {
                c.push_back({key, factory});
            }

            auto& inserted_element = c.back();
            if constexpr (requires { inserted_element.second; }) {
                return inserted_element.second;
            } else {
                return inserted_element;
            }
        } else {
            static_assert(sizeof(Container) == 0, "[ZHLN::Ranges] Container must support emplace_back/push_back for FindOrInsert.");
        }
    }
};

// Global Pipeline Operator
template <typename Range, typename Adapter>
constexpr auto operator|(Range&& r, Adapter&& a) {
    return std::forward<Adapter>(a)(std::forward<Range>(r));
}

// ============================================================================
// Global Factory Functions (Supports both Direct and Pipeline syntax)
// ============================================================================

template <typename... Ranges>
[[nodiscard]] constexpr auto Zip(Ranges&&... ranges) {
    return ZipRange<Ranges...>(std::forward<Ranges>(ranges)...);
}

// 1. Transform Overloads
template <typename Range, typename Func>
[[nodiscard]] constexpr auto Transform(Range&& range, Func func) {
    return TransformRange<Range, Func>(std::forward<Range>(range), func);
}
template <typename Func>
[[nodiscard]] constexpr auto Transform(Func func) {
    return TransformAdapter<Func> {func};
}

// 2. Filter Overloads
template <typename Range, typename Pred>
[[nodiscard]] constexpr auto Filter(Range&& range, Pred pred) {
    return FilterRange<Range, Pred>(std::forward<Range>(range), pred);
}
template <typename Pred>
[[nodiscard]] constexpr auto Filter(Pred pred) {
    return FilterAdapter<Pred> {pred};
}

// 3. Stride Overloads
template <typename Range>
[[nodiscard]] constexpr auto Stride(Range&& range, size_t stride) {
    return StrideRange<Range>(std::forward<Range>(range), stride);
}
[[nodiscard]] constexpr auto Stride(size_t stride) {
    return StrideAdapter {stride};
}

// 4. Take Overloads
template <typename Range>
[[nodiscard]] constexpr auto Take(Range&& range, size_t count) {
    return TakeRange<Range>(std::forward<Range>(range), count);
}
[[nodiscard]] constexpr auto Take(size_t count) {
    return TakeAdapter {count};
}

// 5. Drop Overloads
template <typename Range>
[[nodiscard]] constexpr auto Drop(Range&& range, size_t count) {
    return DropRange<Range>(std::forward<Range>(range), count);
}
[[nodiscard]] constexpr auto Drop(size_t count) {
    return DropAdapter {count};
}

// 6. EraseIf Overloads
template <typename Pred>
[[nodiscard]] constexpr auto EraseIf(Pred pred) {
    return EraseIfAdapter<Pred> {pred};
}

// 7. FindIf Overloads
template <typename Pred>
[[nodiscard]] constexpr auto FindIf(Pred pred) {
    return FindIfAdapter<Pred> {pred};
}

// 8. FindOrInsert Overloads
template <typename Key, typename Factory, typename KeySelector = DefaultKeySelector>
[[nodiscard]] constexpr auto FindOrInsert(Key&& key, Factory&& factory, KeySelector&& key_selector = {}) {
    return FindOrInsertAdapter<std::decay_t<Key>, std::decay_t<Factory>, std::decay_t<KeySelector>> {
        std::forward<Key>(key), std::forward<Factory>(factory), std::forward<KeySelector>(key_selector)
    };
}

// ============================================================================
// FindOr / FindValue Adapters (Associative & Pair Range Lookup)
// ============================================================================

template <typename TargetType, typename Key, typename Fallback, typename KeySelector = DefaultKeySelector>
struct FindOrAdapter {
    Key         key;
    Fallback    fallback;
    KeySelector key_selector {};

    template <typename Container>
    constexpr auto operator()(const Container& c) const {
        using ValueType = std::conditional_t<std::is_void_v<TargetType>, std::decay_t<Fallback>, TargetType>;

        // 1. Specialized for ZHLN::HashMap (returns Value* / const Value*)
        if constexpr (requires { c.Find(key); }) {
            if (const auto* val = c.Find(key)) {
                return static_cast<ValueType>(*val);
            }
            return static_cast<ValueType>(fallback);
        }
        // 2. Specialized for std::map / std::unordered_map (has .find())
        else if constexpr (requires { c.find(key); }) {
            auto it = c.find(key);
            if (it != c.end()) {
                if constexpr (requires { it->second; }) {
                    return static_cast<ValueType>(it->second);
                } else {
                    return static_cast<ValueType>(*it);
                }
            }
            return static_cast<ValueType>(fallback);
        }
        // 3. Fallback for linear pair ranges / ZHLN::Array<Pair>
        else {
            auto it = ZHLN::Ranges::FindIf(c, [this](const auto& element) {
                if constexpr (std::is_invocable_v<KeySelector, decltype(element)>) {
                    return key_selector(element) == key;
                } else {
                    return element.first == key;
                }
            });

            if (it != c.end()) {
                if constexpr (requires { it->second; }) {
                    return static_cast<ValueType>(it->second);
                } else {
                    return static_cast<ValueType>(*it);
                }
            }
            return static_cast<ValueType>(fallback);
        }
    }
};

template <typename Key, typename KeySelector = DefaultKeySelector>
struct FindValueAdapter {
    Key         key;
    KeySelector key_selector {};

    template <typename Container>
    constexpr auto operator()(const Container& c) const {
        using MappedType = decltype([](const Container& cont, const Key& k) {
            if constexpr (requires { cont.Find(k); }) {
                return *cont.Find(k);
            } else if constexpr (requires { cont.find(k)->second; }) {
                return cont.find(k)->second;
            } else {
                return cont.begin()->second;
            }
        }(c, key));

        using OptType = std::optional<std::decay_t<MappedType>>;

        if constexpr (requires { c.Find(key); }) {
            if (const auto* val = c.Find(key)) {
                return OptType(*val);
            }
            return OptType(std::nullopt);
        } else if constexpr (requires { c.find(key); }) {
            auto it = c.find(key);
            if (it != c.end()) {
                return OptType(it->second);
            }
            return OptType(std::nullopt);
        } else {
            auto it = ZHLN::Ranges::FindIf(c, [this](const auto& element) {
                if constexpr (std::is_invocable_v<KeySelector, decltype(element)>) {
                    return key_selector(element) == key;
                } else {
                    return element.first == key;
                }
            });

            if (it != c.end()) {
                return OptType(it->second);
            }
            return OptType(std::nullopt);
        }
    }
};

// --- Factory Functions ---

template <typename TargetType = void, typename Key, typename Fallback, typename KeySelector = DefaultKeySelector>
[[nodiscard]] constexpr auto FindOr(Key&& key, Fallback&& fallback, KeySelector&& key_selector = {}) {
    return FindOrAdapter<TargetType, std::decay_t<Key>, std::decay_t<Fallback>, std::decay_t<KeySelector>> {
        std::forward<Key>(key), std::forward<Fallback>(fallback), std::forward<KeySelector>(key_selector)
    };
}

template <typename Key, typename KeySelector = DefaultKeySelector>
[[nodiscard]] constexpr auto FindValue(Key&& key, KeySelector&& key_selector = {}) {
    return FindValueAdapter<std::decay_t<Key>, std::decay_t<KeySelector>> {std::forward<Key>(key), std::forward<KeySelector>(key_selector)};
}

} // namespace ZHLN::Ranges
