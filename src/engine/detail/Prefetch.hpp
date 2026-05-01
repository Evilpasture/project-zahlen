#pragma once
#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace ZHLN {
enum class CacheLevel : uint8_t { L1 = 3, L2 = 2, L3 = 1, stream = 0 };
enum class AccessType : uint8_t { Read = 0, Write = 1 };

template <AccessType Access = AccessType::Read, CacheLevel Level = CacheLevel::L1>
[[gnu::always_inline]] inline void Prefetch(const void* addr) noexcept {
#if defined(__clang__) || defined(__GNUC__)
	__builtin_prefetch(addr, static_cast<int>(Access), static_cast<int>(Level));
#elif defined(_MSC_VER)
	if constexpr (Access == AccessType::Write) {
#if defined(_M_X64) || defined(_M_IX86)
		_m_prefetchw(const_cast<void*>(addr));
#else
		_mm_prefetch(static_cast<const char*>(addr), _MM_HINT_T0);
#endif
	} else {
		if constexpr (Level == CacheLevel::L1)
			_mm_prefetch(static_cast<const char*>(addr), _MM_HINT_T0);
		else if constexpr (Level == CacheLevel::L2)
			_mm_prefetch(static_cast<const char*>(addr), _MM_HINT_T1);
		else
			_mm_prefetch(static_cast<const char*>(addr), _MM_HINT_NTA);
	}
#endif
}
} // namespace ZHLN