// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <cstddef>
#include <cstdint>
#include <tuple>
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

// Feature payloads contain VkBool32 fields; the header and any trailing
// alignment padding must never be treated as feature bits.
template <typename T>
[[nodiscard]] auto MaskFeatures(const T& requested, const T& supported) noexcept -> T;

template <typename T>
[[nodiscard]] auto HasAnyEnabledFeature(const T& feature) noexcept -> bool;

template <typename T>
[[nodiscard]] auto IsSubsetOf(const T& requested, const T& supported) noexcept -> bool;

template <typename T>
[[nodiscard]] auto QueryFeatureSupport(VkPhysicalDevice physicalDevice) noexcept -> T;

struct AcceptAnyFeature {
    template <typename T>
    [[nodiscard]] constexpr bool operator()(VkPhysicalDevice, const T&) const noexcept { return true; }
};

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

    template <typename T>
    auto Add(FeatureNode<T> node) &&;

    template <typename T, typename Func>
    auto Require(Func&& configure) &&;

    template <typename T, typename Func, typename Predicate = AcceptAnyFeature>
    auto Optional(Func&& configure, Predicate&& accept = {}, bool available = true) &&;

    template <typename T>
    [[nodiscard]] auto Find() const noexcept -> const T*;

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
