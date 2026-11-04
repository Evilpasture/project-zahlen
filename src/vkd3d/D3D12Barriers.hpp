// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// D3D12 synthesis for the neutral barrier values (<vkd3d/Barriers.hpp>),
// owned by the joint layer alongside D3D12Conversions.hpp: Make*()
// lowers a VkD3D::{Memory,Buffer,Image}Barrier to the enhanced-barrier
// structs, and Record() submits a whole span as one
// ID3D12GraphicsCommandList7::Barrier call -- one barrier group per kind,
// never one call per resource. Nothing under src/d3d12 names a VkD3D
// type -- the backend keeps its own native barrier synthesis
// (src/d3d12/Barriers.hpp); this header is how the joint layer (and,
// later, the renderer) records neutral barriers on a D3D12 device.
// There is no state tracking here: call sites own before/after
// knowledge, this layer only translates and submits.

#include "D3D12Conversions.hpp"

#include <vkd3d/Barriers.hpp>

#include <cstdint>
#include <span>

namespace ZHLN::VkD3D {

// Every subresource of the texture (enhanced barriers spell the
// all-subresources range as IndexOrFirstMipLevel == 0xFFFFFFFF).
inline constexpr D3D12_BARRIER_SUBRESOURCE_RANGE kAllSubresources {
    .IndexOrFirstMipLevel = 0xFFFFFFFFu,
    .NumMipLevels         = 0,
    .FirstArraySlice      = 0,
    .NumArraySlices       = 0,
    .FirstPlane           = 0,
    .NumPlanes            = 0,
};

/// Neutral subresource selection -> D3D12 range. Full-range barriers use
/// the all-subresources spelling; partial ranges map "remaining" counts
/// to 0 (all remaining). Depth/stencil aspects select planes (0 = depth,
/// 1 = stencil); color barriers leave the plane range at 0/0 (all planes).
[[nodiscard]] constexpr auto SubresourceRangeFor(const ImageBarrier& barrier) noexcept -> D3D12_BARRIER_SUBRESOURCE_RANGE {
    const bool allMips   = barrier.baseMipLevel == 0 && barrier.mipLevelCount == ImageBarrier::kRemainingMips;
    const bool allLayers = barrier.baseArrayLayer == 0 && barrier.arrayLayerCount == ImageBarrier::kRemainingLayers;
    if (allMips && allLayers) {
        return kAllSubresources;
    }
    const bool     depth      = (barrier.aspect & ImageAspect::Depth) != ImageAspect::None;
    const bool     stencil    = (barrier.aspect & ImageAspect::Stencil) != ImageAspect::None;
    const uint32_t firstPlane = stencil && !depth ? 1u : 0u;
    const uint32_t numPlanes  = (depth && stencil) ? 2u : (depth || stencil) ? 1u : 0u;
    return D3D12_BARRIER_SUBRESOURCE_RANGE {
        .IndexOrFirstMipLevel = barrier.baseMipLevel,
        .NumMipLevels         = barrier.mipLevelCount == ImageBarrier::kRemainingMips ? 0u : barrier.mipLevelCount,
        .FirstArraySlice      = barrier.baseArrayLayer,
        .NumArraySlices       = barrier.arrayLayerCount == ImageBarrier::kRemainingLayers ? 0u : barrier.arrayLayerCount,
        .FirstPlane           = firstPlane,
        .NumPlanes            = numPlanes,
    };
}

// The resource word inside a neutral barrier is the Vulkan handle and is
// ignored here -- the ID3D12Resource* is the identity on this side. A
// transition out of Undefined discards previous contents
// (D3D12_TEXTURE_BARRIER_FLAG_DISCARD), matching the Vulkan
// oldLayout == UNDEFINED semantics. Queue-family fields have no D3D12
// meaning and are not read.
[[nodiscard]] constexpr auto MakeTextureBarrier(const ImageBarrier& barrier, ID3D12Resource* resource) noexcept -> D3D12_TEXTURE_BARRIER {
    D3D12_TEXTURE_BARRIER result {
        .SyncBefore   = ToD3D12Sync(barrier.stageBefore),
        .SyncAfter    = ToD3D12Sync(barrier.stageAfter),
        .AccessBefore = ToD3D12Access(barrier.accessBefore),
        .AccessAfter  = ToD3D12Access(barrier.accessAfter),
        .LayoutBefore = ToD3D12Layout(barrier.layoutBefore),
        .LayoutAfter  = ToD3D12Layout(barrier.layoutAfter),
        .pResource    = resource,
        .Subresources = SubresourceRangeFor(barrier),
        .Flags        = D3D12_TEXTURE_BARRIER_FLAG_NONE,
    };
    if (barrier.layoutBefore == ImageLayout::Undefined) {
        result.Flags = D3D12_TEXTURE_BARRIER_FLAG_DISCARD;
    }
    return result;
}

[[nodiscard]] constexpr auto MakeBufferBarrier(const BufferBarrier& barrier, ID3D12Resource* resource) noexcept -> D3D12_BUFFER_BARRIER {
    return D3D12_BUFFER_BARRIER {
        .SyncBefore   = ToD3D12Sync(barrier.stageBefore),
        .SyncAfter    = ToD3D12Sync(barrier.stageAfter),
        .AccessBefore = ToD3D12Access(barrier.accessBefore),
        .AccessAfter  = ToD3D12Access(barrier.accessAfter),
        .pResource    = resource,
        .Offset       = barrier.offset,
        .Size         = barrier.size,
    };
}

[[nodiscard]] constexpr auto MakeGlobalBarrier(const MemoryBarrier& barrier) noexcept -> D3D12_GLOBAL_BARRIER {
    return D3D12_GLOBAL_BARRIER {
        .SyncBefore   = ToD3D12Sync(barrier.stageBefore),
        .SyncAfter    = ToD3D12Sync(barrier.stageAfter),
        .AccessBefore = ToD3D12Access(barrier.accessBefore),
        .AccessAfter  = ToD3D12Access(barrier.accessAfter),
    };
}

inline void Record(ID3D12GraphicsCommandList7* cmd, std::span<const D3D12_TEXTURE_BARRIER> barriers) noexcept {
    if (barriers.empty()) {
        return;
    }
    const D3D12_BARRIER_GROUP group {
        .Type             = D3D12_BARRIER_TYPE_TEXTURE,
        .NumBarriers      = static_cast<uint32_t>(barriers.size()),
        .pTextureBarriers = barriers.data(),
    };
    cmd->Barrier(1, &group);
}

inline void Record(ID3D12GraphicsCommandList7* cmd, std::span<const D3D12_BUFFER_BARRIER> barriers) noexcept {
    if (barriers.empty()) {
        return;
    }
    const D3D12_BARRIER_GROUP group {
        .Type            = D3D12_BARRIER_TYPE_BUFFER,
        .NumBarriers     = static_cast<uint32_t>(barriers.size()),
        .pBufferBarriers = barriers.data(),
    };
    cmd->Barrier(1, &group);
}

inline void Record(ID3D12GraphicsCommandList7* cmd, std::span<const D3D12_GLOBAL_BARRIER> barriers) noexcept {
    if (barriers.empty()) {
        return;
    }
    const D3D12_BARRIER_GROUP group {
        .Type            = D3D12_BARRIER_TYPE_GLOBAL,
        .NumBarriers     = static_cast<uint32_t>(barriers.size()),
        .pGlobalBarriers = barriers.data(),
    };
    cmd->Barrier(1, &group);
}

} // namespace ZHLN::VkD3D
