// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// The only translation unit in the tree that includes the Microsoft
// DirectX-Headers SDK headers (extern/DirectX-Headers, see CMakeLists.txt:
// zahlen_directx_headers is PRIVATE to zahlen_d3d12). It exists so the
// SDK-free exported header include/d3d12/Enums.hpp cannot drift from the
// real constants: every enumerator is asserted equal to the D3D12/DXGI
// constant it names. If a value here fails, fix Enums.hpp -- do not fix
// this file to match it.
#if __has_include(<wsl/winadapter.h>)
#include <wsl/winadapter.h> // must precede <directx/d3d12.h> on non-Windows hosts
#endif

#include <d3d12/Enums.hpp>
#include <directx/d3d12.h>
#include <directx/dxgiformat.h>

#include <cstdint>

static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::Undefined) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_UNDEFINED));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::Common) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_COMMON));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::GenericRead) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_GENERIC_READ));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::RenderTarget) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_RENDER_TARGET));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::UnorderedAccess) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::DepthStencilWrite) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::DepthStencilRead) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::ShaderResource) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_SHADER_RESOURCE));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::CopySource) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_COPY_SOURCE));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::CopyDest) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_COPY_DEST));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::ResolveSource) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::ResolveDest) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_RESOLVE_DEST));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::ShadingRateSource) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::VideoDecodeRead) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_VIDEO_DECODE_READ));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::VideoDecodeWrite) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_VIDEO_DECODE_WRITE));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::VideoProcessRead) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_VIDEO_PROCESS_READ));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::VideoProcessWrite) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_VIDEO_PROCESS_WRITE));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::VideoEncodeRead) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_VIDEO_ENCODE_READ));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::VideoEncodeWrite) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_VIDEO_ENCODE_WRITE));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::ImageLayout::Present) == static_cast<uint32_t>(D3D12_BARRIER_LAYOUT_PRESENT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R32G32B32A32Float) == static_cast<uint32_t>(DXGI_FORMAT_R32G32B32A32_FLOAT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R32G32B32A32Uint) == static_cast<uint32_t>(DXGI_FORMAT_R32G32B32A32_UINT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R32G32B32Float) == static_cast<uint32_t>(DXGI_FORMAT_R32G32B32_FLOAT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R32G32B32Uint) == static_cast<uint32_t>(DXGI_FORMAT_R32G32B32_UINT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R16G16B16A16Float) == static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R16G16B16A16Uint) == static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_UINT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::D32FloatS8X24Uint) == static_cast<uint32_t>(DXGI_FORMAT_D32_FLOAT_S8X24_UINT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R10G10B10A2Unorm) == static_cast<uint32_t>(DXGI_FORMAT_R10G10B10A2_UNORM));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R11G11B10Float) == static_cast<uint32_t>(DXGI_FORMAT_R11G11B10_FLOAT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R8G8B8A8Unorm) == static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R8G8B8A8Srgb) == static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R32G32Float) == static_cast<uint32_t>(DXGI_FORMAT_R32G32_FLOAT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R32G32Uint) == static_cast<uint32_t>(DXGI_FORMAT_R32G32_UINT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R16G16Float) == static_cast<uint32_t>(DXGI_FORMAT_R16G16_FLOAT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::D32Float) == static_cast<uint32_t>(DXGI_FORMAT_D32_FLOAT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R32Float) == static_cast<uint32_t>(DXGI_FORMAT_R32_FLOAT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R32Uint) == static_cast<uint32_t>(DXGI_FORMAT_R32_UINT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R32Sint) == static_cast<uint32_t>(DXGI_FORMAT_R32_SINT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::D24UnormS8Uint) == static_cast<uint32_t>(DXGI_FORMAT_D24_UNORM_S8_UINT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R8G8Unorm) == static_cast<uint32_t>(DXGI_FORMAT_R8G8_UNORM));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::D16Unorm) == static_cast<uint32_t>(DXGI_FORMAT_D16_UNORM));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R8Unorm) == static_cast<uint32_t>(DXGI_FORMAT_R8_UNORM));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R8Uint) == static_cast<uint32_t>(DXGI_FORMAT_R8_UINT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::R16Uint) == static_cast<uint32_t>(DXGI_FORMAT_R16_UINT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::B8G8R8A8Unorm) == static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::B8G8R8A8Srgb) == static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Format::Unknown) == static_cast<uint32_t>(DXGI_FORMAT_UNKNOWN));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::PipelineStage::None) == static_cast<uint32_t>(D3D12_BARRIER_SYNC_NONE));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::PipelineStage::All) == static_cast<uint32_t>(D3D12_BARRIER_SYNC_ALL));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::PipelineStage::Draw) == static_cast<uint32_t>(D3D12_BARRIER_SYNC_DRAW));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::PipelineStage::IndexInput) == static_cast<uint32_t>(D3D12_BARRIER_SYNC_INDEX_INPUT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::PipelineStage::VertexShading) == static_cast<uint32_t>(D3D12_BARRIER_SYNC_VERTEX_SHADING));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::PipelineStage::PixelShading) == static_cast<uint32_t>(D3D12_BARRIER_SYNC_PIXEL_SHADING));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::PipelineStage::DepthStencil) == static_cast<uint32_t>(D3D12_BARRIER_SYNC_DEPTH_STENCIL));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::PipelineStage::RenderTarget) == static_cast<uint32_t>(D3D12_BARRIER_SYNC_RENDER_TARGET));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::PipelineStage::ComputeShading) == static_cast<uint32_t>(D3D12_BARRIER_SYNC_COMPUTE_SHADING));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::PipelineStage::Raytracing) == static_cast<uint32_t>(D3D12_BARRIER_SYNC_RAYTRACING));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::PipelineStage::Copy) == static_cast<uint32_t>(D3D12_BARRIER_SYNC_COPY));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::PipelineStage::Resolve) == static_cast<uint32_t>(D3D12_BARRIER_SYNC_RESOLVE));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::PipelineStage::ExecuteIndirect) == static_cast<uint32_t>(D3D12_BARRIER_SYNC_EXECUTE_INDIRECT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::PipelineStage::AllShading) == static_cast<uint32_t>(D3D12_BARRIER_SYNC_ALL_SHADING));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::PipelineStage::BuildAccelerationStructure) == static_cast<uint32_t>(D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::PipelineStage::Split) == static_cast<uint32_t>(D3D12_BARRIER_SYNC_SPLIT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Access::Common) == static_cast<uint32_t>(D3D12_BARRIER_ACCESS_COMMON));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Access::VertexBuffer) == static_cast<uint32_t>(D3D12_BARRIER_ACCESS_VERTEX_BUFFER));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Access::ConstantBuffer) == static_cast<uint32_t>(D3D12_BARRIER_ACCESS_CONSTANT_BUFFER));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Access::IndexBuffer) == static_cast<uint32_t>(D3D12_BARRIER_ACCESS_INDEX_BUFFER));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Access::RenderTarget) == static_cast<uint32_t>(D3D12_BARRIER_ACCESS_RENDER_TARGET));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Access::UnorderedAccess) == static_cast<uint32_t>(D3D12_BARRIER_ACCESS_UNORDERED_ACCESS));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Access::DepthStencilWrite) == static_cast<uint32_t>(D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Access::DepthStencilRead) == static_cast<uint32_t>(D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Access::ShaderResource) == static_cast<uint32_t>(D3D12_BARRIER_ACCESS_SHADER_RESOURCE));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Access::StreamOutput) == static_cast<uint32_t>(D3D12_BARRIER_ACCESS_STREAM_OUTPUT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Access::IndirectArgument) == static_cast<uint32_t>(D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Access::CopyDest) == static_cast<uint32_t>(D3D12_BARRIER_ACCESS_COPY_DEST));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Access::CopySource) == static_cast<uint32_t>(D3D12_BARRIER_ACCESS_COPY_SOURCE));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Access::ResolveDest) == static_cast<uint32_t>(D3D12_BARRIER_ACCESS_RESOLVE_DEST));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Access::ResolveSource) == static_cast<uint32_t>(D3D12_BARRIER_ACCESS_RESOLVE_SOURCE));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Access::AccelerationStructureRead) == static_cast<uint32_t>(D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Access::AccelerationStructureWrite) == static_cast<uint32_t>(D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Access::ShadingRateSource) == static_cast<uint32_t>(D3D12_BARRIER_ACCESS_SHADING_RATE_SOURCE));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::Access::NoAccess) == static_cast<uint32_t>(D3D12_BARRIER_ACCESS_NO_ACCESS));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::CompareOp::Never) == static_cast<uint32_t>(D3D12_COMPARISON_FUNC_NEVER));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::CompareOp::Less) == static_cast<uint32_t>(D3D12_COMPARISON_FUNC_LESS));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::CompareOp::Equal) == static_cast<uint32_t>(D3D12_COMPARISON_FUNC_EQUAL));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::CompareOp::LessOrEqual) == static_cast<uint32_t>(D3D12_COMPARISON_FUNC_LESS_EQUAL));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::CompareOp::Greater) == static_cast<uint32_t>(D3D12_COMPARISON_FUNC_GREATER));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::CompareOp::NotEqual) == static_cast<uint32_t>(D3D12_COMPARISON_FUNC_NOT_EQUAL));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::CompareOp::GreaterOrEqual) == static_cast<uint32_t>(D3D12_COMPARISON_FUNC_GREATER_EQUAL));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::CompareOp::Always) == static_cast<uint32_t>(D3D12_COMPARISON_FUNC_ALWAYS));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::CullMode::None) == static_cast<uint32_t>(D3D12_CULL_MODE_NONE));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::CullMode::Front) == static_cast<uint32_t>(D3D12_CULL_MODE_FRONT));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::CullMode::Back) == static_cast<uint32_t>(D3D12_CULL_MODE_BACK));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::FillMode::Wireframe) == static_cast<uint32_t>(D3D12_FILL_MODE_WIREFRAME));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::FillMode::Solid) == static_cast<uint32_t>(D3D12_FILL_MODE_SOLID));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::TextureAddressMode::Wrap) == static_cast<uint32_t>(D3D12_TEXTURE_ADDRESS_MODE_WRAP));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::TextureAddressMode::Mirror) == static_cast<uint32_t>(D3D12_TEXTURE_ADDRESS_MODE_MIRROR));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::TextureAddressMode::Clamp) == static_cast<uint32_t>(D3D12_TEXTURE_ADDRESS_MODE_CLAMP));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::TextureAddressMode::Border) == static_cast<uint32_t>(D3D12_TEXTURE_ADDRESS_MODE_BORDER));
static_assert(static_cast<uint32_t>(ZHLN::D3D12::TextureAddressMode::MirrorOnce) == static_cast<uint32_t>(D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE));
