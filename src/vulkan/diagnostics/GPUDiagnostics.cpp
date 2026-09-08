// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Rendering.hpp"
#include <Zahlen/Log.hpp>
#include <format>
#include <fstream>
#include <string>
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

namespace {

void LogFaultAddress(std::string_view label, const VkDeviceFaultAddressInfoKHR& address) noexcept {
    if (address.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_KHR) {
        return;
    }
    ZHLN::Log(
        "  {}: 0x{:016X} ±{} ({})", label, address.reportedAddress, address.addressPrecision, ZHLN::Reflect::EnumToString(address.addressType)
    );
}

void LogVendorInfo(std::string_view label, const VkDeviceFaultVendorInfoKHR& vendor) noexcept {
    if (vendor.description[0] == '\0' && vendor.vendorFaultCode == 0 && vendor.vendorFaultData == 0) {
        return;
    }
    ZHLN::Log("  {}: code=0x{:08X} data=0x{:016X} \"{}\"", label, vendor.vendorFaultCode, vendor.vendorFaultData, vendor.description);
}

void WriteVendorBinary(const void* data, size_t size) noexcept {
    if (data == nullptr || size == 0) {
        return;
    }
    std::ofstream out("gpu_crash_dump.bin", std::ios::binary);
    if (!out) {
        ZHLN::Log("  Failed to write vendor crash dump ({} bytes)", size);
        return;
    }
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    ZHLN::Log("  Saved vendor crash dump: gpu_crash_dump.bin ({} bytes)", size);
}

void DumpKhrDeviceFault(VkDevice device) noexcept {
    if (vkGetDeviceFaultReportsKHR == nullptr) {
        return;
    }

    uint32_t count = 0;
    VkResult res   = vkGetDeviceFaultReportsKHR(device, 0, &count, nullptr);
    if (res != VK_SUCCESS && res != VK_INCOMPLETE) {
        ZHLN::Log("[GPU DEVICE FAULT] vkGetDeviceFaultReportsKHR count query failed ({})", ZHLN::Reflect::EnumToString(res));
        return;
    }
    if (count == 0) {
        ZHLN::Log("[GPU DEVICE FAULT] vkGetDeviceFaultReportsKHR returned 0 reports");
    } else {
        std::vector<VkDeviceFaultInfoKHR> reports(count);
        for (auto& report: reports) {
            report.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_KHR;
        }
        res = vkGetDeviceFaultReportsKHR(device, 0, &count, reports.data());
        if (res != VK_SUCCESS && res != VK_INCOMPLETE) {
            ZHLN::Log("[GPU DEVICE FAULT] vkGetDeviceFaultReportsKHR payload query failed ({})", ZHLN::Reflect::EnumToString(res));
            return;
        }

        for (uint32_t i = 0; i < count; ++i) {
            const auto& report = reports[i];
            std::string flags;
            ZHLN::Log(
                "\n[GPU DEVICE FAULT] #{} group={} flags={} \"{}\"", i, report.groupId,
                ZHLN::Reflect::EnumToFlagsString(static_cast<VkDeviceFaultFlagBitsKHR>(report.flags), flags), report.description
            );
            LogFaultAddress("Fault Addr", report.faultAddressInfo);
            LogFaultAddress("Instruction Addr", report.instructionAddressInfo);
            LogVendorInfo("Vendor Info", report.vendorInfo);
        }
    }

    if (vkGetDeviceFaultDebugInfoKHR == nullptr) {
        return;
    }
    VkDeviceFaultDebugInfoKHR debug = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_DEBUG_INFO_KHR};
    res                             = vkGetDeviceFaultDebugInfoKHR(device, &debug);
    if ((res != VK_SUCCESS && res != VK_INCOMPLETE) || debug.vendorBinarySize == 0) {
        return;
    }
    std::vector<uint8_t> binary(debug.vendorBinarySize);
    debug.pVendorBinaryData = binary.data();
    res                     = vkGetDeviceFaultDebugInfoKHR(device, &debug);
    if (res != VK_SUCCESS && res != VK_INCOMPLETE) {
        ZHLN::Log("[GPU DEVICE FAULT] vkGetDeviceFaultDebugInfoKHR payload query failed ({})", ZHLN::Reflect::EnumToString(res));
        return;
    }
    WriteVendorBinary(debug.pVendorBinaryData, debug.vendorBinarySize);
}

void DumpExtDeviceFault(VkDevice device) noexcept {
    if (vkGetDeviceFaultInfoEXT == nullptr) {
        return;
    }

    VkDeviceFaultCountsEXT counts = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    VkResult               res    = vkGetDeviceFaultInfoEXT(device, &counts, nullptr);
    if (res != VK_SUCCESS && res != VK_INCOMPLETE) {
        ZHLN::Log("[GPU DEVICE FAULT] vkGetDeviceFaultInfoEXT count query failed ({})", ZHLN::Reflect::EnumToString(res));
        return;
    }

    std::vector<VkDeviceFaultAddressInfoEXT> addressInfos(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT>  vendorInfos(counts.vendorInfoCount);
    std::vector<uint8_t>                     vendorBinary(counts.vendorBinarySize);

    VkDeviceFaultInfoEXT info = {
        .sType             = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
        .pAddressInfos     = addressInfos.empty() ? nullptr : addressInfos.data(),
        .pVendorInfos      = vendorInfos.empty() ? nullptr : vendorInfos.data(),
        .pVendorBinaryData = vendorBinary.empty() ? nullptr : vendorBinary.data(),
    };

    res = vkGetDeviceFaultInfoEXT(device, &counts, &info);
    if (res != VK_SUCCESS && res != VK_INCOMPLETE) {
        ZHLN::Log("[GPU DEVICE FAULT] vkGetDeviceFaultInfoEXT payload query failed ({})", ZHLN::Reflect::EnumToString(res));
        return;
    }

    ZHLN::Log("\n[GPU DEVICE FAULT] {}", info.description);
    for (uint32_t i = 0; i < counts.addressInfoCount; ++i) {
        LogFaultAddress(std::format("Fault Addr #{}", i), addressInfos[i]);
    }
    for (uint32_t i = 0; i < counts.vendorInfoCount; ++i) {
        LogVendorInfo(std::format("Vendor Info #{}", i), vendorInfos[i]);
    }
    WriteVendorBinary(info.pVendorBinaryData, counts.vendorBinarySize);
}

} // namespace

void DeviceFaultTracker::OnDeviceLost() const noexcept {
    if (device == VK_NULL_HANDLE) {
        return;
    }

    // Spec: these queries remain valid after VK_ERROR_DEVICE_LOST.
    if (vkGetDeviceFaultReportsKHR != nullptr) {
        DumpKhrDeviceFault(device);
        return;
    }
    DumpExtDeviceFault(device);
}

} // namespace ZHLN::Vk
