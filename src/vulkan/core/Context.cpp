// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Context.hpp"
#include "../diagnostics/GPUAddressTracker.hpp"
#include "Extensions.hpp"
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
    _instanceObject(std::move(other._instanceObject)), _surface(std::exchange(other._surface, VK_NULL_HANDLE)),
    _physical(std::exchange(other._physical, PhysicalDeviceInfo {})), _device(std::move(other._device)), _present(other._present),
    _enabledFeatures(std::move(other._enabledFeatures)), _addressBindingReportEnabled(std::exchange(other._addressBindingReportEnabled, false)) {
}

auto Context::operator=(Context&& other) noexcept -> Context& {
    if (this != &other) {
        const bool previous_address_report = _addressBindingReportEnabled;
        const bool incoming_address_report = other._addressBindingReportEnabled;

        _device.Reset();
        if (previous_address_report && !incoming_address_report) {
            GPUAddressTracker::Get().SetEnabled(false);
        }

        _instanceObject              = std::move(other._instanceObject);
        _surface                     = std::exchange(other._surface, VK_NULL_HANDLE);
        _physical                    = std::exchange(other._physical, PhysicalDeviceInfo {});
        _device                      = std::move(other._device);
        _present                     = other._present;
        _enabledFeatures             = std::move(other._enabledFeatures);
        _addressBindingReportEnabled = std::exchange(other._addressBindingReportEnabled, false);

        if (_addressBindingReportEnabled && !previous_address_report) {
            GPUAddressTracker::Get().SetEnabled(true);
        }
    }
    return *this;
}

