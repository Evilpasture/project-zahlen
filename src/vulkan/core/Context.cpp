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
    _instanceObject(std::move(other._instanceObject)), _surface(std::exchange(other._surface, VK_NULL_HANDLE)), _physical(std::exchange(other._physical, {})),
    _device(std::exchange(other._device, {})), _present(other._present), _enabledFeatures(std::move(other._enabledFeatures)) {
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
        _present         = other._present;
        _enabledFeatures = std::move(other._enabledFeatures);
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

// ---------------------------------------------------------------------------
// The backend's own device requirements
//
// Capabilities only src/vulkan and its diagnostics use, which no render pass
// branches on: robustness, crash dumps, abort. The renderer never asks for
// these and never learns whether they came on. A capability a pass DOES branch
// on stays in the caller's chain and is read back through Context::HasFeature
// -- mesh shading, ray tracing, paced presentation.
//
// Which side a feature belongs to is the whole question, and the test is
// "does an algorithm change": these do not, so naming them in the renderer was
// only ever plumbing.
// ---------------------------------------------------------------------------

// Which of the backend's optional extensions this device advertised. One
// enumeration answers all of them positionally, so five checks cost a single
// vkEnumerateDeviceExtensionProperties.
struct BackendExtensions {
    bool robustness2    = false;
    bool deviceFaultKhr = false;
    bool deviceFaultExt = false;
    bool constantData   = false;
    bool shaderAbort    = false;

    [[nodiscard]] auto Names() const noexcept -> std::vector<const char*> {
        std::vector<const char*> out;
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

// The matching feature chain. Built with the same FeatureChainBuilder the
// caller uses, so "does this device have the bit" is answered by the same
// query rather than a second, differently-worded probe -- which is what let a
// copy of each answer drift into RenderContext::Impl in the first place.
//
// Every struct here is Optional except dynamicRenderingUnusedAttachments,
// which the renderer's stencil-less secondaries structurally depend on and
// which therefore keeps vetoing device creation exactly as it did while the
// renderer requested it.
[[nodiscard]] auto BuildBackendChain(VkPhysicalDevice physical, ValidationMode validationMode) {
    return FeatureChainBuilder(physical)
        .Optional<VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR>([](auto& f) -> auto { f.swapchainMaintenance1 = VK_TRUE; })
        .Optional<VkPhysicalDeviceRobustness2FeaturesEXT>([validationMode](auto& f) -> auto {
            f.nullDescriptor = VK_TRUE;
            if (validationMode == ZHLN_VALIDATION_GPU) {
                f.robustBufferAccess2 = VK_TRUE;
                f.robustImageAccess2  = VK_TRUE;
            }
        })
        .Require<VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT>([](auto& f) -> auto { f.dynamicRenderingUnusedAttachments = VK_TRUE; })
        // VK_KHR_device_fault: vkGetDeviceFaultReportsKHR after device lost.
        // Optional drops the whole struct when any requested bit is missing, so
        // the extras are mirrored from what the device advertises rather than
        // asked for blind.
        .Optional<VkPhysicalDeviceFaultFeaturesKHR>([physical](auto& f) -> auto {
            const auto supported            = QueryFeatureSupport<VkPhysicalDeviceFaultFeaturesKHR>(physical);
            f.deviceFault                   = VK_TRUE;
            f.deviceFaultVendorBinary       = supported.deviceFaultVendorBinary;
            f.deviceFaultReportMasked       = supported.deviceFaultReportMasked;
            f.deviceFaultDeviceLostOnMasked = supported.deviceFaultDeviceLostOnMasked;
        })
        // VK_EXT_device_fault: shipping drivers still expose the older
        // vkGetDeviceFaultInfoEXT query, so a KHR-less device still dumps.
        .Optional<VkPhysicalDeviceFaultFeaturesEXT>([physical](auto& f) -> auto {
            const auto supported      = QueryFeatureSupport<VkPhysicalDeviceFaultFeaturesEXT>(physical);
            f.deviceFault             = VK_TRUE;
            f.deviceFaultVendorBinary = supported.deviceFaultVendorBinary;
        })
        // Constant-data is abort's message-packing dependency (OpAbortKHR packs
        // UTF-8 strings), enabled on its own so a driver listing abort without
        // constant_data still gets the instruction. Abort itself is not wired
        // up yet -- hang_gpu provokes a hang with an MMU store -- but the
        // capability is recorded so the shader side can adopt OpAbortKHR
        // without another trip through device creation.
        .Optional<VkPhysicalDeviceShaderConstantDataFeaturesKHR>([](auto& f) -> auto { f.shaderConstantData = VK_TRUE; })
        .Optional<VkPhysicalDeviceShaderAbortFeaturesKHR>([](auto& f) -> auto { f.shaderAbort = VK_TRUE; })
        .Build();
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

    // The backend's own requirements, negotiated here rather than requested by
    // the caller: what it enables is a property of the device, not of the
    // renderer above it. Both halves are chained into the single
    // VkDeviceCreateInfo feature chain Vulkan requires, and both must outlive
    // the ZHLN_CreateDevice call below.
    const BackendExtensions backendExts = ScanBackendExtensions(_physical.handle);
    auto                    backend     = BuildBackendChain(_physical.handle, _validationMode);

    std::vector<const char*> extensions = _deviceExtensions;
    if (const auto names = backendExts.Names(); !names.empty()) {
        extensions.insert(extensions.end(), names.begin(), names.end());
    }

    // This backend's structs first, the caller's chain behind them. The caller's
    // half is untouched: its tail simply stops being the end of the list.
    const VkPhysicalDeviceFeatures2* backendRoot = backend.GetRoot(_features);
    const VkPhysicalDeviceFeatures2* root        = backendRoot != nullptr ? backendRoot : _features;

    const ZHLN_DeviceDesc device_desc = {
        .physical          = &ctx._physical,
        .extensions        = extensions.data(),
        .extension_count   = static_cast<uint32_t>(extensions.size()),
        .features          = root,
        .enable_validation = (_validationMode != ZHLN_VALIDATION_OFF),
    };

    // Every Vulkan result enters the error channel through the one mapping:
    // ToFrameError names a lost device FrameResult::DeviceLost and carries
    // everything else verbatim under the Vk::Result category. At bring-up a
    // lost device only reaches main's or_else, which prints it and exits --
    // nothing on the init path branches on it, so the frame vocabulary's
    // "rebuild the device" advice degrades to the accurate message it is.
    if (const VkResult res = ZHLN_CreateDevice(&device_desc, &ctx._device); res != VK_SUCCESS) {
        return std::unexpected(ToFrameError(res));
    }
    // Record what this device enabled for presentation from the inputs above:
    // the pacer resolves its policy from this rather than re-probing.
    ctx._present = ScanPresentSupport(extensions, _features);

    // And every struct the merged chain enabled -- the backend's and the
    // caller's -- copied out so GetFeature<T>() keeps working after both
    // chains are gone. Lookup is by sType, so the order the two halves are
    // concatenated in cannot matter.
    ctx._enabledFeatures = std::move(_enabledFeatures);
    for (EnabledFeature& entry: backend.SnapshotEnabled()) {
        ctx._enabledFeatures.push_back(std::move(entry));
    }

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
