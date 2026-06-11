#pragma once
#include <atomic>
#include <type_traits>

namespace ZHLN {

/**
 * @brief Strictly Trivial (POD) Atomic Wrapper.
 * Uses C++20 std::atomic_ref to operate on raw memory safely.
 */
template <typename T> struct Atomic {
	static_assert(std::is_trivially_copyable_v<T>);
	static_assert(std::is_standard_layout_v<T>);
	static_assert(std::is_scalar_v<T>);
	static_assert((std::is_trivially_default_constructible_v<T> && std::is_trivially_copyable_v<T>));

	// Raw storage aligned to hardware requirements
	alignas(std::atomic_ref<T>::required_alignment) T value;

	// No constructors or destructors! This guarantees is_trivial_v = true.

	[[gnu::always_inline]]
	void store(T desired, std::memory_order order = std::memory_order_seq_cst) noexcept {
		std::atomic_ref<T>(value).store(desired, order);
	}

	[[nodiscard, gnu::always_inline]]
	T load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
		return std::atomic_ref<T>(const_cast<T&>(value)).load(order);
	}

	[[nodiscard, gnu::always_inline]]
	T exchange(T desired, std::memory_order order = std::memory_order_seq_cst) noexcept {
		return std::atomic_ref<T>(value).exchange(desired, order);
	}

	[[gnu::always_inline]]
	bool compare_exchange_weak(T& expected, T desired,
							   std::memory_order success = std::memory_order_seq_cst,
							   std::memory_order failure = std::memory_order_seq_cst) noexcept {
		return std::atomic_ref<T>(value).compare_exchange_weak(expected, desired, success, failure);
	}

	[[gnu::always_inline]]
	bool compare_exchange_strong(T& expected, T desired,
								 std::memory_order success = std::memory_order_seq_cst,
								 std::memory_order failure = std::memory_order_seq_cst) noexcept {
		return std::atomic_ref<T>(value).compare_exchange_strong(expected, desired, success,
																 failure);
	}

	// Integer arithmetic
	[[gnu::always_inline]]
	T fetch_add(T arg, std::memory_order order = std::memory_order_seq_cst) noexcept {
		return std::atomic_ref<T>(value).fetch_add(arg, order);
	}

	[[gnu::always_inline]]
	T fetch_sub(T arg, std::memory_order order = std::memory_order_seq_cst) noexcept {
		return std::atomic_ref<T>(value).fetch_sub(arg, order);
	}
};

// Guarantee at compile time!
static_assert((std::is_trivially_default_constructible_v<Atomic<size_t>> && std::is_trivially_copyable_v<Atomic<size_t>>), "ZHLN::Atomic must be Trivial");

} // namespace ZHLN