namespace {

[[nodiscard]] auto ExtensionEnabled(const std::span<const std::string> enabled, const std::string_view name) noexcept -> bool {
    return std::ranges::any_of(enabled, [name](const std::string& entry) { return entry == name; });
}

[[nodiscard]] auto FeatureBitEnabled(const VkPhysicalDeviceFeatures2* root, const VkStructureType sType, const size_t bitOffset) noexcept -> bool {
    // Chain elements are different Vulkan struct types; reading them through
    // an unrelated C++ struct pointer violates strict aliasing. The common
    // sType/pNext header and target VkBool32 field are inspected as bytes.
    for (const void* cursor = root; cursor != nullptr;) {
        VkStructureType type = VK_STRUCTURE_TYPE_MAX_ENUM;
        const void*     next = nullptr;
        std::memcpy(&type, cursor, sizeof(type));
        std::memcpy(static_cast<void*>(&next), static_cast<const char*>(cursor) + offsetof(VkPhysicalDeviceFeatures2, pNext), sizeof(next));
        if (type == sType) {
            VkBool32 bit = VK_FALSE;
            std::memcpy(&bit, static_cast<const char*>(cursor) + bitOffset, sizeof(bit));
            return bit == VK_TRUE;
        }
        cursor = next;
    }
    return false;
}

[[nodiscard]] auto
    ScanPresentSupport(const std::span<const std::string> extensions, const VkPhysicalDeviceFeatures2* features) noexcept -> DevicePresentSupport {
    const bool fifo_ext     = ExtensionEnabled(extensions, VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME) ||
                              ExtensionEnabled(extensions, VK_EXT_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME);
    const bool timing_ext   = ExtensionEnabled(extensions, VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
    const bool id2_ext      = ExtensionEnabled(extensions, VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
    const bool calib_ext    = ExtensionEnabled(extensions, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) ||
                              ExtensionEnabled(extensions, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    const bool timing_group = timing_ext && id2_ext && calib_ext && vkGetPhysicalDeviceSurfaceCapabilities2KHR != nullptr;

    const bool fifo_bit = FeatureBitEnabled(
        features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR,
        offsetof(VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR, presentModeFifoLatestReady)
    );
    const bool timing_bit = FeatureBitEnabled(
        features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT, offsetof(VkPhysicalDevicePresentTimingFeaturesEXT, presentTiming)
    );
    const bool absolute_bit = FeatureBitEnabled(
        features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT, offsetof(VkPhysicalDevicePresentTimingFeaturesEXT, presentAtAbsoluteTime)
    );
    const bool id2_bit =
        FeatureBitEnabled(features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, offsetof(VkPhysicalDevicePresentId2FeaturesKHR, presentId2));

    return DevicePresentSupport {
        .fifoLatestReady       = fifo_ext && fifo_bit,
        .presentTiming         = timing_group && timing_bit,
        .presentAtAbsoluteTime = timing_group && absolute_bit,
        .presentId2            = timing_group && id2_bit,
    };
}

[[nodiscard]] auto HasAllEnabled(const std::span<const std::string> extensions, const std::initializer_list<std::string_view> names) noexcept -> bool {
    return std::ranges::all_of(names, [extensions](const std::string_view name) { return ExtensionEnabled(extensions, name); });
}

[[nodiscard]] auto BuildLogicalDevice(
    const PhysicalDeviceInfo&          physical,
    const std::span<const std::string> extensionNames,
    const VkPhysicalDeviceFeatures2*   features,
    const bool                         meshShaderRequested,
    const bool                         rayTracingRequested
) noexcept -> std::expected<LogicalDevice, VkResult> // TODO(Evilpasture): Change E return to Error instead of VkResult.
{
    if (physical.handle == VK_NULL_HANDLE || !physical.has_graphics || physical.graphics_family == UINT32_MAX || physical.present_family == UINT32_MAX ||
        physical.transfer_family == UINT32_MAX || physical.compute_family == UINT32_MAX) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED); // TODO(Evilpasture): This is lying to the upper stack. Remove this whenever possible.
    }

    const std::array<uint32_t, 4>           candidates {physical.graphics_family, physical.present_family, physical.transfer_family, physical.compute_family};
    std::array<uint32_t, candidates.size()> unique_families {};
    uint32_t                                unique_family_count = 0;
    for (const uint32_t candidate: candidates) {
        const bool duplicate =
            std::ranges::any_of(std::span(unique_families.data(), unique_family_count), [candidate](const uint32_t family) { return family == candidate; });
        if (!duplicate) {
            unique_families[unique_family_count++] = candidate;
        }
    }

    constexpr float                                        priority = 1.0F;
    std::array<VkDeviceQueueCreateInfo, candidates.size()> queue_infos {};
    for (uint32_t i = 0; i < unique_family_count; ++i) {
        queue_infos[i] = VkDeviceQueueCreateInfo {
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext            = nullptr,
            .flags            = 0,
            .queueFamilyIndex = unique_families[i],
            .queueCount       = 1,
            .pQueuePriorities = &priority,
        };
    }

    const VkPhysicalDeviceFeatures2 default_features {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
    };
    const VkPhysicalDeviceFeatures2* enabled_features = features != nullptr ? features : &default_features;
    std::vector<const char*>         extension_pointers;
    extension_pointers.reserve(extensionNames.size());
    for (const std::string& name: extensionNames) {
        extension_pointers.push_back(name.c_str());
    }

    const VkDeviceCreateInfo create_info {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = enabled_features,
        .flags                   = 0,
        .queueCreateInfoCount    = unique_family_count,
        .pQueueCreateInfos       = queue_infos.data(),
        .enabledLayerCount       = 0,
        .ppEnabledLayerNames     = nullptr,
        .enabledExtensionCount   = static_cast<uint32_t>(extension_pointers.size()),
        .ppEnabledExtensionNames = extension_pointers.empty() ? nullptr : extension_pointers.data(),
        .pEnabledFeatures        = nullptr,
    };

    VkDevice       handle  = VK_NULL_HANDLE;
    const VkResult created = vkCreateDevice(physical.handle, &create_info, nullptr, &handle);
    if (created != VK_SUCCESS) {
        return std::unexpected(created);
    }

    volkLoadDevice(handle);

    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkQueue present_queue  = VK_NULL_HANDLE;
    VkQueue transfer_queue = VK_NULL_HANDLE;
    VkQueue compute_queue  = VK_NULL_HANDLE;
    vkGetDeviceQueue(handle, physical.graphics_family, 0, &graphics_queue);
    vkGetDeviceQueue(handle, physical.present_family, 0, &present_queue);
    vkGetDeviceQueue(handle, physical.transfer_family, 0, &transfer_queue);
    vkGetDeviceQueue(handle, physical.compute_family, 0, &compute_queue);

    const bool descriptor_heap_enabled = vkCmdBindResourceHeapEXT != nullptr && vkCmdBindSamplerHeapEXT != nullptr && vkCmdPushDataEXT != nullptr &&
                                         vkWriteResourceDescriptorsEXT != nullptr && vkWriteSamplerDescriptorsEXT != nullptr;
    if (!descriptor_heap_enabled) {
        ZHLN::LogWarning("[Vulkan] VK_EXT_descriptor_heap entry points are unavailable; descriptor-heap paths are disabled.");
    }

    const MeshShaderLimits mesh_limits                 = QueryMeshShaderLimits(physical.handle);
    const bool             mesh_entry_points_available = vkCmdDrawMeshTasksEXT != nullptr && vkCmdDrawMeshTasksIndirectEXT != nullptr;
    const bool             mesh_shader_enabled         = meshShaderRequested && mesh_entry_points_available && MeshShaderLimitsSufficient(mesh_limits);
    if (meshShaderRequested && !mesh_shader_enabled) {
        if (!mesh_limits.supported) {
            ZHLN::Log("[Vulkan] VK_EXT_mesh_shader is not supported by the physical device; using the vertex pipeline.");
        } else if (!mesh_entry_points_available) {
            ZHLN::LogWarning("[Vulkan] VK_EXT_mesh_shader entry points are unavailable; using the vertex pipeline.");
        } else {
            ZHLN::Log(
                "[Vulkan] Mesh shader limits below engine budget (vertices={}/64, primitives={}/124, taskInvocations={}/32, meshInvocations={}/64); using "
                "vertex pipelines.",
                mesh_limits.max_mesh_output_vertices, mesh_limits.max_mesh_output_primitives, mesh_limits.max_task_work_group_invocations,
                mesh_limits.max_mesh_work_group_invocations
            );
        }
    }

    const bool ray_tracing_extensions = HasAllEnabled(
        extensionNames, {
                            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                            VK_KHR_RAY_QUERY_EXTENSION_NAME,
                            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                        }
    );
    const bool ray_tracing_enabled = rayTracingRequested && ray_tracing_extensions;

    return LogicalDevice {handle,        graphics_queue,          present_queue,       transfer_queue,
                          compute_queue, descriptor_heap_enabled, mesh_shader_enabled, ray_tracing_enabled};
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
    const VkInstance   view = _instanceView != VK_NULL_HANDLE ? _instanceView : _instanceObject.Handle();
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

    auto configured =
        DeviceConfigurator<>(_physical.handle)
            .RequireExtension(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME)
            .RequireExtension<VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT>(
                VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME, [](auto& feature) { feature.dynamicRenderingUnusedAttachments = VK_TRUE; }
            )
            .OptionalExtension<VkPhysicalDeviceRobustness2FeaturesEXT>(
                VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
                [this](auto& feature) {
                    feature.nullDescriptor = VK_TRUE;
                    if (_validationMode == ValidationMode::GPU) {
                        feature.robustBufferAccess2 = VK_TRUE;
                        feature.robustImageAccess2  = VK_TRUE;
                    }
                }
            )
            .OptionalExtension<VkPhysicalDeviceFaultFeaturesKHR>(
                VK_KHR_DEVICE_FAULT_EXTENSION_NAME,
                [](auto& feature) {
                    feature.deviceFault                   = VK_TRUE;
                    feature.deviceFaultVendorBinary       = VK_TRUE;
                    feature.deviceFaultReportMasked       = VK_TRUE;
                    feature.deviceFaultDeviceLostOnMasked = VK_TRUE;
                },
                [](VkPhysicalDevice, const auto& enabled) { return enabled.deviceFault == VK_TRUE; }
            )
            .OptionalExtension<VkPhysicalDeviceFaultFeaturesEXT>(
                VK_EXT_DEVICE_FAULT_EXTENSION_NAME,
                [](auto& feature) {
                    feature.deviceFault             = VK_TRUE;
                    feature.deviceFaultVendorBinary = VK_TRUE;
                },
                [](VkPhysicalDevice, const auto& enabled) { return enabled.deviceFault == VK_TRUE; }
            )
            .OptionalExtension<VkPhysicalDeviceAddressBindingReportFeaturesEXT>(
                VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME, [](auto& feature) { feature.reportAddressBinding = VK_TRUE; },
                [](VkPhysicalDevice, const auto& enabled) { return enabled.reportAddressBinding == VK_TRUE; },
                _validationMode != ValidationMode::Off && _instanceObject.HasAddressBindingMessenger() && vkCreateDebugUtilsMessengerEXT != nullptr
            )
            .OptionalExtension<VkPhysicalDeviceShaderConstantDataFeaturesKHR>(
                VK_KHR_SHADER_CONSTANT_DATA_EXTENSION_NAME, [](auto& feature) { feature.shaderConstantData = VK_TRUE; }
            )
            .OptionalExtension<VkPhysicalDeviceShaderAbortFeaturesKHR>(VK_KHR_SHADER_ABORT_EXTENSION_NAME, [](auto& feature) { feature.shaderAbort = VK_TRUE; })
            .Build();
    if (!configured) {
        return std::unexpected(configured.error());
    }

    std::vector<const char*>        requested_extensions  = _deviceExtensions;
    const std::vector<const char*>& configured_extensions = configured->extensions;
    requested_extensions.insert(requested_extensions.end(), configured_extensions.begin(), configured_extensions.end());

    const VkPhysicalDeviceFeatures2* configured_root = configured->features.GetRoot(_features);
    const VkPhysicalDeviceFeatures2* feature_root    = configured_root != nullptr ? configured_root : _features;

    const std::vector<VkExtensionProperties> available_extensions = EnumerateDeviceExtensions(_physical.handle);
    std::vector<std::string>                 enabled_extensions;
    enabled_extensions.reserve(requested_extensions.size());
    for (const char* requested: requested_extensions) {
        if (requested == nullptr) {
            continue;
        }
        const std::string_view name(requested);
        if (!HasExtension(available_extensions, name)) {
            ZHLN::LogWarning("[Vulkan] Skipping unsupported device extension: {}", name);
            continue;
        }
        if (!std::ranges::contains(enabled_extensions, name)) {
            enabled_extensions.emplace_back(name);
        }
    }

    const bool address_binding_report_enabled = _validationMode != ValidationMode::Off && _instanceObject.HasAddressBindingMessenger() &&
                                                vkCreateDebugUtilsMessengerEXT != nullptr &&
                                                ExtensionEnabled(enabled_extensions, VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME) &&
                                                FeatureBitEnabled(
                                                    feature_root, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT,
                                                    offsetof(VkPhysicalDeviceAddressBindingReportFeaturesEXT, reportAddressBinding)
                                                );

    GPUAddressTracker::Get().SetEnabled(address_binding_report_enabled);
    auto logical_device = BuildLogicalDevice(
        _physical, enabled_extensions, feature_root,
        FeatureBitEnabled(
            feature_root, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, offsetof(VkPhysicalDeviceMeshShaderFeaturesEXT, meshShader)
        ),
        FeatureBitEnabled(
            feature_root, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
            offsetof(VkPhysicalDeviceAccelerationStructureFeaturesKHR, accelerationStructure)
        ) && FeatureBitEnabled(feature_root, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, offsetof(VkPhysicalDeviceRayQueryFeaturesKHR, rayQuery))
    );
    if (!logical_device) {
        GPUAddressTracker::Get().SetEnabled(false);
        return std::unexpected(ToFrameError(logical_device.error()));
    }

    context._device                      = std::move(*logical_device);
    context._addressBindingReportEnabled = address_binding_report_enabled;
    context._present                     = ScanPresentSupport(enabled_extensions, feature_root);

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
