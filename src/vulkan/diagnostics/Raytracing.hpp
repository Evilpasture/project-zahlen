// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../VkError.hpp"
#include "../memory/GeometryView.hpp"
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

struct TlasGeometryDesc {
    VkDeviceAddress instance_data = 0;
};

[[nodiscard]] auto GetBLASSizes(VkDevice device, const GeometryView& geometry) noexcept -> AccelerationStructureSizes;
[[nodiscard]] auto GetTLASSizes(VkDevice device, uint32_t instanceCount) noexcept -> AccelerationStructureSizes;

[[nodiscard]] auto CreateAccelerationStructure(
    VkDevice device,
    VkBuffer buffer,
    VkDeviceSize size,
    AccelerationStructureType type
) noexcept -> std::expected<AccelerationStructure, ErrorCode>;
[[nodiscard]] auto GetAccelerationStructureAddress(VkDevice device, VkAccelerationStructureKHR accelerationStructure) noexcept
    -> VkDeviceAddress;

void BuildBLAS(
    VkCommandBuffer cmd,
    const GeometryView& geometry,
    VkAccelerationStructureKHR destination,
    BufferSlice scratch
) noexcept;
void BuildTLAS(
    VkCommandBuffer cmd,
    const TlasGeometryDesc& desc,
    VkAccelerationStructureKHR destination,
    BufferSlice scratch,
    uint32_t instanceCount
) noexcept;

} // namespace ZHLN::Vk
