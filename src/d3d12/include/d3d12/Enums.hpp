// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// D3D12 spellings of the joint-core renderer enums. This exported seam
// header itself includes no SDK header -- it must stay includable by
// src/vkd3d (and configurable on non-Windows hosts), and the Microsoft
// DirectX-Headers submodule is scoped to the zahlen_d3d12 target only
// (see src/d3d12/CMakeLists.txt). Instead, each enumerator carries the
// numeric value of the D3D12/DXGI constant named in its comment, and
// src/d3d12/EnumValues.cpp -- compiled only into zahlen_d3d12, the one
// target that may include the SDK -- static_asserts every value against
// the real constant. Same contract as Vk::Enums, where ToVk() is a cast,
// never a table. Translations to/from the neutral spellings live in
// src/vkd3d/Enums.hpp.
#include <cstdint>

namespace ZHLN::D3D12 {

// Values: D3D12_BARRIER_LAYOUT (enhanced barriers). PRESENT is an alias of
// COMMON in D3D12; kept distinct here so VkD3D::ImageLayout::PresentSrc has
// a spelling on this side, with the same underlying value.
// NOLINTNEXTLINE(performance-enum-size)
enum class ImageLayout : uint32_t {
    Undefined                    = 0xFFFFFFFFu, // D3D12_BARRIER_LAYOUT_UNDEFINED
    Common                       = 0,           // D3D12_BARRIER_LAYOUT_COMMON
    Present                      = 0,           // D3D12_BARRIER_LAYOUT_PRESENT (alias of COMMON)
    GenericRead                  = 1,           // D3D12_BARRIER_LAYOUT_GENERIC_READ
    RenderTarget                 = 2,           // D3D12_BARRIER_LAYOUT_RENDER_TARGET
    UnorderedAccess              = 3,           // D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS
    DepthStencilWrite            = 4,           // D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE
    DepthStencilRead             = 5,           // D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ
    ShaderResource               = 6,           // D3D12_BARRIER_LAYOUT_SHADER_RESOURCE
    CopySource                   = 7,           // D3D12_BARRIER_LAYOUT_COPY_SOURCE
    CopyDest                     = 8,           // D3D12_BARRIER_LAYOUT_COPY_DEST
    ResolveSource                = 9,           // D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE
    ResolveDest                  = 10,          // D3D12_BARRIER_LAYOUT_RESOLVE_DEST
    ShadingRateSource            = 11,          // D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE
    VideoDecodeRead              = 12,          // D3D12_BARRIER_LAYOUT_VIDEO_DECODE_READ
    VideoDecodeWrite             = 13,          // D3D12_BARRIER_LAYOUT_VIDEO_DECODE_WRITE
    VideoProcessRead             = 14,          // D3D12_BARRIER_LAYOUT_VIDEO_PROCESS_READ
    VideoProcessWrite            = 15,          // D3D12_BARRIER_LAYOUT_VIDEO_PROCESS_WRITE
    VideoEncodeRead              = 16,          // D3D12_BARRIER_LAYOUT_VIDEO_ENCODE_READ
    VideoEncodeWrite             = 17,          // D3D12_BARRIER_LAYOUT_VIDEO_ENCODE_WRITE
};

// Values: DXGI_FORMAT. Formats with no DXGI equivalent (24-bit R8G8B8,
// R8G8_SRGB, R8_SRGB, S8_UINT alone, D16+S8) deliberately have no enumerator
// here; VkD3D conversion maps them to Unknown.
// NOLINTNEXTLINE(performance-enum-size)
enum class Format : uint32_t {
    Unknown              = 0,   // DXGI_FORMAT_UNKNOWN
    R32G32B32A32Float    = 2,   // DXGI_FORMAT_R32G32B32A32_FLOAT
    R32G32B32A32Uint     = 3,   // DXGI_FORMAT_R32G32B32A32_UINT
    R32G32B32Float       = 6,   // DXGI_FORMAT_R32G32B32_FLOAT
    R32G32B32Uint        = 7,   // DXGI_FORMAT_R32G32B32_UINT
    R16G16B16A16Float    = 10,  // DXGI_FORMAT_R16G16B16A16_FLOAT
    R16G16B16A16Uint     = 12,  // DXGI_FORMAT_R16G16B16A16_UINT
    D32FloatS8X24Uint    = 20,  // DXGI_FORMAT_D32_FLOAT_S8X24_UINT
    R10G10B10A2Unorm     = 24,  // DXGI_FORMAT_R10G10B10A2_UNORM
    R11G11B10Float       = 26,  // DXGI_FORMAT_R11G11B10_FLOAT
    R8G8B8A8Unorm        = 28,  // DXGI_FORMAT_R8G8B8A8_UNORM
    R8G8B8A8Srgb         = 29,  // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
    R32G32Float          = 16,  // DXGI_FORMAT_R32G32_FLOAT
    R32G32Uint           = 17,  // DXGI_FORMAT_R32G32_UINT
    R16G16Float          = 34,  // DXGI_FORMAT_R16G16_FLOAT
    D32Float             = 40,  // DXGI_FORMAT_D32_FLOAT
    R32Float             = 41,  // DXGI_FORMAT_R32_FLOAT
    R32Uint              = 42,  // DXGI_FORMAT_R32_UINT
    R32Sint              = 43,  // DXGI_FORMAT_R32_SINT
    D24UnormS8Uint       = 45,  // DXGI_FORMAT_D24_UNORM_S8_UINT
    R8G8Unorm            = 49,  // DXGI_FORMAT_R8G8_UNORM
    D16Unorm             = 55,  // DXGI_FORMAT_D16_UNORM
    R8Unorm              = 61,  // DXGI_FORMAT_R8_UNORM
    R16Uint              = 57,  // DXGI_FORMAT_R16_UINT
    R8Uint               = 62,  // DXGI_FORMAT_R8_UINT
    B8G8R8A8Unorm        = 87,  // DXGI_FORMAT_B8G8R8A8_UNORM
    B8G8R8A8Srgb         = 91,  // DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
};

// Values: D3D12_BARRIER_SYNC.
// NOLINTNEXTLINE(performance-enum-size)
enum class PipelineStage : uint32_t {
    None                       = 0x0u,       // D3D12_BARRIER_SYNC_NONE
    All                        = 0x1u,       // D3D12_BARRIER_SYNC_ALL
    Draw                       = 0x2u,       // D3D12_BARRIER_SYNC_DRAW
    IndexInput                 = 0x4u,       // D3D12_BARRIER_SYNC_INDEX_INPUT
    VertexShading              = 0x8u,       // D3D12_BARRIER_SYNC_VERTEX_SHADING
    PixelShading               = 0x10u,      // D3D12_BARRIER_SYNC_PIXEL_SHADING
    DepthStencil               = 0x20u,      // D3D12_BARRIER_SYNC_DEPTH_STENCIL
    RenderTarget               = 0x40u,      // D3D12_BARRIER_SYNC_RENDER_TARGET
    ComputeShading             = 0x80u,      // D3D12_BARRIER_SYNC_COMPUTE_SHADING
    Raytracing                 = 0x100u,     // D3D12_BARRIER_SYNC_RAYTRACING
    Copy                       = 0x200u,     // D3D12_BARRIER_SYNC_COPY
    Resolve                    = 0x400u,     // D3D12_BARRIER_SYNC_RESOLVE
    ExecuteIndirect            = 0x800u,     // D3D12_BARRIER_SYNC_EXECUTE_INDIRECT
    AllShading                 = 0x1000u,    // D3D12_BARRIER_SYNC_ALL_SHADING
    BuildAccelerationStructure = 0x800000u,  // D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE
    Split                      = 0x80000000u, // D3D12_BARRIER_SYNC_SPLIT
};

[[nodiscard]] constexpr auto operator|(PipelineStage a, PipelineStage b) noexcept -> PipelineStage {
    return static_cast<PipelineStage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr auto operator|=(PipelineStage& a, PipelineStage b) noexcept -> PipelineStage& {
    a = a | b;
    return a;
}

// Values: D3D12_BARRIER_ACCESS.
// NOLINTNEXTLINE(performance-enum-size)
enum class Access : uint32_t {
    Common                      = 0x0u,        // D3D12_BARRIER_ACCESS_COMMON
    VertexBuffer                = 0x1u,        // D3D12_BARRIER_ACCESS_VERTEX_BUFFER
    ConstantBuffer              = 0x2u,        // D3D12_BARRIER_ACCESS_CONSTANT_BUFFER
    IndexBuffer                 = 0x4u,        // D3D12_BARRIER_ACCESS_INDEX_BUFFER
    RenderTarget                = 0x8u,        // D3D12_BARRIER_ACCESS_RENDER_TARGET
    UnorderedAccess             = 0x10u,       // D3D12_BARRIER_ACCESS_UNORDERED_ACCESS
    DepthStencilWrite           = 0x20u,       // D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE
    DepthStencilRead            = 0x40u,       // D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ
    ShaderResource              = 0x80u,       // D3D12_BARRIER_ACCESS_SHADER_RESOURCE
    StreamOutput                = 0x100u,      // D3D12_BARRIER_ACCESS_STREAM_OUTPUT
    IndirectArgument            = 0x200u,      // D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT
    CopyDest                    = 0x400u,      // D3D12_BARRIER_ACCESS_COPY_DEST
    CopySource                  = 0x800u,      // D3D12_BARRIER_ACCESS_COPY_SOURCE
    ResolveDest                 = 0x1000u,     // D3D12_BARRIER_ACCESS_RESOLVE_DEST
    ResolveSource               = 0x2000u,     // D3D12_BARRIER_ACCESS_RESOLVE_SOURCE
    AccelerationStructureRead   = 0x4000u,     // D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ
    AccelerationStructureWrite  = 0x8000u,     // D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE
    ShadingRateSource           = 0x10000u,    // D3D12_BARRIER_ACCESS_SHADING_RATE_SOURCE
    NoAccess                    = 0x80000000u, // D3D12_BARRIER_ACCESS_NO_ACCESS
};

[[nodiscard]] constexpr auto operator|(Access a, Access b) noexcept -> Access {
    return static_cast<Access>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr auto operator|=(Access& a, Access b) noexcept -> Access& {
    a = a | b;
    return a;
}

// Values: D3D12_COMPARISON_FUNC.
// NOLINTNEXTLINE(performance-enum-size)
enum class CompareOp : uint32_t {
    Never = 1, Less = 2, Equal = 3, LessOrEqual = 4, Greater = 5, NotEqual = 6, GreaterOrEqual = 7, Always = 8,
};

// Values: D3D12_CULL_MODE.
// NOLINTNEXTLINE(performance-enum-size)
enum class CullMode : uint32_t { None = 1, Front = 2, Back = 3 };

// Values: D3D12_FILL_MODE.
// NOLINTNEXTLINE(performance-enum-size)
enum class FillMode : uint32_t { Wireframe = 2, Solid = 3 };

// Values: D3D12_TEXTURE_ADDRESS_MODE.
// NOLINTNEXTLINE(performance-enum-size)
enum class TextureAddressMode : uint32_t { Wrap = 1, Mirror = 2, Clamp = 3, Border = 4, MirrorOnce = 5 };

} // namespace ZHLN::D3D12
