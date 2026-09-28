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
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Vertex.hpp>
#include <Zahlen/gui/UIData.hpp>
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

}

struct UIRenderer::Impl {
    TextureManager* textureManager = nullptr;

    struct FormatPipeline {
        VkFormat     format = VK_FORMAT_UNDEFINED;
        Vk::Pipeline pipeline;
    };
    // The window and render textures can have different attachment formats.
    std::array<FormatPipeline, 3> pipelines {};
    VkPipelineLayout              layout = VK_NULL_HANDLE;
    Vk::HeapMappingBundle         mappings;

    [[nodiscard]] auto PipelineFor(VkFormat colorFormat) const noexcept -> VkPipeline {
        for (const auto& variant: pipelines) {
            if (variant.format == colorFormat) {
                return variant.pipeline.Get();
            }
        }
        return VK_NULL_HANDLE;
    }

    std::array<Vk::Buffer, 2>      vbos {};
    std::array<VkDeviceAddress, 2> vboAddresses {};

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

    impl.mappings = Vk::HeapMappingBuilder(ctx.heapManager)
        .Sampler(0, 0, ctx.globalSamplerSlot)
        .BindlessTextureArray(0, 1, ctx.textureManager.BindlessBaseSlot())
        .Build();

    Vk::ShaderStages uiShaders;
    auto             stagesRes = Vk::ShaderStages::Create<Shaders::Modules::UiVS, Shaders::Modules::UiPS>(ctx.ctx.Device());
    if (!stagesRes) {
        return std::unexpected(stagesRes.error());
    }
    uiShaders = std::move(*stagesRes);

    // Dynamic rendering requires each pipeline's color format to match the
    // attachment view exactly. The present target can be sRGB/BGRA, while
    // CreateRenderTexture uses RGBA8 UNORM or RGBA16F. Build one variant per
    // distinct format (the present format may already be RGBA8 UNORM).
    impl.pipelines = {};
    size_t pipelineCount = 0;
    const std::array formats {ctx.presenter.GetPresentFormat(), VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
    for (VkFormat format: formats) {
        if (impl.PipelineFor(format) != VK_NULL_HANDLE) {
            continue;
        }
        auto pipeRes = Vk::PipelineBuilder {}
                           .Shaders(uiShaders)
                           .Layout(impl.layout)
                           .Cache(ctx.pipelineCache.Get())
                           .HeapMappings(&impl.mappings.info, &impl.mappings.info)
                           .ColorFormats(std::array {format})
                           .NoDepth()
                           .AlphaBlend()
                           .CullNone()
                           .Build(ctx.ctx.Device());
        if (!pipeRes) {
            return std::unexpected(pipeRes.error());
        }
        impl.pipelines[pipelineCount].format   = format;
        impl.pipelines[pipelineCount].pipeline = std::move(*pipeRes);
        ++pipelineCount;
    }

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
    ZHLN::Log("UIRenderer: {} format-matched pipelines + double-buffered VBOs ({} bytes).", pipelineCount, bufferSize);
    return {};
}

void UIRenderer::BeginFrame() noexcept {
    if (_impl != nullptr) {
        ++_impl->frameEpoch;
    }
}

auto UIRenderer::SupportsFormat(VkFormat colorFormat) const noexcept -> bool {
    return _impl != nullptr && _impl->PipelineFor(colorFormat) != VK_NULL_HANDLE;
}

void UIRenderer::Record(Vk::CommandEncoder& encoder, uint32_t width, uint32_t height, uint32_t frameIndex, VkFormat colorFormat, const UIDrawData& uiData) noexcept {
    if (_impl == nullptr || uiData.Empty()) {
        return;
    }
    auto& impl = *_impl;
    const VkPipeline pipeline = impl.PipelineFor(colorFormat);
    if (pipeline == VK_NULL_HANDLE || width == 0 || height == 0) {
        return;
    }

    const uint32_t slot        = frameIndex & 1u;
    auto&          vbo         = impl.vbos[slot];
    const size_t   maxVertices = vbo.Size() / (sizeof(VertexPosition) + sizeof(VertexAttributes));

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

    auto  mapped      = vbo.Map();
    auto* positions   = static_cast<VertexPosition*>(mapped.data);
    auto* basePosPtr  = positions + vertexOffset;
    auto* baseAttrPtr = reinterpret_cast<VertexAttributes*>(positions + maxVertices) + vertexOffset;
    std::memcpy(basePosPtr, uiData.positions.data(), safeCount * sizeof(VertexPosition));
    std::memcpy(
        baseAttrPtr, uiData.attributes.data(),
        std::min(safeCount, static_cast<uint32_t>(uiData.attributes.size())) * sizeof(VertexAttributes)
    );

    impl.arenaOffset[slot] = vertexOffset + safeCount;

    RenderContext::Impl::UIObjectConstants uipc {};
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

        encoder.DrawInstanced<Shaders::Modules::UiVS, Shaders::Modules::UiPS>(
            {.pipeline      = pipeline,
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

}
