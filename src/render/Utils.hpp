#pragma once

#include <cmath>
#include <initializer_list>
#include <type_traits>
namespace ZHLN {

template <typename T> [[nodiscard]] constexpr T Min(std::initializer_list<T> list) noexcept {
	// We assume the list isn't empty for proc-gen math
	auto it = list.begin();
	T result = *it;
	while (++it != list.end()) {
		if (*it < result)
			result = *it;
	}
	return result;
}

template <typename T> [[nodiscard]] constexpr const T& Min(const T& a, const T& b) noexcept {
	return (b < a) ? b : a;
}

template <typename T> [[nodiscard]] constexpr const T& Max(const T& a, const T& b) noexcept {
	return (a < b) ? b : a;
}

template <typename T>
[[nodiscard]] constexpr T Clamp(T v, std::type_identity_t<T> lo,
								std::type_identity_t<T> hi) noexcept {
	return (v < lo) ? lo : (hi < v) ? hi : v;
}

// GLSL-style fract: works for float, double, etc.
template <typename T> inline T Fract(T x) {
	return x - std::floor(x);
}

// GLSL-style mix: Linear interpolation
template <typename T, typename U> inline T Mix(const T& a, const T& b, const U& t) {
	return a + t * (b - a);
}

// GLSL-style saturate: Clamps between 0 and 1
template <typename T> inline T Saturate(T x) {
	return Max(static_cast<T>(0), Min(static_cast<T>(1), x));
}

} // namespace ZHLN