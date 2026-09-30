// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "DeviceConfigurator.hpp"
#include <Zahlen/Log.hpp>

namespace ZHLN::Vk {

template <typename... Ts>
template <typename T, typename Required, typename Optional>
auto DeviceConfigurator<Ts...>::RequireWithOptional(Required&& required, Optional&& optional) && {
    T requiredBits {};
    std::forward<Required>(required)(requiredBits);
    T requested = requiredBits;
    std::forward<Optional>(optional)(requested);

    const T supported = QueryFeatureSupport<T>(_physical);
    if (!IsSubsetOf(requiredBits, supported)) {
        _unsupported.push_back(GetStructureType<T>());
    }
    // Keep required bits in the same Vulkan struct as the individually
    // negotiated optional bits. A second Features2/Vulkan12 node is invalid.
    return std::move(*this).Add(FeatureNode<T> {.feature = MaskFeatures(requested, supported), .active = true});
}

template <typename... Ts>
template <typename T, typename Func>
auto DeviceConfigurator<Ts...>::Require(Func&& configure) && {
    return std::move(*this).template RequireWithOptional<T>(std::forward<Func>(configure), [](T&) {});
}

template <typename... Ts>
template <typename T, typename Func>
auto DeviceConfigurator<Ts...>::RequireExtension(std::string_view name, Func&& configure) && {
    const bool offered = _extensions.Supports(name);
    _extensions.Require(name);
    if (!offered) {
        // Do not query a feature struct belonging to an absent extension.
        // Build reports the missing required extension before device creation.
        return std::move(*this).Add(FeatureNode<T> {.feature = T {}, .active = false});
    }
    return std::move(*this).template Require<T>(std::forward<Func>(configure));
}

template <typename... Ts>
template <typename T, typename Func, typename Predicate>
auto DeviceConfigurator<Ts...>::OptionalExtension(
    std::initializer_list<std::string_view> names, Func&& configure, Predicate&& accept, bool condition
) && {
    const bool offered = condition && names.size() != 0 && _extensions.SupportsAll(names);
    auto next = std::move(_features).template Optional<T>(std::forward<Func>(configure), std::forward<Predicate>(accept), offered);
    if (next.template Find<T>() != nullptr) {
        _extensions.OptionalGroup(names);
    }
    return DeviceConfigurator<Ts..., T> {_physical, std::move(_extensions), std::move(next), std::move(_unsupported), _subgroupOps};
}

template <typename... Ts>
template <typename T, typename Func, typename Predicate>
auto DeviceConfigurator<Ts...>::OptionalExtension(std::string_view name, Func&& configure, Predicate&& accept, bool condition) && {
    return std::move(*this).template OptionalExtension<T>({name}, std::forward<Func>(configure), std::forward<Predicate>(accept), condition);
}

template <typename... Ts>
template <typename First, typename Second, typename ConfigureFirst, typename ConfigureSecond, typename Predicate>
auto DeviceConfigurator<Ts...>::OptionalGroup(
    std::initializer_list<std::string_view> names, ConfigureFirst&& first, ConfigureSecond&& second, Predicate&& accept, bool condition
) && {
    First  firstRequested {};
    Second secondRequested {};
    std::forward<ConfigureFirst>(first)(firstRequested);
    std::forward<ConfigureSecond>(second)(secondRequested);

    First  firstActual {};
    Second secondActual {};
    bool   active = false;
    if (condition && names.size() != 0 && _extensions.SupportsAll(names) && _physical != VK_NULL_HANDLE) {
        firstActual  = MaskFeatures(firstRequested, QueryFeatureSupport<First>(_physical));
        secondActual = MaskFeatures(secondRequested, QueryFeatureSupport<Second>(_physical));
        active = HasAnyEnabledFeature(firstActual) && HasAnyEnabledFeature(secondActual) &&
                 std::forward<Predicate>(accept)(_physical, firstActual, secondActual);
        if (active) {
            _extensions.OptionalGroup(names);
        }
    }
    auto firstChain = std::move(_features).Add(FeatureNode<First> {.feature = firstActual, .active = active});
    auto chain = std::move(firstChain).Add(FeatureNode<Second> {.feature = secondActual, .active = active});
    return DeviceConfigurator<Ts..., First, Second> {_physical, std::move(_extensions), std::move(chain), std::move(_unsupported), _subgroupOps};
}

template <typename... Ts>
auto DeviceConfigurator<Ts...>::OptionalMeshShaders() && {
    return std::move(*this).template OptionalExtension<VkPhysicalDeviceMeshShaderFeaturesEXT>(
        VK_EXT_MESH_SHADER_EXTENSION_NAME,
        [](auto& f) {
            f.taskShader         = VK_TRUE;
            f.meshShader         = VK_TRUE;
            f.multiviewMeshShader = VK_TRUE;
            f.meshShaderQueries  = VK_TRUE;
        },
        [](VkPhysicalDevice gpu, const auto& enabled) {
            return enabled.taskShader == VK_TRUE && enabled.meshShader == VK_TRUE && MeshShaderLimitsSufficient(gpu);
        }
    );
}

template <typename... Ts>
auto DeviceConfigurator<Ts...>::OptionalRayTracing() && {
    // Both feature bits and all three extensions are a single capability.
    return std::move(*this).template OptionalGroup<VkPhysicalDeviceAccelerationStructureFeaturesKHR, VkPhysicalDeviceRayQueryFeaturesKHR>(
        {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME},
        [](auto& f) { f.accelerationStructure = VK_TRUE; },
        [](auto& f) { f.rayQuery = VK_TRUE; },
        [](VkPhysicalDevice, const auto& accel, const auto& ray) {
            return accel.accelerationStructure == VK_TRUE && ray.rayQuery == VK_TRUE;
        }
    );
}

template <typename... Ts>
auto DeviceConfigurator<Ts...>::OptionalFifoLatestReady(bool present) && {
    const auto name = _extensions.Supports(VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME)
                          ? VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME
                          : VK_EXT_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME;
    return std::move(*this).template OptionalExtension<VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR>(
        name, [](auto& f) { f.presentModeFifoLatestReady = VK_TRUE; }, AcceptAnyFeature {}, present
    );
}

template <typename... Ts>
auto DeviceConfigurator<Ts...>::OptionalPresentTiming(bool present) && {
    const auto calibrated = _extensions.Supports(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)
                                ? VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME
                                : VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    return std::move(*this).template OptionalGroup<VkPhysicalDevicePresentTimingFeaturesEXT, VkPhysicalDevicePresentId2FeaturesKHR>(
        {VK_EXT_PRESENT_TIMING_EXTENSION_NAME, VK_KHR_PRESENT_ID_2_EXTENSION_NAME, calibrated},
        [](auto& f) { f.presentTiming = VK_TRUE; f.presentAtAbsoluteTime = VK_TRUE; },
        [](auto& f) { f.presentId2 = VK_TRUE; },
        [](VkPhysicalDevice, const auto& timing, const auto& id2) {
            return timing.presentTiming == VK_TRUE && timing.presentAtAbsoluteTime == VK_TRUE && id2.presentId2 == VK_TRUE;
        }, present
    );
}

template <typename... Ts>
auto DeviceConfigurator<Ts...>::OptionalPresentation(bool present) && {
    auto maintenance = std::move(*this).WithSwapchain(present).template OptionalExtension<VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR>(
        VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, [](auto& f) { f.swapchainMaintenance1 = VK_TRUE; }, AcceptAnyFeature {}, present
    );
    return std::move(maintenance).OptionalFifoLatestReady(present).OptionalPresentTiming(present);
}

template <typename... Ts>
auto DeviceConfigurator<Ts...>::Build() && -> std::expected<ConfiguredDevice<Ts...>, ErrorCode> {
    auto extensions = _extensions.Build();
    if (!extensions) {
        return std::unexpected(extensions.error());
    }
    if (!_unsupported.empty()) {
        for (VkStructureType type: _unsupported) {
            ZHLN::Log("[Vulkan] Required device feature struct {} is not supported by the selected GPU", static_cast<uint32_t>(type));
        }
        return std::unexpected(DeviceConfigurationError::MissingRequiredFeature);
    }
    if (_subgroupOps != 0) {
        ReportSubgroupSupport(_physical, _subgroupOps);
    }
    return ConfiguredDevice<Ts...> {std::move(*extensions), std::move(_features)};
}

} // namespace ZHLN::Vk
