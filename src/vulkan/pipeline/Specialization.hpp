// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif


#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ZHLN::Vk {

template <typename T>
class Specialization {
    static_assert(std::is_standard_layout_v<T>, "Specialization<T> maps a field by its ABI offset: make T standard-layout");
    static_assert(!std::is_empty_v<T>, "Specialization<T> of a fieldless struct maps nothing, which silently keeps every default");

  public:
    Specialization() noexcept = default;

    template <typename FieldT>
    void operator()(std::string_view , std::size_t offset) noexcept {
        Record(offset, static_cast<std::uint32_t>(sizeof(FieldT)));
    }

    void Record(std::size_t offset, std::uint32_t size) noexcept {
        entries.push_back(
            VkSpecializationMapEntry {
                .constantID = static_cast<std::uint32_t>(entries.size()),
                .offset     = static_cast<std::uint32_t>(offset),
                .size       = size,
            }
        );
    }

    [[nodiscard]] auto Info(const T& value) const noexcept -> VkSpecializationInfo {
        return {
            .mapEntryCount = static_cast<std::uint32_t>(entries.size()),
            .pMapEntries   = entries.data(),
            .dataSize      = sizeof(T),
            .pData         = &value,
        };
    }

    template <std::size_t N>
    [[nodiscard]] auto Infos(const std::array<T, N>& values) const noexcept -> std::array<VkSpecializationInfo, N> {
        std::array<VkSpecializationInfo, N> infos {};
        for (std::size_t i = 0; i < N; ++i) {
            infos[i] = Info(values[i]);
        }
        return infos;
    }

    [[nodiscard]] auto Entries() const noexcept -> std::span<const VkSpecializationMapEntry> {
        return entries;
    }
    [[nodiscard]] auto Empty() const noexcept -> bool {
        return entries.empty();
    }
    void Clear() noexcept {
        entries.clear();
    }

  private:
    std::vector<VkSpecializationMapEntry> entries;
};

}
