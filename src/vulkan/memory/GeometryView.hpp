// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "DeviceAddress.hpp"
#include <cstdint>
#include <optional>
#include <type_traits>

namespace ZHLN::Vk {

// A value-type description of a drawable triangle range -- what a bottom-level
// acceleration structure is built from. Renderer code hands these around; the
// VkAccelerationStructureGeometryKHR / build-range structs are synthesized only
// where the Vulkan API consumes them.
class GeometryView {
  public:
    /// A VkIndexType does not fit in the one byte this view has for it
    /// (VK_INDEX_TYPE_NONE_KHR is 1000165000), so the width is stored as a token.
    enum class IndexFormat : uint8_t {
        None   = 0,
        UInt16 = 1,
        UInt32 = 2,
    };

    constexpr GeometryView() noexcept = default;

    // -------------------------------------------------------------------------
    // Static Factories
    // -------------------------------------------------------------------------

    /// Raw address factory
    [[nodiscard]] static constexpr auto Triangles(
        VkDeviceAddress vertexAddr,
        uint32_t        vertexCount,
        uint32_t        vertexStride,
        VkFormat        vertexFormat = VK_FORMAT_R32G32B32_SFLOAT
    ) noexcept -> GeometryView {
        GeometryView g;
        g._vertexAddr   = vertexAddr;
        g._vertexCount  = vertexCount;
        g._vertexStride = static_cast<uint16_t>(vertexStride);
        g._vertexFormat = static_cast<uint8_t>(vertexFormat);
        return g;
    }

    /// Type-deducing factory for typed device addresses (deduces sizeof(VertexT))
    template <typename VertexT>
        requires (!std::is_void_v<VertexT>)
    [[nodiscard]] static constexpr auto Triangles(
        DeviceAddress<VertexT> vertexAddr,
        uint32_t               vertexCount,
        VkFormat               vertexFormat = VK_FORMAT_R32G32B32_SFLOAT
    ) noexcept -> GeometryView {
        return Triangles(vertexAddr.Get(), vertexCount, static_cast<uint32_t>(sizeof(VertexT)), vertexFormat);
    }

    /// Explicit-vertex-type factory for untyped device addresses. DeviceAddress<>
    /// converts to DeviceAddress<VertexT> in neither direction on its own, and the
    /// renderer's mesh buffer addresses are untyped -- this overload is what lets
    /// Triangles<VertexPosition>(mesh.vboAddress, count) read the stride from VertexT.
    template <typename VertexT>
        requires (!std::is_void_v<VertexT>)
    [[nodiscard]] static constexpr auto Triangles(
        DeviceAddress<> vertexAddr,
        uint32_t        vertexCount,
        VkFormat        vertexFormat = VK_FORMAT_R32G32B32_SFLOAT
    ) noexcept -> GeometryView {
        return Triangles(vertexAddr.Get(), vertexCount, static_cast<uint32_t>(sizeof(VertexT)), vertexFormat);
    }

    /// Untyped DeviceAddress<> overload
    [[nodiscard]] static constexpr auto Triangles(
        DeviceAddress<void> vertexAddr,
        uint32_t            vertexCount,
        uint32_t            vertexStride,
        VkFormat            vertexFormat = VK_FORMAT_R32G32B32_SFLOAT
    ) noexcept -> GeometryView {
        return Triangles(vertexAddr.Get(), vertexCount, vertexStride, vertexFormat);
    }

    // -------------------------------------------------------------------------
    // Fluent Configuration ("Tell, Don't Ask")
    // -------------------------------------------------------------------------

    [[nodiscard]] constexpr auto Indexed(
        VkDeviceAddress indexAddr,
        uint32_t        indexCount,
        VkIndexType     indexType = VK_INDEX_TYPE_UINT32
    ) const noexcept -> GeometryView {
        GeometryView copy = *this;
        copy._indexAddr   = indexAddr;
        copy._indexCount  = indexCount;
        copy._indexFormat = EncodeIndexType(indexType);
        return copy;
    }

    template <typename IndexT>
        requires (!std::is_void_v<IndexT>)
    [[nodiscard]] constexpr auto Indexed(DeviceAddress<IndexT> indexAddr, uint32_t indexCount) const noexcept -> GeometryView {
        constexpr VkIndexType type = (sizeof(IndexT) == 2) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        return Indexed(indexAddr.Get(), indexCount, type);
    }

