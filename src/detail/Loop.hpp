#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>
namespace ZHLN {
template <size_t N, typename F> constexpr void Unroll(F&& f) {
	[&f]<size_t... Is>(std::index_sequence<Is...>) -> auto {
		(f(std::integral_constant<size_t, Is>{}), ...);
	}(std::make_index_sequence<N>{});
}

template <typename T, size_t N, typename F> constexpr void Unroll(F&& f) {
	constexpr size_t MAX_UNROLL = (sizeof(T) > 32) ? 4 : 8;
	constexpr size_t ActualN = (N > MAX_UNROLL) ? MAX_UNROLL : N;

	[&f]<size_t... Is>(std::index_sequence<Is...>) -> auto {
		(f(std::integral_constant<size_t, Is>{}), ...);
	}(std::make_index_sequence<ActualN>{});
}

template <size_t Factor, typename F> constexpr void UnrollLoop(size_t total, F&& f) {
	size_t i = 0;
	for (; i + Factor <= total; i += Factor) {
		Unroll<Factor>([&](auto index) -> auto { f(i + index); });
	}
	for (; i < total; ++i) {
		f(i);
	}
}

template <size_t N, typename F> constexpr void Repeat(F&& f) {
	[&f]<size_t... Is>(std::index_sequence<Is...>) -> auto {
		((static_cast<void>(Is), f()), ...);
	}(std::make_index_sequence<N>{});
}
} // namespace ZHLN
