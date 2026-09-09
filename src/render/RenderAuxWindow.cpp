// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/RenderAuxWindow.cpp
// Extra OS windows are owned by Engine. This file owns only the GPU present
// targets for those windows: a second VkSwapchainKHR on the live device,
// never a second Engine. PresentUI uses the same DrawFrame path as EndFrame.

#include "RenderInternal.hpp"
#include "Resources.hpp"
#include <Zahlen/Math3D.hpp>
#include <algorithm>
#include <cstring>

namespace ZHLN {

namespace {

[[nodiscard]] auto BuildUiPipeline(RenderContext::Impl& impl, VkFormat colorFormat) -> std::expected<Vk::Pipeline, Error> {
    using enum Resource::ShaderID;
    return Vk::ShaderStages::Create(impl.ctx.Device(), Resource::GetShaderProgram(Ui))
        .and_then([&](auto&& shaders) -> std::expected<Vk::Pipeline, Error> {
            return Vk::PipelineBuilder {}
                .Shaders(shaders)
                .Layout(impl.emptyPipelineLayout)
                .HeapMappings(&impl.sceneHeapMappings.info, &impl.sceneHeapMappings.info)
                .ColorFormats(std::array {colorFormat})
                .NoDepth()
                .AlphaBlend()
                .CullNone()
                .Build(impl.ctx.Device());
        });
}

[[nodiscard]] auto FindExtra(RenderContext::Impl& impl, const Window& aux) noexcept -> RenderContext::Impl::ExtraPresentation* {
    for (auto& extra: impl.extraPresentations) {
        if (extra.window == &aux) {
            return &extra;
        }
    }
    return nullptr;
}

[[nodiscard]] auto FindExtra(const RenderContext::Impl& impl, const Window& aux) noexcept -> const RenderContext::Impl::ExtraPresentation* {
    for (const auto& extra: impl.extraPresentations) {
        if (extra.window == &aux) {
            return &extra;
        }
    }
    return nullptr;
}

void IdleDevice(RenderContext::Impl& impl) noexcept {
    if (impl.ctx.Device() == VK_NULL_HANDLE) {
        return;
    }
    (void) Vk::WaitIdle(impl.ctx.Device());
}

[[nodiscard]] auto MapFrameResult(ZHLN_FrameResult res) noexcept -> RenderFrameResult {
    using enum RenderFrameResult;
    switch (res) {
        case ZHLN_FrameResult_Ok:
            return Success;
        case ZHLN_FrameResult_Suboptimal:
            return Suboptimal;
        case ZHLN_FrameResult_OutOfDate:
            return OutOfDate;
        case ZHLN_FrameResult_DeviceLost:
            return DeviceLost;
        default:
            return Error;
    }
}

[[nodiscard]] auto EnsureAuxPipeline(RenderContext::Impl& impl, RenderContext::Impl::ExtraPresentation& extra) -> std::expected<void, Error> {
    const VkFormat auxFormat  = extra.presentation.GetPresentFormat();
    const VkFormat mainFormat = impl.presentation.GetPresentFormat();
    extra.uiPipeline          = {};
    if (auxFormat == mainFormat) {
        return {};
    }
    return BuildUiPipeline(impl, auxFormat).and_then([&](Vk::Pipeline&& pipeline) -> std::expected<void, Error> {
        extra.uiPipeline = std::move(pipeline);
        return {};
    });
}

} // namespace

void RenderContext::Impl::DestroyPresentations() noexcept {
    if (extraPresentations.empty()) {
        return;
    }
    IdleDevice(*this);
    extraPresentations.clear();
}

void RenderContext::Impl::RemovePresentation(Window& aux) noexcept {
    const auto it = std::find_if(extraPresentations.begin(), extraPresentations.end(), [&](const ExtraPresentation& extra) {
        return extra.window == &aux;
    });
    if (it == extraPresentations.end()) {
        return;
    }
    IdleDevice(*this);
    extraPresentations.erase(it);
}

auto RenderContext::Impl::HasPresentation(const Window& aux) const noexcept -> bool {
    const ExtraPresentation* extra = FindExtra(*this, aux);
    return extra != nullptr && extra->presentation.swapchain.Valid();
}

auto RenderContext::Impl::AddPresentation(Window& aux) noexcept -> std::expected<void, Error> {
    using Vk::PresentationError;
    using Vk::SurfaceCreationError;

    if (presentationMode != PresentationMode::NativeSwapchain) {
        return std::unexpected(PresentationError::NativeSwapchainRequired);
    }
    if (ctx.Device() == VK_NULL_HANDLE) {
        return std::unexpected(PresentationError::ContextInvalid);
    }
    if (&aux == &window) {
        return std::unexpected(PresentationError::PrimaryWindowAlreadyPresented);
    }
    if (HasPresentation(aux)) {
        return {};
    }
    RemovePresentation(aux);

    int width  = 0;
    int height = 0;
    auto surfaceRes = aux.CreateVulkanSurface(ctx.Instance(), ctx.Physical(), width, height);
    if (!surfaceRes) {
        return std::unexpected(surfaceRes.error());
    }

    ExtraPresentation extra;
    extra.surface = Vk::Surface(ctx.Instance(), static_cast<VkSurfaceKHR>(*surfaceRes));
    if (extra.surface.Get() == VK_NULL_HANDLE || width <= 0 || height <= 0) {
        return std::unexpected(SurfaceCreationError::WindowSurfaceCreationFailed);
    }

    if (auto initRes = extra.presentation.Init(ctx, allocator, extra.surface.Get(), static_cast<uint32_t>(width), static_cast<uint32_t>(height), true);
        !initRes) {
        return std::unexpected(initRes.error());
    }

    const size_t vboBytes = static_cast<size_t>(kMaxUiVertices) * (sizeof(VertexPosition) + sizeof(VertexAttributes));
    auto         vboRes   = Vk::Buffer::Create(
        allocator.Get(), vboBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU
    );
    if (!vboRes) {
        return std::unexpected(vboRes.error());
    }
    extra.uiVbo        = std::move(*vboRes);
    extra.uiVboAddress = ctx.BufferAddress(extra.uiVbo.Handle());

    extra.sync = Vk::FrameSync<2>::Create(ctx.Device());
    extra.pools = Vk::CommandPools<2, Vk::QueueType::Graphics>::Create(
        ctx.Device(), {.queueFamily = ctx.PhysicalInfo().graphics_family, .buffersPerPool = 1}
    );
    extra.frameIndex = 0;
    if (!extra.sync.Valid() || !extra.pools.Valid()) {
        return std::unexpected(PresentationError::SyncCreationFailed);
    }

    if (auto pipeRes = EnsureAuxPipeline(*this, extra); !pipeRes) {
        return std::unexpected(pipeRes.error());
    }

    extra.window = &aux;
    extraPresentations.push_back(std::move(extra));
    return {};
}

auto RenderContext::Impl::PresentUI(Window& aux) noexcept -> std::expected<void, Error> {
    using Vk::PresentationError;

    struct UiQueueGuard {
        RenderQueues& queues;
        ~UiQueueGuard() noexcept {
            queues.uiBatches.clear();
        }
    } uiGuard {queues};

    ExtraPresentation* extra = FindExtra(*this, aux);
    if (extra == nullptr || !extra->presentation.swapchain.Valid()) {
        return std::unexpected(PresentationError::WindowNotPresented);
    }
    if (!aux.IsRunning()) {
        RemovePresentation(aux);
        return std::unexpected(PresentationError::WindowNotPresented);
    }

    const Extent2D size = aux.GetSize();
    if (size.width == 0 || size.height == 0) {
        return {};
    }

    const VkExtent2D scExtent = extra->presentation.swapchain.Get().extent;
    if (size.width != scExtent.width || size.height != scExtent.height) {
        if (auto rebuilt = extra->presentation.Rebuild(size.width, size.height); !rebuilt) {
            return std::unexpected(rebuilt.error());
        }
        if (auto pipeRes = EnsureAuxPipeline(*this, *extra); !pipeRes) {
            return std::unexpected(pipeRes.error());
        }
    }

    std::expected<void, Error> rebuildRes {};
    const ZHLN_FrameResult     frameRes = Vk::DrawFrame<2>(
        {.ctx               = ctx,
         .swapchain         = extra->presentation.swapchain,
         .sync              = extra->sync,
         .pools             = extra->pools,
         .presentSemaphores = extra->presentation.presentSemaphores},
        extra->frameIndex,
        [&](VkCommandBuffer cmd, uint32_t imageIndex) {
            const auto& swap = extra->presentation.swapchain.Get();
            const size_t maxVertices = extra->uiVbo.Size() / (sizeof(VertexPosition) + sizeof(VertexAttributes));
            auto&        srcVbo      = frames.uiVbos[frame_index];
            const size_t srcMax      = srcVbo.Size() / (sizeof(VertexPosition) + sizeof(VertexAttributes));
            const size_t copyVerts   = std::min(maxVertices, srcMax);
            if (extra->uiVbo.Valid() && srcVbo.Valid() && copyVerts > 0 && !queues.uiBatches.empty()) {
                auto srcMap = srcVbo.Map();
                auto dstMap = extra->uiVbo.Map();
                if (srcMap.data != nullptr && dstMap.data != nullptr) {
                    std::memcpy(dstMap.data, srcMap.data, copyVerts * sizeof(VertexPosition));
                    std::memcpy(
                        static_cast<char*>(dstMap.data) + maxVertices * sizeof(VertexPosition),
                        static_cast<char*>(srcMap.data) + srcMax * sizeof(VertexPosition),
                        copyVerts * sizeof(VertexAttributes)
                    );
                }
            }

            Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> target {
                .handle = swap.images[imageIndex],
                .view   = swap.views[imageIndex],
                .extent = {.width = swap.extent.width, .height = swap.extent.height, .depth = 1},
                .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                .format = swap.format,
            };
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, target.handle);

            Vk::CommandEncoder encoder(cmd, &ctx);
            Vk::DynamicPass(swap.extent)
                .AddColor(target, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, kClearColorScene)
                .Execute(cmd, [&]() {
                    if (queues.uiBatches.empty() || !uiPipeline.Valid()) {
                        return;
                    }
                    BindHeapsAndPushFrame(cmd);

                    UIObjectConstants uipc {};
                    uipc.orthoMatrix = Math::CreateOrthoMatrix(swap.extent.width, swap.extent.height);
                    const VkRect2D defaultScissor {
                        .offset = {.x = 0, .y = 0},
                        .extent = {.width = swap.extent.width, .height = swap.extent.height},
                    };
                    const VkPipeline pipeline = extra->uiPipeline.Valid() ? extra->uiPipeline.Get() : uiPipeline.Get();

                    for (const auto& batch: queues.uiBatches) {
                        uipc.albedoIdx       = batch.bindlessTextureIndex != 0 ? batch.bindlessTextureIndex : textureManager.GetBindlessIndex(batch.texture);
                        uipc.isSDF           = batch.isSDF ? 1u : 0u;
                        uipc.useTextureColor = batch.useTextureColor ? 1u : 0u;
                        uipc.posAddress      = extra->uiVboAddress + (batch.vertexStart * sizeof(VertexPosition));
                        uipc.attrAddress =
                            extra->uiVboAddress + (maxVertices * sizeof(VertexPosition)) + (batch.vertexStart * sizeof(VertexAttributes));

                        Vk::ScopedScissor scissorGuard(
                            cmd, {.target   = batch.useScissor ?
                                                  VkRect2D {
                                                      .offset = {.x = batch.scissorRect.x, .y = batch.scissorRect.y},
                                                      .extent = {.width = batch.scissorRect.width, .height = batch.scissorRect.height}
                                                  } :
                                                  defaultScissor,
                                  .fallback = defaultScissor}
                        );

                        encoder.DrawInstanced(
                            {.pipeline      = pipeline,
                             .layout        = uiPipelineLayout,
                             .heap          = true,
                             .vertexCount   = batch.vertexCount,
                             .instanceCount = 1,
                             .firstVertex   = 0,
                             .firstInstance = 0},
                            uipc, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                        );
                    }
                });

            Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(cmd, target.handle);
        },
        [&]() {
            rebuildRes = extra->presentation.Rebuild(size.width, size.height);
            if (rebuildRes) {
                rebuildRes = EnsureAuxPipeline(*this, *extra);
            }
        }
    );

    if (!rebuildRes) {
        return std::unexpected(rebuildRes.error());
    }
    if (frameRes != ZHLN_FrameResult_Ok && frameRes != ZHLN_FrameResult_Suboptimal) {
        return std::unexpected(MapFrameResult(frameRes));
    }
    if (frameRes == ZHLN_FrameResult_Suboptimal) {
        return std::unexpected(Suboptimal);
    }
    return {};
}

auto RenderContext::AddPresentation(Window& window) noexcept -> RenderResult {
    return _impl->AddPresentation(window);
}

void RenderContext::RemovePresentation(Window& window) noexcept {
    _impl->RemovePresentation(window);
}

auto RenderContext::HasPresentation(const Window& window) const noexcept -> bool {
    return _impl->HasPresentation(window);
}

auto RenderContext::PresentUI(Window& window) noexcept -> RenderResult {
    return _impl->PresentUI(window);
}

} // namespace ZHLN
