// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once


namespace ZHLN::Vk {

void GetBLASSizes(VkDevice device, const ZHLN_BlasGeometryDesc& desc, uint32_t primCount, ZHLN_AccelerationStructureSizes& outSizes) noexcept;
void GetTLASSizes(VkDevice device, uint32_t instanceCount, ZHLN_AccelerationStructureSizes& outSizes) noexcept;

[[nodiscard]] auto
    CreateAccelerationStructure(VkDevice device, VkBuffer buffer, VkDeviceSize size, ZHLN_AccelerationStructureType type) noexcept -> VkAccelerationStructureKHR;
[[nodiscard]] auto GetAccelerationStructureAddress(VkDevice device, VkAccelerationStructureKHR as) noexcept -> VkDeviceAddress;

void BuildBLAS(VkCommandBuffer cmd, const ZHLN_BlasGeometryDesc& desc, VkAccelerationStructureKHR dst, BufferSlice scratch, uint32_t primCount) noexcept;
void BuildTLAS(VkCommandBuffer cmd, const ZHLN_TlasGeometryDesc& desc, VkAccelerationStructureKHR dst, BufferSlice scratch, uint32_t instanceCount) noexcept;

}
