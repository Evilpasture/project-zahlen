#pragma once

#include <algorithm>
#include <cstddef>
#include <format>
#include <string_view>

namespace ZHLN {

/**
 * @brief A fixed-capacity, null-terminated string that never allocates.
 * @tparam Capacity The maximum number of characters including the null terminator.
 */
template <size_t Capacity> class FixedString {
	static_assert(Capacity > 0, "Capacity must be at least 1 for null terminator");

  public:
	constexpr FixedString() noexcept : _len(0) { _data[0] = '\0'; }

	constexpr FixedString(const char* s) noexcept { assign(s); }

	constexpr FixedString(std::string_view sv) noexcept { assign(sv); }

	// Explicitly copyable
	constexpr FixedString(const FixedString&) noexcept = default;
	constexpr FixedString& operator=(const FixedString&) noexcept = default;

	constexpr void assign(std::string_view sv) noexcept {
		_len = std::min(sv.size(), Capacity - 1);
		for (size_t i = 0; i < _len; ++i) {
			_data[i] = sv[i];
		}
		_data[_len] = '\0';
	}

	constexpr void append(std::string_view sv) noexcept {
		size_t remaining = Capacity - 1 - _len;
		size_t to_copy = std::min(sv.size(), remaining);
		for (size_t i = 0; i < to_copy; ++i) {
			_data[_len + i] = sv[i];
		}
		_len += to_copy;
		_data[_len] = '\0';
	}

	[[nodiscard]] constexpr const char* data() const noexcept { return _data; }
	[[nodiscard]] constexpr const char* c_str() const noexcept { return _data; }
	[[nodiscard]] constexpr size_t size() const noexcept { return _len; }
	[[nodiscard]] constexpr bool empty() const noexcept { return _len == 0; }
	[[nodiscard]] constexpr size_t capacity() const noexcept { return Capacity; }

	constexpr char& operator[](size_t i) noexcept { return _data[i]; }
	constexpr const char& operator[](size_t i) const noexcept { return _data[i]; }

	// Conversion to std::string_view for compatibility with std::print
	[[nodiscard]] constexpr operator std::string_view() const noexcept {
		return std::string_view{_data, _len};
	}

	// Comparison
	constexpr auto operator<=>(const FixedString& other) const noexcept {
		return std::string_view(*this) <=> std::string_view(other);
	}

	constexpr bool operator==(const FixedString& other) const noexcept {
		return std::string_view(*this) == std::string_view(other);
	}

	constexpr void clear() noexcept {
		_len = 0;
		_data[0] = '\0';
	}

  private:
	char _data[Capacity];
	size_t _len;
};

// Helper for type deduction: ZHLN::FixedString str{"Hello"};
template <size_t N> FixedString(const char (&)[N]) -> FixedString<N>;

// Common aliases
using String32 = FixedString<32>;
using String64 = FixedString<64>;
using String128 = FixedString<128>;
using String256 = FixedString<256>;

} // namespace ZHLN

namespace std {
template <size_t N> struct formatter<ZHLN::FixedString<N>, char> : formatter<string_view, char> {

	// 2. Use the fully qualified format_context
	auto format(const ZHLN::FixedString<N>& str, format_context& ctx) const {
		// 3. Cast to string_view (ensure your FixedString has this operator or a helper)
		return formatter<string_view, char>::format(string_view(str), ctx);
	}
};
} // namespace std