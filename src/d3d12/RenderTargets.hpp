#pragma once

// The dynamic-rendering equivalent, D3D12 side. There is no render-pass
// object and no BeginRenderPass: a pass is layout transitions (recorded
// through Barriers.hpp into D3D12_BARRIER_LAYOUT_RENDER_TARGET /
// DEPTH_STENCIL_WRITE) followed by this binding -- OMSetRenderTargets +
// viewport + scissor, the analogue of RasterPassContext's BeginRendering
// half. Kept as one free verb so the future graph-managed pass context
// has exactly one binding site to call.

#include "Sdk.hpp"

#include <cstdint>
#include <span>

namespace ZHLN::D3D12 {

inline void BindRenderTargets(
    ID3D12GraphicsCommandList*                 cmd,
    std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> colorTargets,
    const D3D12_CPU_DESCRIPTOR_HANDLE*           depthStencil,
    const D3D12_VIEWPORT&                        viewport,
    const D3D12_RECT&                            scissor
) noexcept {
    cmd->OMSetRenderTargets(static_cast<uint32_t>(colorTargets.size()), colorTargets.data(), FALSE, depthStencil);
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);
}

} // namespace ZHLN::D3D12
