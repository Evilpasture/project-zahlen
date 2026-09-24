// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Free functions over the VK_KHR_acceleration_structure entry points. There is
// no context object to carry: the entry points are Volk globals routed through
// the one device volkLoadDevice loaded (the engine is single-device by
// design), so a build needs only the handles it operates on.
//
// Callers gate on Context::RayTracingSupported() -- all three of
// acceleration_structure, ray_query and deferred_host_operations enabled --
// and never reach these without it: a missing entry point here would be a
// null call, not a fallback.

namespace ZHLN::Vk {

void GetBLASSizes(VkDevice device, const ZHLN_BlasGeometryDesc& desc, uint32_t primCount, ZHLN_AccelerationStructureSizes& outSizes) noexcept;
void GetTLASSizes(VkDevice device, uint32_t instanceCount, ZHLN_AccelerationStructureSizes& outSizes) noexcept;

[[nodiscard]] auto
    CreateAccelerationStructure(VkDevice device, VkBuffer buffer, VkDeviceSize size, ZHLN_AccelerationStructureType type) noexcept -> VkAccelerationStructureKHR;
void DestroyAccelerationStructure(VkDevice device, VkAccelerationStructureKHR as) noexcept;
[[nodiscard]] auto GetAccelerationStructureAddress(VkDevice device, VkAccelerationStructureKHR as) noexcept -> VkDeviceAddress;

void BuildBLAS(VkCommandBuffer cmd, const ZHLN_BlasGeometryDesc& desc, VkAccelerationStructureKHR dst, VkDeviceAddress scratch, uint32_t primCount) noexcept;
void BuildTLAS(VkCommandBuffer cmd, const ZHLN_TlasGeometryDesc& desc, VkAccelerationStructureKHR dst, VkDeviceAddress scratch, uint32_t instanceCount) noexcept;

} // namespace ZHLN::Vk
