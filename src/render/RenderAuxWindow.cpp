// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/RenderAuxWindow.cpp
// Second OS window on the live RenderContext (UI editor Preview). Same device,
// a second VkSwapchainKHR. Never a second Engine and never ExecuteImmediate
// for present: acquire, record, QueueSubmit wait image-available, present.

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

} // namespace

void RenderContext::Impl::DetachWindow() noexcept {
    if (attachedWindow == nullptr && !attachedPresentation.swapchain.Valid() && attachedSurface.Get() == VK_NULL_HANDLE) {
        if (attachedImageAvailable == VK_NULL_HANDLE && attachedRenderFinished == VK_NULL_HANDLE && !attachedUiVbo.Valid()) {
            return;
        }
    }

    if (ctx.Device() != VK_NULL_HANDLE) {
        auto idle = Vk::WaitIdle(ctx.Device());
        if (!idle) {
            ZHLN::Log("[Render] DetachWindow: WaitIdle failed ({})", idle.error().Message());
        }
        if (attachedImageAvailable != VK_NULL_HANDLE) {
            ZHLN_DestroySemaphore(ctx.Device(), attachedImageAvailable);
            attachedImageAvailable = VK_NULL_HANDLE;
        }
        if (attachedRenderFinished != VK_NULL_HANDLE) {
            ZHLN_DestroySemaphore(ctx.Device(), attachedRenderFinished);
            attachedRenderFinished = VK_NULL_HANDLE;
        }
    }

    attachedUiPipeline = {};
    attachedUiVbo      = {};
    attachedUiVboAddress = 0;
    attachedPresentation = {};
    attachedSurface      = {};
    attachedWindow       = nullptr;
}

auto RenderContext::Impl::AttachWindow(Window& window) noexcept -> bool {
    if (presentationMode != PresentationMode::NativeSwapchain) {
        ZHLN::Log("[Render] AttachWindow: NativeSwapchain presentation is required");
        return false;
    }
    if (ctx.Device() == VK_NULL_HANDLE) {
        return false;
    }
    if (attachedWindow == &window && attachedPresentation.swapchain.Valid()) {
        return true;
    }

    DetachWindow();

    int width  = 0;
    int height = 0;
    auto surfaceRes = window.CreateVulkanSurface(ctx.Instance(), ctx.Physical(), width, height);
    if (!surfaceRes) {
        ZHLN::Log("[Render] AttachWindow: surface creation failed ({})", surfaceRes.error().Message());
        return false;
    }
    auto* rawSurface = static_cast<VkSurfaceKHR>(*surfaceRes);
    if (rawSurface == VK_NULL_HANDLE || width <= 0 || height <= 0) {
        ZHLN::Log("[Render] AttachWindow: no presentable surface");
        return false;
    }

    VkBool32 supported = VK_FALSE;
    const VkResult supportRes =
        vkGetPhysicalDeviceSurfaceSupportKHR(ctx.Physical(), ctx.PhysicalInfo().present_family, rawSurface, &supported);
    if (supportRes != VK_SUCCESS || supported != VK_TRUE) {
        ZHLN::Log("[Render] AttachWindow: present family does not support this surface");
        vkDestroySurfaceKHR(ctx.Instance(), rawSurface, nullptr);
        return false;
    }

    attachedSurface = Vk::Surface(ctx.Instance(), rawSurface);
    auto initRes    = attachedPresentation.Init(ctx, allocator, attachedSurface.Get(), static_cast<uint32_t>(width), static_cast<uint32_t>(height), true);
    if (!initRes) {
        ZHLN::Log("[Render] AttachWindow: swapchain init failed ({})", initRes.error().Message());
        DetachWindow();
        return false;
    }

    const size_t vboBytes = static_cast<size_t>(kMaxUiVertices) * (sizeof(VertexPosition) + sizeof(VertexAttributes));
    auto vboRes = Vk::Buffer::Create(
        allocator.Get(), vboBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU
    );
    if (!vboRes) {
        ZHLN::Log("[Render] AttachWindow: UI VBO allocation failed ({})", vboRes.error().Message());
        DetachWindow();
        return false;
    }
    attachedUiVbo        = std::move(*vboRes);
    attachedUiVboAddress = ctx.BufferAddress(attachedUiVbo.Handle());

    attachedImageAvailable = ZHLN_CreateSemaphore(ctx.Device());
    attachedRenderFinished = ZHLN_CreateSemaphore(ctx.Device());
    if (attachedImageAvailable == VK_NULL_HANDLE || attachedRenderFinished == VK_NULL_HANDLE) {
        ZHLN::Log("[Render] AttachWindow: semaphore creation failed");
        DetachWindow();
        return false;
    }

    const VkFormat auxFormat  = attachedPresentation.GetPresentFormat();
    const VkFormat mainFormat = presentation.GetPresentFormat();
    if (auxFormat != mainFormat) {
        auto pipeRes = BuildUiPipeline(*this, auxFormat);
        if (!pipeRes) {
            ZHLN::Log("[Render] AttachWindow: aux UI pipeline failed ({})", pipeRes.error().Message());
            DetachWindow();
            return false;
        }
        attachedUiPipeline = std::move(*pipeRes);
    }

    attachedWindow = &window;
    ZHLN::Log("[Render] Attached auxiliary window ({}x{}, format {})", width, height, static_cast<int>(auxFormat));
    return true;
}

