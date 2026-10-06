// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Raytracing.hpp"

namespace ZHLN::Vk {
namespace {

[[nodiscard]] auto MakeBlasGeometry(const BlasGeometryDesc& desc) noexcept -> VkAccelerationStructureGeometryKHR {
    return VkAccelerationStructureGeometryKHR {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .geometry = VkAccelerationStructureGeometryDataKHR {
            .triangles = VkAccelerationStructureGeometryTrianglesDataKHR {
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                .vertexFormat = desc.vertex_format,
                .vertexData = {.deviceAddress = desc.vertex_data},
                .vertexStride = desc.vertex_stride,
                .maxVertex = desc.max_vertex,
                .indexType = desc.index_type,
                .indexData = {.deviceAddress = desc.index_data},
            },
        },
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
    };
}

[[nodiscard]] auto MakeTlasGeometry(const VkDeviceAddress instanceData) noexcept -> VkAccelerationStructureGeometryKHR {
    return VkAccelerationStructureGeometryKHR {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry = VkAccelerationStructureGeometryDataKHR {
            .instances = VkAccelerationStructureGeometryInstancesDataKHR {
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                .arrayOfPointers = VK_FALSE,
                .data = {.deviceAddress = instanceData},
            },
        },
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
    };
}

[[nodiscard]] auto MakeBuildInfo(
    const VkAccelerationStructureTypeKHR type,
    const VkAccelerationStructureGeometryKHR& geometry,
    const VkAccelerationStructureKHR destination,
    const VkDeviceAddress scratch
) noexcept -> VkAccelerationStructureBuildGeometryInfoKHR {
    return VkAccelerationStructureBuildGeometryInfoKHR {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = type,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .dstAccelerationStructure = destination,
        .geometryCount = 1,
        .pGeometries = &geometry,
        .scratchData = {.deviceAddress = scratch},
    };
}

[[nodiscard]] auto QuerySizes(
    const VkDevice device,
    const VkAccelerationStructureTypeKHR type,
    const VkAccelerationStructureGeometryKHR& geometry,
    const uint32_t primitiveCount
) noexcept -> AccelerationStructureSizes {
    if (device == VK_NULL_HANDLE || vkGetAccelerationStructureBuildSizesKHR == nullptr) {
        return {};
    }
    const VkAccelerationStructureBuildGeometryInfoKHR buildInfo = MakeBuildInfo(type, geometry, VK_NULL_HANDLE, 0);
    VkAccelerationStructureBuildSizesInfoKHR sizes {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
    };
    vkGetAccelerationStructureBuildSizesKHR(
        device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primitiveCount, &sizes
    );
    return {
        .acceleration_structure_size = sizes.accelerationStructureSize,
        .build_scratch_size = sizes.buildScratchSize,
        .update_scratch_size = sizes.updateScratchSize,
    };
}

void CmdBuild(
    const VkCommandBuffer cmd,
    const VkAccelerationStructureTypeKHR type,
    const VkAccelerationStructureGeometryKHR& geometry,
    const VkAccelerationStructureKHR destination,
    const VkDeviceAddress scratch,
    const uint32_t primitiveCount
) noexcept {
    if (vkCmdBuildAccelerationStructuresKHR == nullptr) {
        return;
    }
    const VkAccelerationStructureBuildGeometryInfoKHR buildInfo = MakeBuildInfo(type, geometry, destination, scratch);
    const VkAccelerationStructureBuildRangeInfoKHR rangeInfo {.primitiveCount = primitiveCount};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = {&rangeInfo};
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, ranges);
}

} // namespace

auto GetBLASSizes(const VkDevice device, const BlasGeometryDesc& desc, const uint32_t primitiveCount) noexcept
    -> AccelerationStructureSizes {
    const VkAccelerationStructureGeometryKHR geometry = MakeBlasGeometry(desc);
    return QuerySizes(device, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geometry, primitiveCount);
}

auto GetTLASSizes(const VkDevice device, const uint32_t instanceCount) noexcept -> AccelerationStructureSizes {
    const VkAccelerationStructureGeometryKHR geometry = MakeTlasGeometry(0);
    return QuerySizes(device, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, geometry, instanceCount);
}

auto CreateAccelerationStructure(
    const VkDevice device,
    const VkBuffer buffer,
    const VkDeviceSize size,
    const AccelerationStructureType type
) noexcept -> std::expected<AccelerationStructure, VkResult> {
    const VkAccelerationStructureCreateInfoKHR createInfo {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = buffer,
        .size = size,
        .type = type == AccelerationStructureType::BottomLevel
            ? VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR
            : VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
    };
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    if (const VkResult result = vkCreateAccelerationStructureKHR(device, &createInfo, nullptr, &handle); result != VK_SUCCESS) {
        return std::unexpected(result);
    }
    return AccelerationStructure(device, handle);
}

auto GetAccelerationStructureAddress(const VkDevice device, const VkAccelerationStructureKHR accelerationStructure) noexcept
    -> VkDeviceAddress {
    const VkAccelerationStructureDeviceAddressInfoKHR info {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
        .accelerationStructure = accelerationStructure,
    };
    return vkGetAccelerationStructureDeviceAddressKHR(device, &info);
}

void BuildBLAS(
    const VkCommandBuffer cmd,
    const BlasGeometryDesc& desc,
    const VkAccelerationStructureKHR destination,
    const BufferSlice scratch,
    const uint32_t primitiveCount
) noexcept {
    CmdBuild(
        cmd, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, MakeBlasGeometry(desc), destination, scratch.Address(), primitiveCount
    );
}

void BuildTLAS(
    const VkCommandBuffer cmd,
    const TlasGeometryDesc& desc,
    const VkAccelerationStructureKHR destination,
    const BufferSlice scratch,
    const uint32_t instanceCount
) noexcept {
    CmdBuild(cmd, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, MakeTlasGeometry(desc.instance_data), destination, scratch.Address(), instanceCount);
}

} // namespace ZHLN::Vk
