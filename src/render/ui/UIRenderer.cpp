// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "UIRenderer.hpp"
#include "../RenderInternal.hpp"
#include "../Resources.hpp"
#include "../TextureManager.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Vertex.hpp>
#include <Zahlen/gui/UIData.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ZHLN {

namespace {

enum class UIRendererError : uint8_t {
    SetupFailed ZHLN_ANNOTATION(ZHLN::Description<"UIRenderer pipeline or buffer setup failed"> {}) = 1,
};

constexpr uint32_t kMaxUiVertices = 100'000;

template <typename Formats>
struct UIVariants;

template <VkFormat... Formats>
struct UIVariants<std::integer_sequence<VkFormat, Formats...>> {
    std::tuple<Vk::TypedPipeline<1, false, Vk::AttachmentFormats<VK_FORMAT_UNDEFINED, Formats>>...> pipelines;

    template <VkFormat Format>
    [[nodiscard]] auto Get() noexcept -> auto& {
        return std::get<Vk::TypedPipeline<1, false, Vk::AttachmentFormats<VK_FORMAT_UNDEFINED, Format>>>(pipelines);
    }

    [[nodiscard]] auto Supports(VkFormat format) const noexcept -> bool {
        return std::apply(
            [format](const auto&... pipeline) {
                return ((format == std::remove_cvref_t<decltype(pipeline)>::FormatSet::color_formats[0] && pipeline.Valid()) || ...);
            },
            pipelines
        );
    }
};

} // namespace

struct UIRenderer::Impl {
    TextureManager* textureManager = nullptr;
    Vk::Allocator*  allocator      = nullptr;

    UIVariants<SupportedUITargetFormats> pipelines;
    VkFormat                             fallbackFormat = VK_FORMAT_UNDEFINED;
    Vk::Pipeline                         fallbackPipeline;
    VkPipelineLayout                     layout = VK_NULL_HANDLE;
    Vk::HeapMappingBundle                mappings;

    std::array<Vk::Buffer, Vk::kFramesInFlight>      vbos {};
    std::array<VkDeviceAddress, Vk::kFramesInFlight> vboAddresses {};

    std::array<uint32_t, Vk::kFramesInFlight> arenaOffset {};
    std::array<uint32_t, Vk::kFramesInFlight> arenaFrame {};
    uint32_t                                  frameEpoch = 0;

    ~Impl() {
        if (allocator != nullptr) {
            for (auto& buffer: vbos)
                allocator->DestroyBuffer(buffer);
        }
    }
};

UIRenderer::UIRenderer(): _impl(std::make_unique<Impl>()) {
}

UIRenderer::~UIRenderer() = default;

UIRenderer::UIRenderer(UIRenderer&&) noexcept                    = default;
auto UIRenderer::operator=(UIRenderer&&) noexcept -> UIRenderer& = default;

