// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ZHLN::Vk {

struct EnabledFeature {
    VkStructureType       sType = VK_STRUCTURE_TYPE_MAX_ENUM;
    std::vector<uint64_t> words;
};

using EnabledFeatureSet = std::vector<EnabledFeature>;

template <typename T>
[[nodiscard]] auto FindEnabledFeature(const EnabledFeatureSet& enabled) noexcept -> const T*;

template <typename T>
[[nodiscard]] constexpr auto GetStructureType() noexcept -> VkStructureType;

template <typename T>
struct FeatureNode {
    T    feature;
    bool active = true;
};

template <typename... Ts>
class FeatureChain {
    std::tuple<FeatureNode<Ts>...> _features;
    VkPhysicalDevice               _physicalDevice = VK_NULL_HANDLE;

  public:
    FeatureChain() = default;
    FeatureChain(VkPhysicalDevice physicalDevice, std::tuple<FeatureNode<Ts>...>&& t);

    template <typename T, typename Func>
    auto Require(Func&& configure) &&;

    template <typename T, typename Func>
    auto Optional(Func&& configure) &&;

    FeatureChain<Ts...>& Build();

    const VkPhysicalDeviceFeatures2* GetRoot(const VkPhysicalDeviceFeatures2* tail = nullptr);

    [[nodiscard]] auto SnapshotEnabled() const -> EnabledFeatureSet;
};

class FeatureChainBuilder {
  public:
    explicit FeatureChainBuilder(VkPhysicalDevice physicalDevice) noexcept: _physicalDevice(physicalDevice) {
    }

    template <typename T, typename Func>
    auto Require(Func&& configure);

    template <typename T, typename Func>
    auto Optional(Func&& configure);

  private:
    VkPhysicalDevice _physicalDevice;
};

struct FeatureFactory {
    template <typename T>
    [[nodiscard]] static constexpr auto Create(auto&& configure) noexcept -> T;
};

}

#include "Features.inl"
