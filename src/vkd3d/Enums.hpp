// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// API-neutral spellings of the joint-core renderer enums, plus the only
// translation tables in the tree: VkD3D <-> Vk and VkD3D <-> D3D12. The
// backend spellings are casts (Vk::ToVk / static_cast<D3D12_*>), so a neutral
// value that has no equivalent on one side maps to that side's
// Undefined/Unknown/None and is documented at the case, never silently
// reinterpreted. Renderer code does not use these yet -- src/render keeps
// naming Vk:: spellings until the backend-selection migration.
#include <cstdint>
#include <d3d12/Enums.hpp>
#include <vulkan/Enums.hpp>

namespace ZHLN::VkD3D {

// NOLINTNEXTLINE(performance-enum-size)
enum class ImageLayout : uint32_t {
    Undefined, General, ColorAttachmentOptimal, DepthStencilAttachmentOptimal, DepthAttachmentOptimal,
    ShaderReadOnlyOptimal, PresentSrc, TransferSrcOptimal, TransferDstOptimal,
};

[[nodiscard]] constexpr auto ToVk(ImageLayout layout) noexcept -> Vk::ImageLayout {
    switch (layout) {
        case ImageLayout::Undefined: return Vk::ImageLayout::Undefined;
        case ImageLayout::General: return Vk::ImageLayout::General;
        case ImageLayout::ColorAttachmentOptimal: return Vk::ImageLayout::ColorAttachmentOptimal;
        case ImageLayout::DepthStencilAttachmentOptimal: return Vk::ImageLayout::DepthStencilAttachmentOptimal;
        case ImageLayout::DepthAttachmentOptimal: return Vk::ImageLayout::DepthAttachmentOptimal;
        case ImageLayout::ShaderReadOnlyOptimal: return Vk::ImageLayout::ShaderReadOnlyOptimal;
        case ImageLayout::PresentSrc: return Vk::ImageLayout::PresentSrc;
        case ImageLayout::TransferSrcOptimal: return Vk::ImageLayout::TransferSrcOptimal;
        case ImageLayout::TransferDstOptimal: return Vk::ImageLayout::TransferDstOptimal;
    }
    return Vk::ImageLayout::Undefined;
}

[[nodiscard]] constexpr auto ToD3D12(ImageLayout layout) noexcept -> D3D12::ImageLayout {
    switch (layout) {
        case ImageLayout::Undefined: return D3D12::ImageLayout::Undefined;
        // Vulkan GENERAL (any storage/read/write use) is D3D12's writable layout.
        case ImageLayout::General: return D3D12::ImageLayout::UnorderedAccess;
        case ImageLayout::ColorAttachmentOptimal: return D3D12::ImageLayout::RenderTarget;
        case ImageLayout::DepthStencilAttachmentOptimal: return D3D12::ImageLayout::DepthStencilWrite;
        case ImageLayout::DepthAttachmentOptimal: return D3D12::ImageLayout::DepthStencilWrite;
        case ImageLayout::ShaderReadOnlyOptimal: return D3D12::ImageLayout::ShaderResource;
        case ImageLayout::PresentSrc: return D3D12::ImageLayout::Present;
        case ImageLayout::TransferSrcOptimal: return D3D12::ImageLayout::CopySource;
        case ImageLayout::TransferDstOptimal: return D3D12::ImageLayout::CopyDest;
    }
    return D3D12::ImageLayout::Undefined;
}

[[nodiscard]] constexpr auto FromVk(Vk::ImageLayout layout) noexcept -> ImageLayout {
    switch (layout) {
        case Vk::ImageLayout::Undefined: return ImageLayout::Undefined;
        case Vk::ImageLayout::General: return ImageLayout::General;
        case Vk::ImageLayout::ColorAttachmentOptimal: return ImageLayout::ColorAttachmentOptimal;
        case Vk::ImageLayout::DepthStencilAttachmentOptimal: return ImageLayout::DepthStencilAttachmentOptimal;
        case Vk::ImageLayout::DepthAttachmentOptimal: return ImageLayout::DepthAttachmentOptimal;
        case Vk::ImageLayout::ShaderReadOnlyOptimal: return ImageLayout::ShaderReadOnlyOptimal;
        case Vk::ImageLayout::PresentSrc: return ImageLayout::PresentSrc;
        case Vk::ImageLayout::TransferSrcOptimal: return ImageLayout::TransferSrcOptimal;
        case Vk::ImageLayout::TransferDstOptimal: return ImageLayout::TransferDstOptimal;
    }
    return ImageLayout::Undefined;
}

[[nodiscard]] constexpr auto FromD3D12(D3D12::ImageLayout layout) noexcept -> ImageLayout {
    switch (layout) {
        case D3D12::ImageLayout::Undefined: return ImageLayout::Undefined;
        case D3D12::ImageLayout::Common: return ImageLayout::PresentSrc; // PRESENT aliases COMMON in D3D12
        case D3D12::ImageLayout::GenericRead:
        case D3D12::ImageLayout::ShaderResource: return ImageLayout::ShaderReadOnlyOptimal;
        case D3D12::ImageLayout::RenderTarget: return ImageLayout::ColorAttachmentOptimal;
        case D3D12::ImageLayout::UnorderedAccess: return ImageLayout::General;
        case D3D12::ImageLayout::DepthStencilWrite: return ImageLayout::DepthStencilAttachmentOptimal;
        case D3D12::ImageLayout::DepthStencilRead: return ImageLayout::DepthAttachmentOptimal;
        case D3D12::ImageLayout::CopySource: return ImageLayout::TransferSrcOptimal;
        case D3D12::ImageLayout::CopyDest: return ImageLayout::TransferDstOptimal;
        default: return ImageLayout::General; // video/resolve/shading-rate layouts have no Vk spelling here yet
    }
}

// NOLINTNEXTLINE(performance-enum-size)
enum class Format : uint32_t {
    Undefined, R32G32B32A32Sfloat, R32G32B32A32Uint, R32G32B32Sfloat, R32G32B32Uint, R16G16B16A16Sfloat, R16G16B16A16Uint, R32G32Sfloat, R32G32Uint, R32Sfloat, R32Uint, R32Sint, R16G16Sfloat, R8G8B8A8Unorm, R8G8B8A8Srgb, B8G8R8A8Unorm, B8G8R8A8Srgb, R8G8B8Unorm, R8G8B8Srgb, B8G8R8Srgb, R8G8Unorm, R8G8Srgb, R8Unorm, R8Uint, R8Srgb, A8B8G8R8UnormPack32, A8B8G8R8SrgbPack32, A2B10G10R10UnormPack32, B10G11R11UfloatPack32, D32Sfloat, D32SfloatS8Uint, D24UnormS8Uint, X8D24UnormPack32, D16Unorm, D16UnormS8Uint, S8Uint,
};

[[nodiscard]] constexpr auto ToVk(Format format) noexcept -> Vk::Format {
    switch (format) {
        case Format::Undefined: return Vk::Format::Undefined;
        case Format::R32G32B32A32Sfloat: return Vk::Format::R32G32B32A32Sfloat;
        case Format::R32G32B32A32Uint: return Vk::Format::R32G32B32A32Uint;
        case Format::R32G32B32Sfloat: return Vk::Format::R32G32B32Sfloat;
        case Format::R32G32B32Uint: return Vk::Format::R32G32B32Uint;
        case Format::R16G16B16A16Sfloat: return Vk::Format::R16G16B16A16Sfloat;
        case Format::R16G16B16A16Uint: return Vk::Format::R16G16B16A16Uint;
        case Format::R32G32Sfloat: return Vk::Format::R32G32Sfloat;
        case Format::R32G32Uint: return Vk::Format::R32G32Uint;
        case Format::R32Sfloat: return Vk::Format::R32Sfloat;
        case Format::R32Uint: return Vk::Format::R32Uint;
        case Format::R32Sint: return Vk::Format::R32Sint;
        case Format::R16G16Sfloat: return Vk::Format::R16G16Sfloat;
        case Format::R8G8B8A8Unorm: return Vk::Format::R8G8B8A8Unorm;
        case Format::R8G8B8A8Srgb: return Vk::Format::R8G8B8A8Srgb;
        case Format::B8G8R8A8Unorm: return Vk::Format::B8G8R8A8Unorm;
        case Format::B8G8R8A8Srgb: return Vk::Format::B8G8R8A8Srgb;
        case Format::R8G8B8Unorm: return Vk::Format::R8G8B8Unorm;
        case Format::R8G8B8Srgb: return Vk::Format::R8G8B8Srgb;
        case Format::B8G8R8Srgb: return Vk::Format::B8G8R8Srgb;
        case Format::R8G8Unorm: return Vk::Format::R8G8Unorm;
        case Format::R8G8Srgb: return Vk::Format::R8G8Srgb;
        case Format::R8Unorm: return Vk::Format::R8Unorm;
        case Format::R8Uint: return Vk::Format::R8Uint;
        case Format::R8Srgb: return Vk::Format::R8Srgb;
        case Format::A8B8G8R8UnormPack32: return Vk::Format::A8B8G8R8UnormPack32;
        case Format::A8B8G8R8SrgbPack32: return Vk::Format::A8B8G8R8SrgbPack32;
        case Format::A2B10G10R10UnormPack32: return Vk::Format::A2B10G10R10UnormPack32;
        case Format::B10G11R11UfloatPack32: return Vk::Format::B10G11R11UfloatPack32;
        case Format::D32Sfloat: return Vk::Format::D32Sfloat;
        case Format::D32SfloatS8Uint: return Vk::Format::D32SfloatS8Uint;
        case Format::D24UnormS8Uint: return Vk::Format::D24UnormS8Uint;
        case Format::X8D24UnormPack32: return Vk::Format::X8D24UnormPack32;
        case Format::D16Unorm: return Vk::Format::D16Unorm;
        case Format::D16UnormS8Uint: return Vk::Format::D16UnormS8Uint;
        case Format::S8Uint: return Vk::Format::S8Uint;
    }
    return Vk::Format::Undefined;
}

[[nodiscard]] constexpr auto ToD3D12(Format format) noexcept -> D3D12::Format {
    switch (format) {
        case Format::Undefined: return D3D12::Format::Unknown;
        case Format::R32G32B32A32Sfloat: return D3D12::Format::R32G32B32A32Float;
        case Format::R32G32B32A32Uint: return D3D12::Format::R32G32B32A32Uint;
        case Format::R32G32B32Sfloat: return D3D12::Format::R32G32B32Float;
        case Format::R32G32B32Uint: return D3D12::Format::R32G32B32Uint;
        case Format::R16G16B16A16Sfloat: return D3D12::Format::R16G16B16A16Float;
        case Format::R16G16B16A16Uint: return D3D12::Format::R16G16B16A16Uint;
        case Format::R32G32Sfloat: return D3D12::Format::R32G32Float;
        case Format::R32G32Uint: return D3D12::Format::R32G32Uint;
        case Format::R32Sfloat: return D3D12::Format::R32Float;
        case Format::R32Uint: return D3D12::Format::R32Uint;
        case Format::R32Sint: return D3D12::Format::R32Sint;
        case Format::R16G16Sfloat: return D3D12::Format::R16G16Float;
        case Format::R8G8B8A8Unorm: return D3D12::Format::R8G8B8A8Unorm;
        case Format::R8G8B8A8Srgb: return D3D12::Format::R8G8B8A8Srgb;
        case Format::B8G8R8A8Unorm: return D3D12::Format::B8G8R8A8Unorm;
        case Format::B8G8R8A8Srgb: return D3D12::Format::B8G8R8A8Srgb;
        case Format::R8G8B8Unorm: return D3D12::Format::Unknown; // no DXGI equivalent
        case Format::R8G8B8Srgb: return D3D12::Format::Unknown; // no DXGI equivalent
        case Format::B8G8R8Srgb: return D3D12::Format::Unknown; // no DXGI equivalent
        case Format::R8G8Unorm: return D3D12::Format::R8G8Unorm;
        case Format::R8G8Srgb: return D3D12::Format::Unknown; // no DXGI equivalent
        case Format::R8Unorm: return D3D12::Format::R8Unorm;
        case Format::R8Uint: return D3D12::Format::R8Uint;
        case Format::R8Srgb: return D3D12::Format::Unknown; // no DXGI equivalent
        case Format::A8B8G8R8UnormPack32: return D3D12::Format::R8G8B8A8Unorm;
        case Format::A8B8G8R8SrgbPack32: return D3D12::Format::R8G8B8A8Srgb;
        case Format::A2B10G10R10UnormPack32: return D3D12::Format::R10G10B10A2Unorm;
        case Format::B10G11R11UfloatPack32: return D3D12::Format::R11G11B10Float;
        case Format::D32Sfloat: return D3D12::Format::D32Float;
        case Format::D32SfloatS8Uint: return D3D12::Format::D32FloatS8X24Uint;
        case Format::D24UnormS8Uint: return D3D12::Format::D24UnormS8Uint;
        case Format::X8D24UnormPack32: return D3D12::Format::D24UnormS8Uint;
        case Format::D16Unorm: return D3D12::Format::D16Unorm;
        case Format::D16UnormS8Uint: return D3D12::Format::Unknown; // no DXGI equivalent
        case Format::S8Uint: return D3D12::Format::Unknown; // no DXGI equivalent
    }
    return D3D12::Format::Unknown;
}

[[nodiscard]] constexpr auto FromVk(Vk::Format format) noexcept -> Format {
    switch (format) {
        case Vk::Format::Undefined: return Format::Undefined;
        case Vk::Format::R32G32B32A32Sfloat: return Format::R32G32B32A32Sfloat;
        case Vk::Format::R32G32B32A32Uint: return Format::R32G32B32A32Uint;
        case Vk::Format::R32G32B32Sfloat: return Format::R32G32B32Sfloat;
        case Vk::Format::R32G32B32Uint: return Format::R32G32B32Uint;
        case Vk::Format::R16G16B16A16Sfloat: return Format::R16G16B16A16Sfloat;
        case Vk::Format::R16G16B16A16Uint: return Format::R16G16B16A16Uint;
        case Vk::Format::R32G32Sfloat: return Format::R32G32Sfloat;
        case Vk::Format::R32G32Uint: return Format::R32G32Uint;
        case Vk::Format::R32Sfloat: return Format::R32Sfloat;
        case Vk::Format::R32Uint: return Format::R32Uint;
        case Vk::Format::R32Sint: return Format::R32Sint;
        case Vk::Format::R16G16Sfloat: return Format::R16G16Sfloat;
        case Vk::Format::R8G8B8A8Unorm: return Format::R8G8B8A8Unorm;
        case Vk::Format::R8G8B8A8Srgb: return Format::R8G8B8A8Srgb;
        case Vk::Format::B8G8R8A8Unorm: return Format::B8G8R8A8Unorm;
        case Vk::Format::B8G8R8A8Srgb: return Format::B8G8R8A8Srgb;
        case Vk::Format::R8G8B8Unorm: return Format::R8G8B8Unorm;
        case Vk::Format::R8G8B8Srgb: return Format::R8G8B8Srgb;
        case Vk::Format::B8G8R8Srgb: return Format::B8G8R8Srgb;
        case Vk::Format::R8G8Unorm: return Format::R8G8Unorm;
        case Vk::Format::R8G8Srgb: return Format::R8G8Srgb;
        case Vk::Format::R8Unorm: return Format::R8Unorm;
        case Vk::Format::R8Uint: return Format::R8Uint;
        case Vk::Format::R8Srgb: return Format::R8Srgb;
        case Vk::Format::A8B8G8R8UnormPack32: return Format::A8B8G8R8UnormPack32;
        case Vk::Format::A8B8G8R8SrgbPack32: return Format::A8B8G8R8SrgbPack32;
        case Vk::Format::A2B10G10R10UnormPack32: return Format::A2B10G10R10UnormPack32;
        case Vk::Format::B10G11R11UfloatPack32: return Format::B10G11R11UfloatPack32;
        case Vk::Format::D32Sfloat: return Format::D32Sfloat;
        case Vk::Format::D32SfloatS8Uint: return Format::D32SfloatS8Uint;
        case Vk::Format::D24UnormS8Uint: return Format::D24UnormS8Uint;
        case Vk::Format::X8D24UnormPack32: return Format::X8D24UnormPack32;
        case Vk::Format::D16Unorm: return Format::D16Unorm;
        case Vk::Format::D16UnormS8Uint: return Format::D16UnormS8Uint;
        case Vk::Format::S8Uint: return Format::S8Uint;
    }
    return Format::Undefined;
}

[[nodiscard]] constexpr auto FromD3D12(D3D12::Format format) noexcept -> Format {
    switch (format) {
        case D3D12::Format::Unknown: return Format::Undefined;
        case D3D12::Format::R32G32B32A32Float: return Format::R32G32B32A32Sfloat;
        case D3D12::Format::R32G32B32A32Uint: return Format::R32G32B32A32Uint;
        case D3D12::Format::R32G32B32Float: return Format::R32G32B32Sfloat;
        case D3D12::Format::R32G32B32Uint: return Format::R32G32B32Uint;
        case D3D12::Format::R16G16B16A16Float: return Format::R16G16B16A16Sfloat;
        case D3D12::Format::R16G16B16A16Uint: return Format::R16G16B16A16Uint;
        case D3D12::Format::R32G32Float: return Format::R32G32Sfloat;
        case D3D12::Format::R32G32Uint: return Format::R32G32Uint;
        case D3D12::Format::R32Float: return Format::R32Sfloat;
        case D3D12::Format::R32Uint: return Format::R32Uint;
        case D3D12::Format::R32Sint: return Format::R32Sint;
        case D3D12::Format::R16G16Float: return Format::R16G16Sfloat;
        case D3D12::Format::R8G8B8A8Unorm: return Format::R8G8B8A8Unorm;
        case D3D12::Format::R8G8B8A8Srgb: return Format::R8G8B8A8Srgb;
        case D3D12::Format::B8G8R8A8Unorm: return Format::B8G8R8A8Unorm;
        case D3D12::Format::B8G8R8A8Srgb: return Format::B8G8R8A8Srgb;
        case D3D12::Format::R8G8Unorm: return Format::R8G8Unorm;
        case D3D12::Format::R8Unorm: return Format::R8Unorm;
        case D3D12::Format::R8Uint: return Format::R8Uint;
        case D3D12::Format::R10G10B10A2Unorm: return Format::A2B10G10R10UnormPack32;
        case D3D12::Format::R11G11B10Float: return Format::B10G11R11UfloatPack32;
        case D3D12::Format::D32Float: return Format::D32Sfloat;
        case D3D12::Format::D32FloatS8X24Uint: return Format::D32SfloatS8Uint;
        case D3D12::Format::D24UnormS8Uint: return Format::D24UnormS8Uint;
        case D3D12::Format::D16Unorm: return Format::D16Unorm;
        default: return Format::Undefined;
    }
}

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

[[nodiscard]] constexpr auto ToVk(PipelineStage stage) noexcept -> Vk::PipelineStage {
    Vk::PipelineStage result = Vk::PipelineStage::StageNone;
    if (Has(stage, PipelineStage::None)) { result = result | Vk::PipelineStage::StageNone; }
    if (Has(stage, PipelineStage::TopOfPipe)) { result = result | Vk::PipelineStage::TopOfPipe; }
    if (Has(stage, PipelineStage::BottomOfPipe)) { result = result | Vk::PipelineStage::BottomOfPipe; }
    if (Has(stage, PipelineStage::AllCommands)) { result = result | Vk::PipelineStage::AllCommands; }
    if (Has(stage, PipelineStage::AllGraphics)) { result = result | Vk::PipelineStage::AllGraphics; }
    if (Has(stage, PipelineStage::Compute)) { result = result | Vk::PipelineStage::Compute; }
    if (Has(stage, PipelineStage::Fragment)) { result = result | Vk::PipelineStage::Fragment; }
    if (Has(stage, PipelineStage::Vertex)) { result = result | Vk::PipelineStage::Vertex; }
    if (Has(stage, PipelineStage::ColorAttachment)) { result = result | Vk::PipelineStage::ColorAttachment; }
    if (Has(stage, PipelineStage::EarlyFragment)) { result = result | Vk::PipelineStage::EarlyFragment; }
    if (Has(stage, PipelineStage::LateFragment)) { result = result | Vk::PipelineStage::LateFragment; }
    if (Has(stage, PipelineStage::Indirect)) { result = result | Vk::PipelineStage::Indirect; }
    if (Has(stage, PipelineStage::Transfer)) { result = result | Vk::PipelineStage::Transfer; }
    if (Has(stage, PipelineStage::Copy)) { result = result | Vk::PipelineStage::Copy; }
    if (Has(stage, PipelineStage::Clear)) { result = result | Vk::PipelineStage::Clear; }
    if (Has(stage, PipelineStage::Host)) { result = result | Vk::PipelineStage::Host; }
    if (Has(stage, PipelineStage::IndexInput)) { result = result | Vk::PipelineStage::IndexInput; }
    if (Has(stage, PipelineStage::VertexInput)) { result = result | Vk::PipelineStage::VertexInput; }
    if (Has(stage, PipelineStage::VertexAttributeInput)) { result = result | Vk::PipelineStage::VertexAttributeInput; }
    if (Has(stage, PipelineStage::AccelerationStructureBuild)) { result = result | Vk::PipelineStage::AccelerationStructureBuild; }
    return result;
}

[[nodiscard]] constexpr auto ToD3D12(PipelineStage stage) noexcept -> D3D12::PipelineStage {
    D3D12::PipelineStage result = D3D12::PipelineStage::None;
    if (Has(stage, PipelineStage::None)) { result = result | D3D12::PipelineStage::None; }
    if (Has(stage, PipelineStage::TopOfPipe)) { result = result | D3D12::PipelineStage::None; }
    if (Has(stage, PipelineStage::BottomOfPipe)) { result = result | D3D12::PipelineStage::All; }
    if (Has(stage, PipelineStage::AllCommands)) { result = result | D3D12::PipelineStage::All; }
    if (Has(stage, PipelineStage::AllGraphics)) { result = result | D3D12::PipelineStage::Draw; }
    if (Has(stage, PipelineStage::Compute)) { result = result | D3D12::PipelineStage::ComputeShading; }
    if (Has(stage, PipelineStage::Fragment)) { result = result | D3D12::PipelineStage::PixelShading; }
    if (Has(stage, PipelineStage::Vertex)) { result = result | D3D12::PipelineStage::VertexShading; }
    if (Has(stage, PipelineStage::ColorAttachment)) { result = result | D3D12::PipelineStage::RenderTarget; }
    if (Has(stage, PipelineStage::EarlyFragment)) { result = result | D3D12::PipelineStage::DepthStencil; }
    if (Has(stage, PipelineStage::LateFragment)) { result = result | D3D12::PipelineStage::DepthStencil; }
    if (Has(stage, PipelineStage::Indirect)) { result = result | D3D12::PipelineStage::ExecuteIndirect; }
    if (Has(stage, PipelineStage::Transfer)) { result = result | D3D12::PipelineStage::Copy; }
    if (Has(stage, PipelineStage::Copy)) { result = result | D3D12::PipelineStage::Copy; }
    if (Has(stage, PipelineStage::Clear)) { result = result | D3D12::PipelineStage::ComputeShading; }
    if (Has(stage, PipelineStage::Host)) { result = result | D3D12::PipelineStage::None; }
    if (Has(stage, PipelineStage::IndexInput)) { result = result | D3D12::PipelineStage::IndexInput; }
    if (Has(stage, PipelineStage::VertexInput)) { result = result | D3D12::PipelineStage::IndexInput; }
    if (Has(stage, PipelineStage::VertexAttributeInput)) { result = result | D3D12::PipelineStage::IndexInput; }
    if (Has(stage, PipelineStage::AccelerationStructureBuild)) { result = result | D3D12::PipelineStage::BuildAccelerationStructure; }
    return result;
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

[[nodiscard]] constexpr auto ToVk(Access access) noexcept -> Vk::Access {
    Vk::Access result = Vk::Access::AccessNone;
    if (Has(access, Access::ShaderRead)) { result = result | Vk::Access::ShaderRead; }
    if (Has(access, Access::ShaderWrite)) { result = result | Vk::Access::ShaderWrite; }
    if (Has(access, Access::ShaderStorageRead)) { result = result | Vk::Access::ShaderStorageRead; }
    if (Has(access, Access::ShaderStorageWrite)) { result = result | Vk::Access::ShaderStorageWrite; }
    if (Has(access, Access::UniformRead)) { result = result | Vk::Access::UniformRead; }
    if (Has(access, Access::IndirectRead)) { result = result | Vk::Access::IndirectRead; }
    if (Has(access, Access::IndexRead)) { result = result | Vk::Access::IndexRead; }
    if (Has(access, Access::VertexAttributeRead)) { result = result | Vk::Access::VertexAttributeRead; }
    if (Has(access, Access::TransferRead)) { result = result | Vk::Access::TransferRead; }
    if (Has(access, Access::TransferWrite)) { result = result | Vk::Access::TransferWrite; }
    if (Has(access, Access::HostRead)) { result = result | Vk::Access::HostRead; }
    if (Has(access, Access::HostWrite)) { result = result | Vk::Access::HostWrite; }
    if (Has(access, Access::MemoryRead)) { result = result | Vk::Access::MemoryRead; }
    if (Has(access, Access::MemoryWrite)) { result = result | Vk::Access::MemoryWrite; }
    if (Has(access, Access::ColorRead)) { result = result | Vk::Access::ColorRead; }
    if (Has(access, Access::ColorWrite)) { result = result | Vk::Access::ColorWrite; }
    if (Has(access, Access::DepthRead)) { result = result | Vk::Access::DepthRead; }
    if (Has(access, Access::DepthWrite)) { result = result | Vk::Access::DepthWrite; }
    if (Has(access, Access::AccelerationStructureRead)) { result = result | Vk::Access::AccelerationStructureRead; }
    if (Has(access, Access::AccelerationStructureWrite)) { result = result | Vk::Access::AccelerationStructureWrite; }
    return result;
}

[[nodiscard]] constexpr auto ToD3D12(Access access) noexcept -> D3D12::Access {
    D3D12::Access result = D3D12::Access::Common;
    if (Has(access, Access::ShaderRead)) { result = result | D3D12::Access::ShaderResource; }
    if (Has(access, Access::ShaderWrite)) { result = result | D3D12::Access::UnorderedAccess; }
    if (Has(access, Access::ShaderStorageRead)) { result = result | D3D12::Access::ShaderResource; }
    if (Has(access, Access::ShaderStorageWrite)) { result = result | D3D12::Access::UnorderedAccess; }
    if (Has(access, Access::UniformRead)) { result = result | D3D12::Access::ConstantBuffer; }
    if (Has(access, Access::IndirectRead)) { result = result | D3D12::Access::IndirectArgument; }
    if (Has(access, Access::IndexRead)) { result = result | D3D12::Access::IndexBuffer; }
    if (Has(access, Access::VertexAttributeRead)) { result = result | D3D12::Access::VertexBuffer; }
    if (Has(access, Access::TransferRead)) { result = result | D3D12::Access::CopySource; }
    if (Has(access, Access::TransferWrite)) { result = result | D3D12::Access::CopyDest; }
    if (Has(access, Access::HostRead)) { result = result | D3D12::Access::Common; }
    if (Has(access, Access::HostWrite)) { result = result | D3D12::Access::Common; }
    if (Has(access, Access::MemoryRead)) { result = result | D3D12::Access::Common; }
    if (Has(access, Access::MemoryWrite)) { result = result | D3D12::Access::Common; }
    if (Has(access, Access::ColorRead)) { result = result | D3D12::Access::RenderTarget; }
    if (Has(access, Access::ColorWrite)) { result = result | D3D12::Access::RenderTarget; }
    if (Has(access, Access::DepthRead)) { result = result | D3D12::Access::DepthStencilRead; }
    if (Has(access, Access::DepthWrite)) { result = result | D3D12::Access::DepthStencilWrite; }
    if (Has(access, Access::AccelerationStructureRead)) { result = result | D3D12::Access::AccelerationStructureRead; }
    if (Has(access, Access::AccelerationStructureWrite)) { result = result | D3D12::Access::AccelerationStructureWrite; }
    return result;
}

// NOLINTNEXTLINE(performance-enum-size)
enum class ImageAspect : uint32_t { None = 0, Color = 1u << 0, Depth = 1u << 1, Stencil = 1u << 2 };

[[nodiscard]] constexpr auto operator|(ImageAspect a, ImageAspect b) noexcept -> ImageAspect {
    return static_cast<ImageAspect>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
[[nodiscard]] constexpr auto operator&(ImageAspect a, ImageAspect b) noexcept -> ImageAspect {
    return static_cast<ImageAspect>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
[[nodiscard]] constexpr auto ToVk(ImageAspect aspect) noexcept -> Vk::ImageAspect {
    Vk::ImageAspect result = Vk::ImageAspect::None;
    if ((aspect & ImageAspect::Color) != ImageAspect::None) { result = result | Vk::ImageAspect::Color; }
    if ((aspect & ImageAspect::Depth) != ImageAspect::None) { result = result | Vk::ImageAspect::Depth; }
    if ((aspect & ImageAspect::Stencil) != ImageAspect::None) { result = result | Vk::ImageAspect::Stencil; }
    return result;
}
// D3D12 selects depth/stencil planes via D3D12_BARRIER_SUBRESOURCE_RANGE
// PlaneSlice, not an aspect enum; the neutral flags carry the information
// and the backend lowers them when the barrier code lands.

// NOLINTNEXTLINE(performance-enum-size)
enum class CompareOp : uint32_t { Never, Less, Equal, LessOrEqual, Greater, NotEqual, GreaterOrEqual, Always };

[[nodiscard]] constexpr auto ToVk(CompareOp op) noexcept -> Vk::CompareOp {
    switch (op) {
        case CompareOp::Never: return Vk::CompareOp::Never;
        case CompareOp::Less: return Vk::CompareOp::Less;
        case CompareOp::Equal: return Vk::CompareOp::Equal;
        case CompareOp::LessOrEqual: return Vk::CompareOp::LessOrEqual;
        case CompareOp::Greater: return Vk::CompareOp::Greater;
        case CompareOp::NotEqual: return Vk::CompareOp::NotEqual;
        case CompareOp::GreaterOrEqual: return Vk::CompareOp::GreaterOrEqual;
        case CompareOp::Always: return Vk::CompareOp::Always;
    }
    return Vk::CompareOp::Never;
}
[[nodiscard]] constexpr auto ToD3D12(CompareOp op) noexcept -> D3D12::CompareOp {
    switch (op) {
        case CompareOp::Never: return D3D12::CompareOp::Never;
        case CompareOp::Less: return D3D12::CompareOp::Less;
        case CompareOp::Equal: return D3D12::CompareOp::Equal;
        case CompareOp::LessOrEqual: return D3D12::CompareOp::LessOrEqual;
        case CompareOp::Greater: return D3D12::CompareOp::Greater;
        case CompareOp::NotEqual: return D3D12::CompareOp::NotEqual;
        case CompareOp::GreaterOrEqual: return D3D12::CompareOp::GreaterOrEqual;
        case CompareOp::Always: return D3D12::CompareOp::Always;
    }
    return D3D12::CompareOp::Never;
}

// NOLINTNEXTLINE(performance-enum-size)
enum class IndexType : uint32_t { Uint16, Uint32 };
[[nodiscard]] constexpr auto ToVk(IndexType t) noexcept -> Vk::IndexType {
    return t == IndexType::Uint16 ? Vk::IndexType::Uint16 : Vk::IndexType::Uint32;
}
[[nodiscard]] constexpr auto ToD3D12(IndexType t) noexcept -> D3D12::Format {
    return t == IndexType::Uint16 ? D3D12::Format::R16Uint : D3D12::Format::R32Uint;
}

} // namespace ZHLN::VkD3D
