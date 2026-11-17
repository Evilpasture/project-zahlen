#pragma once

// Intentionally use Unix header only.
#include <alloca.h>
#include <concepts>

namespace ZHLN {

template <typename T>
[[nodiscard, gnu::always_inline]] inline auto Alloca(std::unsigned_integral auto count = 1) noexcept -> T* {
    constexpr std::size_t kMaxAllocaBytes = 16384;
    const std::size_t     total_bytes     = count * sizeof(T);
    if (total_bytes > kMaxAllocaBytes) [[unlikely]] {
        std::abort();
    }
    return static_cast<T*>(alloca(total_bytes));
}

} // namespace ZHLN
