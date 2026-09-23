// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/core/Features.hpp
#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ZHLN::Vk {

// One feature struct, copied out of a built chain and tagged with the sType it
// was chained under. A chain is a variadic template, so nothing that has to
// hold one can name its type -- Context stores this instead and looks entries
// up by sType, the one identifier every Vulkan feature struct carries and
// every reader already knows.
//
// `words` and not bytes: GetFeature<T>() hands this storage back as a T*, and
// these structs are a two-word header followed by VkBool32s, so they want the
// eight-byte alignment a uint64_t buffer gives and a byte buffer would not.
struct EnabledFeature {
    VkStructureType       sType = VK_STRUCTURE_TYPE_MAX_ENUM;
    std::vector<uint64_t> words;
};

using EnabledFeatureSet = std::vector<EnabledFeature>;

// The enabled struct of type T in a snapshot, or nullptr when the chain did not
// enable it -- either never requested, or requested through Optional and
// dropped because the device lacks one of the bits that were asked for.
template <typename T>
[[nodiscard]] auto FindEnabledFeature(const EnabledFeatureSet& enabled) noexcept -> const T*;

/**
 * @brief Compile-time Type-to-Enum mapping.
 * Mirrors the GetFormatAspect function structure without requiring macros.
 */
template <typename T>
[[nodiscard]] constexpr auto GetStructureType() noexcept -> VkStructureType;

/**
 * @brief Wrapper to associate a runtime active/inactive flag with a compile-time feature struct.
 */
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

    // Links the enabled structs and returns the head of the chain.
    //
    // `tail`, when non-null, is chained on after the last of them, so two
    // chains built independently -- a caller's and this backend's own -- can be
    // handed to vkCreateDevice as the single chain it requires, with neither
    // needing to name the other's type. A Vulkan device takes exactly one
    // feature chain, and a struct whose sType appears twice in it is invalid,
    // so the two halves must request disjoint structs.
    const VkPhysicalDeviceFeatures2* GetRoot(const VkPhysicalDeviceFeatures2* tail = nullptr);

    // Copies every struct this chain enables into a type-erased snapshot, so a
    // consumer can ask what got enabled by type long after this chain -- a
    // local in somebody's bring-up function -- has gone out of scope.
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

} // namespace ZHLN::Vk

#include "Features.inl"
