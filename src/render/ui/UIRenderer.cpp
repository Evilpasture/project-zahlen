// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "UIRenderer.hpp"
#include "../RenderInternal.hpp"
#include "../Resources.hpp"
#include "../TextureManager.hpp"
#include <ShaderBindings.hpp>
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

    /// Vertices handed out of each slot so far this frame, and the frame each
    /// figure belongs to. The frame is identified by `frameEpoch` -- bumped by
    /// BeginFrame -- and not by `frameIndex`, whose low bit names the slot: a
    /// caller that counts frames and one that reports the slot both pass a
    /// value that only alternates, so neither can tell this frame's second
    /// Record from the next frame's first.
    std::array<uint32_t, 2> arenaOffset {};
    std::array<uint32_t, 2> arenaFrame {};
    uint32_t                frameEpoch = 0;
};

UIRenderer::UIRenderer(): _impl(std::make_unique<Impl>()) {}

UIRenderer::~UIRenderer() = default;

UIRenderer::UIRenderer(UIRenderer&&) noexcept                    = default;
auto UIRenderer::operator=(UIRenderer&&) noexcept -> UIRenderer& = default;

auto UIRenderer::Init(RenderContext::Impl& ctx) -> std::expected<void, ErrorCode> {
    if (_impl == nullptr) {
        _impl = std::make_unique<UIRenderer::Impl>();
    }
    auto& impl      = *_impl;
    impl.textureManager   = &ctx.textureManager;
    impl.layout     = ctx.emptyPipelineLayout;

    const Vk::ReflectedStageInput reflectInputs[2] = {
        {.shader = Vk::CreateShaderDesc<Shaders::Modules::UiVS>(), .stage = VK_SHADER_STAGE_VERTEX_BIT},
        {.shader = Vk::CreateShaderDesc<Shaders::Modules::UiPS>(), .stage = VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    Vk::ReflectedLayout uiLayout;
    if (!uiLayout.Build(ctx.ctx.Device(), std::span {reflectInputs})) {
        return std::unexpected(UIRendererError::SetupFailed);
    }

    impl.mappings.entries.clear();
    if (!uiLayout.sets[0].bindings.empty()) {
        for (const auto& b: uiLayout.sets[0].bindings) {
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
    auto             stagesRes = Vk::ShaderStages::Create<Shaders::Modules::UiVS, Shaders::Modules::UiPS>(ctx.ctx.Device());
    if (!stagesRes) {
        return std::unexpected(stagesRes.error());
    }
    uiShaders = std::move(*stagesRes);

    const VkFormat swapchainFormat = ctx.session.presentation.GetPresentFormat();
    auto           pipeRes         =
        Vk::PipelineBuilder {}
            .Shaders(uiShaders)
            .Layout(impl.layout)
            .Cache(ctx.pipelineCache.Get())
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

void UIRenderer::BeginFrame() noexcept {
    if (_impl != nullptr) {
        ++_impl->frameEpoch;
    }
}

void UIRenderer::Record(Vk::CommandEncoder& encoder, uint32_t width, uint32_t height, uint32_t frameIndex, const UIDrawData& uiData) noexcept {
    if (_impl == nullptr || uiData.Empty() || !_impl->pipeline.Valid()) {
        return;
    }
    auto& impl = *_impl;
    if (width == 0 || height == 0) {
        return;
    }

    const uint32_t slot        = frameIndex & 1u;
    auto&          vbo         = impl.vbos[slot];
    const size_t   maxVertices = vbo.Size() / (sizeof(VertexPosition) + sizeof(VertexAttributes));

    // The first Record of this frame for this slot rewinds it; every later one
    // appends. A frame draws UI into as many windows as the app has, and each
    // of those calls owns its own vertices in the slot.
    if (impl.arenaFrame[slot] != impl.frameEpoch) {
        impl.arenaFrame[slot]  = impl.frameEpoch;
        impl.arenaOffset[slot] = 0;
    }
    const uint32_t vertexOffset = impl.arenaOffset[slot];
    const uint32_t room         = vertexOffset < maxVertices ? static_cast<uint32_t>(maxVertices) - vertexOffset : 0u;
    const uint32_t safeCount    = std::min(static_cast<uint32_t>(uiData.positions.size()), room);
    if (safeCount == 0) {
        return;
    }

    // The payload is immutable for the frame; copy it straight into the mapped
    // VBO slot (positions first, attributes at the second half) so the GPU
    // reads only what this frame's producer built.
    auto  mapped      = vbo.Map();
    auto* positions   = static_cast<VertexPosition*>(mapped.data);
    auto* basePosPtr  = positions + vertexOffset;
    auto* baseAttrPtr = reinterpret_cast<VertexAttributes*>(positions + maxVertices) + vertexOffset;
    std::memcpy(basePosPtr, uiData.positions.data(), safeCount * sizeof(VertexPosition));
    std::memcpy(
        baseAttrPtr, uiData.attributes.data(),
        std::min(safeCount, static_cast<uint32_t>(uiData.attributes.size())) * sizeof(VertexAttributes)
    );

    // The vertices are spoken for now: the next Record this frame appends after
    // them, and the draws below address exactly this range.
    impl.arenaOffset[slot] = vertexOffset + safeCount;

    UIObjectConstants uipc {};
    uipc.orthoMatrix = Math::CreateOrthoMatrix(static_cast<float>(width), static_cast<float>(height));

    const VkRect2D defaultScissor = {
        .offset = {.x = 0, .y = 0},
        .extent = {.width = width, .height = height},
    };
    const VkDeviceAddress baseVboAddress = impl.vboAddresses[slot];

    for (const auto& batch: uiData.batches) {
        uint32_t albedo = batch.bindlessTextureIndex;
        if (albedo == 0 && impl.textureManager != nullptr) {
            albedo = impl.textureManager->GetBindlessIndex(batch.texture);
        }
        uipc.albedoIdx       = albedo;
        uipc.isSDF           = batch.isSDF ? 1u : 0u;
        uipc.useTextureColor = batch.useTextureColor ? 1u : 0u;
        // `firstVertex` stays 0: the shader indexes the pool with SV_VertexID
        // from the address it is handed, so the batch's place in the slot is the
        // address, and passing it twice would double-count it.
        uipc.posAddress      = baseVboAddress + (vertexOffset + batch.vertexStart) * sizeof(VertexPosition);
        uipc.attrAddress     = baseVboAddress + (maxVertices * sizeof(VertexPosition)) + (vertexOffset + batch.vertexStart) * sizeof(VertexAttributes);

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
