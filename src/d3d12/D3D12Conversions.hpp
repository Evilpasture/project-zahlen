#pragma once

// Neutral -> D3D12 translation. This is the ONLY place the neutral
// vocabulary (VkD3D::, <vkd3d/Enums.hpp>) meets the Microsoft DirectX
// headers, and it is private to the d3d12 backend: it lives under
// src/d3d12 (not any include/ seam), so only zahlen_d3d12 translation
// units -- the one target with the DirectX-Headers include path -- can
// ever see it. Every mapping names real SDK constants
// (DXGI_FORMAT_*, D3D12_BARRIER_*), so the compiler checks the tables;
// there is no hand-typed integer anywhere in this header.
//
// The reverse direction (neutral <- Vk::) lives in
// src/vkd3d/VulkanConversions.hpp; the neutral header itself contains
// no backend values at all.

#if __has_include(<wsl/winadapter.h>)
#include <wsl/winadapter.h>
#endif
#include <directx/d3d12.h>
#include <directx/dxgiformat.h>
#include <vkd3d/Enums.hpp>

#include <cstdint>

namespace ZHLN::D3D12 {

[[nodiscard]] constexpr auto ToD3D12Layout(VkD3D::ImageLayout layout) noexcept -> D3D12_BARRIER_LAYOUT {
    switch (layout) {
        case VkD3D::ImageLayout::Undefined: return D3D12_BARRIER_LAYOUT_UNDEFINED;
        // Vulkan GENERAL (any storage/read/write use) is D3D12's writable layout.
        case VkD3D::ImageLayout::General: return D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
        case VkD3D::ImageLayout::ColorAttachmentOptimal: return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
        case VkD3D::ImageLayout::DepthStencilAttachmentOptimal: return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
        case VkD3D::ImageLayout::DepthAttachmentOptimal: return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
        case VkD3D::ImageLayout::ShaderReadOnlyOptimal: return D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
        // PRESENT and COMMON are the same D3D12 layout value.
        case VkD3D::ImageLayout::PresentSrc: return D3D12_BARRIER_LAYOUT_PRESENT;
        case VkD3D::ImageLayout::TransferSrcOptimal: return D3D12_BARRIER_LAYOUT_COPY_SOURCE;
        case VkD3D::ImageLayout::TransferDstOptimal: return D3D12_BARRIER_LAYOUT_COPY_DEST;
    }
    return D3D12_BARRIER_LAYOUT_UNDEFINED;
}

[[nodiscard]] constexpr auto FromD3D12Layout(D3D12_BARRIER_LAYOUT layout) noexcept -> VkD3D::ImageLayout {
    switch (layout) {
        case D3D12_BARRIER_LAYOUT_UNDEFINED: return VkD3D::ImageLayout::Undefined;
        // PRESENT aliases COMMON in D3D12, so one case covers both.
        case D3D12_BARRIER_LAYOUT_COMMON: return VkD3D::ImageLayout::PresentSrc;
        case D3D12_BARRIER_LAYOUT_GENERIC_READ:
        case D3D12_BARRIER_LAYOUT_SHADER_RESOURCE: return VkD3D::ImageLayout::ShaderReadOnlyOptimal;
        case D3D12_BARRIER_LAYOUT_RENDER_TARGET: return VkD3D::ImageLayout::ColorAttachmentOptimal;
        case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS: return VkD3D::ImageLayout::General;
        case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE: return VkD3D::ImageLayout::DepthStencilAttachmentOptimal;
        case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ: return VkD3D::ImageLayout::DepthAttachmentOptimal;
        case D3D12_BARRIER_LAYOUT_COPY_SOURCE: return VkD3D::ImageLayout::TransferSrcOptimal;
        case D3D12_BARRIER_LAYOUT_COPY_DEST: return VkD3D::ImageLayout::TransferDstOptimal;
        default: return VkD3D::ImageLayout::General; // video/resolve/shading-rate layouts have no Vk spelling here yet
    }
}

[[nodiscard]] constexpr auto ToDxgi(VkD3D::Format format) noexcept -> DXGI_FORMAT {
    switch (format) {
        case VkD3D::Format::Undefined: return DXGI_FORMAT_UNKNOWN;
        case VkD3D::Format::R32G32B32A32Sfloat: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case VkD3D::Format::R32G32B32A32Uint: return DXGI_FORMAT_R32G32B32A32_UINT;
        case VkD3D::Format::R32G32B32Sfloat: return DXGI_FORMAT_R32G32B32_FLOAT;
        case VkD3D::Format::R32G32B32Uint: return DXGI_FORMAT_R32G32B32_UINT;
        case VkD3D::Format::R16G16B16A16Sfloat: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case VkD3D::Format::R16G16B16A16Uint: return DXGI_FORMAT_R16G16B16A16_UINT;
        case VkD3D::Format::R32G32Sfloat: return DXGI_FORMAT_R32G32_FLOAT;
        case VkD3D::Format::R32G32Uint: return DXGI_FORMAT_R32G32_UINT;
        case VkD3D::Format::R32Sfloat: return DXGI_FORMAT_R32_FLOAT;
        case VkD3D::Format::R32Uint: return DXGI_FORMAT_R32_UINT;
        case VkD3D::Format::R32Sint: return DXGI_FORMAT_R32_SINT;
        case VkD3D::Format::R16G16Sfloat: return DXGI_FORMAT_R16G16_FLOAT;
        case VkD3D::Format::R8G8B8A8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VkD3D::Format::R8G8B8A8Srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case VkD3D::Format::B8G8R8A8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case VkD3D::Format::B8G8R8A8Srgb: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case VkD3D::Format::R8G8B8Unorm: return DXGI_FORMAT_UNKNOWN; // no DXGI equivalent
        case VkD3D::Format::R8G8B8Srgb: return DXGI_FORMAT_UNKNOWN; // no DXGI equivalent
        case VkD3D::Format::B8G8R8Srgb: return DXGI_FORMAT_UNKNOWN; // no DXGI equivalent
        case VkD3D::Format::R8G8Unorm: return DXGI_FORMAT_R8G8_UNORM;
        case VkD3D::Format::R8G8Srgb: return DXGI_FORMAT_UNKNOWN; // no DXGI equivalent
        case VkD3D::Format::R8Unorm: return DXGI_FORMAT_R8_UNORM;
        case VkD3D::Format::R8Uint: return DXGI_FORMAT_R8_UINT;
        case VkD3D::Format::R8Srgb: return DXGI_FORMAT_UNKNOWN; // no DXGI equivalent
        case VkD3D::Format::A8B8G8R8UnormPack32: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VkD3D::Format::A8B8G8R8SrgbPack32: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case VkD3D::Format::A2B10G10R10UnormPack32: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case VkD3D::Format::B10G11R11UfloatPack32: return DXGI_FORMAT_R11G11B10_FLOAT;
        case VkD3D::Format::D32Sfloat: return DXGI_FORMAT_D32_FLOAT;
        case VkD3D::Format::D32SfloatS8Uint: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case VkD3D::Format::D24UnormS8Uint: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case VkD3D::Format::X8D24UnormPack32: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case VkD3D::Format::D16Unorm: return DXGI_FORMAT_D16_UNORM;
        case VkD3D::Format::D16UnormS8Uint: return DXGI_FORMAT_UNKNOWN; // no DXGI equivalent
        case VkD3D::Format::S8Uint: return DXGI_FORMAT_UNKNOWN; // no DXGI equivalent
    }
    return DXGI_FORMAT_UNKNOWN;
}

[[nodiscard]] constexpr auto FromDxgi(DXGI_FORMAT format) noexcept -> VkD3D::Format {
    switch (format) {
        case DXGI_FORMAT_UNKNOWN: return VkD3D::Format::Undefined;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return VkD3D::Format::R32G32B32A32Sfloat;
        case DXGI_FORMAT_R32G32B32A32_UINT: return VkD3D::Format::R32G32B32A32Uint;
        case DXGI_FORMAT_R32G32B32_FLOAT: return VkD3D::Format::R32G32B32Sfloat;
        case DXGI_FORMAT_R32G32B32_UINT: return VkD3D::Format::R32G32B32Uint;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return VkD3D::Format::R16G16B16A16Sfloat;
        case DXGI_FORMAT_R16G16B16A16_UINT: return VkD3D::Format::R16G16B16A16Uint;
        case DXGI_FORMAT_R32G32_FLOAT: return VkD3D::Format::R32G32Sfloat;
        case DXGI_FORMAT_R32G32_UINT: return VkD3D::Format::R32G32Uint;
        case DXGI_FORMAT_R32_FLOAT: return VkD3D::Format::R32Sfloat;
        case DXGI_FORMAT_R32_UINT: return VkD3D::Format::R32Uint;
        case DXGI_FORMAT_R32_SINT: return VkD3D::Format::R32Sint;
        case DXGI_FORMAT_R16G16_FLOAT: return VkD3D::Format::R16G16Sfloat;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return VkD3D::Format::R8G8B8A8Unorm;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return VkD3D::Format::R8G8B8A8Srgb;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return VkD3D::Format::B8G8R8A8Unorm;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return VkD3D::Format::B8G8R8A8Srgb;
        case DXGI_FORMAT_R8G8_UNORM: return VkD3D::Format::R8G8Unorm;
        case DXGI_FORMAT_R8_UNORM: return VkD3D::Format::R8Unorm;
        case DXGI_FORMAT_R8_UINT: return VkD3D::Format::R8Uint;
        case DXGI_FORMAT_R10G10B10A2_UNORM: return VkD3D::Format::A2B10G10R10UnormPack32;
        case DXGI_FORMAT_R11G11B10_FLOAT: return VkD3D::Format::B10G11R11UfloatPack32;
        case DXGI_FORMAT_D32_FLOAT: return VkD3D::Format::D32Sfloat;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return VkD3D::Format::D32SfloatS8Uint;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return VkD3D::Format::D24UnormS8Uint;
        case DXGI_FORMAT_D16_UNORM: return VkD3D::Format::D16Unorm;
        default: return VkD3D::Format::Undefined;
    }
}

[[nodiscard]] constexpr auto ToDxgi(VkD3D::IndexType type) noexcept -> DXGI_FORMAT {
    return type == VkD3D::IndexType::Uint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
}

// Many-to-one by design: D3D12 barrier sync has no top/bottom-of-pipe or
// host stage, and its input-assembly stages collapse into INDEX_INPUT.
// Unmapped neutral stages contribute D3D12_BARRIER_SYNC_NONE.
[[nodiscard]] constexpr auto ToD3D12Sync(VkD3D::PipelineStage stage) noexcept -> D3D12_BARRIER_SYNC {
    std::uint32_t result = static_cast<std::uint32_t>(D3D12_BARRIER_SYNC_NONE);
    const auto add = [&result](D3D12_BARRIER_SYNC sync) {
        result |= static_cast<std::uint32_t>(sync);
    };
    if (VkD3D::Has(stage, VkD3D::PipelineStage::BottomOfPipe)) { add(D3D12_BARRIER_SYNC_ALL); }
    if (VkD3D::Has(stage, VkD3D::PipelineStage::AllCommands)) { add(D3D12_BARRIER_SYNC_ALL); }
    if (VkD3D::Has(stage, VkD3D::PipelineStage::AllGraphics)) { add(D3D12_BARRIER_SYNC_DRAW); }
    if (VkD3D::Has(stage, VkD3D::PipelineStage::Compute)) { add(D3D12_BARRIER_SYNC_COMPUTE_SHADING); }
    if (VkD3D::Has(stage, VkD3D::PipelineStage::Fragment)) { add(D3D12_BARRIER_SYNC_PIXEL_SHADING); }
    if (VkD3D::Has(stage, VkD3D::PipelineStage::Vertex)) { add(D3D12_BARRIER_SYNC_VERTEX_SHADING); }
    if (VkD3D::Has(stage, VkD3D::PipelineStage::ColorAttachment)) { add(D3D12_BARRIER_SYNC_RENDER_TARGET); }
    if (VkD3D::Has(stage, VkD3D::PipelineStage::EarlyFragment)) { add(D3D12_BARRIER_SYNC_DEPTH_STENCIL); }
    if (VkD3D::Has(stage, VkD3D::PipelineStage::LateFragment)) { add(D3D12_BARRIER_SYNC_DEPTH_STENCIL); }
    if (VkD3D::Has(stage, VkD3D::PipelineStage::Indirect)) { add(D3D12_BARRIER_SYNC_EXECUTE_INDIRECT); }
    if (VkD3D::Has(stage, VkD3D::PipelineStage::Transfer)) { add(D3D12_BARRIER_SYNC_COPY); }
    if (VkD3D::Has(stage, VkD3D::PipelineStage::Copy)) { add(D3D12_BARRIER_SYNC_COPY); }
    if (VkD3D::Has(stage, VkD3D::PipelineStage::Clear)) { add(D3D12_BARRIER_SYNC_COMPUTE_SHADING); }
    if (VkD3D::Has(stage, VkD3D::PipelineStage::IndexInput)) { add(D3D12_BARRIER_SYNC_INDEX_INPUT); }
    if (VkD3D::Has(stage, VkD3D::PipelineStage::VertexInput)) { add(D3D12_BARRIER_SYNC_INDEX_INPUT); }
    if (VkD3D::Has(stage, VkD3D::PipelineStage::VertexAttributeInput)) { add(D3D12_BARRIER_SYNC_INDEX_INPUT); }
    if (VkD3D::Has(stage, VkD3D::PipelineStage::AccelerationStructureBuild)) {
        add(D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE);
    }
    return static_cast<D3D12_BARRIER_SYNC>(result);
}

// Host/memory-wide neutral accesses have no D3D12 barrier access bit;
// they collapse to COMMON (no barrier access), as documented per case.
[[nodiscard]] constexpr auto ToD3D12Access(VkD3D::Access access) noexcept -> D3D12_BARRIER_ACCESS {
    std::uint32_t result = static_cast<std::uint32_t>(D3D12_BARRIER_ACCESS_COMMON);
    const auto add = [&result](D3D12_BARRIER_ACCESS bit) {
        result |= static_cast<std::uint32_t>(bit);
    };
    if (VkD3D::Has(access, VkD3D::Access::ShaderRead)) { add(D3D12_BARRIER_ACCESS_SHADER_RESOURCE); }
    if (VkD3D::Has(access, VkD3D::Access::ShaderWrite)) { add(D3D12_BARRIER_ACCESS_UNORDERED_ACCESS); }
    if (VkD3D::Has(access, VkD3D::Access::ShaderStorageRead)) { add(D3D12_BARRIER_ACCESS_SHADER_RESOURCE); }
    if (VkD3D::Has(access, VkD3D::Access::ShaderStorageWrite)) { add(D3D12_BARRIER_ACCESS_UNORDERED_ACCESS); }
    if (VkD3D::Has(access, VkD3D::Access::UniformRead)) { add(D3D12_BARRIER_ACCESS_CONSTANT_BUFFER); }
    if (VkD3D::Has(access, VkD3D::Access::IndirectRead)) { add(D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT); }
    if (VkD3D::Has(access, VkD3D::Access::IndexRead)) { add(D3D12_BARRIER_ACCESS_INDEX_BUFFER); }
    if (VkD3D::Has(access, VkD3D::Access::VertexAttributeRead)) { add(D3D12_BARRIER_ACCESS_VERTEX_BUFFER); }
    if (VkD3D::Has(access, VkD3D::Access::TransferRead)) { add(D3D12_BARRIER_ACCESS_COPY_SOURCE); }
    if (VkD3D::Has(access, VkD3D::Access::TransferWrite)) { add(D3D12_BARRIER_ACCESS_COPY_DEST); }
    if (VkD3D::Has(access, VkD3D::Access::ColorRead)) { add(D3D12_BARRIER_ACCESS_RENDER_TARGET); }
    if (VkD3D::Has(access, VkD3D::Access::ColorWrite)) { add(D3D12_BARRIER_ACCESS_RENDER_TARGET); }
    if (VkD3D::Has(access, VkD3D::Access::DepthRead)) { add(D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ); }
    if (VkD3D::Has(access, VkD3D::Access::DepthWrite)) { add(D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE); }
    if (VkD3D::Has(access, VkD3D::Access::AccelerationStructureRead)) {
        add(D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);
    }
    if (VkD3D::Has(access, VkD3D::Access::AccelerationStructureWrite)) {
        add(D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE);
    }
    return static_cast<D3D12_BARRIER_ACCESS>(result);
}

[[nodiscard]] constexpr auto ToD3D12Compare(VkD3D::CompareOp op) noexcept -> D3D12_COMPARISON_FUNC {
    switch (op) {
        case VkD3D::CompareOp::Never: return D3D12_COMPARISON_FUNC_NEVER;
        case VkD3D::CompareOp::Less: return D3D12_COMPARISON_FUNC_LESS;
        case VkD3D::CompareOp::Equal: return D3D12_COMPARISON_FUNC_EQUAL;
        case VkD3D::CompareOp::LessOrEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case VkD3D::CompareOp::Greater: return D3D12_COMPARISON_FUNC_GREATER;
        case VkD3D::CompareOp::NotEqual: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case VkD3D::CompareOp::GreaterOrEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case VkD3D::CompareOp::Always: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    return D3D12_COMPARISON_FUNC_NEVER;
}

} // namespace ZHLN::D3D12
