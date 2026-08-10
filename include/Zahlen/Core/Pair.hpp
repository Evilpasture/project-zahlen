// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

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
