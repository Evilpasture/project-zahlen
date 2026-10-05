// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Context.hpp"
#include "RenderCore.h"
#include "RenderCore.hpp"
#include "../diagnostics/GPUAddressTracker.hpp"
#include <cstddef>
#include <cstring>
#include <vector>

namespace ZHLN::Vk {

ZHLN_PhysicalDeviceInfo SelectDevice(VkInstance instance, VkSurfaceKHR surface) noexcept {
    ZHLN_DeviceSelectDesc select_desc = {.instance = instance, .surface = surface, .score_fn = nullptr, .score_userdata = nullptr};
    return ZHLN_SelectPhysicalDevice(&select_desc);
}


Context::~Context() noexcept {
    if (_device.handle != VK_NULL_HANDLE) {
        vkDestroyDevice(_device.handle, nullptr);
    }
    if (_addressBindingReportEnabled) {
        GPUAddressTracker::Get().SetEnabled(false);
    }
}

Context::Context(Context&& other) noexcept:
    _instanceObject(std::move(other._instanceObject)), _surface(std::exchange(other._surface, VK_NULL_HANDLE)), _physical(std::exchange(other._physical, {})),
    _device(std::exchange(other._device, {})), _present(other._present), _enabledFeatures(std::move(other._enabledFeatures)),
    _addressBindingReportEnabled(std::exchange(other._addressBindingReportEnabled, false)) {
}

auto Context::operator=(Context&& other) noexcept -> Context& {
    if (this != &other) {
        if (_device.handle != VK_NULL_HANDLE) {
            vkDestroyDevice(_device.handle, nullptr);
        }
        if (_addressBindingReportEnabled) {
            GPUAddressTracker::Get().SetEnabled(false);
        }
        _instanceObject = std::move(other._instanceObject);
        _surface        = std::exchange(other._surface, VK_NULL_HANDLE);
        _physical       = std::exchange(other._physical, {});
        _device                      = std::exchange(other._device, {});
        _present                     = other._present;
        _enabledFeatures             = std::move(other._enabledFeatures);
        _addressBindingReportEnabled = std::exchange(other._addressBindingReportEnabled, false);
    }
    return *this;
}

namespace {

[[nodiscard]] auto ExtensionEnabled(const std::vector<const char*>& enabled, const char* name) noexcept -> bool {
    for (const char* entry: enabled) {
        if (entry != nullptr && std::strcmp(entry, name) == 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto FeatureBitEnabled(const VkPhysicalDeviceFeatures2* root, VkStructureType sType, size_t bitOffset) noexcept -> bool {
    // Chain elements are different Vulkan struct types; reading them through
    // an unrelated C++ struct pointer violates strict aliasing. The shared
    // sType/pNext header and VkBool32 fields are inspected as bytes instead.
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

[[nodiscard]] auto ScanPresentSupport(const std::vector<const char*>& extensions, const VkPhysicalDeviceFeatures2* features) noexcept
    -> DevicePresentSupport {
    const bool fifoExt = ExtensionEnabled(extensions, VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME) ||
                         ExtensionEnabled(extensions, VK_EXT_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME);
    const bool timingExt = ExtensionEnabled(extensions, VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
    const bool id2Ext    = ExtensionEnabled(extensions, VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
    const bool calibExt  = ExtensionEnabled(extensions, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) ||
                          ExtensionEnabled(extensions, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    // Closed-loop pacing also queries surface capabilities2 on the instance.
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
        features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, offsetof(VkPhysicalDevicePresentId2FeaturesKHR, presentId2)
    );

    return DevicePresentSupport {
        .fifoLatestReady       = fifoExt && fifoBit,
        .presentTiming         = timingGroup && timingBit,
        .presentAtAbsoluteTime = timingGroup && absoluteBit,
        .presentId2            = timingGroup && id2Bit,
    };
}

}

std::expected<Vk::Instance, ZHLN::ErrorCode> Context::Builder::BuildInstance() noexcept {
    _instanceObject = Instance::Create(_appName, _appVersion, _instanceExtensions, _validationMode);
    if (!_instanceObject.Valid()) {
        return std::unexpected(ContextError::InstanceCreationFailed);
    }
    _instanceView = _instanceObject.Handle();
    return std::move(_instanceObject);
}

std::expected<ZHLN_PhysicalDeviceInfo, ZHLN::ErrorCode> Context::Builder::SelectPhysicalDevice() const noexcept {
    const VkInstance        view = _instanceView != VK_NULL_HANDLE ? _instanceView : _instanceObject.Handle();
    ZHLN_DeviceSelectDesc   select_desc = {.instance = view, .surface = _surface, .score_fn = _scoreFn, .score_userdata = _scoreUserdata};
    ZHLN_PhysicalDeviceInfo info        = ZHLN_SelectPhysicalDevice(&select_desc);
    if (info.handle == VK_NULL_HANDLE) {
        return std::unexpected(ContextError::NoSuitableDeviceFound);
    }
    return info;
}

std::expected<Context, ErrorCode> Context::Builder::Build() noexcept {
    Context ctx;
    ctx._surface  = _surface;
    ctx._physical = _physical;

    auto backend = DeviceConfigurator<>(_physical.handle)
        .RequireExtension(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME)
        .RequireExtension<VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT>(
            VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME, [](auto& f) { f.dynamicRenderingUnusedAttachments = VK_TRUE; }
        )
        .OptionalExtension<VkPhysicalDeviceRobustness2FeaturesEXT>(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, [this](auto& f) {
            f.nullDescriptor = VK_TRUE;
            if (_validationMode == ZHLN_VALIDATION_GPU) {
                f.robustBufferAccess2 = VK_TRUE;
                f.robustImageAccess2  = VK_TRUE;
            }
        })
        .OptionalExtension<VkPhysicalDeviceFaultFeaturesKHR>(VK_KHR_DEVICE_FAULT_EXTENSION_NAME, [](auto& f) {
            f.deviceFault                   = VK_TRUE;
            f.deviceFaultVendorBinary       = VK_TRUE;
            f.deviceFaultReportMasked       = VK_TRUE;
            f.deviceFaultDeviceLostOnMasked = VK_TRUE;
        }, [](VkPhysicalDevice, const auto& enabled) { return enabled.deviceFault == VK_TRUE; })
        .OptionalExtension<VkPhysicalDeviceFaultFeaturesEXT>(VK_EXT_DEVICE_FAULT_EXTENSION_NAME, [](auto& f) {
            f.deviceFault             = VK_TRUE;
            f.deviceFaultVendorBinary = VK_TRUE;
        }, [](VkPhysicalDevice, const auto& enabled) { return enabled.deviceFault == VK_TRUE; })
        .OptionalExtension<VkPhysicalDeviceAddressBindingReportFeaturesEXT>(
            VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME,
            [](auto& f) { f.reportAddressBinding = VK_TRUE; },
            [](VkPhysicalDevice, const auto& enabled) { return enabled.reportAddressBinding == VK_TRUE; },
            _validationMode != ZHLN_VALIDATION_OFF && _instanceObject.HasAddressBindingMessenger() && vkCreateDebugUtilsMessengerEXT != nullptr
        )
        .OptionalExtension<VkPhysicalDeviceShaderConstantDataFeaturesKHR>(
            VK_KHR_SHADER_CONSTANT_DATA_EXTENSION_NAME, [](auto& f) { f.shaderConstantData = VK_TRUE; }
        )
        .OptionalExtension<VkPhysicalDeviceShaderAbortFeaturesKHR>(
            VK_KHR_SHADER_ABORT_EXTENSION_NAME, [](auto& f) { f.shaderAbort = VK_TRUE; }
        )
        .Build();
    if (!backend) {
        return std::unexpected(backend.error());
    }

    std::vector<const char*> extensions = _deviceExtensions;
    const std::vector<const char*>& backendExts = backend->extensions;
    extensions.insert(extensions.end(), backendExts.begin(), backendExts.end());

    const VkPhysicalDeviceFeatures2* backendRoot = backend->features.GetRoot(_features);
    const VkPhysicalDeviceFeatures2* root        = backendRoot != nullptr ? backendRoot : _features;
    const bool addressBindingReportEnabled =
        _validationMode != ZHLN_VALIDATION_OFF && _instanceObject.HasAddressBindingMessenger() && vkCreateDebugUtilsMessengerEXT != nullptr &&
        ExtensionEnabled(extensions, VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME) &&
        FeatureBitEnabled(
            root, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT,
            offsetof(VkPhysicalDeviceAddressBindingReportFeaturesEXT, reportAddressBinding)
        );

    const ZHLN_DeviceDesc device_desc = {
        .physical          = &ctx._physical,
        .extensions        = extensions.data(),
        .extension_count   = static_cast<uint32_t>(extensions.size()),
        .features          = root,
        .enable_validation = (_validationMode != ZHLN_VALIDATION_OFF),
    };

    GPUAddressTracker::Get().SetEnabled(addressBindingReportEnabled);
    if (const VkResult res = ZHLN_CreateDevice(&device_desc, &ctx._device); res != VK_SUCCESS) {
        GPUAddressTracker::Get().SetEnabled(false);
        return std::unexpected(ToFrameError(res));
    }
    ctx._addressBindingReportEnabled = addressBindingReportEnabled;
    // The C ABI checks entry points/limits. Only advertise paths whose whole
    // feature+extension bundle was actually negotiated into this device.
    ctx._device.mesh_shader_enabled = ctx._device.mesh_shader_enabled &&
        ExtensionEnabled(extensions, VK_EXT_MESH_SHADER_EXTENSION_NAME) &&
        FeatureBitEnabled(root, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, offsetof(VkPhysicalDeviceMeshShaderFeaturesEXT, taskShader)) &&
        FeatureBitEnabled(root, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, offsetof(VkPhysicalDeviceMeshShaderFeaturesEXT, meshShader));
    ctx._device.ray_tracing_enabled = ctx._device.ray_tracing_enabled &&
        FeatureBitEnabled(root, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
                          offsetof(VkPhysicalDeviceAccelerationStructureFeaturesKHR, accelerationStructure)) &&
        FeatureBitEnabled(root, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, offsetof(VkPhysicalDeviceRayQueryFeaturesKHR, rayQuery));
    ctx._present = ScanPresentSupport(extensions, _features);

    ctx._enabledFeatures = std::move(_enabledFeatures);
    for (EnabledFeature& entry: backend->features.SnapshotEnabled()) {
        ctx._enabledFeatures.push_back(std::move(entry));
    }

    if (!_instanceObject.Valid()) {
        return std::unexpected(ContextError::InstanceCreationFailed);
    }
    ctx._instanceObject = std::move(_instanceObject);

    return ctx;
}

}
