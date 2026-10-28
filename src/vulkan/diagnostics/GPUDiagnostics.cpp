// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GPUDiagnostics.hpp"
#include "GPUAddressTracker.hpp"
#include "Rendering.hpp"
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <vector>

namespace ZHLN::Vk {

namespace {

void LogFaultAddress(const GPUAddressTracker* tracker, std::string_view label, const VkDeviceFaultAddressInfoKHR& address) noexcept {
    if (address.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_KHR) {
        return;
    }

    ZHLN::Log("  {}: 0x{:016X} ±{} ({})", label, address.reportedAddress, address.addressPrecision, ZHLN::Reflect::EnumToString(address.addressType));

    if (tracker == nullptr || !tracker->Enabled()) {
        return;
    }

    const auto symbols = tracker->Resolve(address.reportedAddress);
    if (symbols.empty()) {
        ZHLN::Log("    -> No live device-address binding covers this address (possibly freed or out-of-bounds).");
        return;
    }

    for (const GpuAllocationSymbol& symbol: symbols) {
        const std::string_view name   = symbol.name.empty() ? (symbol.isDriverInternal ? "[Driver-Internal Object]" : "[Unnamed Vulkan Object]") :
                                                              std::string_view(symbol.name);
        const std::string_view origin = symbol.isDriverInternal ? "driver-internal" : "application";
        ZHLN::Log(
            "    -> {} ({}, type={}, handle=0x{:016X}, base=0x{:016X}, size={} bytes, offset=+0x{:X})", name, origin,
            ZHLN::Reflect::EnumToString(symbol.objectType), symbol.objectHandle, symbol.baseAddress, symbol.size, address.reportedAddress - symbol.baseAddress
        );
    }
}

void LogVendorInfo(std::string_view label, const VkDeviceFaultVendorInfoKHR& vendor) noexcept {
    if (vendor.description[0] == '\0' && vendor.vendorFaultCode == 0 && vendor.vendorFaultData == 0) {
        return;
    }
    ZHLN::Log("  {}: code=0x{:08X} data=0x{:016X} \"{}\"", label, vendor.vendorFaultCode, vendor.vendorFaultData, vendor.description);
}

void WriteVendorBinary(const void* data, size_t size, std::string_view destination) noexcept {
    if (data == nullptr || size == 0) {
        return;
    }
    if (destination.empty()) {
        ZHLN::Log("  Vendor crash dump not written: no destination configured (DiagnosticConfig::crashDumpPath).");
        return;
    }

    const std::filesystem::path path(destination);
    std::error_code             ec;
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        ZHLN::Log("  Failed to write vendor crash dump ({} bytes) to '{}'", size, path.string());
        return;
    }
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    ZHLN::Log("  Saved vendor crash dump: {} ({} bytes)", path.string(), size);
}

void LogShaderAbortMessages(const void* data, uint64_t size) noexcept {
    if (data == nullptr || size == 0) {
        return;
    }

    const auto*    bytes  = static_cast<const uint8_t*>(data);
    const uint8_t* cursor = bytes;
    const uint8_t* end    = bytes + size;
    uint32_t       index  = 0;

    while (static_cast<uint64_t>(end - cursor) >= sizeof(uint64_t)) {
        uint64_t length = 0;
        std::memcpy(&length, cursor, sizeof(length));
        cursor += sizeof(uint64_t);
        if (length > static_cast<uint64_t>(end - cursor)) {
            ZHLN::Log("  Abort Msg #{}: truncated (claimed {} bytes, {} remain)", index, length, static_cast<uint64_t>(end - cursor));
            break;
        }

        std::string text;
        text.reserve(static_cast<size_t>(length));
        for (uint64_t i = 0; i < length; ++i) {
            const char ch = static_cast<char>(cursor[i]);
            text.push_back((ch >= 0x20 && ch < 0x7F) ? ch : '.');
        }
        ZHLN::Log("  Abort Msg #{} ({} bytes): \"{}\"", index, length, text);
        cursor += length;

        const auto addr    = reinterpret_cast<uintptr_t>(cursor);
        const auto aligned = (addr + 7u) & ~static_cast<uintptr_t>(7u);
        cursor             = reinterpret_cast<const uint8_t*>(aligned);
        if (cursor > end) {
            break;
        }
        ++index;
    }
}

void DumpKhrDeviceFault(VkDevice device, const GPUAddressTracker* tracker, std::string_view crashDumpPath) noexcept {
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
        std::vector<VkDeviceFaultInfoKHR> reports(count, VkDeviceFaultInfoKHR {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_KHR});
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
            LogFaultAddress(tracker, "Fault Addr", report.faultAddressInfo);
            LogFaultAddress(tracker, "Instruction Addr", report.instructionAddressInfo);
            LogVendorInfo("Vendor Info", report.vendorInfo);
        }
    }

    if (vkGetDeviceFaultDebugInfoKHR == nullptr) {
        return;
    }

    VkDeviceFaultShaderAbortMessageInfoKHR abortInfo {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_SHADER_ABORT_MESSAGE_INFO_KHR};
    VkDeviceFaultDebugInfoKHR              debugInfo {
        .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_DEBUG_INFO_KHR,
        .pNext = &abortInfo,
    };

    res = vkGetDeviceFaultDebugInfoKHR(device, &debugInfo);
    if (res != VK_SUCCESS && res != VK_INCOMPLETE) {
        return;
    }
    if (debugInfo.vendorBinarySize == 0 && abortInfo.messageDataSize == 0) {
        return;
    }

    std::vector<uint8_t> binary(debugInfo.vendorBinarySize);
    std::vector<uint8_t> abortBytes(abortInfo.messageDataSize);
    debugInfo.pVendorBinaryData = binary.empty() ? nullptr : binary.data();
    abortInfo.pMessageData      = abortBytes.empty() ? nullptr : abortBytes.data();

    res = vkGetDeviceFaultDebugInfoKHR(device, &debugInfo);
    if (res != VK_SUCCESS && res != VK_INCOMPLETE) {
        ZHLN::Log("[GPU DEVICE FAULT] vkGetDeviceFaultDebugInfoKHR payload query failed ({})", ZHLN::Reflect::EnumToString(res));
        return;
    }

    LogShaderAbortMessages(abortInfo.pMessageData, abortInfo.messageDataSize);
    WriteVendorBinary(debugInfo.pVendorBinaryData, debugInfo.vendorBinarySize, crashDumpPath);
}