void RenderContext::Impl::PresentAttachedWindow() noexcept {
    if (!HasAttachedWindow() || attachedWindow == nullptr) {
        queues.uiBatches.clear();
        return;
    }
    if (!attachedWindow->IsRunning()) {
        DetachWindow();
        queues.uiBatches.clear();
        return;
    }

    const Extent2D size = attachedWindow->GetSize();
    if (size.width == 0 || size.height == 0) {
        queues.uiBatches.clear();
        return;
    }

    const VkExtent2D scExtent = attachedPresentation.swapchain.Get().extent;
    if (size.width != scExtent.width || size.height != scExtent.height) {
        auto rebuilt = attachedPresentation.Rebuild(size.width, size.height);
        if (!rebuilt) {
            ZHLN::Log("[Render] PresentAttachedWindow: rebuild failed ({})", rebuilt.error().Message());
            DetachWindow();
            queues.uiBatches.clear();
            return;
        }
        const VkFormat auxFormat  = attachedPresentation.GetPresentFormat();
        const VkFormat mainFormat = presentation.GetPresentFormat();
        attachedUiPipeline        = {};
        if (auxFormat != mainFormat) {
            auto pipeRes = BuildUiPipeline(*this, auxFormat);
            if (!pipeRes) {
                ZHLN::Log("[Render] PresentAttachedWindow: aux UI pipeline rebuild failed");
                DetachWindow();
                queues.uiBatches.clear();
                return;
            }
            attachedUiPipeline = std::move(*pipeRes);
        }
    }

    const auto& swap = attachedPresentation.swapchain.Get();
    uint32_t    imageIndex = 0;
    const ZHLN_AcquireDesc acquireDesc {
        .swapchain       = swap.handle,
        .image_available = attachedImageAvailable,
        .timeout_ns      = UINT64_MAX,
    };
    const ZHLN_FrameResult acquired = ZHLN_AcquireImage(ctx.Device(), &acquireDesc, &imageIndex);
    if (acquired == ZHLN_FrameResult_OutOfDate) {
        attachedPresentation.Rebuild(size.width, size.height);
        queues.uiBatches.clear();
        return;
    }
    if (acquired == ZHLN_FrameResult_DeviceLost) {
        ZHLN::Log("[Render] PresentAttachedWindow: device lost on acquire");
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

    const size_t maxVertices = attachedUiVbo.Size() / (sizeof(VertexPosition) + sizeof(VertexAttributes));
    auto&        srcVbo      = frames.uiVbos[frame_index];
    const size_t srcMax      = srcVbo.Size() / (sizeof(VertexPosition) + sizeof(VertexAttributes));
    const size_t copyVerts   = std::min(maxVertices, srcMax);
    if (attachedUiVbo.Valid() && srcVbo.Valid() && copyVerts > 0 && !queues.uiBatches.empty()) {
        auto srcMap = srcVbo.Map();
        auto dstMap = attachedUiVbo.Map();
        if (srcMap.data != nullptr && dstMap.data != nullptr) {
            std::memcpy(dstMap.data, srcMap.data, copyVerts * sizeof(VertexPosition));
            std::memcpy(
                static_cast<char*>(dstMap.data) + maxVertices * sizeof(VertexPosition),
                static_cast<char*>(srcMap.data) + srcMax * sizeof(VertexPosition),
                copyVerts * sizeof(VertexAttributes)
            );
        }
    }

    auto [cmd, fence] = graphicsCmdRing.Acquire();
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
                const VkPipeline pipeline = attachedUiPipeline.Valid() ? attachedUiPipeline.Get() : uiPipeline.Get();

                for (const auto& batch: queues.uiBatches) {
                    uipc.albedoIdx       = batch.bindlessTextureIndex != 0 ? batch.bindlessTextureIndex : textureManager.GetBindlessIndex(batch.texture);
                    uipc.isSDF           = batch.isSDF ? 1u : 0u;
                    uipc.useTextureColor = batch.useTextureColor ? 1u : 0u;
                    uipc.posAddress      = attachedUiVboAddress + (batch.vertexStart * sizeof(VertexPosition));
                    uipc.attrAddress =
                        attachedUiVboAddress + (maxVertices * sizeof(VertexPosition)) + (batch.vertexStart * sizeof(VertexAttributes));

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

    const VkSemaphore renderFinished =
        attachedPresentation.presentSemaphores.Valid() ? attachedPresentation.presentSemaphores[imageIndex] : attachedRenderFinished;

    auto submitRes = Vk::QueueSubmit(
        ctx.GraphicsQueue(), static_cast<VkCommandBuffer>(cmd), attachedImageAvailable, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        renderFinished, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, fence
    );
    if (!submitRes) {
        ZHLN::Log("[Render] PresentAttachedWindow: QueueSubmit failed ({})", submitRes.error().Message());
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
        attachedPresentation.Rebuild(size.width, size.height);
    }

    queues.uiBatches.clear();
}

auto RenderContext::AttachWindow(Window& window) noexcept -> bool {
    return _impl->AttachWindow(window);
}

void RenderContext::DetachWindow() noexcept {
    _impl->DetachWindow();
}

auto RenderContext::HasAttachedWindow() const noexcept -> bool {
    return _impl->HasAttachedWindow();
}

void RenderContext::PresentAttachedWindow() noexcept {
    _impl->PresentAttachedWindow();
}

} // namespace ZHLN
