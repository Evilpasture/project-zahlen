// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Rendering.hpp"

namespace ZHLN::Vk {

#if !defined(ZHLN_CUSTOM_GPU_DIAGNOSTICS_BACKEND)
GPUCrashTrackerCallbacks CreateConfiguredGPUCrashTracker(
    GPUVendor /*vendor*/,
    VkDevice /*device*/,
    VkPhysicalDevice /*physical*/,
    DiagnosticConfig /*config*/
) {
    return {};
}
#endif

DebugUtilsTracker::DebugUtilsTracker(VkDevice inDevice, DiagnosticConfig inConfig): config(inConfig), device(inDevice) {
    cmdInsertDebugLabel = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(vkGetDeviceProcAddr(device, "vkCmdInsertDebugUtilsLabelEXT"));
}

void DebugUtilsTracker::WriteCheckpoint(VkCommandBuffer cmd, std::string_view name) const {
    if (!config.enableMarkers || cmdInsertDebugLabel == nullptr || cmd == VK_NULL_HANDLE || name.empty()) {
        return;
    }

    const VkDebugUtilsLabelEXT label = {
        .sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
        .pNext      = nullptr,
        .pLabelName = name.data(),
        .color      = {0.20F, 0.65F, 1.0F, 1.0F},
    };
    cmdInsertDebugLabel(cmd, &label);
}

void DebugUtilsTracker::RegisterShader(std::span<const uint32_t> /*spirv*/, std::string_view /*entryPoint*/) const {
}

void DebugUtilsTracker::OnDeviceLost() const {
}

void DebugUtilsTracker::Shutdown() {
    cmdInsertDebugLabel = nullptr;
    device              = VK_NULL_HANDLE;
}

} // namespace ZHLN::Vk