void DumpExtDeviceFault(VkDevice device, const GPUAddressTracker* tracker, std::string_view crashDumpPath) noexcept {
    if (vkGetDeviceFaultInfoEXT == nullptr) {
        return;
    }

    VkDeviceFaultCountsEXT counts {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    VkResult               res = vkGetDeviceFaultInfoEXT(device, &counts, nullptr);
    if (res != VK_SUCCESS && res != VK_INCOMPLETE) {
        ZHLN::Log("[GPU DEVICE FAULT] vkGetDeviceFaultInfoEXT count query failed ({})", ZHLN::Reflect::EnumToString(res));
        return;
    }

    std::vector<VkDeviceFaultAddressInfoEXT> addressInfos(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT>  vendorInfos(counts.vendorInfoCount);
    std::vector<uint8_t>                     vendorBinary(counts.vendorBinarySize);

    VkDeviceFaultInfoEXT info {
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
        LogFaultAddress(tracker, std::format("Fault Addr #{}", i), addressInfos[i]);
    }
    for (uint32_t i = 0; i < counts.vendorInfoCount; ++i) {
        LogVendorInfo(std::format("Vendor Info #{}", i), vendorInfos[i]);
    }
    WriteVendorBinary(info.pVendorBinaryData, counts.vendorBinarySize, crashDumpPath);
}

} // namespace

auto VKAPI_CALL GPUDiagnostics::AddressBindingCallback(
    const VkDebugUtilsMessageSeverityFlagBitsEXT /*severity*/,
    const VkDebugUtilsMessageTypeFlagsEXT        type,
    const VkDebugUtilsMessengerCallbackDataEXT*  callbackData,
    void*                                        userData
) noexcept -> VkBool32 {
    if ((type & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) != 0 && callbackData != nullptr && userData != nullptr) {
        for (const auto* node = static_cast<const VkBaseInStructure*>(callbackData->pNext); node != nullptr; node = node->pNext) {
            if (node->sType == VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) {
                static_cast<GPUAddressTracker*>(userData)->OnBindingEvent(
                    *reinterpret_cast<const VkDeviceAddressBindingCallbackDataEXT*>(node), callbackData
                );
                break;
            }
        }
    }
    return VK_FALSE;
}

void GPUDiagnostics::Create(Context& ctx, DiagnosticConfig config) noexcept {
    Shutdown();
    _ctx           = &ctx;
    _instance      = ctx.Instance().Handle();
    _device        = ctx.Device();
    _config        = std::move(config);
    _markersActive = _config.enableMarkers && vkCmdInsertDebugUtilsLabelEXT != nullptr && _device != VK_NULL_HANDLE;

    ctx.SetAddressTracker(&_addressTracker);

    if (ctx.DeviceAddressBindingReportEnabled() && _instance != VK_NULL_HANDLE && vkCreateDebugUtilsMessengerEXT != nullptr) {
        _addressTracker.SetEnabled(true);
        const VkDebugUtilsMessengerCreateInfoEXT addressBindingInfo {
            .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .pNext           = nullptr,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT,
            .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT,
            .pfnUserCallback = &GPUDiagnostics::AddressBindingCallback,
            .pUserData       = &_addressTracker,
        };
        if (vkCreateDebugUtilsMessengerEXT(_instance, &addressBindingInfo, nullptr, &_addressBindingMessenger) != VK_SUCCESS) {
            _addressBindingMessenger = VK_NULL_HANDLE;
            _addressTracker.SetEnabled(false);
        }
    }
}

void GPUDiagnostics::Shutdown() noexcept {
    _addressTracker.SetEnabled(false);
    if (_addressBindingMessenger != VK_NULL_HANDLE && _instance != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT != nullptr) {
        vkDestroyDebugUtilsMessengerEXT(_instance, _addressBindingMessenger, nullptr);
    }
    if (_ctx != nullptr) {
        _ctx->SetAddressTracker(nullptr);
        _ctx = nullptr;
    }
    _addressBindingMessenger = VK_NULL_HANDLE;
    _instance                = VK_NULL_HANDLE;
    _device                  = VK_NULL_HANDLE;
    _markersActive           = false;
    _config                  = {};
}

void GPUDiagnostics::WriteCheckpointSlow(VkCommandBuffer cmd, std::string_view name) const noexcept {
    const String128            labelName(name);
    const VkDebugUtilsLabelEXT label {
        .sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
        .pNext      = nullptr,
        .pLabelName = labelName.c_str(),
        .color      = {0.20F, 0.65F, 1.0F, 1.0F},
    };
    vkCmdInsertDebugUtilsLabelEXT(cmd, &label);
}

void GPUDiagnostics::OnDeviceLost() const noexcept {
    if (_device == VK_NULL_HANDLE || !_config.enableCrashDumps) {
        return;
    }

    if (vkGetDeviceFaultReportsKHR != nullptr) {
        DumpKhrDeviceFault(_device, &_addressTracker, _config.crashDumpPath);
    } else if (vkGetDeviceFaultInfoEXT != nullptr) {
        DumpExtDeviceFault(_device, &_addressTracker, _config.crashDumpPath);
    }
}

} // namespace ZHLN::Vk
