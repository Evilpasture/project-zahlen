// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// ZHLN::VkD3D -- the backend-neutral vocabulary: ONLY the neutral enums
// plus their flag operators. No Vulkan or D3D12 constants and no
// translation tables live here. neutral<->Vk:: conversion lives in
// src/vkd3d/VulkanConversions.hpp; neutral->D3D12 translation lives in
// src/d3d12/D3D12Conversions.hpp, written against the real Microsoft
// DirectX headers so the compiler -- not a hand-typed table -- checks
// every mapping. A neutral value with no equivalent on one side maps to
// that side's Undefined/Unknown/None and is documented at the case,
// never silently reinterpreted. Renderer code does not use these yet --
// src/render keeps naming Vk:: spellings until the backend-selection
// migration. Values must never be reordered casually: several families
// serialize to disk (pipeline cache keys, shader bytecode headers,
// baked assets), so keep the numeric layout stable.
#include <cstdint>

namespace ZHLN::VkD3D {

// NOLINTNEXTLINE(performance-enum-size)
enum class ImageLayout : uint32_t {
    Undefined, General, ColorAttachmentOptimal, DepthStencilAttachmentOptimal, DepthAttachmentOptimal,
    ShaderReadOnlyOptimal, PresentSrc, TransferSrcOptimal, TransferDstOptimal,
};





// NOLINTNEXTLINE(performance-enum-size)
enum class Format : uint32_t {
    Undefined, R32G32B32A32Sfloat, R32G32B32A32Uint, R32G32B32Sfloat, R32G32B32Uint, R16G16B16A16Sfloat, R16G16B16A16Uint, R32G32Sfloat, R32G32Uint, R32Sfloat, R32Uint, R32Sint, R16G16Sfloat, R8G8B8A8Unorm, R8G8B8A8Srgb, B8G8R8A8Unorm, B8G8R8A8Srgb, R8G8B8Unorm, R8G8B8Srgb, B8G8R8Srgb, R8G8Unorm, R8G8Srgb, R8Unorm, R8Uint, R8Srgb, A8B8G8R8UnormPack32, A8B8G8R8SrgbPack32, A2B10G10R10UnormPack32, B10G11R11UfloatPack32, D32Sfloat, D32SfloatS8Uint, D24UnormS8Uint, X8D24UnormPack32, D16Unorm, D16UnormS8Uint, S8Uint,
};





// Neutral synchronization-2 stage/access flags. Bit positions are this
// header's own (not VK_PIPELINE_STAGE_2_* / D3D12_BARRIER_SYNC values);
// conversion tests each bit, so many-to-one collapses on the D3D12 side
// (e.g. Fragment+EarlyFragment -> PIXEL_SHADING|DEPTH_STENCIL) are explicit.
// NOLINTNEXTLINE(performance-enum-size)
enum class PipelineStage : uint32_t {
    None = 0,
    TopOfPipe = 1u << 0,
    BottomOfPipe = 1u << 1,
    AllCommands = 1u << 2,
    AllGraphics = 1u << 3,
    Compute = 1u << 4,
    Fragment = 1u << 5,
    Vertex = 1u << 6,
    ColorAttachment = 1u << 7,
    EarlyFragment = 1u << 8,
    LateFragment = 1u << 9,
    Indirect = 1u << 10,
    Transfer = 1u << 11,
    Copy = 1u << 12,
    Clear = 1u << 13,
    Host = 1u << 14,
    IndexInput = 1u << 15,
    VertexInput = 1u << 16,
    VertexAttributeInput = 1u << 17,
    AccelerationStructureBuild = 1u << 18,
};

[[nodiscard]] constexpr auto operator|(PipelineStage a, PipelineStage b) noexcept -> PipelineStage {
    return static_cast<PipelineStage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr auto operator|=(PipelineStage& a, PipelineStage b) noexcept -> PipelineStage& { a = a | b; return a; }
[[nodiscard]] constexpr auto operator&(PipelineStage a, PipelineStage b) noexcept -> PipelineStage {
    return static_cast<PipelineStage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
[[nodiscard]] constexpr auto Has(PipelineStage flags, PipelineStage bits) noexcept -> bool {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(bits)) != 0;
}



// NOLINTNEXTLINE(performance-enum-size)
enum class Access : uint32_t {
    None = 0, ShaderRead = 1u << 0, ShaderWrite = 1u << 1, ShaderStorageRead = 1u << 2, ShaderStorageWrite = 1u << 3,
    UniformRead = 1u << 4, IndirectRead = 1u << 5, IndexRead = 1u << 6, VertexAttributeRead = 1u << 7,
    TransferRead = 1u << 8, TransferWrite = 1u << 9, HostRead = 1u << 10, HostWrite = 1u << 11,
    MemoryRead = 1u << 12, MemoryWrite = 1u << 13, ColorRead = 1u << 14, ColorWrite = 1u << 15,
    DepthRead = 1u << 16, DepthWrite = 1u << 17, AccelerationStructureRead = 1u << 18, AccelerationStructureWrite = 1u << 19,
};

[[nodiscard]] constexpr auto operator|(Access a, Access b) noexcept -> Access {
    return static_cast<Access>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr auto operator|=(Access& a, Access b) noexcept -> Access& { a = a | b; return a; }
[[nodiscard]] constexpr auto operator&(Access a, Access b) noexcept -> Access {
    return static_cast<Access>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
[[nodiscard]] constexpr auto Has(Access flags, Access bits) noexcept -> bool {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(bits)) != 0;
}



// NOLINTNEXTLINE(performance-enum-size)
enum class ImageAspect : uint32_t { None = 0, Color = 1u << 0, Depth = 1u << 1, Stencil = 1u << 2 };

[[nodiscard]] constexpr auto operator|(ImageAspect a, ImageAspect b) noexcept -> ImageAspect {
    return static_cast<ImageAspect>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
[[nodiscard]] constexpr auto operator&(ImageAspect a, ImageAspect b) noexcept -> ImageAspect {
    return static_cast<ImageAspect>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
// D3D12 selects depth/stencil planes via D3D12_BARRIER_SUBRESOURCE_RANGE
// PlaneSlice, not an aspect enum; the neutral flags carry the information
// and the backend lowers them when the barrier code lands.

// NOLINTNEXTLINE(performance-enum-size)
enum class CompareOp : uint32_t { Never, Less, Equal, LessOrEqual, Greater, NotEqual, GreaterOrEqual, Always };


// NOLINTNEXTLINE(performance-enum-size)
enum class IndexType : uint32_t { Uint16, Uint32 };

} // namespace ZHLN::VkD3D
