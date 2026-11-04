#pragma once

// Enhanced-barrier recording for the d3d12 backend. The synthesis mirrors
// the Vulkan side's barrier values (src/vulkan Barriers.hpp): a barrier is
// a plain driver struct built on the stack from the neutral vocabulary
// (VkD3D::* via D3D12Conversions.hpp), and Record() submits a whole span
// as one ID3D12GraphicsCommandList7::Barrier call -- one barrier group per
// kind, never one call per resource. There is no state tracking here:
// call sites (the render graph, later) own before/after knowledge, this
// layer only translates and submits.

#include "D3D12Conversions.hpp"
#include "Sdk.hpp"

#include <cstdint>
#include <span>

namespace ZHLN::D3D12 {

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

[[nodiscard]] constexpr auto MakeTextureBarrier(
    ID3D12Resource*           resource,
    VkD3D::ImageLayout        layoutBefore,
    VkD3D::ImageLayout        layoutAfter,
    VkD3D::PipelineStage      syncBefore,
    VkD3D::PipelineStage      syncAfter,
    VkD3D::Access             accessBefore,
    VkD3D::Access             accessAfter,
    D3D12_BARRIER_SUBRESOURCE_RANGE subresources = kAllSubresources
) noexcept -> D3D12_TEXTURE_BARRIER {
    return D3D12_TEXTURE_BARRIER {
        .SyncBefore   = ToD3D12Sync(syncBefore),
        .SyncAfter    = ToD3D12Sync(syncAfter),
        .AccessBefore = ToD3D12Access(accessBefore),
        .AccessAfter  = ToD3D12Access(accessAfter),
        .LayoutBefore = ToD3D12Layout(layoutBefore),
        .LayoutAfter  = ToD3D12Layout(layoutAfter),
        .pResource    = resource,
        .Subresources = subresources,
        .Flags        = D3D12_TEXTURE_BARRIER_FLAG_NONE,
    };
}

[[nodiscard]] constexpr auto MakeBufferBarrier(
    ID3D12Resource*      resource,
    VkD3D::PipelineStage syncBefore,
    VkD3D::PipelineStage syncAfter,
    VkD3D::Access        accessBefore,
    VkD3D::Access        accessAfter,
    uint64_t             offset = 0,
    uint64_t             size   = UINT64_MAX
) noexcept -> D3D12_BUFFER_BARRIER {
    return D3D12_BUFFER_BARRIER {
        .SyncBefore   = ToD3D12Sync(syncBefore),
        .SyncAfter    = ToD3D12Sync(syncAfter),
        .AccessBefore = ToD3D12Access(accessBefore),
        .AccessAfter  = ToD3D12Access(accessAfter),
        .pResource    = resource,
        .Offset       = offset,
        .Size         = size,
    };
}

[[nodiscard]] constexpr auto MakeGlobalBarrier(
    VkD3D::PipelineStage syncBefore,
    VkD3D::PipelineStage syncAfter,
    VkD3D::Access        accessBefore,
    VkD3D::Access        accessAfter
) noexcept -> D3D12_GLOBAL_BARRIER {
    return D3D12_GLOBAL_BARRIER {
        .SyncBefore   = ToD3D12Sync(syncBefore),
        .SyncAfter    = ToD3D12Sync(syncAfter),
        .AccessBefore = ToD3D12Access(accessBefore),
        .AccessAfter  = ToD3D12Access(accessAfter),
    };
}

inline void Record(ID3D12GraphicsCommandList7* cmd, std::span<const D3D12_TEXTURE_BARRIER> barriers) noexcept {
    if (barriers.empty()) {
        return;
    }
    const D3D12_BARRIER_GROUP group {
        .Type           = D3D12_BARRIER_TYPE_TEXTURE,
        .NumBarriers    = static_cast<uint32_t>(barriers.size()),
        .pTextureBarriers = barriers.data(),
    };
    cmd->Barrier(1, &group);
}

inline void Record(ID3D12GraphicsCommandList7* cmd, std::span<const D3D12_BUFFER_BARRIER> barriers) noexcept {
    if (barriers.empty()) {
        return;
    }
    const D3D12_BARRIER_GROUP group {
        .Type          = D3D12_BARRIER_TYPE_BUFFER,
        .NumBarriers   = static_cast<uint32_t>(barriers.size()),
        .pBufferBarriers = barriers.data(),
    };
    cmd->Barrier(1, &group);
}

inline void Record(ID3D12GraphicsCommandList7* cmd, std::span<const D3D12_GLOBAL_BARRIER> barriers) noexcept {
    if (barriers.empty()) {
        return;
    }
    const D3D12_BARRIER_GROUP group {
        .Type           = D3D12_BARRIER_TYPE_GLOBAL,
        .NumBarriers    = static_cast<uint32_t>(barriers.size()),
        .pGlobalBarriers = barriers.data(),
    };
    cmd->Barrier(1, &group);
}

// The one-shot verb for a single texture transition; batched transitions
// compose MakeTextureBarrier values and Record() the span instead.
inline void TransitionTexture(
    ID3D12GraphicsCommandList7* cmd,
    ID3D12Resource*             resource,
    VkD3D::ImageLayout          layoutBefore,
    VkD3D::ImageLayout          layoutAfter,
    VkD3D::PipelineStage        syncBefore,
    VkD3D::PipelineStage        syncAfter,
    VkD3D::Access               accessBefore,
    VkD3D::Access               accessAfter
) noexcept {
    const D3D12_TEXTURE_BARRIER barrier =
        MakeTextureBarrier(resource, layoutBefore, layoutAfter, syncBefore, syncAfter, accessBefore, accessAfter);
    Record(cmd, std::span<const D3D12_TEXTURE_BARRIER> {&barrier, 1});
}

} // namespace ZHLN::D3D12
