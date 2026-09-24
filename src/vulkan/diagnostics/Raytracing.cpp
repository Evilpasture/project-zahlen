// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Raytracing.hpp"

namespace ZHLN::Vk {

void GetBLASSizes(const VkDevice device, const ZHLN_BlasGeometryDesc& desc, uint32_t primCount, ZHLN_AccelerationStructureSizes& outSizes) noexcept {
    ZHLN_GetBlasSizes(device, &desc, primCount, &outSizes);
}

void GetTLASSizes(const VkDevice device, uint32_t instanceCount, ZHLN_AccelerationStructureSizes& outSizes) noexcept {
    ZHLN_GetTlasSizes(device, instanceCount, &outSizes);
}

auto CreateAccelerationStructure(const VkDevice device, VkBuffer buffer, VkDeviceSize size, ZHLN_AccelerationStructureType type) noexcept
    -> VkAccelerationStructureKHR {
    return ZHLN_CreateAS(device, buffer, size, type);
}

void DestroyAccelerationStructure(const VkDevice device, VkAccelerationStructureKHR as) noexcept {
    ZHLN_DestroyAS(device, as);
}

auto GetAccelerationStructureAddress(const VkDevice device, VkAccelerationStructureKHR as) noexcept -> VkDeviceAddress {
    return ZHLN_GetASAddress(device, as);
}

void BuildBLAS(VkCommandBuffer cmd, const ZHLN_BlasGeometryDesc& desc, VkAccelerationStructureKHR dst, VkDeviceAddress scratch, uint32_t primCount) noexcept {
    ZHLN_CmdBuildBlas(cmd, &desc, dst, scratch, primCount);
}

void BuildTLAS(VkCommandBuffer cmd, const ZHLN_TlasGeometryDesc& desc, VkAccelerationStructureKHR dst, VkDeviceAddress scratch, uint32_t instanceCount) noexcept {
    ZHLN_CmdBuildTlas(cmd, &desc, dst, scratch, instanceCount);
}

} // namespace ZHLN::Vk