    /// Untyped index address: the index width cannot be deduced from DeviceAddress<>,
    /// so 32-bit is the assumption.
    [[nodiscard]] constexpr auto Indexed(DeviceAddress<> indexAddr, uint32_t indexCount) const noexcept -> GeometryView {
        return Indexed(indexAddr.Get(), indexCount, VK_INDEX_TYPE_UINT32);
    }

    /// Conditional indexing: automatically returns an unindexed view if addr is 0/nullptr or count is 0
    [[nodiscard]] constexpr auto MaybeIndexed(
        VkDeviceAddress indexAddr,
        uint32_t        indexCount,
        VkIndexType     indexType = VK_INDEX_TYPE_UINT32
    ) const noexcept -> GeometryView {
        if (indexAddr == 0 || indexCount == 0) {
            GeometryView copy = *this;
            copy._indexAddr   = 0;
            copy._indexCount  = 0;
            copy._indexFormat = IndexFormat::None;
            return copy;
        }
        return Indexed(indexAddr, indexCount, indexType);
    }

    template <typename IndexT>
        requires (!std::is_void_v<IndexT>)
    [[nodiscard]] constexpr auto MaybeIndexed(DeviceAddress<IndexT> indexAddr, uint32_t indexCount) const noexcept -> GeometryView {
        return MaybeIndexed(indexAddr.Get(), indexCount, (sizeof(IndexT) == 2) ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    }

    /// Untyped index address; see Indexed(DeviceAddress<>).
    [[nodiscard]] constexpr auto MaybeIndexed(DeviceAddress<> indexAddr, uint32_t indexCount) const noexcept -> GeometryView {
        return MaybeIndexed(indexAddr.Get(), indexCount, VK_INDEX_TYPE_UINT32);
    }

    [[nodiscard]] constexpr auto Opaque(bool isOpaque = true) const noexcept -> GeometryView {
        GeometryView copy = *this;
        if (isOpaque) {
            copy._flags |= VK_GEOMETRY_OPAQUE_BIT_KHR;
        } else {
            copy._flags &= ~static_cast<VkGeometryFlagsKHR>(VK_GEOMETRY_OPAQUE_BIT_KHR);
        }
        return copy;
    }

    [[nodiscard]] constexpr auto WithFlags(VkGeometryFlagsKHR flags) const noexcept -> GeometryView {
        GeometryView copy = *this;
        copy._flags       = flags;
        return copy;
    }

    // -------------------------------------------------------------------------
    // Fluent Slicing / Submesh Views
    // -------------------------------------------------------------------------

    /// Slice by raw indices (for indexed) or vertices (for unindexed)
    [[nodiscard]] constexpr auto Submesh(uint32_t firstIndex, uint32_t count) const noexcept -> GeometryView {
        GeometryView sub = *this;
        if (HasIndices()) {
            sub._indexAddr  = _indexAddr + static_cast<VkDeviceAddress>(firstIndex) * IndexSize();
            sub._indexCount = count;
        } else {
            sub._vertexAddr  = _vertexAddr + static_cast<VkDeviceAddress>(firstIndex) * _vertexStride;
            sub._vertexCount = count;
        }
        return sub;
    }

    /// Slices by triangle count (1 primitive = 3 indices / vertices)
    [[nodiscard]] constexpr auto Primitives(uint32_t firstTriangle, uint32_t triangleCount) const noexcept -> GeometryView {
        return Submesh(firstTriangle * 3u, triangleCount * 3u);
    }

    // -------------------------------------------------------------------------
    // Observers
    // -------------------------------------------------------------------------

    [[nodiscard]] constexpr auto VertexAddress() const noexcept -> DeviceAddress<> { return DeviceAddress<>(_vertexAddr); }
    [[nodiscard]] constexpr auto IndexAddress() const noexcept -> DeviceAddress<> { return DeviceAddress<>(_indexAddr); }
    [[nodiscard]] constexpr auto VertexCount() const noexcept -> uint32_t { return _vertexCount; }
    [[nodiscard]] constexpr auto IndexCount() const noexcept -> uint32_t { return _indexCount; }
    [[nodiscard]] constexpr auto VertexStride() const noexcept -> uint32_t { return _vertexStride; }
    [[nodiscard]] constexpr auto VertexFormat() const noexcept -> VkFormat { return static_cast<VkFormat>(_vertexFormat); }
    [[nodiscard]] constexpr auto IndexType() const noexcept -> VkIndexType { return DecodeIndexType(); }
    [[nodiscard]] constexpr auto Flags() const noexcept -> VkGeometryFlagsKHR { return _flags; }
    [[nodiscard]] constexpr auto HasIndices() const noexcept -> bool { return _indexAddr != 0 && _indexFormat != IndexFormat::None; }
    [[nodiscard]] constexpr auto PrimitiveCount() const noexcept -> uint32_t {
        return HasIndices() ? (_indexCount / 3u) : (_vertexCount / 3u);
    }
    [[nodiscard]] constexpr auto Valid() const noexcept -> bool { return _vertexAddr != 0 && _vertexCount >= 3; }
    explicit constexpr operator bool() const noexcept { return Valid(); }

    // -------------------------------------------------------------------------
    // JIT Driver Stack Synthesis
    // -------------------------------------------------------------------------

    [[nodiscard]] constexpr auto AsGeometryKHR(std::optional<VkGeometryFlagsKHR> overrideFlags = std::nullopt) const noexcept
        -> VkAccelerationStructureGeometryKHR {
        return {
            .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .pNext        = nullptr,
            .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
            .geometry     = {
                .triangles = {
                    .sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                    .pNext         = nullptr,
                    .vertexFormat  = static_cast<VkFormat>(_vertexFormat),
                    .vertexData    = {.deviceAddress = _vertexAddr},
                    .vertexStride  = _vertexStride,
                    .maxVertex     = _vertexCount > 0 ? _vertexCount - 1 : 0,
                    // The API requires NONE whenever indexData is null.
                    .indexType     = HasIndices() ? DecodeIndexType() : VK_INDEX_TYPE_NONE_KHR,
                    .indexData     = {.deviceAddress = _indexAddr},
                    .transformData = {.deviceAddress = 0},
                }
            },
            .flags        = overrideFlags.value_or(_flags),
        };
    }

    [[nodiscard]] constexpr auto AsRangeInfo() const noexcept -> VkAccelerationStructureBuildRangeInfoKHR {
        return {
            .primitiveCount  = PrimitiveCount(),
            .primitiveOffset = 0,
            .firstVertex     = 0,
            .transformOffset = 0,
        };
    }

  private:
    [[nodiscard]] static constexpr auto EncodeIndexType(VkIndexType type) noexcept -> IndexFormat {
        switch (type) {
            case VK_INDEX_TYPE_UINT16: return IndexFormat::UInt16;
            case VK_INDEX_TYPE_UINT32: return IndexFormat::UInt32;
            default:                   return IndexFormat::None;
        }
    }

    [[nodiscard]] constexpr auto DecodeIndexType() const noexcept -> VkIndexType {
        switch (_indexFormat) {
            case IndexFormat::UInt16: return VK_INDEX_TYPE_UINT16;
            case IndexFormat::UInt32: return VK_INDEX_TYPE_UINT32;
            default:                  return VK_INDEX_TYPE_NONE_KHR;
        }
    }

    [[nodiscard]] constexpr auto IndexSize() const noexcept -> uint32_t {
        return _indexFormat == IndexFormat::UInt16 ? 2u : 4u;
    }

    VkDeviceAddress    _vertexAddr   = 0;                          // 8 bytes (offset 0)
    VkDeviceAddress    _indexAddr    = 0;                          // 8 bytes (offset 8)
    uint32_t           _vertexCount  = 0;                          // 4 bytes (offset 16)
    uint32_t           _indexCount   = 0;                          // 4 bytes (offset 20)
    VkGeometryFlagsKHR _flags        = VK_GEOMETRY_OPAQUE_BIT_KHR; // 4 bytes (offset 24)
    uint16_t           _vertexStride = 0;                          // 2 bytes (offset 28)
    uint8_t            _vertexFormat = 0;                          // 1 byte  (offset 30)
    IndexFormat        _indexFormat  = IndexFormat::None;          // 1 byte  (offset 31)
};

static_assert(sizeof(GeometryView) == 32, "GeometryView must remain exactly 32 bytes.");
static_assert(std::is_trivially_copyable_v<GeometryView>);

} // namespace ZHLN::Vk
