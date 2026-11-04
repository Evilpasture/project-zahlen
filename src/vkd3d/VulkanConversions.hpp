#pragma once

// Neutral <-> Vulkan conversion. The neutral vocabulary (VkD3D::*) is
// defined by <vkd3d/Enums.hpp>; the Vulkan spellings by the vulkan seam
// (<vulkan/Enums.hpp>, whose values ARE the VK_* constants, so these
// converters are static_casts, not tables). The D3D12 direction lives in
// src/d3d12/D3D12Conversions.hpp -- nothing here names a D3D12 value,
// and the D3D12 backend never includes this header.

#include <vulkan/Enums.hpp>
#include <vkd3d/Enums.hpp>

#include <cstdint>

namespace ZHLN::VkD3D {

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

[[nodiscard]] constexpr auto ToVk(ImageAspect aspect) noexcept -> Vk::ImageAspect {
    Vk::ImageAspect result = Vk::ImageAspect::None;
    if ((aspect & ImageAspect::Color) != ImageAspect::None) { result = result | Vk::ImageAspect::Color; }
    if ((aspect & ImageAspect::Depth) != ImageAspect::None) { result = result | Vk::ImageAspect::Depth; }
    if ((aspect & ImageAspect::Stencil) != ImageAspect::None) { result = result | Vk::ImageAspect::Stencil; }
    return result;
}

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

[[nodiscard]] constexpr auto ToVk(IndexType t) noexcept -> Vk::IndexType {
    return t == IndexType::Uint16 ? Vk::IndexType::Uint16 : Vk::IndexType::Uint32;
}

} // namespace ZHLN::VkD3D
