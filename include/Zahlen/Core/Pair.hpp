#pragma once

namespace ZHLN {
template <typename T1, typename T2>
struct Pair {
    T1 first;
    T2 second;

    constexpr bool operator==(const Pair&) const  = default;
    auto           operator<=>(const Pair&) const = default;
};
} // namespace ZHLN
