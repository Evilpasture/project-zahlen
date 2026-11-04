// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Barriers.hpp"

// Anchor TU for the header-only native barrier synthesis, with the
// compile-time half of a barrier sanity check: a present -> render-target
// transition and a compute -> indirect buffer barrier land in the
// D3D12_*_BARRIER fields the runtime will hand to
// ID3D12GraphicsCommandList7::Barrier. The recording half is exercised
// once command lists exist (Phase 3+); nothing here runs at runtime.
namespace {

[[maybe_unused]] constexpr D3D12_TEXTURE_BARRIER kPresentToRenderTarget = ZHLN::D3D12::MakeTextureBarrier(
    nullptr,
    D3D12_BARRIER_LAYOUT_PRESENT,
    D3D12_BARRIER_LAYOUT_RENDER_TARGET,
    D3D12_BARRIER_SYNC_ALL,
    D3D12_BARRIER_SYNC_RENDER_TARGET,
    D3D12_BARRIER_ACCESS_COMMON,
    D3D12_BARRIER_ACCESS_RENDER_TARGET
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
    D3D12_BARRIER_SYNC_COMPUTE_SHADING,
    D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
    D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT
);
static_assert(kComputeToIndirect.SyncBefore == D3D12_BARRIER_SYNC_COMPUTE_SHADING);
static_assert(kComputeToIndirect.SyncAfter == D3D12_BARRIER_SYNC_EXECUTE_INDIRECT);
static_assert(kComputeToIndirect.AccessBefore == D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
static_assert(kComputeToIndirect.AccessAfter == D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
static_assert(kComputeToIndirect.Size == UINT64_MAX);

} // namespace
