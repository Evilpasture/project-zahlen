// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/RenderAuxWindow.cpp
// Extra OS windows are owned by Engine. This file owns only the GPU present
// targets for those windows: a second VkSwapchainKHR on the live device,
// never a second Engine. PresentUI acquires, records, QueueSubmit (wait
// image-available), presents.

#include "RenderInternal.hpp"
#include "Resources.hpp"
#include <Zahlen/Log.hpp>
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
    auto idle = Vk::WaitIdle(impl.ctx.Device());
    if (!idle) {
        ZHLN::Log("[Render] extra presentation: WaitIdle failed ({})", idle.error().Message());
    }
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

auto RenderContext::Impl::AddPresentation(Window& aux) noexcept -> bool {
    if (presentationMode != PresentationMode::NativeSwapchain) {
        ZHLN::Log("[Render] AddPresentation: NativeSwapchain presentation is required");
        return false;
    }
    if (ctx.Device() == VK_NULL_HANDLE) {
        return false;
    }
    if (&aux == &window) {
        ZHLN::Log("[Render] AddPresentation: primary window already has a swapchain");
        return false;
    }
    if (HasPresentation(aux)) {
        return true;
    }
    RemovePresentation(aux);

    int  width  = 0;
    int  height = 0;
    auto surfaceRes = aux.CreateVulkanSurface(ctx.Instance(), ctx.Physical(), width, height);
    if (!surfaceRes) {
        ZHLN::Log("[Render] AddPresentation: surface creation failed ({})", surfaceRes.error().Message());
        return false;
    }
    auto* rawSurface = static_cast<VkSurfaceKHR>(*surfaceRes);
    if (rawSurface == VK_NULL_HANDLE || width <= 0 || height <= 0) {
        ZHLN::Log("[Render] AddPresentation: no presentable surface");
        return false;
    }

    VkBool32       supported  = VK_FALSE;
    const VkResult supportRes =
        vkGetPhysicalDeviceSurfaceSupportKHR(ctx.Physical(), ctx.PhysicalInfo().present_family, rawSurface, &supported);
    if (supportRes != VK_SUCCESS || supported != VK_TRUE) {
        ZHLN::Log("[Render] AddPresentation: present family does not support this surface");
        vkDestroySurfaceKHR(ctx.Instance(), rawSurface, nullptr);
        return false;
    }

    ExtraPresentation extra;
    extra.surface = Vk::Surface(ctx.Instance(), rawSurface);
    auto initRes  = extra.presentation.Init(ctx, allocator, extra.surface.Get(), static_cast<uint32_t>(width), static_cast<uint32_t>(height), true);
    if (!initRes) {
        ZHLN::Log("[Render] AddPresentation: swapchain init failed ({})", initRes.error().Message());
        return false;
    }

    const size_t vboBytes = static_cast<size_t>(kMaxUiVertices) * (sizeof(VertexPosition) + sizeof(VertexAttributes));
    auto         vboRes   = Vk::Buffer::Create(
        allocator.Get(), vboBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU
    );
    if (!vboRes) {
        ZHLN::Log("[Render] AddPresentation: UI VBO allocation failed ({})", vboRes.error().Message());
        return false;
    }
    extra.uiVbo        = std::move(*vboRes);
    extra.uiVboAddress = ctx.BufferAddress(extra.uiVbo.Handle());

    extra.sync = Vk::FrameSync<2>::Create(ctx.Device());
    extra.pools = Vk::CommandPools<2, Vk::QueueType::Graphics>::Create(
        ctx.Device(), {.queueFamily = ctx.PhysicalInfo().graphics_family, .buffersPerPool = 1}
    );
    extra.frameIndex = 0;
    if (!extra.sync.Valid() || !extra.pools.Valid()) {
        ZHLN::Log("[Render] AddPresentation: frame sync / command pool creation failed");
        return false;
    }

    const VkFormat auxFormat  = extra.presentation.GetPresentFormat();
    const VkFormat mainFormat = presentation.GetPresentFormat();
    if (auxFormat != mainFormat) {
        auto pipeRes = BuildUiPipeline(*this, auxFormat);
        if (!pipeRes) {
            ZHLN::Log("[Render] AddPresentation: aux UI pipeline failed ({})", pipeRes.error().Message());
            return false;
        }
        extra.uiPipeline = std::move(*pipeRes);
    }

    extra.window = &aux;
    extraPresentations.push_back(std::move(extra));
    ZHLN::Log("[Render] Extra presentation ({}x{}, format {})", width, height, static_cast<int>(auxFormat));
    return true;
}

