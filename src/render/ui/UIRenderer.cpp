// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "UIRendererAccess.hpp"
#include "../Resources.hpp"
#include "../TextureManager.hpp"
#include <array>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/Math3D.hpp>
#include <algorithm>
#include <span>
#include <cstring>
#include <memory>
#include <utility>

namespace ZHLN {

namespace {

enum class UIRendererError : uint8_t {
    SetupFailed ZHLN_ANNOTATION(ZHLN::Description<"UIRenderer pipeline or buffer setup failed">{}) = 1,
};

constexpr uint32_t kMaxUiVertices = 100'000;

} // namespace

struct UIRenderer::Impl {
    TextureManager* textureManager = nullptr;

    Vk::Pipeline     pipeline;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    RenderContext::Impl::HeapMappingSet mappings;

    std::array<Vk::Buffer, 2>      vbos {};
    std::array<VkDeviceAddress, 2> vboAddresses {};

    ZHLN::Array<UIBatch>          batches;
    ZHLN::Array<VertexPosition>   cpuPositions;
    ZHLN::Array<VertexAttributes> cpuAttributes;
};

UIRenderer::UIRenderer(): _impl(std::make_unique<Impl>()) {}

UIRenderer::~UIRenderer() = default;

UIRenderer::UIRenderer(UIRenderer&&) noexcept                    = default;
auto UIRenderer::operator=(UIRenderer&&) noexcept -> UIRenderer& = default;

void UIRenderer::SubmitUI(
    const UIBatch*          batches,
    uint32_t                batchCount,
    const VertexPosition*   positions,
    const VertexAttributes* attributes,
    uint32_t                vertexCount
) noexcept {
    if (_impl == nullptr || batchCount == 0 || vertexCount == 0 || positions == nullptr || attributes == nullptr) {
        return;
    }
    _impl->cpuPositions.assign(positions, positions + vertexCount);
    _impl->cpuAttributes.assign(attributes, attributes + vertexCount);
    _impl->batches.clear();
    _impl->batches.reserve(batchCount);
    for (uint32_t i = 0; i < batchCount; ++i) {
        _impl->batches.push_back(batches[i]);
    }
}

void UIRenderer::Clear() noexcept {
    if (_impl == nullptr) {
        return;
    }
    _impl->batches.clear();
    _impl->cpuPositions.clear();
    _impl->cpuAttributes.clear();
}

auto UIRenderer::Empty() const noexcept -> bool {
    return _impl == nullptr || _impl->batches.empty();
}

auto UIRendererAccess::Init(UIRenderer& ui, RenderContext::Impl& ctx) -> std::expected<void, Error> {
    if (ui._impl == nullptr) {
        ui._impl = std::make_unique<UIRenderer::Impl>();
    }
    auto& impl      = *ui._impl;
    impl.textureManager   = &ctx.textureManager;
    impl.layout     = ctx.emptyPipelineLayout;

    using enum Resource::ShaderID;
    const auto program = Resource::GetShaderProgram(Ui);

    const Vk::ReflectedStageInput reflectInputs[2] = {
        {.shader = Vk::CreateShaderDesc(program.vertex), .stage = VK_SHADER_STAGE_VERTEX_BIT},
        {.shader = Vk::CreateShaderDesc(program.fragment), .stage = VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    Vk::SlangReflectedLayout uiLayout;
    if (!uiLayout.Build(ctx.ctx.Device(), std::span {reflectInputs})) {
        return std::unexpected(UIRendererError::SetupFailed);
    }

    impl.mappings.entries.clear();
    if (!uiLayout.reflectedSets[0].bindings.empty()) {
        for (const auto& b: uiLayout.reflectedSets[0].bindings) {
            VkDescriptorSetAndBindingMappingEXT entry = {
                .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
                .pNext         = nullptr,
                .descriptorSet = 0,
                .firstBinding  = b.binding,
                .bindingCount  = 1,
                .resourceMask  = 0,
                .source        = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT,
                .sourceData    = {},
            };
            if (b.binding == 0) {
                entry.resourceMask                         = VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT;
                entry.sourceData.constantOffset.heapOffset = static_cast<uint32_t>(ctx.heapManager.SamplerOffset(ctx.globalSamplerSlot.index));
            } else if (b.binding == 1) {
                entry.resourceMask                              = VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT;
                entry.sourceData.constantOffset.heapOffset      = static_cast<uint32_t>(ctx.heapManager.ResourceOffset(ctx.textureHeapBase));
                entry.sourceData.constantOffset.heapArrayStride = static_cast<uint32_t>(ctx.heapManager.ResourceStride());
            } else {
                continue;
            }
            impl.mappings.entries.push_back(entry);
        }
        impl.mappings.Finalize();
    }

    Vk::ShaderStages uiShaders;
    auto             stagesRes = Vk::ShaderStages::Create(ctx.ctx.Device(), program);
    if (!stagesRes) {
        return std::unexpected(stagesRes.error());
    }
    uiShaders = std::move(*stagesRes);

    const VkFormat swapchainFormat = ctx.session.presentation.GetPresentFormat();
    auto           pipeRes         =
        Vk::PipelineBuilder {}
            .Shaders(uiShaders)
            .Layout(impl.layout)
            .HeapMappings(&impl.mappings.info, &impl.mappings.info)
            .ColorFormats(std::array {swapchainFormat})
            .NoDepth()
            .AlphaBlend()
            .CullNone()
            .Build(ctx.ctx.Device());
    if (!pipeRes) {
        return std::unexpected(pipeRes.error());
    }
    impl.pipeline = std::move(*pipeRes);

    const size_t bufferSize = static_cast<size_t>(kMaxUiVertices) * (sizeof(VertexPosition) + sizeof(VertexAttributes));
    for (int i = 0; i < 2; ++i) {
        auto res = Vk::Buffer::Create(
            ctx.allocator.Get(), bufferSize, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::CPUToGPU
        );
        if (!res) {
            return std::unexpected(res.error());
        }
        impl.vbos[i]          = std::move(*res);
        impl.vboAddresses[i]  = ctx.ctx.BufferAddress(impl.vbos[i].Handle());
    }
    ZHLN::Log("UIRenderer: pipeline + double-buffered VBOs ({} bytes).", bufferSize);
    return {};
}

void UIRendererAccess::Record(
    UIRenderer& ui, Vk::CommandEncoder& encoder, uint32_t width, uint32_t height, uint32_t frameIndex
) noexcept {
    if (ui._impl == nullptr || ui._impl->batches.empty() || !ui._impl->pipeline.Valid()) {
        return;
    }
    auto& impl = *ui._impl;
    if (width == 0 || height == 0) {
        return;
    }

    const uint32_t slot        = frameIndex & 1u;
    auto&          vbo         = impl.vbos[slot];
    const size_t   maxVertices = vbo.Size() / (sizeof(VertexPosition) + sizeof(VertexAttributes));
    const uint32_t safeCount   = std::min(static_cast<uint32_t>(impl.cpuPositions.size()), static_cast<uint32_t>(maxVertices));
    if (safeCount == 0) {
        return;
    }

    auto  mapped     = vbo.Map();
    auto* basePosPtr = static_cast<VertexPosition*>(mapped.data);
    auto* baseAttrPtr = reinterpret_cast<VertexAttributes*>(basePosPtr + maxVertices);
    std::memcpy(basePosPtr, impl.cpuPositions.data(), safeCount * sizeof(VertexPosition));
    std::memcpy(baseAttrPtr, impl.cpuAttributes.data(), std::min(safeCount, static_cast<uint32_t>(impl.cpuAttributes.size())) * sizeof(VertexAttributes));

    UIObjectConstants uipc {};
    uipc.orthoMatrix = Math::CreateOrthoMatrix(static_cast<float>(width), static_cast<float>(height));

    const VkRect2D defaultScissor = {
        .offset = {.x = 0, .y = 0},
        .extent = {.width = width, .height = height},
    };
    const VkDeviceAddress baseVboAddress = impl.vboAddresses[slot];

    for (const auto& batch: impl.batches) {
        uint32_t albedo = batch.bindlessTextureIndex;
        if (albedo == 0 && impl.textureManager != nullptr) {
            albedo = impl.textureManager->GetBindlessIndex(batch.texture);
        }
        uipc.albedoIdx       = albedo;
        uipc.isSDF           = batch.isSDF ? 1u : 0u;
        uipc.useTextureColor = batch.useTextureColor ? 1u : 0u;
        uipc.posAddress      = baseVboAddress + (batch.vertexStart * sizeof(VertexPosition));
        uipc.attrAddress     = baseVboAddress + (maxVertices * sizeof(VertexPosition)) + (batch.vertexStart * sizeof(VertexAttributes));

        Vk::ScopedScissor scissorGuard(
            encoder.cmd,
            {.target   = batch.useScissor ? VkRect2D {
                                               .offset = {.x = batch.scissorRect.x, .y = batch.scissorRect.y},
                                               .extent = {.width = batch.scissorRect.width, .height = batch.scissorRect.height},
                                           } :
                                           defaultScissor,
             .fallback = defaultScissor}
        );

        encoder.DrawInstanced(
            {.pipeline      = impl.pipeline.Get(),
             .layout        = impl.layout,
             .heap          = true,
             .vertexCount   = batch.vertexCount,
             .instanceCount = 1,
             .firstVertex   = 0,
             .firstInstance = 0},
            uipc, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
        );
    }
}

} // namespace ZHLN
