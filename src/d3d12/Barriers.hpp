// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Enhanced-barrier recording for the d3d12 backend, in native terms
// only: a barrier is a plain driver struct built on the stack from SDK
// barrier vocabulary, and Record() submits a whole span as one
// ID3D12GraphicsCommandList7::Barrier call -- one barrier group per
// kind, never one call per resource. There is no state tracking here:
// call sites (the render graph, later) own before/after knowledge, this
// layer only translates and submits. Neutral (VkD3D::) barrier values
// are lowered to these structs by the joint layer
// (src/vkd3d/D3D12Barriers.hpp); nothing in this backend names a VkD3D
// type.

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
    ID3D12Resource*                 resource,
    D3D12_BARRIER_LAYOUT            layoutBefore,
    D3D12_BARRIER_LAYOUT            layoutAfter,
    D3D12_BARRIER_SYNC              syncBefore,
    D3D12_BARRIER_SYNC              syncAfter,
    D3D12_BARRIER_ACCESS            accessBefore,
    D3D12_BARRIER_ACCESS            accessAfter,
    D3D12_BARRIER_SUBRESOURCE_RANGE subresources = kAllSubresources
) noexcept -> D3D12_TEXTURE_BARRIER {
    return D3D12_TEXTURE_BARRIER {
        .SyncBefore   = syncBefore,
        .SyncAfter    = syncAfter,
        .AccessBefore = accessBefore,
        .AccessAfter  = accessAfter,
        .LayoutBefore = layoutBefore,
        .LayoutAfter  = layoutAfter,
        .pResource    = resource,
        .Subresources = subresources,
        .Flags        = D3D12_TEXTURE_BARRIER_FLAG_NONE,
    };
}

[[nodiscard]] constexpr auto MakeBufferBarrier(
    ID3D12Resource*      resource,
    D3D12_BARRIER_SYNC   syncBefore,
    D3D12_BARRIER_SYNC   syncAfter,
    D3D12_BARRIER_ACCESS accessBefore,
    D3D12_BARRIER_ACCESS accessAfter,
    uint64_t             offset = 0,
    uint64_t             size   = UINT64_MAX
) noexcept -> D3D12_BUFFER_BARRIER {
    return D3D12_BUFFER_BARRIER {
        .SyncBefore   = syncBefore,
        .SyncAfter    = syncAfter,
        .AccessBefore = accessBefore,
        .AccessAfter  = accessAfter,
        .pResource    = resource,
        .Offset       = offset,
        .Size         = size,
    };
}

[[nodiscard]] constexpr auto MakeGlobalBarrier(
    D3D12_BARRIER_SYNC syncBefore, D3D12_BARRIER_SYNC syncAfter, D3D12_BARRIER_ACCESS accessBefore, D3D12_BARRIER_ACCESS accessAfter
) noexcept -> D3D12_GLOBAL_BARRIER {
    return D3D12_GLOBAL_BARRIER {
        .SyncBefore   = syncBefore,
        .SyncAfter    = syncAfter,
        .AccessBefore = accessBefore,
        .AccessAfter  = accessAfter,
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

// The one-shot verb for a single texture transition; batched transitions
// compose MakeTextureBarrier values and Record() the span instead.
inline void TransitionTexture(
    ID3D12GraphicsCommandList7* cmd,
    ID3D12Resource*             resource,
    D3D12_BARRIER_LAYOUT        layoutBefore,
    D3D12_BARRIER_LAYOUT        layoutAfter,
    D3D12_BARRIER_SYNC          syncBefore,
    D3D12_BARRIER_SYNC          syncAfter,
    D3D12_BARRIER_ACCESS        accessBefore,
    D3D12_BARRIER_ACCESS        accessAfter
) noexcept {
    const D3D12_TEXTURE_BARRIER barrier =
        MakeTextureBarrier(resource, layoutBefore, layoutAfter, syncBefore, syncAfter, accessBefore, accessAfter);
    Record(cmd, std::span<const D3D12_TEXTURE_BARRIER> {&barrier, 1});
}

} // namespace ZHLN::D3D12
