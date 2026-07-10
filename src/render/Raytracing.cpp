// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Raytracing.hpp"

namespace ZHLN::Vk {

bool RayTracingContext::Init(VkDevice device) noexcept {
    bool ok = ZHLN_InitRayTracingContext(device, &_raw);
    if (!ok) {
        _raw.device = VK_NULL_HANDLE;
    }
    return ok;
}

void RayTracingContext::GetBlasSizes(const ZHLN_BlasGeometryDesc& desc, uint32_t primCount, ZHLN_AccelerationStructureSizes& outSizes) const noexcept {
    ZHLN_GetBlasSizes(&_raw, &desc, primCount, &outSizes);
}

void RayTracingContext::GetTlasSizes(uint32_t instanceCount, ZHLN_AccelerationStructureSizes& outSizes) const noexcept {
    ZHLN_GetTlasSizes(&_raw, instanceCount, &outSizes);
}

VkAccelerationStructureKHR RayTracingContext::CreateAS(VkBuffer buffer, VkDeviceSize size, ZHLN_AccelerationStructureType type) const noexcept {
    return ZHLN_CreateAS(&_raw, buffer, size, type);
}

void RayTracingContext::DestroyAS(VkAccelerationStructureKHR as) const noexcept {
    ZHLN_DestroyAS(&_raw, as);
}

VkDeviceAddress RayTracingContext::GetASAddress(VkAccelerationStructureKHR as) const noexcept {
    return ZHLN_GetASAddress(&_raw, as);
}

void RayTracingContext::CmdBuildBlas(
    VkCommandBuffer              cmd,
    const ZHLN_BlasGeometryDesc& desc,
    VkAccelerationStructureKHR   dst,
    VkDeviceAddress              scratch,
    uint32_t                     primCount
) const noexcept {
    ZHLN_CmdBuildBlas(&_raw, cmd, &desc, dst, scratch, primCount);
}

void RayTracingContext::CmdBuildTlas(
    VkCommandBuffer              cmd,
    const ZHLN_TlasGeometryDesc& desc,
    VkAccelerationStructureKHR   dst,
    VkDeviceAddress              scratch,
    uint32_t                     instanceCount
) const noexcept {
    ZHLN_CmdBuildTlas(&_raw, cmd, &desc, dst, scratch, instanceCount);
}
} // namespace ZHLN::Vk
