// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "D3D12Conversions.hpp"

// Anchor TU: D3D12Conversions.hpp is otherwise header-only, so include it
// here -- in the one target that may see the DirectX headers -- to keep it
// compiling. The asserts pin the mapping LOGIC (which SDK constant each
// neutral value selects, including the documented collapses and
// no-equivalent cases); the SDK values themselves come from the real
// headers, so there is no hand-typed integer left to proofread.
static_assert(ZHLN::D3D12::ToD3D12Layout(ZHLN::VkD3D::ImageLayout::ColorAttachmentOptimal) == D3D12_BARRIER_LAYOUT_RENDER_TARGET);
static_assert(ZHLN::D3D12::ToD3D12Layout(ZHLN::VkD3D::ImageLayout::PresentSrc) == D3D12_BARRIER_LAYOUT_COMMON);
static_assert(ZHLN::D3D12::FromD3D12Layout(D3D12_BARRIER_LAYOUT_GENERIC_READ) == ZHLN::VkD3D::ImageLayout::ShaderReadOnlyOptimal);
static_assert(ZHLN::D3D12::ToDxgi(ZHLN::VkD3D::Format::D32SfloatS8Uint) == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
static_assert(ZHLN::D3D12::ToDxgi(ZHLN::VkD3D::Format::R8G8B8Srgb) == DXGI_FORMAT_UNKNOWN); // no DXGI equivalent
static_assert(ZHLN::D3D12::FromDxgi(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) == ZHLN::VkD3D::Format::B8G8R8A8Srgb);
static_assert(ZHLN::D3D12::ToDxgi(ZHLN::VkD3D::IndexType::Uint16) == DXGI_FORMAT_R16_UINT);
static_assert(ZHLN::D3D12::ToD3D12Sync(ZHLN::VkD3D::PipelineStage::Compute) == D3D12_BARRIER_SYNC_COMPUTE_SHADING);
static_assert(ZHLN::D3D12::ToD3D12Sync(ZHLN::VkD3D::PipelineStage::VertexInput) == D3D12_BARRIER_SYNC_INDEX_INPUT); // collapse
static_assert(ZHLN::D3D12::ToD3D12Access(ZHLN::VkD3D::Access::UniformRead) == D3D12_BARRIER_ACCESS_CONSTANT_BUFFER);
static_assert(ZHLN::D3D12::ToD3D12Access(ZHLN::VkD3D::Access::HostRead) == D3D12_BARRIER_ACCESS_COMMON); // collapse
static_assert(ZHLN::D3D12::ToD3D12Compare(ZHLN::VkD3D::CompareOp::LessOrEqual) == D3D12_COMPARISON_FUNC_LESS_EQUAL);
