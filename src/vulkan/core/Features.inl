// src/vulkan/core/Features.inl
#pragma once
#include "Features.hpp"

#include <cstring>

namespace ZHLN::Vk {

// GetStructureType Implementation

template <typename T>
[[nodiscard]] constexpr auto GetStructureType() noexcept -> VkStructureType {
    if constexpr (std::is_same_v<T, VkPhysicalDeviceVulkan11Features>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceVulkan12Features>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceVulkan13Features>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceMaintenance5FeaturesKHR>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceFeatures2>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceAccelerationStructureFeaturesKHR>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceRayQueryFeaturesKHR>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceRobustness2FeaturesEXT>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceDescriptorHeapFeaturesEXT>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceMeshShaderFeaturesEXT>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceAddressBindingReportFeaturesEXT>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceFaultFeaturesKHR>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_KHR;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceFaultFeaturesEXT>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceShaderConstantDataFeaturesKHR>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CONSTANT_DATA_FEATURES_KHR;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceShaderAbortFeaturesKHR>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ABORT_FEATURES_KHR;
    } else if constexpr (std::is_same_v<T, VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR;
    } else if constexpr (std::is_same_v<T, VkPhysicalDevicePresentTimingFeaturesEXT>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT;
    } else if constexpr (std::is_same_v<T, VkPhysicalDevicePresentId2FeaturesKHR>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR;
    } else {
        // C++23: Safe compile-time error only if an unregistered Type is instantiated
        static_assert(sizeof(T) == 0, "Vulkan structure type mapping not registered for this Type in GetStructureType().");
        return VK_STRUCTURE_TYPE_MAX_ENUM;
    }
}

// FeatureChain Implementation

template <typename T>
[[nodiscard]] auto FindEnabledFeature(const EnabledFeatureSet& enabled) noexcept -> const T* {
    const VkStructureType want = GetStructureType<T>();
    for (const EnabledFeature& entry: enabled) {
        if (entry.sType == want) {
            return reinterpret_cast<const T*>(entry.words.data());
        }
    }
    return nullptr;
}

template <typename... Ts>
FeatureChain<Ts...>::FeatureChain(VkPhysicalDevice physicalDevice, std::tuple<FeatureNode<Ts>...>&& t):
    _features(std::move(t)), _physicalDevice(physicalDevice) {
}

template <typename... Ts>
template <typename T>
auto FeatureChain<Ts...>::Add(FeatureNode<T> node) && {
    static_assert((!std::is_same_v<T, Ts> && ...), "a Vulkan feature struct may appear only once in the device chain");
    return FeatureChain<Ts..., T>(_physicalDevice, std::tuple_cat(std::move(_features), std::make_tuple(std::move(node))));
}

template <typename... Ts>
template <typename T, typename Func>
auto FeatureChain<Ts...>::Require(Func&& configure) && {
    T feature {};
    std::forward<Func>(configure)(feature);
    feature.sType = GetStructureType<T>();
    feature.pNext = nullptr;
    return std::move(*this).Add(FeatureNode<T> {.feature = feature, .active = true});
}

template <typename... Ts>
template <typename T>
[[nodiscard]] auto FeatureChain<Ts...>::Find() const noexcept -> const T* {
    static_assert((0 + ... + static_cast<int>(std::is_same_v<T, Ts>)) == 1, "feature type must occur exactly once in the chain");
    const auto& node = std::get<FeatureNode<T>>(_features);
    return node.active ? &node.feature : nullptr;
}

// Vulkan feature structures have a header followed by VkBool32 fields, but
// many have four bytes of *trailing padding*. Explicitly name the final field
// so neither masking nor the "any enabled" check can mistake padding for a bit.
template <typename T>
[[nodiscard]] consteval auto FeaturePayloadEnd() noexcept -> size_t {
    if constexpr (std::is_same_v<T, VkPhysicalDeviceFeatures2>) {
        static_assert(sizeof(VkPhysicalDeviceFeatures) == 55 * sizeof(VkBool32));
        return offsetof(VkPhysicalDeviceFeatures2, features) + sizeof(VkPhysicalDeviceFeatures);
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceVulkan11Features>) {
        return offsetof(VkPhysicalDeviceVulkan11Features, shaderDrawParameters) + sizeof(VkBool32);
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceVulkan12Features>) {
        return offsetof(VkPhysicalDeviceVulkan12Features, subgroupBroadcastDynamicId) + sizeof(VkBool32);
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceVulkan13Features>) {
        return offsetof(VkPhysicalDeviceVulkan13Features, maintenance4) + sizeof(VkBool32);
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR>) {
        return offsetof(VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR, swapchainMaintenance1) + sizeof(VkBool32);
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceMaintenance5FeaturesKHR>) {
        return offsetof(VkPhysicalDeviceMaintenance5FeaturesKHR, maintenance5) + sizeof(VkBool32);
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceAccelerationStructureFeaturesKHR>) {
        return offsetof(VkPhysicalDeviceAccelerationStructureFeaturesKHR, descriptorBindingAccelerationStructureUpdateAfterBind) + sizeof(VkBool32);
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceRayQueryFeaturesKHR>) {
        return offsetof(VkPhysicalDeviceRayQueryFeaturesKHR, rayQuery) + sizeof(VkBool32);
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceRobustness2FeaturesEXT>) {
        return offsetof(VkPhysicalDeviceRobustness2FeaturesEXT, nullDescriptor) + sizeof(VkBool32);
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceDescriptorHeapFeaturesEXT>) {
        return offsetof(VkPhysicalDeviceDescriptorHeapFeaturesEXT, descriptorHeapCaptureReplay) + sizeof(VkBool32);
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT>) {
        return offsetof(VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT, dynamicRenderingUnusedAttachments) + sizeof(VkBool32);
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceMeshShaderFeaturesEXT>) {
        return offsetof(VkPhysicalDeviceMeshShaderFeaturesEXT, meshShaderQueries) + sizeof(VkBool32);
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceAddressBindingReportFeaturesEXT>) {
        return offsetof(VkPhysicalDeviceAddressBindingReportFeaturesEXT, reportAddressBinding) + sizeof(VkBool32);
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceFaultFeaturesKHR>) {
        return offsetof(VkPhysicalDeviceFaultFeaturesKHR, deviceFaultDeviceLostOnMasked) + sizeof(VkBool32);
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceFaultFeaturesEXT>) {
        return offsetof(VkPhysicalDeviceFaultFeaturesEXT, deviceFaultVendorBinary) + sizeof(VkBool32);
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceShaderConstantDataFeaturesKHR>) {
        return offsetof(VkPhysicalDeviceShaderConstantDataFeaturesKHR, shaderConstantData) + sizeof(VkBool32);
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceShaderAbortFeaturesKHR>) {
        return offsetof(VkPhysicalDeviceShaderAbortFeaturesKHR, shaderAbort) + sizeof(VkBool32);
    } else if constexpr (std::is_same_v<T, VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR>) {
        return offsetof(VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR, presentModeFifoLatestReady) + sizeof(VkBool32);
    } else if constexpr (std::is_same_v<T, VkPhysicalDevicePresentTimingFeaturesEXT>) {
        return offsetof(VkPhysicalDevicePresentTimingFeaturesEXT, presentAtRelativeTime) + sizeof(VkBool32);
    } else if constexpr (std::is_same_v<T, VkPhysicalDevicePresentId2FeaturesKHR>) {
        return offsetof(VkPhysicalDevicePresentId2FeaturesKHR, presentId2) + sizeof(VkBool32);
    } else {
        static_assert(sizeof(T) == 0, "Vulkan feature payload not registered for this type");
    }
}

template <typename T>
[[nodiscard]] consteval auto FeaturePayloadStart() noexcept -> size_t {
    static_assert(std::is_standard_layout_v<T>);
    constexpr size_t start = offsetof(T, pNext) + sizeof(void*);
    constexpr size_t end   = FeaturePayloadEnd<T>();
    static_assert(start < end && end <= sizeof(T) && (end - start) % sizeof(VkBool32) == 0);
    return start;
}

template <typename T>
[[nodiscard]] inline auto ReadFeatureBit(const T& feature, size_t offset) noexcept -> VkBool32 {
    VkBool32 bit = VK_FALSE;
    std::memcpy(&bit, reinterpret_cast<const char*>(&feature) + offset, sizeof(bit));
    return bit;
}

template <typename T>
inline void WriteFeatureBit(T& feature, size_t offset, VkBool32 bit) noexcept {
    std::memcpy(reinterpret_cast<char*>(&feature) + offset, &bit, sizeof(bit));
}

// A Features2 struct is the root of a vkGetPhysicalDeviceFeatures2 query,
// never an element of its own pNext chain.
template <typename T>
[[nodiscard]] inline auto QueryFeatureSupport(VkPhysicalDevice physicalDevice) noexcept -> T {
    T features {};
    features.sType = GetStructureType<T>();
    if (physicalDevice == VK_NULL_HANDLE) {
        return features;
    }
    if constexpr (std::is_same_v<T, VkPhysicalDeviceFeatures2>) {
        vkGetPhysicalDeviceFeatures2(physicalDevice, &features);
    } else {
        VkPhysicalDeviceFeatures2 root {};
        root.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        root.pNext = &features;
        vkGetPhysicalDeviceFeatures2(physicalDevice, &root);
        features.pNext = nullptr;
    }
    return features;
}

template <typename T>
[[nodiscard]] inline auto IsSubsetOf(const T& requested, const T& supported) noexcept -> bool {
    constexpr size_t start = FeaturePayloadStart<T>();
    constexpr size_t end   = FeaturePayloadEnd<T>();
    for (size_t offset = start; offset < end; offset += sizeof(VkBool32)) {
        if (ReadFeatureBit(requested, offset) != VK_FALSE && ReadFeatureBit(supported, offset) == VK_FALSE) {
            return false;
        }
    }
    return true;
}

template <typename T>
[[nodiscard]] inline auto MaskFeatures(const T& requested, const T& supported) noexcept -> T {
    T result {};
    result.sType = GetStructureType<T>();
    constexpr size_t start = FeaturePayloadStart<T>();
    constexpr size_t end   = FeaturePayloadEnd<T>();
    for (size_t offset = start; offset < end; offset += sizeof(VkBool32)) {
        const VkBool32 bit = ReadFeatureBit(requested, offset) != VK_FALSE && ReadFeatureBit(supported, offset) != VK_FALSE ? VK_TRUE : VK_FALSE;
        WriteFeatureBit(result, offset, bit);
    }
    return result;
}

template <typename T>
[[nodiscard]] inline auto HasAnyEnabledFeature(const T& feature) noexcept -> bool {
    constexpr size_t start = FeaturePayloadStart<T>();
    constexpr size_t end   = FeaturePayloadEnd<T>();
    for (size_t offset = start; offset < end; offset += sizeof(VkBool32)) {
        if (ReadFeatureBit(feature, offset) != VK_FALSE) {
            return true;
        }
    }
    return false;
}

template <typename... Ts>
template <typename T, typename Func, typename Predicate>
auto FeatureChain<Ts...>::Optional(Func&& configure, Predicate&& accept, bool available) && {
    T requested {};
    std::forward<Func>(configure)(requested);

    T    actual {};
    bool active = false;
    if (available && _physicalDevice != VK_NULL_HANDLE) {
        actual = MaskFeatures(requested, QueryFeatureSupport<T>(_physicalDevice));
        active = HasAnyEnabledFeature(actual) && std::forward<Predicate>(accept)(_physicalDevice, actual);
    }
    return std::move(*this).Add(FeatureNode<T> {.feature = actual, .active = active});
}

template <typename... Ts>
FeatureChain<Ts...>& FeatureChain<Ts...>::Build() {
    return *this;
}

template <typename... Ts>
const VkPhysicalDeviceFeatures2* FeatureChain<Ts...>::GetRoot(const VkPhysicalDeviceFeatures2* tail) {
    const VkPhysicalDeviceFeatures2* root = tail;
    bool anyActive = false;
    std::apply(
        [&](auto&... nodes) {
            auto link = [&](auto& node) {
                if (!node.active) {
                    return;
                }
                using FeatureType = std::remove_reference_t<decltype(node.feature)>;
                node.feature.sType = GetStructureType<FeatureType>();
                node.feature.pNext = const_cast<VkPhysicalDeviceFeatures2*>(root);
                root = reinterpret_cast<const VkPhysicalDeviceFeatures2*>(&node.feature);
                anyActive = true;
            };
            (link(nodes), ...);
        },
        _features
    );
    // A caller that supplied a tail already owns it. Report only nodes from
    // this chain, so it can choose the tail when all optional nodes were off.
    return anyActive ? root : nullptr;
}

template <typename... Ts>
[[nodiscard]] auto FeatureChain<Ts...>::SnapshotEnabled() const -> EnabledFeatureSet {
    EnabledFeatureSet out;
    std::apply(
        [&out](const auto&... nodes) {
            // One lambda call per node; the inactive ones contribute nothing, so
            // the snapshot is exactly the set GetRoot() chained.
            (
                [&] {
                    if (!nodes.active) {
                        return;
                    }
                    using Feature      = std::remove_cvref_t<decltype(nodes.feature)>;
                    constexpr size_t kWords = (sizeof(Feature) + sizeof(uint64_t) - 1) / sizeof(uint64_t);

                    EnabledFeature entry;
                    entry.sType = GetStructureType<Feature>();
                    entry.words.resize(kWords);
                    std::memcpy(entry.words.data(), &nodes.feature, sizeof(Feature));

                    // The copy's pNext still points into this chain, which the
                    // snapshot deliberately outlives. Nothing walks these as a
                    // chain -- lookup is by sType -- so null it rather than let
                    // a reader find a dangling pointer.
                    reinterpret_cast<Feature*>(entry.words.data())->pNext = nullptr;

                    out.push_back(std::move(entry));
                }(),
                ...
            );
        },
        _features
    );
    return out;
}

// FeatureChainBuilder Implementation

template <typename T, typename Func>
auto FeatureChainBuilder::Require(Func&& configure) {
    return FeatureChain<>(_physicalDevice, std::make_tuple()).template Require<T>(std::forward<Func>(configure));
}

template <typename T, typename Func>
auto FeatureChainBuilder::Optional(Func&& configure) {
    return FeatureChain<>(_physicalDevice, std::make_tuple()).template Optional<T>(std::forward<Func>(configure));
}

// FeatureFactory Implementation

template <typename T>
[[nodiscard]] constexpr auto FeatureFactory::Create(auto&& configure) noexcept -> T {
    T features {};
    features.sType = GetStructureType<T>();
    configure(features);
    return features;
}

} // namespace ZHLN::Vk
