// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../VkError.hpp"
#include "Extensions.hpp"
#include "Features.hpp"
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <initializer_list>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ZHLN::Vk {

enum class DeviceConfigurationError : uint8_t {
    MissingRequiredFeature ZHLN_ANNOTATION(ZHLN::Description<"A required Vulkan device feature is not supported">{}) = 1,
};

// Properties (unlike feature bits) live on the VkPhysicalDeviceProperties2
// chain. Call these only after the corresponding extension has been found.
[[nodiscard]] bool MeshShaderLimitsSufficient(VkPhysicalDevice physical) noexcept;
void ReportSubgroupSupport(VkPhysicalDevice physical, VkSubgroupFeatureFlags requiredOps) noexcept;

template <typename... Ts>
struct ConfiguredDevice {
    ExtensionResult    extensions;
    FeatureChain<Ts...> features;
};

// Resolves each optional feature and its extension(s) together. The result
// owns both extension names and the feature chain until vkCreateDevice has
// consumed them; ContextBuilder copies the enabled-feature snapshot.
template <typename... Ts>
class DeviceConfigurator {
  public:
    explicit DeviceConfigurator(VkPhysicalDevice physical) requires(sizeof...(Ts) == 0):
        _physical(physical), _extensions(ExtensionBuilder::ForDevice(physical)), _features(physical, std::tuple<>{}) {}

    auto RequireExtension(std::string_view name) && -> DeviceConfigurator&& {
        _extensions.Require(name);
        return std::move(*this);
    }

    auto OptionalExtension(std::string_view name, bool condition = true) && -> DeviceConfigurator&& {
        if (condition) {
            _extensions.Optional(name);
        }
        return std::move(*this);
    }

    auto WithSwapchain(bool present) && -> DeviceConfigurator&& {
        if (present) {
            _extensions.Require(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            _extensions.Optional(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME);
        }
        return std::move(*this);
    }

    auto SubgroupDiagnostics(VkSubgroupFeatureFlags ops) && -> DeviceConfigurator&& {
        _subgroupOps = ops;
        return std::move(*this);
    }

    template <typename T, typename Required, typename Optional>
    auto RequireWithOptional(Required&& required, Optional&& optional) &&;

    template <typename T, typename Func>
    auto Require(Func&& configure) &&;

    template <typename T, typename Func>
    auto RequireExtension(std::string_view name, Func&& configure) &&;

    template <typename T, typename Func, typename Predicate = AcceptAnyFeature>
    auto OptionalExtension(
        std::initializer_list<std::string_view> names, Func&& configure, Predicate&& accept = {}, bool condition = true
    ) &&;

    template <typename T, typename Func, typename Predicate = AcceptAnyFeature>
    auto OptionalExtension(std::string_view name, Func&& configure, Predicate&& accept = {}, bool condition = true) &&;

    template <typename First, typename Second, typename ConfigureFirst, typename ConfigureSecond, typename Predicate>
    auto OptionalGroup(
        std::initializer_list<std::string_view> names, ConfigureFirst&& first, ConfigureSecond&& second, Predicate&& accept,
        bool condition = true
    ) &&;

    auto OptionalMeshShaders() &&;
    auto OptionalRayTracing() &&;
    auto OptionalPresentation(bool present) &&;
    auto OptionalFifoLatestReady(bool present) &&;
    auto OptionalPresentTiming(bool present) &&;

    [[nodiscard]] auto Build() && -> std::expected<ConfiguredDevice<Ts...>, Vk::Error>;

  private:
    template <typename...>
    friend class DeviceConfigurator;

    DeviceConfigurator(
        VkPhysicalDevice physical, ExtensionBuilder extensions, FeatureChain<Ts...> features,
        std::vector<VkStructureType> unsupported, VkSubgroupFeatureFlags subgroupOps
    ):
        _physical(physical), _extensions(std::move(extensions)), _features(std::move(features)),
        _unsupported(std::move(unsupported)), _subgroupOps(subgroupOps) {}

    template <typename T>
    auto Add(FeatureNode<T> node) && -> DeviceConfigurator<Ts..., T> {
        return {_physical, std::move(_extensions), std::move(_features).Add(std::move(node)), std::move(_unsupported), _subgroupOps};
    }

    VkPhysicalDevice            _physical = VK_NULL_HANDLE;
    ExtensionBuilder            _extensions;
    FeatureChain<Ts...>         _features;
    std::vector<VkStructureType> _unsupported;
    VkSubgroupFeatureFlags      _subgroupOps = 0;
};

}

#include "DeviceConfigurator.inl"
