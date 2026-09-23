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

// Context Implementation

Context::~Context() noexcept {
    if (_device.handle != VK_NULL_HANDLE) {
        vkDestroyDevice(_device.handle, nullptr);
    }
    // _instanceObject's destructor tears down the persistent debug messenger
    // (if any) and then the instance, in that order, folding its diagnostics
    // into the process totals.
}

Context::Context(Context&& other) noexcept:
    _instanceObject(std::move(other._instanceObject)), _surface(std::exchange(other._surface, VK_NULL_HANDLE)),
    _physical(std::exchange(other._physical, {})), _device(std::exchange(other._device, {})), _present(other._present) {
}

auto Context::operator=(Context&& other) noexcept -> Context& {
    if (this != &other) {
        if (_device.handle != VK_NULL_HANDLE) {
            vkDestroyDevice(_device.handle, nullptr);
        }
        // Instance::operator= retires our instance (messenger first) and
        // re-points the diagnostics forwarding at this object.
        _instanceObject = std::move(other._instanceObject);
        _surface        = std::exchange(other._surface, VK_NULL_HANDLE);
        _physical       = std::exchange(other._physical, {});
        _device         = std::exchange(other._device, {});
        _present        = other._present;
    }
    return *this;
}

namespace {

// A requested extension name made it into the enabled set handed to device creation.
[[nodiscard]] auto ExtensionEnabled(const std::vector<const char*>& enabled, const char* name) noexcept -> bool {
    for (const char* entry: enabled) {
        if (entry != nullptr && std::strcmp(entry, name) == 0) {
            return true;
        }
    }
    return false;
}

// The header every Vulkan out-structure opens with, laid out by the compiler:
// sType, four bytes of padding, then pNext. The chain walk only ever touches
// these two fields plus the queried bit's offsetof -- no per-struct knowledge,
// and no hand-computed offsets (pNext sits at offset 8, not sizeof(sType)).
struct ChainHeader {
    VkStructureType sType;
    const void*     pNext;
};
static_assert(offsetof(ChainHeader, pNext) == offsetof(VkPhysicalDeviceFeatures2, pNext));

// Walks a VkPhysicalDeviceFeatures2 pNext chain for one feature struct's sType and
// answers whether its requested bit reads VK_TRUE.
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
    // The EXT fifo-latest-ready alias shares the KHR feature struct, sType and present
    // mode enumerant, so one feature scan covers both spellings: the extension side
    // accepts either name.
    const bool fifoExt = ExtensionEnabled(extensions, VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME) ||
                         ExtensionEnabled(extensions, VK_EXT_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME);
    // VK_EXT_present_timing depends on present_id2 and calibrated timestamps; all three
    // must have been enabled together (see RenderInitDevice's OptionalGroup).
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

} // namespace

// Builder Implementation

std::expected<Vk::Instance, ZHLN::ErrorCode> Context::Builder::BuildInstance() noexcept {
    // Ownership leaves with the return value: the caller must keep the
    // Vk::Instance alive and hand it back via Instance(Vk::Instance&&)
    // before Build(). A builder that still owns an instance when it dies
    // destroys it (RAII -- failed bring-ups cannot leak).
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

    const ZHLN_DeviceDesc device_desc = {
        .physical          = &ctx._physical,
        .extensions        = _deviceExtensions.data(),
        .extension_count   = static_cast<uint32_t>(_deviceExtensions.size()),
        .features          = _features,
        .enable_validation = (_validationMode != ZHLN_VALIDATION_OFF),
    };

    // Every Vulkan result enters the error channel through the one mapping:
    // ToFrameError names a lost device FrameResult::DeviceLost and carries
    // everything else verbatim under the VulkanResult category. At bring-up a
    // lost device only reaches main's or_else, which prints it and exits --
    // nothing on the init path branches on it, so the frame vocabulary's
    // "rebuild the device" advice degrades to the accurate message it is.
    if (const VkResult res = ZHLN_CreateDevice(&device_desc, &ctx._device); res != VK_SUCCESS) {
        return std::unexpected(ToFrameError(res));
    }
    // Record what this device enabled for presentation from the inputs above:
    // the pacer resolves its policy from this rather than re-probing.
    ctx._present = ScanPresentSupport(_deviceExtensions, _features);

    // Only take ownership of the instance once device creation succeeds.
    // The persistent debug messenger already exists: Instance::Create set it
    // up (errors AND warnings -- a warning the engine cannot explain is a
    // warning worth fixing at the source) and owns its teardown.
    if (!_instanceObject.Valid()) {
        return std::unexpected(ContextError::InstanceCreationFailed);
    }
    ctx._instanceObject = std::move(_instanceObject);

    return ctx;
}

} // namespace ZHLN::Vk
