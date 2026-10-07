// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Context.hpp"
#include "Extensions.hpp"
#include "../diagnostics/GPUAddressTracker.hpp"
#include <Zahlen/Log.hpp>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace ZHLN::Vk {

Context::~Context() noexcept {
    // Keep the address tracker enabled through device destruction so that the
    // driver's final unbind notifications can retire their ranges.
    _device.Reset();
    if (_addressBindingReportEnabled) {
        GPUAddressTracker::Get().SetEnabled(false);
    }
}

Context::Context(Context&& other) noexcept:
    _instanceObject(std::move(other._instanceObject)),
    _surface(std::exchange(other._surface, VK_NULL_HANDLE)),
    _physical(std::exchange(other._physical, PhysicalDeviceInfo {})),
    _device(std::move(other._device)),
    _present(other._present),
    _enabledFeatures(std::move(other._enabledFeatures)),
    _addressBindingReportEnabled(std::exchange(other._addressBindingReportEnabled, false)) {}

auto Context::operator=(Context&& other) noexcept -> Context& {
    if (this != &other) {
        const bool previousAddressReport = _addressBindingReportEnabled;
        const bool incomingAddressReport = other._addressBindingReportEnabled;

        _device.Reset();
        if (previousAddressReport && !incomingAddressReport) {
            GPUAddressTracker::Get().SetEnabled(false);
        }

        _instanceObject = std::move(other._instanceObject);
        _surface        = std::exchange(other._surface, VK_NULL_HANDLE);
        _physical       = std::exchange(other._physical, PhysicalDeviceInfo {});
        _device         = std::move(other._device);
        _present        = other._present;
        _enabledFeatures = std::move(other._enabledFeatures);
        _addressBindingReportEnabled = std::exchange(other._addressBindingReportEnabled, false);

        if (_addressBindingReportEnabled && !previousAddressReport) {
            GPUAddressTracker::Get().SetEnabled(true);
        }
    }
    return *this;
}

