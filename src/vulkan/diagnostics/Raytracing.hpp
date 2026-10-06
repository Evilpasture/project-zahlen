// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <cstdint>

namespace ZHLN::Vk {

enum class AccelerationStructureType : uint8_t {
    TopLevel,
    BottomLevel,
};

struct AccelerationStructureSizes {
    VkDeviceSize acceleration_structure_size = 0;
    VkDeviceSize build_scratch_size = 0;
    VkDeviceSize update_scratch_size = 0;
};

struct BlasGeometryDesc {
    VkDeviceAddress vertex_data = 0;
    uint32_t        vertex_stride = 0;
    uint32_t        max_vertex = 0;
    VkFormat        vertex_format = VK_FORMAT_R32G32B32_SFLOAT;
    VkDeviceAddress index_data = 0;
    VkIndexType     index_type = VK_INDEX_TYPE_NONE_KHR;
};

struct TlasGeometryDesc {
    VkDeviceAddress instance_data = 0;
};

[[nodiscard]] auto GetBLASSizes(VkDevice device, const BlasGeometryDesc& desc, uint32_t primitiveCount) noexcept
    -> AccelerationStructureSizes;
[[nodiscard]] auto GetTLASSizes(VkDevice device, uint32_t instanceCount) noexcept -> AccelerationStructureSizes;

[[nodiscard]] auto CreateAccelerationStructure(
    VkDevice device,
    VkBuffer buffer,
    VkDeviceSize size,
    AccelerationStructureType type
) noexcept -> std::expected<AccelerationStructure, VkResult>;
[[nodiscard]] auto GetAccelerationStructureAddress(VkDevice device, VkAccelerationStructureKHR accelerationStructure) noexcept
    -> VkDeviceAddress;

void BuildBLAS(
    VkCommandBuffer cmd,
    const BlasGeometryDesc& desc,
    VkAccelerationStructureKHR destination,
    BufferSlice scratch,
    uint32_t primitiveCount
) noexcept;
void BuildTLAS(
    VkCommandBuffer cmd,
    const TlasGeometryDesc& desc,
    VkAccelerationStructureKHR destination,
    BufferSlice scratch,
    uint32_t instanceCount
) noexcept;

} // namespace ZHLN::Vk
