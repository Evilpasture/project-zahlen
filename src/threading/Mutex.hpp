// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <detail/Atomic.hpp>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

#ifndef NDEBUG
#define ZHLN_DEBUG 1
#endif

namespace ZHLN {

// Forward declare the Fiber system
struct Fiber;
extern Fiber* GetCurrentFiber() noexcept;
extern void YieldFiber() noexcept;

inline constexpr bool kIsDebugMutex =
#ifdef ZHLN_DEBUG
	true;
#else
	false;
#endif

/**
 * @brief High-Performance, 1-Byte Mutex.
 * Satisfies the C++ `BasicLockable` requirement (`std::lock_guard` compatible).
 */
class Mutex {
  public:
	constexpr Mutex() noexcept = default;
	~Mutex() = default;

	[[gnu::flatten, gnu::hot, gnu::always_inline]]
	void lock() noexcept {
		if constexpr (kIsDebugMutex) {
			CheckPreLock();
		}

		uint8_t expected = UNLOCKED;
		if (_bits.compare_exchange_strong(expected, LOCKED, std::memory_order_acquire,
										  std::memory_order_relaxed)) [[likely]] {
			if constexpr (kIsDebugMutex) {
				PostLock();
			}
			return;
		}
		LockSlow();
	}

	[[gnu::flatten, gnu::hot, gnu::always_inline]]
	void unlock() noexcept {
		if constexpr (kIsDebugMutex) {
			PreUnlock();
			ClearOwner();
		}

		uint8_t expected = LOCKED;
		if (_bits.compare_exchange_strong(expected, UNLOCKED, std::memory_order_release,
										  std::memory_order_relaxed)) [[likely]] {
			return;
		}
		UnlockSlow();
	}

	[[gnu::flatten, gnu::hot, gnu::always_inline]]
	bool try_lock() noexcept {
		if constexpr (kIsDebugMutex) {
			CheckPreLock();
		}

		uint8_t expected = UNLOCKED;
		bool success = _bits.compare_exchange_strong(expected, LOCKED, std::memory_order_acquire,
													 std::memory_order_relaxed);
		if constexpr (kIsDebugMutex) {
			if (success) {
				PostLock();
			}
		}
		return success;
	}

  private:
	static constexpr uint8_t UNLOCKED = 0x00;
	static constexpr uint8_t LOCKED = 0x01;
	static constexpr uint8_t HAS_WAITERS = 0x02;
	static constexpr uint8_t POISONED = 0x04;

	ZHLN::Atomic<uint8_t> _bits;

	[[gnu::cold, gnu::noinline]] void LockSlow() noexcept;
	[[gnu::cold, gnu::noinline]] void UnlockSlow() noexcept;

	// --- Debug Variables & Helpers ---
#ifdef ZHLN_DEBUG
	alignas(32) ZHLN::Atomic<bool> _hasOwner;
	ZHLN::Atomic<uintptr_t> _owner;

	void CheckPreLock() noexcept;
	void PostLock() noexcept;
	void PreUnlock() noexcept;
	void ClearOwner() noexcept;
#else
	constexpr void CheckPreLock() noexcept {}
	constexpr void PostLock() noexcept {}
	constexpr void PreUnlock() noexcept {}
	constexpr void ClearOwner() noexcept {}
#endif
};

// Guarantee 1-byte footprint in Release builds
static_assert(kIsDebugMutex || sizeof(Mutex) == 1,
			  "ZHLN::Mutex must be exactly 1 byte in Release mode!");

// Guarantee it's a perfect POD
static_assert(kIsDebugMutex || (std::is_trivially_default_constructible_v<Mutex> &&
								std::is_trivially_copyable_v<Mutex>),
			  "Mutex MUST be trivial in Release mode!");

/**
 * @brief Trivial RAII guard to avoid including <mutex> in interface headers.
 */
struct MutexGuard {
	Mutex& _m;
	explicit MutexGuard(Mutex& m) noexcept : _m(m) { _m.lock(); }
	~MutexGuard() noexcept { _m.unlock(); }

	MutexGuard(const MutexGuard&) = delete;
	MutexGuard& operator=(const MutexGuard&) = delete;
};

inline void CPURelax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
	_mm_pause();
#elif defined(__aarch64__)
	__asm__ __volatile__("yield" ::: "memory");
#else
	std::this_thread::yield();
#endif
}

} // namespace ZHLN
