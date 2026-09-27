// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Context.hpp"
#include "RenderCore.h"
#include "RenderCore.hpp"
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
}

Context::Context(Context&& other) noexcept:
    _instanceObject(std::move(other._instanceObject)), _surface(std::exchange(other._surface, VK_NULL_HANDLE)), _physical(std::exchange(other._physical, {})),
    _device(std::exchange(other._device, {})), _present(other._present), _enabledFeatures(std::move(other._enabledFeatures)) {
}

auto Context::operator=(Context&& other) noexcept -> Context& {
    if (this != &other) {
        if (_device.handle != VK_NULL_HANDLE) {
            vkDestroyDevice(_device.handle, nullptr);
        }
        _instanceObject = std::move(other._instanceObject);
        _surface        = std::exchange(other._surface, VK_NULL_HANDLE);
        _physical       = std::exchange(other._physical, {});
        _device         = std::exchange(other._device, {});
        _present         = other._present;
        _enabledFeatures = std::move(other._enabledFeatures);
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

struct ChainHeader {
    VkStructureType sType;
    const void*     pNext;
};
static_assert(offsetof(ChainHeader, pNext) == offsetof(VkPhysicalDeviceFeatures2, pNext));

[[nodiscard]] auto FeatureBitEnabled(const VkPhysicalDeviceFeatures2* root, VkStructureType sType, size_t bitOffset) noexcept -> bool {
    for (const auto* cursor = reinterpret_cast<const ChainHeader*>(root); cursor != nullptr;
         cursor = static_cast<const ChainHeader*>(cursor->pNext)) {
        if (cursor->sType == sType) {
            const auto* bit = reinterpret_cast<const VkBool32*>(reinterpret_cast<const char*>(cursor) + bitOffset);
            return *bit == VK_TRUE;
        }
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
    const bool timingGroup = timingExt && id2Ext && calibExt;

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


struct BackendExtensions {
    bool robustness2    = false;
    bool deviceFaultKhr = false;
    bool deviceFaultExt = false;
    bool constantData   = false;
    bool shaderAbort    = false;

    [[nodiscard]] auto Names() const noexcept -> std::vector<const char*> {
        std::vector<const char*> out;
        out.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
        if (robustness2) {
            out.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
        }
        if (deviceFaultKhr) {
            out.push_back(VK_KHR_DEVICE_FAULT_EXTENSION_NAME);
        }
        if (deviceFaultExt) {
            out.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        }
        if (constantData) {
            out.push_back(VK_KHR_SHADER_CONSTANT_DATA_EXTENSION_NAME);
        }
        if (shaderAbort) {
            out.push_back(VK_KHR_SHADER_ABORT_EXTENSION_NAME);
        }
        return out;
    }
};

[[nodiscard]] auto ScanBackendExtensions(VkPhysicalDevice physical) noexcept -> BackendExtensions {
    if (physical == VK_NULL_HANDLE) {
        return {};
    }
    const auto q = QueryDeviceExtensions(
        physical, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, VK_KHR_DEVICE_FAULT_EXTENSION_NAME, VK_EXT_DEVICE_FAULT_EXTENSION_NAME,
        VK_KHR_SHADER_CONSTANT_DATA_EXTENSION_NAME, VK_KHR_SHADER_ABORT_EXTENSION_NAME
    );
    return BackendExtensions {
        .robustness2    = q[0],
        .deviceFaultKhr = q[1],
        .deviceFaultExt = q[2],
        .constantData   = q[3],
        .shaderAbort    = q[4],
    };
}

[[nodiscard]] auto BuildBackendChain(VkPhysicalDevice physical, ValidationMode validationMode) {
    return FeatureChainBuilder(physical)
        .Optional<VkPhysicalDeviceRobustness2FeaturesEXT>([validationMode](auto& f) -> auto {
            f.nullDescriptor = VK_TRUE;
            if (validationMode == ZHLN_VALIDATION_GPU) {
                f.robustBufferAccess2 = VK_TRUE;
                f.robustImageAccess2  = VK_TRUE;
            }
        })
        .Require<VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT>([](auto& f) -> auto { f.dynamicRenderingUnusedAttachments = VK_TRUE; })
        .Optional<VkPhysicalDeviceFaultFeaturesKHR>([physical](auto& f) -> auto {
            const auto supported            = QueryFeatureSupport<VkPhysicalDeviceFaultFeaturesKHR>(physical);
            f.deviceFault                   = VK_TRUE;
            f.deviceFaultVendorBinary       = supported.deviceFaultVendorBinary;
            f.deviceFaultReportMasked       = supported.deviceFaultReportMasked;
            f.deviceFaultDeviceLostOnMasked = supported.deviceFaultDeviceLostOnMasked;
        })
        .Optional<VkPhysicalDeviceFaultFeaturesEXT>([physical](auto& f) -> auto {
            const auto supported      = QueryFeatureSupport<VkPhysicalDeviceFaultFeaturesEXT>(physical);
            f.deviceFault             = VK_TRUE;
            f.deviceFaultVendorBinary = supported.deviceFaultVendorBinary;
        })
        .Optional<VkPhysicalDeviceShaderConstantDataFeaturesKHR>([](auto& f) -> auto { f.shaderConstantData = VK_TRUE; })
        .Optional<VkPhysicalDeviceShaderAbortFeaturesKHR>([](auto& f) -> auto { f.shaderAbort = VK_TRUE; })
        .Build();
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

    const BackendExtensions backendExts = ScanBackendExtensions(_physical.handle);
    auto                    backend     = BuildBackendChain(_physical.handle, _validationMode);

    std::vector<const char*> extensions = _deviceExtensions;
    if (const auto names = backendExts.Names(); !names.empty()) {
        extensions.insert(extensions.end(), names.begin(), names.end());
    }

    const VkPhysicalDeviceFeatures2* backendRoot = backend.GetRoot(_features);
    const VkPhysicalDeviceFeatures2* root        = backendRoot != nullptr ? backendRoot : _features;

    const ZHLN_DeviceDesc device_desc = {
        .physical          = &ctx._physical,
        .extensions        = extensions.data(),
        .extension_count   = static_cast<uint32_t>(extensions.size()),
        .features          = root,
        .enable_validation = (_validationMode != ZHLN_VALIDATION_OFF),
    };

    if (const VkResult res = ZHLN_CreateDevice(&device_desc, &ctx._device); res != VK_SUCCESS) {
        return std::unexpected(ToFrameError(res));
    }
    ctx._present = ScanPresentSupport(extensions, _features);

    ctx._enabledFeatures = std::move(_enabledFeatures);
    for (EnabledFeature& entry: backend.SnapshotEnabled()) {
        ctx._enabledFeatures.push_back(std::move(entry));
    }

    if (!_instanceObject.Valid()) {
        return std::unexpected(ContextError::InstanceCreationFailed);
    }
    ctx._instanceObject = std::move(_instanceObject);

    return ctx;
}

}
