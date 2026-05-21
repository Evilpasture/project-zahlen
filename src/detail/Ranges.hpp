// File: src/detail/Ranges.hpp
#pragma once

#include <tuple>
#include <utility>
#include <iterator>

namespace ZHLN::Ranges {

template <typename... Iterators>
class ZipIterator {
public:
	using value_type = std::tuple<typename std::iterator_traits<Iterators>::reference...>;
	using reference = value_type;
	using pointer = void;
	using difference_type = std::ptrdiff_t;
	using iterator_category = std::input_iterator_tag;

	constexpr ZipIterator() = default;
	constexpr explicit ZipIterator(Iterators... iters) : _iters(iters...) {}

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
		return std::apply([](auto&... it) {
			return reference(*it...);
		}, _iters);
	}

	[[nodiscard]] constexpr bool operator==(const ZipIterator& other) const {
		// Stop iterating if ANY child iterator matches its end (mimics std::views::zip)
		return any_equal(other, std::index_sequence_for<Iterators...>{});
	}

	[[nodiscard]] constexpr bool operator!=(const ZipIterator& other) const {
		return !(*this == other);
	}

private:
	template <size_t... Is>
	[[nodiscard]] constexpr bool any_equal(const ZipIterator& other, std::index_sequence<Is...>) const {
		return ( (std::get<Is>(_iters) == std::get<Is>(other._iters)) || ... );
	}

	std::tuple<Iterators...> _iters;
};

template <typename... Ranges>
class ZipRange {
public:
	constexpr explicit ZipRange(Ranges&&... ranges)
		: _ranges(std::forward<Ranges>(ranges)...) {}

	[[nodiscard]] constexpr auto begin() {
		return std::apply([](auto&&... r) {
			using std::begin;
			return ZipIterator(begin(r)...);
		}, _ranges);
	}

	[[nodiscard]] constexpr auto end() {
		return std::apply([](auto&&... r) {
			using std::end;
			return ZipIterator(end(r)...);
		}, _ranges);
	}

private:
	std::tuple<Ranges...> _ranges;
};

/**
 * @brief Combines multiple ranges/arrays into a single zip-range.
 * Yields structured-binding compatible std::tuple elements.
 */
template <typename... Ranges>
[[nodiscard]] constexpr auto Zip(Ranges&&... ranges) {
	return ZipRange<Ranges...>(std::forward<Ranges>(ranges)...);
}

} // namespace ZHLN::Ranges