namespace {

[[nodiscard]] auto ExtensionEnabled(const std::span<const std::string> enabled, const std::string_view name) noexcept -> bool {
    return std::ranges::any_of(enabled, [name](const std::string& entry) { return entry == name; });
}

[[nodiscard]] auto FeatureBitEnabled(const VkPhysicalDeviceFeatures2* root, const VkStructureType sType, const size_t bitOffset) noexcept
    -> bool {
    // Chain elements are different Vulkan struct types; reading them through
    // an unrelated C++ struct pointer violates strict aliasing. The common
    // sType/pNext header and target VkBool32 field are inspected as bytes.
    for (const void* cursor = root; cursor != nullptr;) {
        VkStructureType type = VK_STRUCTURE_TYPE_MAX_ENUM;
        const void*     next = nullptr;
        std::memcpy(&type, cursor, sizeof(type));
        std::memcpy(&next, static_cast<const char*>(cursor) + offsetof(VkPhysicalDeviceFeatures2, pNext), sizeof(next));
        if (type == sType) {
            VkBool32 bit = VK_FALSE;
            std::memcpy(&bit, static_cast<const char*>(cursor) + bitOffset, sizeof(bit));
            return bit == VK_TRUE;
        }
        cursor = next;
    }
    return false;
}

[[nodiscard]] auto ScanPresentSupport(
    const std::span<const std::string> extensions,
    const VkPhysicalDeviceFeatures2* features
) noexcept -> DevicePresentSupport {
    const bool fifoExt = ExtensionEnabled(extensions, VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME) ||
                         ExtensionEnabled(extensions, VK_EXT_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME);
    const bool timingExt = ExtensionEnabled(extensions, VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
    const bool id2Ext    = ExtensionEnabled(extensions, VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
    const bool calibExt  = ExtensionEnabled(extensions, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) ||
                           ExtensionEnabled(extensions, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    const bool timingGroup = timingExt && id2Ext && calibExt && vkGetPhysicalDeviceSurfaceCapabilities2KHR != nullptr;

    const bool fifoBit = FeatureBitEnabled(
        features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR,
        offsetof(VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR, presentModeFifoLatestReady)
    );
    const bool timingBit = FeatureBitEnabled(
        features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT,
        offsetof(VkPhysicalDevicePresentTimingFeaturesEXT, presentTiming)
    );
    const bool absoluteBit = FeatureBitEnabled(
        features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT,
        offsetof(VkPhysicalDevicePresentTimingFeaturesEXT, presentAtAbsoluteTime)
    );
    const bool id2Bit = FeatureBitEnabled(
        features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR,
        offsetof(VkPhysicalDevicePresentId2FeaturesKHR, presentId2)
    );

    return DevicePresentSupport {
        .fifoLatestReady       = fifoExt && fifoBit,
        .presentTiming         = timingGroup && timingBit,
        .presentAtAbsoluteTime = timingGroup && absoluteBit,
        .presentId2            = timingGroup && id2Bit,
    };
}

[[nodiscard]] auto HasAllEnabled(const std::span<const std::string> extensions, const std::initializer_list<std::string_view> names) noexcept
    -> bool {
    return std::ranges::all_of(names, [extensions](const std::string_view name) { return ExtensionEnabled(extensions, name); });
}

[[nodiscard]] auto BuildLogicalDevice(
    const PhysicalDeviceInfo& physical,
    const std::span<const std::string> extensionNames,
    const VkPhysicalDeviceFeatures2* features,
    const bool meshShaderRequested,
    const bool rayTracingRequested
) noexcept -> std::expected<LogicalDevice, VkResult> {
    if (physical.handle == VK_NULL_HANDLE || !physical.has_graphics || physical.graphics_family == UINT32_MAX ||
        physical.present_family == UINT32_MAX || physical.transfer_family == UINT32_MAX || physical.compute_family == UINT32_MAX) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    const std::array<uint32_t, 4> candidates {
        physical.graphics_family, physical.present_family, physical.transfer_family, physical.compute_family
    };
    std::array<uint32_t, candidates.size()> uniqueFamilies {};
    uint32_t uniqueFamilyCount = 0;
    for (const uint32_t candidate: candidates) {
        const bool duplicate = std::ranges::any_of(
            std::span(uniqueFamilies.data(), uniqueFamilyCount), [candidate](const uint32_t family) { return family == candidate; }
        );
        if (!duplicate) {
            uniqueFamilies[uniqueFamilyCount++] = candidate;
        }
    }

    constexpr float priority = 1.0F;
    std::array<VkDeviceQueueCreateInfo, candidates.size()> queueInfos {};
    for (uint32_t i = 0; i < uniqueFamilyCount; ++i) {
        queueInfos[i] = VkDeviceQueueCreateInfo {
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext            = nullptr,
            .flags            = 0,
            .queueFamilyIndex = uniqueFamilies[i],
            .queueCount       = 1,
            .pQueuePriorities = &priority,
        };
    }

    const VkPhysicalDeviceFeatures2 defaultFeatures {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
    };
    const VkPhysicalDeviceFeatures2* enabledFeatures = features != nullptr ? features : &defaultFeatures;
    std::vector<const char*> extensionPointers;
    extensionPointers.reserve(extensionNames.size());
    for (const std::string& name: extensionNames) {
        extensionPointers.push_back(name.c_str());
    }

    const VkDeviceCreateInfo createInfo {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = enabledFeatures,
        .flags                   = 0,
        .queueCreateInfoCount    = uniqueFamilyCount,
        .pQueueCreateInfos       = queueInfos.data(),
        .enabledLayerCount       = 0,
        .ppEnabledLayerNames     = nullptr,
        .enabledExtensionCount   = static_cast<uint32_t>(extensionPointers.size()),
        .ppEnabledExtensionNames = extensionPointers.empty() ? nullptr : extensionPointers.data(),
        .pEnabledFeatures        = nullptr,
    };

    VkDevice handle = VK_NULL_HANDLE;
    const VkResult created = vkCreateDevice(physical.handle, &createInfo, nullptr, &handle);
    if (created != VK_SUCCESS) {
        return std::unexpected(created);
    }

    volkLoadDevice(handle);

    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue  = VK_NULL_HANDLE;
    VkQueue transferQueue = VK_NULL_HANDLE;
    VkQueue computeQueue  = VK_NULL_HANDLE;
    vkGetDeviceQueue(handle, physical.graphics_family, 0, &graphicsQueue);
    vkGetDeviceQueue(handle, physical.present_family, 0, &presentQueue);
    vkGetDeviceQueue(handle, physical.transfer_family, 0, &transferQueue);
    vkGetDeviceQueue(handle, physical.compute_family, 0, &computeQueue);

    const bool descriptorHeapEnabled = vkCmdBindResourceHeapEXT != nullptr && vkCmdBindSamplerHeapEXT != nullptr &&
        vkCmdPushDataEXT != nullptr && vkWriteResourceDescriptorsEXT != nullptr && vkWriteSamplerDescriptorsEXT != nullptr;
    if (!descriptorHeapEnabled) {
        ZHLN::LogWarning("[Vulkan] VK_EXT_descriptor_heap entry points are unavailable; descriptor-heap paths are disabled.");
    }

    const MeshShaderLimits meshLimits = QueryMeshShaderLimits(physical.handle);
    const bool meshEntryPointsAvailable = vkCmdDrawMeshTasksEXT != nullptr && vkCmdDrawMeshTasksIndirectEXT != nullptr;
    const bool meshShaderEnabled = meshShaderRequested && meshEntryPointsAvailable && MeshShaderLimitsSufficient(meshLimits);
    if (meshShaderRequested && !meshShaderEnabled) {
        if (!meshLimits.supported) {
            ZHLN::Log("[Vulkan] VK_EXT_mesh_shader is not supported by the physical device; using the vertex pipeline.");
        } else if (!meshEntryPointsAvailable) {
            ZHLN::LogWarning("[Vulkan] VK_EXT_mesh_shader entry points are unavailable; using the vertex pipeline.");
        } else {
            ZHLN::Log(
                "[Vulkan] Mesh shader limits below engine budget (vertices={}/64, primitives={}/124, taskInvocations={}/32, meshInvocations={}/64); using vertex pipelines.",
                meshLimits.max_mesh_output_vertices, meshLimits.max_mesh_output_primitives, meshLimits.max_task_work_group_invocations,
                meshLimits.max_mesh_work_group_invocations
            );
        }
    }

    const bool rayTracingExtensions = HasAllEnabled(extensionNames, {
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    });
    const bool rayTracingEnabled = rayTracingRequested && rayTracingExtensions;

    return LogicalDevice {
        handle, graphicsQueue, presentQueue, transferQueue, computeQueue,
        descriptorHeapEnabled, meshShaderEnabled, rayTracingEnabled
    };
}

} // namespace

std::expected<Vk::Instance, ErrorCode> Context::Builder::BuildInstance() noexcept {
    _instanceObject = Instance::Create(_appName, _appVersion, _instanceExtensions, _validationMode);
    if (!_instanceObject.Valid()) {
        return std::unexpected(ContextError::InstanceCreationFailed);
    }
    _instanceView = _instanceObject.Handle();
    return std::move(_instanceObject);
}

auto Context::Builder::SelectPhysicalDevice() const noexcept -> std::expected<PhysicalDeviceInfo, ErrorCode> {
    const VkInstance view = _instanceView != VK_NULL_HANDLE ? _instanceView : _instanceObject.Handle();
    PhysicalDeviceInfo info = ZHLN::Vk::SelectPhysicalDevice(view, _surface, _scoreFn, _scoreUserdata);
    if (info.handle == VK_NULL_HANDLE) {
        return std::unexpected(ContextError::NoSuitableDeviceFound);
    }
    return info;
}

std::expected<Context, ErrorCode> Context::Builder::Build() noexcept {
    Context context;
    context._surface  = _surface;
    context._physical = _physical;

    auto configured = DeviceConfigurator<>(_physical.handle)
        .RequireExtension(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME)
        .RequireExtension<VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT>(
            VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME,
            [](auto& feature) { feature.dynamicRenderingUnusedAttachments = VK_TRUE; }
        )
        .OptionalExtension<VkPhysicalDeviceRobustness2FeaturesEXT>(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, [this](auto& feature) {
            feature.nullDescriptor = VK_TRUE;
            if (_validationMode == ValidationMode::GPU) {
                feature.robustBufferAccess2 = VK_TRUE;
                feature.robustImageAccess2  = VK_TRUE;
            }
        })
        .OptionalExtension<VkPhysicalDeviceFaultFeaturesKHR>(VK_KHR_DEVICE_FAULT_EXTENSION_NAME, [](auto& feature) {
            feature.deviceFault                   = VK_TRUE;
            feature.deviceFaultVendorBinary       = VK_TRUE;
            feature.deviceFaultReportMasked       = VK_TRUE;
            feature.deviceFaultDeviceLostOnMasked = VK_TRUE;
        }, [](VkPhysicalDevice, const auto& enabled) { return enabled.deviceFault == VK_TRUE; })
        .OptionalExtension<VkPhysicalDeviceFaultFeaturesEXT>(VK_EXT_DEVICE_FAULT_EXTENSION_NAME, [](auto& feature) {
            feature.deviceFault             = VK_TRUE;
            feature.deviceFaultVendorBinary = VK_TRUE;
        }, [](VkPhysicalDevice, const auto& enabled) { return enabled.deviceFault == VK_TRUE; })
        .OptionalExtension<VkPhysicalDeviceAddressBindingReportFeaturesEXT>(
            VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME,
            [](auto& feature) { feature.reportAddressBinding = VK_TRUE; },
            [](VkPhysicalDevice, const auto& enabled) { return enabled.reportAddressBinding == VK_TRUE; },
            _validationMode != ValidationMode::Off && _instanceObject.HasAddressBindingMessenger() &&
                vkCreateDebugUtilsMessengerEXT != nullptr
        )
        .OptionalExtension<VkPhysicalDeviceShaderConstantDataFeaturesKHR>(
            VK_KHR_SHADER_CONSTANT_DATA_EXTENSION_NAME, [](auto& feature) { feature.shaderConstantData = VK_TRUE; }
        )
        .OptionalExtension<VkPhysicalDeviceShaderAbortFeaturesKHR>(
            VK_KHR_SHADER_ABORT_EXTENSION_NAME, [](auto& feature) { feature.shaderAbort = VK_TRUE; }
        )
        .Build();
    if (!configured) {
        return std::unexpected(configured.error());
    }

    std::vector<const char*> requestedExtensions = _deviceExtensions;
    const std::vector<const char*>& configuredExtensions = configured->extensions;
    requestedExtensions.insert(requestedExtensions.end(), configuredExtensions.begin(), configuredExtensions.end());

    const VkPhysicalDeviceFeatures2* configuredRoot = configured->features.GetRoot(_features);
    const VkPhysicalDeviceFeatures2* featureRoot = configuredRoot != nullptr ? configuredRoot : _features;

    const std::vector<VkExtensionProperties> availableExtensions = EnumerateDeviceExtensions(_physical.handle);
    std::vector<std::string> enabledExtensions;
    enabledExtensions.reserve(requestedExtensions.size());
    for (const char* requested: requestedExtensions) {
        if (requested == nullptr) {
            continue;
        }
        const std::string_view name(requested);
        if (!HasExtension(availableExtensions, name)) {
            ZHLN::LogWarning("[Vulkan] Skipping unsupported device extension: {}", name);
            continue;
        }
        if (!std::ranges::contains(enabledExtensions, name)) {
            enabledExtensions.emplace_back(name);
        }
    }

    const bool addressBindingReportEnabled = _validationMode != ValidationMode::Off &&
        _instanceObject.HasAddressBindingMessenger() && vkCreateDebugUtilsMessengerEXT != nullptr &&
        ExtensionEnabled(enabledExtensions, VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME) &&
        FeatureBitEnabled(
            featureRoot, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT,
            offsetof(VkPhysicalDeviceAddressBindingReportFeaturesEXT, reportAddressBinding)
        );

    GPUAddressTracker::Get().SetEnabled(addressBindingReportEnabled);
    auto logicalDevice = BuildLogicalDevice(
        _physical, enabledExtensions, featureRoot,
        FeatureBitEnabled(
            featureRoot, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
            offsetof(VkPhysicalDeviceMeshShaderFeaturesEXT, meshShader)
        ),
        FeatureBitEnabled(
            featureRoot, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
            offsetof(VkPhysicalDeviceAccelerationStructureFeaturesKHR, accelerationStructure)
        ) && FeatureBitEnabled(
            featureRoot, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
            offsetof(VkPhysicalDeviceRayQueryFeaturesKHR, rayQuery)
        )
    );
    if (!logicalDevice) {
        GPUAddressTracker::Get().SetEnabled(false);
        return std::unexpected(ToFrameError(logicalDevice.error()));
    }

    context._device = std::move(*logicalDevice);
    context._addressBindingReportEnabled = addressBindingReportEnabled;
    context._present = ScanPresentSupport(enabledExtensions, featureRoot);

    context._enabledFeatures = std::move(_enabledFeatures);
    for (EnabledFeature& entry: configured->features.SnapshotEnabled()) {
        context._enabledFeatures.push_back(std::move(entry));
    }

    if (!_instanceObject.Valid()) {
        return std::unexpected(ContextError::InstanceCreationFailed);
    }
    context._instanceObject = std::move(_instanceObject);
    return context;
}

} // namespace ZHLN::Vk
