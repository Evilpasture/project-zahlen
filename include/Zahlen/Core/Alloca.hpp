#pragma once

// Intentionally use Unix header only.
#include <alloca.h>
#include <concepts>

namespace ZHLN {

template <typename T>
[[nodiscard, gnu::always_inline]] inline auto Alloca(std::unsigned_integral auto count) noexcept -> T* {
    return static_cast<T*>(alloca(count * sizeof(T)));
}

} // namespace ZHLN
