#pragma once

namespace ZHLN {
template <typename T> [[nodiscard]] constexpr const T& Min(const T& a, const T& b) noexcept {
	return (b < a) ? b : a;
}

template <typename T> [[nodiscard]] constexpr const T& Max(const T& a, const T& b) noexcept {
	return (a < b) ? b : a;
}

template <typename T>
[[nodiscard]] constexpr const T& Clamp(const T& v, const T& lo, const T& hi) noexcept {
	return (v < lo) ? lo : (hi < v) ? hi : v;
}
} // namespace ZHLN