void RenderContext::Impl::PresentUI(Window& aux) noexcept {
    ExtraPresentation* extra = FindExtra(*this, aux);
    if (extra == nullptr || !extra->presentation.swapchain.Valid()) {
        queues.uiBatches.clear();
        return;
    }
    if (!aux.IsRunning()) {
        RemovePresentation(aux);
        queues.uiBatches.clear();
        return;
    }

    const Extent2D size = aux.GetSize();
    if (size.width == 0 || size.height == 0) {
        queues.uiBatches.clear();
        return;
    }

    const VkExtent2D scExtent = extra->presentation.swapchain.Get().extent;
    if (size.width != scExtent.width || size.height != scExtent.height) {
        auto rebuilt = extra->presentation.Rebuild(size.width, size.height);
        if (!rebuilt) {
            ZHLN::Log("[Render] PresentUI: rebuild failed ({})", rebuilt.error().Message());
            RemovePresentation(aux);
            queues.uiBatches.clear();
            return;
        }
        const VkFormat auxFormat  = extra->presentation.GetPresentFormat();
        const VkFormat mainFormat = presentation.GetPresentFormat();
        extra->uiPipeline         = {};
        if (auxFormat != mainFormat) {
            auto pipeRes = BuildUiPipeline(*this, auxFormat);
            if (!pipeRes) {
                ZHLN::Log("[Render] PresentUI: aux UI pipeline rebuild failed");
                RemovePresentation(aux);
                queues.uiBatches.clear();
                return;
            }
            extra->uiPipeline = std::move(*pipeRes);
        }
    }

    const auto&           swap  = extra->presentation.swapchain.Get();
    const ZHLN_FrameSync& frame = extra->sync[extra->frameIndex];
    if (extra->sync.Wait(extra->frameIndex) == VK_ERROR_DEVICE_LOST) {
        ZHLN::Log("[Render] PresentUI: device lost waiting for frame");
        queues.uiBatches.clear();
        return;
    }

    uint32_t               imageIndex = 0;
    const ZHLN_AcquireDesc acquireDesc {
        .swapchain       = swap.handle,
        .image_available = frame.image_available,
        .timeout_ns      = UINT64_MAX,
    };
    const ZHLN_FrameResult acquired = ZHLN_AcquireImage(ctx.Device(), &acquireDesc, &imageIndex);
    if (acquired == ZHLN_FrameResult_OutOfDate) {
        auto rebuilt = extra->presentation.Rebuild(size.width, size.height);
        if (!rebuilt) {
            ZHLN::Log("[Render] PresentUI: rebuild after outdated acquire failed ({})", rebuilt.error().Message());
            RemovePresentation(aux);
        }
        queues.uiBatches.clear();
        return;
    }
    if (acquired == ZHLN_FrameResult_DeviceLost) {
        ZHLN::Log("[Render] PresentUI: device lost on acquire");
        queues.uiBatches.clear();
        return;
    }
    if (acquired != ZHLN_FrameResult_Ok && acquired != ZHLN_FrameResult_Suboptimal) {
        queues.uiBatches.clear();
        return;
    }
    if (imageIndex >= swap.image_count) {
        queues.uiBatches.clear();
        return;
    }

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

    extra->sync.ResetFence(extra->frameIndex);
    extra->pools[extra->frameIndex].Reset();
    const auto cmd = extra->pools.Cmd(extra->frameIndex);
    {
        Vk::CommandBufferGuard recordGuard(cmd);
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
    }

    const VkSemaphore renderFinished = extra->presentation.presentSemaphores[imageIndex];

    auto submitRes = Vk::QueueSubmit(
        ctx.GraphicsQueue(), static_cast<VkCommandBuffer>(cmd), frame.image_available, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        renderFinished, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, frame.in_flight
    );
    if (!submitRes) {
        ZHLN::Log("[Render] PresentUI: QueueSubmit failed ({})", submitRes.error().Message());
        RemovePresentation(aux);
        queues.uiBatches.clear();
        return;
    }

    const ZHLN_PresentDesc presentDesc {
        .present_queue   = ctx.PresentQueue(),
        .swapchain       = swap.handle,
        .render_finished = renderFinished,
        .image_index     = imageIndex,
    };
    const ZHLN_FrameResult presented = ZHLN_PresentFrame(&presentDesc);
    if (presented == ZHLN_FrameResult_OutOfDate || presented == ZHLN_FrameResult_Suboptimal) {
        auto rebuilt = extra->presentation.Rebuild(size.width, size.height);
        if (!rebuilt) {
            ZHLN::Log("[Render] PresentUI: rebuild after present failed ({})", rebuilt.error().Message());
            RemovePresentation(aux);
            queues.uiBatches.clear();
            return;
        }
    }

    extra->frameIndex ^= 1u;
    queues.uiBatches.clear();
}

auto RenderContext::AddPresentation(Window& window) noexcept -> bool {
    return _impl->AddPresentation(window);
}

void RenderContext::RemovePresentation(Window& window) noexcept {
    _impl->RemovePresentation(window);
}

auto RenderContext::HasPresentation(const Window& window) const noexcept -> bool {
    return _impl->HasPresentation(window);
}

void RenderContext::PresentUI(Window& window) noexcept {
    _impl->PresentUI(window);
}

} // namespace ZHLN