auto UIRenderer::Init(RenderContext::Impl& ctx) -> std::expected<void, ErrorCode> {
    if (_impl == nullptr) {
        _impl = std::make_unique<UIRenderer::Impl>();
    }
    auto& impl = *_impl;
    DestroyBuffers(ctx.allocator);
    impl.textureManager = &ctx.textureManager;
    impl.allocator      = &ctx.allocator;
    impl.layout         = ctx.emptyPipelineLayout;

    const Vk::ReflectedStageInput reflectInputs[2] = {
        {.shader = Vk::CreateShaderDesc<Shaders::Modules::UiVS>(), .stage = VK_SHADER_STAGE_VERTEX_BIT},
        {.shader = Vk::CreateShaderDesc<Shaders::Modules::UiPS>(), .stage = VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    Vk::ReflectedLayout uiLayout;
    if (!uiLayout.Build(ctx.ctx.Device(), std::span {reflectInputs})) {
        return std::unexpected(UIRendererError::SetupFailed);
    }

    impl.mappings =
        Vk::HeapMappingBuilder(ctx.heapManager).Sampler(0, 0, ctx.globalSamplerSlot).BindlessTextureArray(0, 1, ctx.textureManager.BindlessBaseSlot()).Build();

    auto uiShaders = Vk::ShaderStagesView::Create<Shaders::Modules::UiVS, Shaders::Modules::UiPS>();
    if (!uiShaders) {
        return std::unexpected(uiShaders.error());
    }

    // A typed builder can produce a typed pipeline only from constant formats.
    // Build just the offscreen variants and the active presentation format; an
    // unusual native swapchain format retains a checked runtime fallback.
    impl.pipelines               = {};
    impl.fallbackPipeline        = {};
    impl.fallbackFormat          = VK_FORMAT_UNDEFINED;
    const VkFormat presentFormat = ctx.presenter.GetPresentFormat();
    auto           MakeBuilder   = [&]() {
        Vk::PipelineBuilder builder;
        builder.Shaders(*uiShaders)
            .Layout(impl.layout)
            .Cache(ctx.pipelineCache.Get())
            .HeapMappings(&impl.mappings.info, &impl.mappings.info)
            .AlphaBlend()
            .CullNone();
        return builder;
    };
    auto BuildVariant = [&]<VkFormat Format>(auto& pipeline) -> std::expected<void, ErrorCode> {
        auto result = std::move(MakeBuilder()).ColorFormats<Format>().NoDepth().Build(ctx.ctx.Device());
        if (!result) {
            return std::unexpected(result.error());
        }
        pipeline = std::move(*result);
        return {};
    };

    size_t                         pipelineCount = 0;
    std::expected<void, ErrorCode> built;
    std::apply(
        [&](auto&... pipelines) {
            (
                [&] {
                    using Pipeline            = std::remove_cvref_t<decltype(pipelines)>;
                    constexpr VkFormat Format = Pipeline::FormatSet::color_formats[0];
                    if (built && (Format == presentFormat || Format == VK_FORMAT_R8G8B8A8_UNORM || Format == VK_FORMAT_R16G16B16A16_SFLOAT)) {
                        built = BuildVariant.template operator()<Format>(pipelines);
                        if (built) {
                            ++pipelineCount;
                        }
                    }
                }(),
                ...
            );
        },
        impl.pipelines.pipelines
    );
    if (!built) {
        return std::unexpected(built.error());
    }

    if (!impl.pipelines.Supports(presentFormat)) {
        auto fallback = std::move(MakeBuilder()).ColorFormats(std::array {presentFormat}).NoDepth().Build(ctx.ctx.Device());
        if (!fallback) {
            return std::unexpected(fallback.error());
        }
        impl.fallbackPipeline = fallback->Release();
        impl.fallbackFormat   = presentFormat;
        ++pipelineCount;
    }

    ZHLN::defer  rollback([&] { DestroyBuffers(ctx.allocator); });
    const size_t bufferSize = static_cast<size_t>(kMaxUiVertices) * (sizeof(VertexPosition) + sizeof(VertexSurface));
    for (uint32_t i = 0; i < Vk::kFramesInFlight; ++i) {
        auto res =
            Vk::Buffer::Create(ctx.allocator, bufferSize, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::CPUToGPU);
        if (!res) {
            return std::unexpected(res.error());
        }
        impl.vbos[i]         = std::move(*res);
        impl.vboAddresses[i] = ctx.ctx.BufferAddress(impl.vbos[i].Handle());
    }
    rollback.Dismiss();
    ZHLN::Log("UIRenderer: {} format-matched pipelines + double-buffered VBOs ({} bytes).", pipelineCount, bufferSize);
    return {};
}

void UIRenderer::DestroyBuffers(Vk::Allocator& allocator) noexcept {
    if (_impl != nullptr) {
        for (auto& buffer: _impl->vbos) {
            allocator.DestroyBuffer(buffer);
        }
    }
}

void UIRenderer::BeginFrame() noexcept {
    if (_impl != nullptr) {
        ++_impl->frameEpoch;
    }
}

auto UIRenderer::SupportsFormat(VkFormat colorFormat) const noexcept -> bool {
    return _impl != nullptr && (_impl->pipelines.Supports(colorFormat) || (_impl->fallbackFormat == colorFormat && _impl->fallbackPipeline.Valid()));
}

template <typename DrawBatch>
void UIRenderer::RecordBatches(
    Vk::CommandEncoder& encoder,
    uint32_t            width,
    uint32_t            height,
    uint32_t            frameIndex,
    const UIDrawData&   uiData,
    DrawBatch&&         drawBatch
) noexcept {
    if (_impl == nullptr || uiData.Empty() || width == 0 || height == 0) {
        return;
    }
    auto& impl = *_impl;

    const uint32_t slot        = Vk::FrameSlot(frameIndex);
    auto&          vbo         = impl.vbos[slot];
    const size_t   maxVertices = vbo.Size() / (sizeof(VertexPosition) + sizeof(VertexSurface));

    if (impl.arenaFrame[slot] != impl.frameEpoch) {
        impl.arenaFrame[slot]  = impl.frameEpoch;
        impl.arenaOffset[slot] = 0;
    }
    const uint32_t vertexOffset = impl.arenaOffset[slot];
    const uint32_t room         = vertexOffset < maxVertices ? static_cast<uint32_t>(maxVertices) - vertexOffset : 0u;
    const uint32_t safeCount    = std::min({static_cast<uint32_t>(uiData.positions.size()), static_cast<uint32_t>(uiData.surfaces.size()), room});
    if (safeCount == 0) {
        return;
    }

    auto mapped = vbo.Map(*impl.allocator);
    if (!mapped)
        return;
    auto* positions = mapped->As<VertexPosition>();
    auto* basePosPtr     = positions + vertexOffset;
    auto* baseSurfacePtr = reinterpret_cast<VertexSurface*>(positions + maxVertices) + vertexOffset;
    std::memcpy(basePosPtr, uiData.positions.data(), safeCount * sizeof(VertexPosition));
    std::memcpy(baseSurfacePtr, uiData.surfaces.data(), safeCount * sizeof(VertexSurface));

    impl.arenaOffset[slot] = vertexOffset + safeCount;

    RenderContext::Impl::UIObjectConstants uipc {};
    uipc.orthoMatrix = Math::CreateOrthoMatrix(static_cast<float>(width), static_cast<float>(height));

    const VkRect2D defaultScissor = {
        .offset = {.x = 0, .y = 0},
        .extent = {.width = width, .height = height},
    };
    const VkDeviceAddress baseVboAddress = impl.vboAddresses[slot];

    for (const auto& batch: uiData.batches) {
        if (batch.vertexStart >= safeCount)
            continue;
        const uint32_t batchCount = std::min(batch.vertexCount, safeCount - batch.vertexStart);
        if (batchCount == 0)
            continue;

        uint32_t albedo = batch.bindlessTextureIndex;
        if (albedo == 0 && impl.textureManager != nullptr) {
            albedo = impl.textureManager->GetBindlessIndex(batch.texture);
        }
        uipc.albedoIdx       = albedo;
        uipc.isSDF           = batch.isSDF ? 1u : 0u;
        uipc.useTextureColor = batch.useTextureColor ? 1u : 0u;
        uipc.posAddress      = baseVboAddress + (vertexOffset + batch.vertexStart) * sizeof(VertexPosition);
        uipc.surfaceAddress  = baseVboAddress + (maxVertices * sizeof(VertexPosition)) + (vertexOffset + batch.vertexStart) * sizeof(VertexSurface);

        Vk::ScopedScissor scissorGuard(
            encoder.cmd, {.target   = batch.useScissor ?
                                          VkRect2D {
                                              .offset = {.x = batch.scissorRect.x, .y = batch.scissorRect.y},
                                              .extent = {.width = batch.scissorRect.width, .height = batch.scissorRect.height},
                                        } :
                                          defaultScissor,
                          .fallback = defaultScissor}
        );

        drawBatch(encoder, batchCount, uipc);
    }
}

template <VkFormat Format>
void UIRenderer::Record(
    const UIColorPass<Format>& pass,
    Vk::CommandEncoder&        encoder,
    uint32_t                   width,
    uint32_t                   height,
    uint32_t                   frameIndex,
    const UIDrawData&          uiData
) noexcept {
    if (_impl == nullptr || !_impl->pipelines.Get<Format>().Valid() || uiData.Empty() || width == 0 || height == 0) {
        return;
    }
    // DrawHeap only pushes constants and draws; it cannot rebind an unchecked
    // VkPipeline over the format-checked binding supplied by DynamicPass.
    pass.Bind(encoder.cmd, _impl->pipelines.Get<Format>());
    RecordBatches(encoder, width, height, frameIndex, uiData, [](Vk::CommandEncoder& cmd, uint32_t count, const auto& constants) {
        cmd.DrawHeap<Shaders::Modules::UiVS, Shaders::Modules::UiPS>(count, 1, constants);
    });
}

void UIRenderer::RecordFallback(
    Vk::CommandEncoder& encoder,
    uint32_t            width,
    uint32_t            height,
    uint32_t            frameIndex,
    VkFormat            colorFormat,
    const UIDrawData&   uiData
) noexcept {
    if (_impl == nullptr || _impl->fallbackFormat != colorFormat || !_impl->fallbackPipeline.Valid()) {
        return;
    }
    const VkPipeline       pipeline = _impl->fallbackPipeline.Get();
    const VkPipelineLayout layout   = _impl->layout;
    RecordBatches(encoder, width, height, frameIndex, uiData, [pipeline, layout](Vk::CommandEncoder& cmd, uint32_t count, const auto& constants) {
        cmd.DrawInstanced<Shaders::Modules::UiVS, Shaders::Modules::UiPS>(
            {.pipeline = pipeline, .layout = layout, .heap = true, .vertexCount = count, .instanceCount = 1}, constants,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
        );
    });
}

template void UIRenderer::Record<
    VK_FORMAT_B8G8R8A8_SRGB>(const UIColorPass<VK_FORMAT_B8G8R8A8_SRGB>&, Vk::CommandEncoder&, uint32_t, uint32_t, uint32_t, const UIDrawData&) noexcept;
template void UIRenderer::Record<
    VK_FORMAT_B8G8R8A8_UNORM>(const UIColorPass<VK_FORMAT_B8G8R8A8_UNORM>&, Vk::CommandEncoder&, uint32_t, uint32_t, uint32_t, const UIDrawData&) noexcept;
template void UIRenderer::Record<
    VK_FORMAT_R8G8B8A8_SRGB>(const UIColorPass<VK_FORMAT_R8G8B8A8_SRGB>&, Vk::CommandEncoder&, uint32_t, uint32_t, uint32_t, const UIDrawData&) noexcept;
template void UIRenderer::Record<
    VK_FORMAT_R8G8B8A8_UNORM>(const UIColorPass<VK_FORMAT_R8G8B8A8_UNORM>&, Vk::CommandEncoder&, uint32_t, uint32_t, uint32_t, const UIDrawData&) noexcept;
template void UIRenderer::Record<VK_FORMAT_R16G16B16A16_SFLOAT>(
    const UIColorPass<VK_FORMAT_R16G16B16A16_SFLOAT>&,
    Vk::CommandEncoder&,
    uint32_t,
    uint32_t,
    uint32_t,
    const UIDrawData&
) noexcept;

} // namespace ZHLN
