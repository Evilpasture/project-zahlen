// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/detail/RadixSort.hpp
#pragma once

#include <cstdint>

namespace ZHLN {
struct SortKey {
    uint64_t value;

    /**
     * @brief Packs the primary (material) and secondary (mesh) pointer keys.
     * Folds high-order 64-bit virtual address space bits (like ASLR offsets)
     * into 32-bit keys using a shift-XOR hash fold to prevent pointer collisions.
     */
    static SortKey Pack(const void* material, const void* mesh) noexcept {
        auto mat_val  = reinterpret_cast<uintptr_t>(material);
        auto mesh_val = reinterpret_cast<uintptr_t>(mesh);

        auto mat_id  = static_cast<uint32_t>((mat_val >> 3) ^ (mat_val >> 32));
        auto mesh_id = static_cast<uint32_t>((mesh_val >> 3) ^ (mesh_val >> 32));

        return {(static_cast<uint64_t>(mat_id) << 32) | mesh_id};
    }
};

struct SortItem {
    SortKey  key;
    uint32_t payload; // Index mapping to the original drawQueue slot
};

// Small swap helper

template <typename T>
inline void Swap(T& a, T& b) noexcept {
    T temp = static_cast<T&&>(a); // Raw move semantics without <utility>
    a      = static_cast<T&&>(b);
    b      = static_cast<T&&>(temp);
}

/**
 * @brief Performs a highly optimized, stable Radix Sort over 64-bit keys.
 */
inline void RadixSort64(SortItem* __restrict items, SortItem* __restrict temp, uint32_t size) {
    SortItem* in  = items;
    SortItem* out = temp;

    // 8 passes of 8 bits each to sort the full 64-bit key space
    for (uint32_t shift = 0; shift < 64; shift += 8) {
        uint32_t count[256] = {0};

        // 1. Calculate Histograms
        for (uint32_t i = 0; i < size; ++i) {
            auto bucket = static_cast<uint8_t>((in[i].key.value >> shift) & 0xFF);
            count[bucket]++;
        }

        // 2. Calculate Prefix Offsets
        uint32_t prefixes[256];
        prefixes[0] = 0;
        for (uint32_t i = 1; i < 256; ++i) {
            prefixes[i] = prefixes[i - 1] + count[i - 1];
        }

        // 3. Scatter items into buckets
        for (uint32_t i = 0; i < size; ++i) {
            auto     bucket = static_cast<uint8_t>((in[i].key.value >> shift) & 0xFF);
            uint32_t dest   = prefixes[bucket]++;
            out[dest]       = in[i];
        }

        // Swap buffer pointers
        Swap(in, out);
    }

    // Since we execute exactly 8 passes (even), the final sorted output
    // is guaranteed to reside back inside the original 'items' buffer.
}

} // namespace ZHLN
