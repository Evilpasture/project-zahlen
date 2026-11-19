#pragma once

#include <type_traits>

namespace ZHLN {
template <typename T>
concept TriviallyCopyableStandardLayout = std::is_trivially_copyable_v<T> && std::is_trivially_default_constructible_v<T> && std::is_standard_layout_v<T>;
}
