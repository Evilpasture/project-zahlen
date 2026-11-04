// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Vulkan spellings of the joint-core renderer enums. Each enumerator's value
// IS the Vk* constant, so ToVk() is a cast, never a translation table -- the
// API-neutral spellings and the translations live in src/vkd3d/Enums.hpp.
// Exported through the zahlen_vulkan_address seam (src/vulkan/include) so
// src/vkd3d and src/render can name these without reaching into src/vulkan.
#include <volk.h>

#include <cstdint>

namespace ZHLN::Vk {

// NOLINTNEXTLINE(performance-enum-size)
enum class ImageLayout : uint32_t {
    Undefined                    = VK_IMAGE_LAYOUT_UNDEFINED,
    General                      = VK_IMAGE_LAYOUT_GENERAL,
    ColorAttachmentOptimal       = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    DepthStencilAttachmentOptimal = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    DepthAttachmentOptimal       = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
    ShaderReadOnlyOptimal        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    PresentSrc                   = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    TransferSrcOptimal           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    TransferDstOptimal           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
};

[[nodiscard]] constexpr auto ToVk(ImageLayout layout) noexcept -> VkImageLayout {
    return static_cast<VkImageLayout>(layout);
}
[[nodiscard]] constexpr auto FromVk(VkImageLayout layout) noexcept -> ImageLayout {
    return static_cast<ImageLayout>(layout);
}

// NOLINTNEXTLINE(performance-enum-size)
enum class Format : uint32_t {
    Undefined               = VK_FORMAT_UNDEFINED,
    R32G32B32A32Sfloat      = VK_FORMAT_R32G32B32A32_SFLOAT,
    R32G32B32A32Uint        = VK_FORMAT_R32G32B32A32_UINT,
    R32G32B32Sfloat         = VK_FORMAT_R32G32B32_SFLOAT,
    R32G32B32Uint           = VK_FORMAT_R32G32B32_UINT,
    R16G16B16A16Sfloat      = VK_FORMAT_R16G16B16A16_SFLOAT,
    R16G16B16A16Uint        = VK_FORMAT_R16G16B16A16_UINT,
    R32G32Sfloat            = VK_FORMAT_R32G32_SFLOAT,
    R32G32Uint              = VK_FORMAT_R32G32_UINT,
    R32Sfloat               = VK_FORMAT_R32_SFLOAT,
    R32Uint                 = VK_FORMAT_R32_UINT,
    R32Sint                 = VK_FORMAT_R32_SINT,
    R16G16Sfloat            = VK_FORMAT_R16G16_SFLOAT,
    R8G8B8A8Unorm           = VK_FORMAT_R8G8B8A8_UNORM,
    R8G8B8A8Srgb            = VK_FORMAT_R8G8B8A8_SRGB,
    B8G8R8A8Unorm           = VK_FORMAT_B8G8R8A8_UNORM,
    B8G8R8A8Srgb            = VK_FORMAT_B8G8R8A8_SRGB,
    R8G8B8Unorm             = VK_FORMAT_R8G8B8_UNORM,
    R8G8B8Srgb              = VK_FORMAT_R8G8B8_SRGB,
    B8G8R8Srgb              = VK_FORMAT_B8G8R8_SRGB,
    R8G8Unorm               = VK_FORMAT_R8G8_UNORM,
    R8G8Srgb                = VK_FORMAT_R8G8_SRGB,
    R8Unorm                 = VK_FORMAT_R8_UNORM,
    R8Uint                  = VK_FORMAT_R8_UINT,
    R8Srgb                  = VK_FORMAT_R8_SRGB,
    A8B8G8R8UnormPack32     = VK_FORMAT_A8B8G8R8_UNORM_PACK32,
    A8B8G8R8SrgbPack32      = VK_FORMAT_A8B8G8R8_SRGB_PACK32,
    A2B10G10R10UnormPack32  = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
    B10G11R11UfloatPack32   = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
    D32Sfloat               = VK_FORMAT_D32_SFLOAT,
    D32SfloatS8Uint         = VK_FORMAT_D32_SFLOAT_S8_UINT,
    D24UnormS8Uint          = VK_FORMAT_D24_UNORM_S8_UINT,
    X8D24UnormPack32        = VK_FORMAT_X8_D24_UNORM_PACK32,
    D16Unorm                = VK_FORMAT_D16_UNORM,
    D16UnormS8Uint          = VK_FORMAT_D16_UNORM_S8_UINT,
    S8Uint                  = VK_FORMAT_S8_UINT,
};

[[nodiscard]] constexpr auto ToVk(Format format) noexcept -> VkFormat {
    return static_cast<VkFormat>(format);
}
[[nodiscard]] constexpr auto FromVk(VkFormat format) noexcept -> Format {
    return static_cast<Format>(format);
}

// Synchronization-2 pipeline stages. Superset of the former BarrierStage
// enumerators (kept as an alias in execution/RenderQueue.hpp).
// NOLINTNEXTLINE(performance-enum-size)
enum class PipelineStage : VkPipelineStageFlags2 {
    StageNone                  = VK_PIPELINE_STAGE_2_NONE,
    None                       = VK_PIPELINE_STAGE_2_NONE,
    TopOfPipe                  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
    BottomOfPipe               = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
    AllCommands                = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    AllGraphics                = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
    Compute                    = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
    Fragment                   = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
    Vertex                     = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
    ColorAttachment            = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    EarlyFragment              = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
    LateFragment               = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
    Indirect                   = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
    Transfer                   = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
    Copy                       = VK_PIPELINE_STAGE_2_COPY_BIT,
    Clear                      = VK_PIPELINE_STAGE_2_CLEAR_BIT,
    Host                       = VK_PIPELINE_STAGE_2_HOST_BIT,
    IndexInput                 = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
    VertexInput                = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
    VertexAttributeInput       = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
    AccelerationStructureBuild = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
};

// NOLINTNEXTLINE(performance-enum-size)
enum class Access : VkAccessFlags2 {
    AccessNone                 = VK_ACCESS_2_NONE,
    None                       = VK_ACCESS_2_NONE,
    ShaderRead                 = VK_ACCESS_2_SHADER_READ_BIT,
    ShaderWrite                = VK_ACCESS_2_SHADER_WRITE_BIT,
    ShaderStorageRead          = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
    ShaderStorageWrite         = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    UniformRead                = VK_ACCESS_2_UNIFORM_READ_BIT,
    IndirectRead               = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
    IndexRead                  = VK_ACCESS_2_INDEX_READ_BIT,
    VertexAttributeRead        = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT,
    TransferRead               = VK_ACCESS_2_TRANSFER_READ_BIT,
    TransferWrite              = VK_ACCESS_2_TRANSFER_WRITE_BIT,
    HostRead                   = VK_ACCESS_2_HOST_READ_BIT,
    HostWrite                  = VK_ACCESS_2_HOST_WRITE_BIT,
    MemoryRead                 = VK_ACCESS_2_MEMORY_READ_BIT,
    MemoryWrite                = VK_ACCESS_2_MEMORY_WRITE_BIT,
    ColorRead                  = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
    ColorWrite                 = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
    DepthRead                  = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
    DepthWrite                 = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    AccelerationStructureRead  = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
    AccelerationStructureWrite = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
};

[[nodiscard]] constexpr auto ToVk(PipelineStage stage) noexcept -> VkPipelineStageFlags2 {
    return static_cast<VkPipelineStageFlags2>(stage);
}
[[nodiscard]] constexpr auto ToVk(Access access) noexcept -> VkAccessFlags2 {
    return static_cast<VkAccessFlags2>(access);
}
[[nodiscard]] constexpr auto operator|(PipelineStage a, PipelineStage b) noexcept -> PipelineStage {
    return static_cast<PipelineStage>(ToVk(a) | ToVk(b));
}
constexpr auto operator|=(PipelineStage& a, PipelineStage b) noexcept -> PipelineStage& {
    a = a | b;
    return a;
}
[[nodiscard]] constexpr auto operator&(PipelineStage a, PipelineStage b) noexcept -> PipelineStage {
    return static_cast<PipelineStage>(ToVk(a) & ToVk(b));
}
[[nodiscard]] constexpr auto Has(PipelineStage flags, PipelineStage bits) noexcept -> bool {
    return (ToVk(flags) & ToVk(bits)) != 0;
}
[[nodiscard]] constexpr auto operator|(Access a, Access b) noexcept -> Access {
    return static_cast<Access>(ToVk(a) | ToVk(b));
}
constexpr auto operator|=(Access& a, Access b) noexcept -> Access& {
    a = a | b;
    return a;
}
[[nodiscard]] constexpr auto operator&(Access a, Access b) noexcept -> Access {
    return static_cast<Access>(ToVk(a) & ToVk(b));
}
[[nodiscard]] constexpr auto Has(Access flags, Access bits) noexcept -> bool {
    return (ToVk(flags) & ToVk(bits)) != 0;
}

// NOLINTNEXTLINE(performance-enum-size)
enum class ImageAspect : VkImageAspectFlags {
    None    = VK_IMAGE_ASPECT_NONE,
    Color   = VK_IMAGE_ASPECT_COLOR_BIT,
    Depth   = VK_IMAGE_ASPECT_DEPTH_BIT,
    Stencil = VK_IMAGE_ASPECT_STENCIL_BIT,
};

[[nodiscard]] constexpr auto ToVk(ImageAspect aspect) noexcept -> VkImageAspectFlags {
    return static_cast<VkImageAspectFlags>(aspect);
}
[[nodiscard]] constexpr auto operator|(ImageAspect a, ImageAspect b) noexcept -> ImageAspect {
    return static_cast<ImageAspect>(ToVk(a) | ToVk(b));
}
constexpr auto operator|=(ImageAspect& a, ImageAspect b) noexcept -> ImageAspect& {
    a = a | b;
    return a;
}
[[nodiscard]] constexpr auto operator&(ImageAspect a, ImageAspect b) noexcept -> ImageAspect {
    return static_cast<ImageAspect>(ToVk(a) & ToVk(b));
}
[[nodiscard]] constexpr auto Has(ImageAspect flags, ImageAspect bits) noexcept -> bool {
    return (ToVk(flags) & ToVk(bits)) != 0;
}

// NOLINTNEXTLINE(performance-enum-size)
enum class CompareOp : uint32_t {
    Never = VK_COMPARE_OP_NEVER,             Less = VK_COMPARE_OP_LESS,
    Equal = VK_COMPARE_OP_EQUAL,             LessOrEqual = VK_COMPARE_OP_LESS_OR_EQUAL,
    Greater = VK_COMPARE_OP_GREATER,         NotEqual = VK_COMPARE_OP_NOT_EQUAL,
    GreaterOrEqual = VK_COMPARE_OP_GREATER_OR_EQUAL, Always = VK_COMPARE_OP_ALWAYS,
};
[[nodiscard]] constexpr auto ToVk(CompareOp op) noexcept -> VkCompareOp { return static_cast<VkCompareOp>(op); }
[[nodiscard]] constexpr auto FromVk(VkCompareOp op) noexcept -> CompareOp { return static_cast<CompareOp>(op); }

// NOLINTNEXTLINE(performance-enum-size)
enum class BlendFactor : uint32_t {
    Zero = VK_BLEND_FACTOR_ZERO, One = VK_BLEND_FACTOR_ONE,
    SrcAlpha = VK_BLEND_FACTOR_SRC_ALPHA, OneMinusSrcAlpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    SrcColor = VK_BLEND_FACTOR_SRC_COLOR, OneMinusSrcColor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
    DstColor = VK_BLEND_FACTOR_DST_COLOR, OneMinusDstColor = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
    DstAlpha = VK_BLEND_FACTOR_DST_ALPHA, OneMinusDstAlpha = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
};
[[nodiscard]] constexpr auto ToVk(BlendFactor f) noexcept -> VkBlendFactor { return static_cast<VkBlendFactor>(f); }
[[nodiscard]] constexpr auto FromVk(VkBlendFactor f) noexcept -> BlendFactor { return static_cast<BlendFactor>(f); }

// NOLINTNEXTLINE(performance-enum-size)
enum class BlendOp : uint32_t {
    Add = VK_BLEND_OP_ADD, Subtract = VK_BLEND_OP_SUBTRACT, ReverseSubtract = VK_BLEND_OP_REVERSE_SUBTRACT,
    Min = VK_BLEND_OP_MIN, Max = VK_BLEND_OP_MAX,
};
[[nodiscard]] constexpr auto ToVk(BlendOp op) noexcept -> VkBlendOp { return static_cast<VkBlendOp>(op); }
[[nodiscard]] constexpr auto FromVk(VkBlendOp op) noexcept -> BlendOp { return static_cast<BlendOp>(op); }

// NOLINTNEXTLINE(performance-enum-size)
enum class Filter : uint32_t { Nearest = VK_FILTER_NEAREST, Linear = VK_FILTER_LINEAR };
[[nodiscard]] constexpr auto ToVk(Filter f) noexcept -> VkFilter { return static_cast<VkFilter>(f); }
[[nodiscard]] constexpr auto FromVk(VkFilter f) noexcept -> Filter { return static_cast<Filter>(f); }

// No Vk::SamplerAddressMode here: pipeline/Sampler.hpp already owns that
// name for its packed-token domain enum (Repeat/MirroredRepeat/ClampToEdge/
// ClampToBorder, values matching VK_SAMPLER_ADDRESS_MODE_*), and render code
// passes Vk::SamplerAddressMode to Sampler::WithAddressModes.

// NOLINTNEXTLINE(performance-enum-size)
enum class SamplerMipmapMode : uint32_t { Nearest = VK_SAMPLER_MIPMAP_MODE_NEAREST, Linear = VK_SAMPLER_MIPMAP_MODE_LINEAR };
[[nodiscard]] constexpr auto ToVk(SamplerMipmapMode m) noexcept -> VkSamplerMipmapMode { return static_cast<VkSamplerMipmapMode>(m); }
[[nodiscard]] constexpr auto FromVk(VkSamplerMipmapMode m) noexcept -> SamplerMipmapMode { return static_cast<SamplerMipmapMode>(m); }

// NOLINTNEXTLINE(performance-enum-size)
enum class CullMode : VkCullModeFlags {
    None = VK_CULL_MODE_NONE, Front = VK_CULL_MODE_FRONT_BIT, Back = VK_CULL_MODE_BACK_BIT, FrontAndBack = VK_CULL_MODE_FRONT_AND_BACK,
};
[[nodiscard]] constexpr auto ToVk(CullMode m) noexcept -> VkCullModeFlags { return static_cast<VkCullModeFlags>(m); }
[[nodiscard]] constexpr auto operator|(CullMode a, CullMode b) noexcept -> CullMode { return static_cast<CullMode>(ToVk(a) | ToVk(b)); }

// NOLINTNEXTLINE(performance-enum-size)
enum class FrontFace : uint32_t { CounterClockwise = VK_FRONT_FACE_COUNTER_CLOCKWISE, Clockwise = VK_FRONT_FACE_CLOCKWISE };
[[nodiscard]] constexpr auto ToVk(FrontFace f) noexcept -> VkFrontFace { return static_cast<VkFrontFace>(f); }
[[nodiscard]] constexpr auto FromVk(VkFrontFace f) noexcept -> FrontFace { return static_cast<FrontFace>(f); }

// NOLINTNEXTLINE(performance-enum-size)
enum class PolygonMode : uint32_t { Fill = VK_POLYGON_MODE_FILL, Line = VK_POLYGON_MODE_LINE, Point = VK_POLYGON_MODE_POINT };
[[nodiscard]] constexpr auto ToVk(PolygonMode m) noexcept -> VkPolygonMode { return static_cast<VkPolygonMode>(m); }
[[nodiscard]] constexpr auto FromVk(VkPolygonMode m) noexcept -> PolygonMode { return static_cast<PolygonMode>(m); }

// NOLINTNEXTLINE(performance-enum-size)
enum class PrimitiveTopology : uint32_t {
    PointList = VK_PRIMITIVE_TOPOLOGY_POINT_LIST, LineList = VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
    LineStrip = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, TriangleList = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    TriangleStrip = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, TriangleFan = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
};
[[nodiscard]] constexpr auto ToVk(PrimitiveTopology t) noexcept -> VkPrimitiveTopology { return static_cast<VkPrimitiveTopology>(t); }
[[nodiscard]] constexpr auto FromVk(VkPrimitiveTopology t) noexcept -> PrimitiveTopology { return static_cast<PrimitiveTopology>(t); }

// NOLINTNEXTLINE(performance-enum-size)
enum class AttachmentLoadOp : uint32_t { Load = VK_ATTACHMENT_LOAD_OP_LOAD, Clear = VK_ATTACHMENT_LOAD_OP_CLEAR, DontCare = VK_ATTACHMENT_LOAD_OP_DONT_CARE };
[[nodiscard]] constexpr auto ToVk(AttachmentLoadOp op) noexcept -> VkAttachmentLoadOp { return static_cast<VkAttachmentLoadOp>(op); }
[[nodiscard]] constexpr auto FromVk(VkAttachmentLoadOp op) noexcept -> AttachmentLoadOp { return static_cast<AttachmentLoadOp>(op); }

// NOLINTNEXTLINE(performance-enum-size)
enum class AttachmentStoreOp : uint32_t { Store = VK_ATTACHMENT_STORE_OP_STORE, DontCare = VK_ATTACHMENT_STORE_OP_DONT_CARE, None = VK_ATTACHMENT_STORE_OP_NONE };
[[nodiscard]] constexpr auto ToVk(AttachmentStoreOp op) noexcept -> VkAttachmentStoreOp { return static_cast<VkAttachmentStoreOp>(op); }
[[nodiscard]] constexpr auto FromVk(VkAttachmentStoreOp op) noexcept -> AttachmentStoreOp { return static_cast<AttachmentStoreOp>(op); }

// NOLINTNEXTLINE(performance-enum-size)
enum class IndexType : uint32_t { Uint16 = VK_INDEX_TYPE_UINT16, Uint32 = VK_INDEX_TYPE_UINT32, None = VK_INDEX_TYPE_NONE_KHR, Uint8 = VK_INDEX_TYPE_UINT8 };
[[nodiscard]] constexpr auto ToVk(IndexType t) noexcept -> VkIndexType { return static_cast<VkIndexType>(t); }
[[nodiscard]] constexpr auto FromVk(VkIndexType t) noexcept -> IndexType { return static_cast<IndexType>(t); }

// NOLINTNEXTLINE(performance-enum-size)
enum class ShaderStage : VkShaderStageFlags {
    None = 0, Vertex = VK_SHADER_STAGE_VERTEX_BIT, Fragment = VK_SHADER_STAGE_FRAGMENT_BIT,
    Compute = VK_SHADER_STAGE_COMPUTE_BIT, Task = VK_SHADER_STAGE_TASK_BIT_EXT, Mesh = VK_SHADER_STAGE_MESH_BIT_EXT,
    AllGraphics = VK_SHADER_STAGE_ALL_GRAPHICS, All = VK_SHADER_STAGE_ALL,
};
// VkPipelineShaderStageCreateInfo::stage is the one Vulkan field spelled in
// the singular FlagBits type, so ToVk returns VkShaderStageFlagBits: call
// sites write `.stage = ToVk(stage)` with no further cast. Where a
// VkShaderStageFlags (uint32_t) is expected -- stageFlags masks, combined
// stages -- the FlagBits value converts implicitly, bits unchanged.
[[nodiscard]] constexpr auto ToVk(ShaderStage s) noexcept -> VkShaderStageFlagBits { return static_cast<VkShaderStageFlagBits>(s); }
[[nodiscard]] constexpr auto operator|(ShaderStage a, ShaderStage b) noexcept -> ShaderStage { return static_cast<ShaderStage>(ToVk(a) | ToVk(b)); }
constexpr auto operator|=(ShaderStage& a, ShaderStage b) noexcept -> ShaderStage& { a = a | b; return a; }
[[nodiscard]] constexpr auto operator&(ShaderStage a, ShaderStage b) noexcept -> ShaderStage { return static_cast<ShaderStage>(ToVk(a) & ToVk(b)); }
[[nodiscard]] constexpr auto Has(ShaderStage flags, ShaderStage bits) noexcept -> bool { return (ToVk(flags) & ToVk(bits)) != 0; }

} // namespace ZHLN::Vk
