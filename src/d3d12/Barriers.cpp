// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Barriers.hpp"

// Anchor TU for the header-only barrier synthesis, and the compile-time
// half of the barrier test plan: these asserts verify that a neutral
// (VkD3D::) transition lands in the D3D12_TEXTURE_BARRIER fields the
// runtime will hand to ID3D12GraphicsCommandList7::Barrier -- layout,
// sync, and access all translated, subresources defaulting to "all".
// The recording half is exercised once command lists exist (Phase 3+);
// nothing here runs at runtime.
namespace {

[[maybe_unused]] constexpr D3D12_TEXTURE_BARRIER kPresentToRenderTarget = ZHLN::D3D12::MakeTextureBarrier(
    nullptr,
    ZHLN::VkD3D::ImageLayout::PresentSrc,
    ZHLN::VkD3D::ImageLayout::ColorAttachmentOptimal,
    ZHLN::VkD3D::PipelineStage::BottomOfPipe,
    ZHLN::VkD3D::PipelineStage::ColorAttachment,
    ZHLN::VkD3D::Access::None,
    ZHLN::VkD3D::Access::ColorWrite
);
static_assert(kPresentToRenderTarget.LayoutBefore == D3D12_BARRIER_LAYOUT_PRESENT);
static_assert(kPresentToRenderTarget.LayoutAfter == D3D12_BARRIER_LAYOUT_RENDER_TARGET);
static_assert(kPresentToRenderTarget.SyncBefore == D3D12_BARRIER_SYNC_ALL);
static_assert(kPresentToRenderTarget.SyncAfter == D3D12_BARRIER_SYNC_RENDER_TARGET);
static_assert(kPresentToRenderTarget.AccessBefore == D3D12_BARRIER_ACCESS_COMMON);
static_assert(kPresentToRenderTarget.AccessAfter == D3D12_BARRIER_ACCESS_RENDER_TARGET);
static_assert(kPresentToRenderTarget.Subresources.IndexOrFirstMipLevel == 0xFFFFFFFFu);

[[maybe_unused]] constexpr D3D12_BUFFER_BARRIER kComputeToIndirect = ZHLN::D3D12::MakeBufferBarrier(
    nullptr,
    ZHLN::VkD3D::PipelineStage::Compute,
    ZHLN::VkD3D::PipelineStage::Indirect,
    ZHLN::VkD3D::Access::ShaderWrite,
    ZHLN::VkD3D::Access::IndirectRead
);
static_assert(kComputeToIndirect.SyncBefore == D3D12_BARRIER_SYNC_COMPUTE_SHADING);
static_assert(kComputeToIndirect.SyncAfter == D3D12_BARRIER_SYNC_EXECUTE_INDIRECT);
static_assert(kComputeToIndirect.AccessBefore == D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
static_assert(kComputeToIndirect.AccessAfter == D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
static_assert(kComputeToIndirect.Size == UINT64_MAX);

} // namespace
