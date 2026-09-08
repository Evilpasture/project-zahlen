// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Rendering.hpp"
#include <Zahlen/Log.hpp>
#include <fstream>
#include <vector>

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

void DeviceFaultTracker::OnDeviceLost() const noexcept {
    if (device == VK_NULL_HANDLE || vkGetDeviceFaultInfoKHR == nullptr) {
        return;
    }

    // Spec: vkGetDeviceFaultInfoKHR remains valid after VK_ERROR_DEVICE_LOST.
    VkDeviceFaultCountsKHR counts = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_KHR};
    VkResult               res    = vkGetDeviceFaultInfoKHR(device, &counts, nullptr);
    if (res != VK_SUCCESS && res != VK_INCOMPLETE) {
        ZHLN::Log("[GPU DEVICE FAULT] vkGetDeviceFaultInfoKHR count query failed ({})", ZHLN::Reflect::EnumToString(res));
        return;
    }

    std::vector<VkDeviceFaultAddressInfoKHR> addressInfos(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoKHR>  vendorInfos(counts.vendorInfoCount);
    std::vector<uint8_t>                     vendorBinary(counts.vendorBinarySize);

    VkDeviceFaultInfoKHR info = {
        .sType             = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_KHR,
        .pAddressInfos     = addressInfos.empty() ? nullptr : addressInfos.data(),
        .pVendorInfos      = vendorInfos.empty() ? nullptr : vendorInfos.data(),
        .pVendorBinaryData = vendorBinary.empty() ? nullptr : vendorBinary.data(),
    };

    res = vkGetDeviceFaultInfoKHR(device, &counts, &info);
    if (res != VK_SUCCESS && res != VK_INCOMPLETE) {
        ZHLN::Log("[GPU DEVICE FAULT] vkGetDeviceFaultInfoKHR payload query failed ({})", ZHLN::Reflect::EnumToString(res));
        return;
    }

    ZHLN::Log("\n[GPU DEVICE FAULT] {}", info.description);

    for (uint32_t i = 0; i < counts.addressInfoCount; ++i) {
        const auto& a = addressInfos[i];
        ZHLN::Log(
            "  Fault Addr #{}: 0x{:016X} ±{} ({})", i, a.reportedAddress, a.addressPrecision, ZHLN::Reflect::EnumToString(a.addressType)
        );
    }

    for (uint32_t i = 0; i < counts.vendorInfoCount; ++i) {
        const auto& v = vendorInfos[i];
        ZHLN::Log("  Vendor Info #{}: code=0x{:08X} data=0x{:016X} \"{}\"", i, v.vendorFaultCode, v.vendorFaultData, v.description);
    }

    if (!vendorBinary.empty()) {
        std::ofstream out("gpu_crash_dump.bin", std::ios::binary);
        if (!out) {
            ZHLN::Log("  Failed to write vendor crash dump ({} bytes)", vendorBinary.size());
            return;
        }
        out.write(reinterpret_cast<const char*>(vendorBinary.data()), static_cast<std::streamsize>(vendorBinary.size()));
        ZHLN::Log("  Saved vendor crash dump: gpu_crash_dump.bin ({} bytes)", vendorBinary.size());
    }
}

} // namespace ZHLN::Vk
