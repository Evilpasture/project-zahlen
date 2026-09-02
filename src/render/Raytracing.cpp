// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Raytracing.hpp"
#include <Zahlen/Log.hpp>

namespace ZHLN::Vk {

bool RayTracingContext::Init(VkDevice device) noexcept {
    bool ok = ZHLN_InitRayTracingContext(device, &_raw);
    if (!ok) {
        _raw.device = VK_NULL_HANDLE;
    }
    return ok;
}

void RayTracingContext::GetBLASSizes(const ZHLN_BlasGeometryDesc& desc, uint32_t primCount, ZHLN_AccelerationStructureSizes& outSizes) const noexcept {
    ZHLN_GetBlasSizes(&_raw, &desc, primCount, &outSizes);
}

void RayTracingContext::GetTLASSizes(uint32_t instanceCount, ZHLN_AccelerationStructureSizes& outSizes) const noexcept {
    ZHLN_GetTlasSizes(&_raw, instanceCount, &outSizes);
}

VkAccelerationStructureKHR RayTracingContext::CreateAccelerationStructure(VkBuffer buffer, VkDeviceSize size, ZHLN_AccelerationStructureType type) const noexcept {
    return ZHLN_CreateAS(&_raw, buffer, size, type);
}

void RayTracingContext::DestroyAccelerationStructure(VkAccelerationStructureKHR as) const noexcept {
    ZHLN_DestroyAS(&_raw, as);
}

VkDeviceAddress RayTracingContext::GetAccelerationStructureAddress(VkAccelerationStructureKHR as) const noexcept {
    // Never ask the driver for an address of a missing AS. A zero address is
    // the null-descriptor sentinel used by the heap writer when the optional
    // nullDescriptor feature is enabled.
    if (!Valid() || as == VK_NULL_HANDLE || _raw.get_address == nullptr) {
        return 0;
    }

    const VkDeviceAddress address = ZHLN_GetASAddress(&_raw, as);
    if (address != 0 && !IsAccelerationStructureAddressAligned(address)) {
        // This is a producer/allocation bug, not a reason to round the address:
        // masking it could turn a valid AS address into the middle of another
        // allocation. The descriptor writer rejects the raw value as well.
        ZHLN::Log(
            "[RayTracing] acceleration-structure address 0x{:x} is not aligned to {} bytes; refusing to repair it.",
            static_cast<uint64_t>(address), kAccelerationStructureAddressAlignment
        );
    }
    return address;
}

void RayTracingContext::BuildBLAS(
    VkCommandBuffer              cmd,
    const ZHLN_BlasGeometryDesc& desc,
    VkAccelerationStructureKHR   dst,
    VkDeviceAddress              scratch,
    uint32_t                     primCount
) const noexcept {
    ZHLN_CmdBuildBlas(&_raw, cmd, &desc, dst, scratch, primCount);
}

void RayTracingContext::BuildTLAS(
    VkCommandBuffer              cmd,
    const ZHLN_TlasGeometryDesc& desc,
    VkAccelerationStructureKHR   dst,
    VkDeviceAddress              scratch,
    uint32_t                     instanceCount
) const noexcept {
    ZHLN_CmdBuildTlas(&_raw, cmd, &desc, dst, scratch, instanceCount);
}
} // namespace ZHLN::Vk
