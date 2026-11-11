// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "render/DrawQueueManager.hpp"
#include "render/FrameDestinations.hpp"
#include "render/FrameResources.hpp"
#include "render/GeometryManager.hpp"
#include "render/GpuAbi.hpp"
#include "render/GpuLayout.hpp"
#include "render/RenderFrame.hpp"
#include "render/TargetManager.hpp"
#include "render/TextureManager.hpp"
#include "render/features/ShadowAtlas.hpp"
#include "render/features/ShadowRenderer.hpp"
#include "render/features/VolumetricFogSystem.hpp"
#include "render/passes/lighting/LightingResources.hpp"
#include "render/passes/postprocess/PostProcessResources.hpp"
#include "render/pipelines/ReflectionPipeline.hpp"
#include "vulkan/Rendering.hpp"
#include "vulkan/core/FrameStorage.hpp"
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Render/Types.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace ZHLN {

struct IBLPayload {
    Vk::Image     brdfLutImage;
    Vk::ImageView brdfLutView;
    Vk::Image     prefilteredImage;
    Vk::ImageView prefilteredView;
    Vk::Image     visualSkyImage;
    Vk::ImageView visualSkyView;

    std::array<ZHLN::FVec4, 9> shCoeffs {};
    VkFormat                    prefilteredFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    uint64_t                    contentHash       = 0;
    int                         environmentMode   = 0;

    void Destroy(Vk::Allocator& allocator) noexcept {
        brdfLutView     = {};
        prefilteredView = {};
        visualSkyView   = {};
        allocator.DestroyImage(brdfLutImage);
        allocator.DestroyImage(prefilteredImage);
        allocator.DestroyImage(visualSkyImage);
    }
};

struct WorkerCmdContext {
    std::array<Vk::CommandPool<Vk::QueueType::Graphics>, Vk::kFramesInFlight> pools;
    std::array<ZHLN::Atomic<uint32_t>, Vk::kFramesInFlight>                   cmdCount {};
};

struct Renderer {
    using GraphResources = TargetManager::GraphResources;

    Vk::Context   ctx;
    Vk::Allocator allocator;

    mutable Vk::CommandRing<Vk::QueueType::Graphics, 8> graphicsCmdRing;
    mutable Vk::CommandRing<Vk::QueueType::Transfer, 8> transferCmdRing;
    Vk::StagingRingBuffer                               stagingRingBuffer;
    mutable Vk::StagingRingBuffer                       transferRingBuffer;
    Vk::DeletionQueue                                   deletionQueue;

    Vk::HeapManager heapManager;
    // The open frame transaction. Declared after the heap so it closes before the
    // heap it points at is torn down.
    std::optional<Vk::HeapFrameScope> heapFrame;

    TargetManager   targets;
    GraphResources& graphResources = targets.Graph();

    FrameResources frames;
    // Both halves share the same physical type; the explicit graph resolvers
    // identify whichever half is Previous or Current on a given frame.
    PingPong<Vk::RenderTarget<Res_AccumCurrent>> accumulationHistory;
    Vk::Buffer                                   clusterBoundsBuffer;
    Vk::Buffer                                   morphDeltasBuffer;

    Vk::SwapchainPresenter presenter;
    // Primary scene depth is a renderer resource, not part of presentation state.
    Vk::RenderTarget<Vk::Format::D32_SFloat_S8_UInt> primaryDepthTarget;
    FrameDestinations                                destinations;

    DrawQueueManager queues;
    GraphicsSettings settings {};

    TextureManager      textureManager;
    GeometryManager     geometry;
    ShadowRenderer      shadows;
    ShadowAtlas         shadowAtlas;
    VolumetricFogSystem fog;
    // Ray-traced and screen-space lighting compute passes (GTAO and RCGI),
    // each a pipeline bundled with its heap bindings.
    LightingResources lighting;
    // What runs after the scene is lit: the two fullscreen passes, the LTC
    // lookup-table views, and the bloom/HDR chain. Both groups are addressed the
    // same way -- one member per pass, pipeline and bindings together.
    PostProcessResources post;
    ReflectionPipeline   reflectionPipeline;

    uint32_t viewportX = 0;
    uint32_t viewportY = 0;
    uint32_t viewportW = 0;
    uint32_t viewportH = 0;

    JPH::Mat44 current_view_proj    = JPH::Mat44::sIdentity();
    JPH::Mat44 unjittered_view_proj = JPH::Mat44::sIdentity();

    JPH::Mat44    view_matrix    = JPH::Mat44::sIdentity();
    JPH::Mat44    shadowProjView = JPH::Mat44::sIdentity();
    FrameUniforms currentUniforms {};
    float         currentDt = 0.0166f;

    bool     enableMeshShading = true;
    uint32_t blueNoiseTexIdx   = 0;

    uint32_t activeLineVertexCount = 0;

    static constexpr size_t       kParallelRecordingSlots = 2;
    ZHLN::Array<WorkerCmdContext> workerCmds;

    ZHLN::Array<Light> mappedLights;

    // Per-frame transient state, declared in RenderFrame.hpp: the frame loop
    // resets it between frames and the frame a pass is handed reads it as
    // `frame.state`.
    FrameTransientState frameState;

    IBLPayload iblPayload;

    Renderer() noexcept:
        targets(ctx, allocator, graphicsCmdRing), textureManager(ctx, allocator, stagingRingBuffer, graphicsCmdRing, heapManager),
        geometry(ctx, allocator, transferRingBuffer, transferCmdRing, deletionQueue) {
    }

    [[nodiscard]] constexpr auto EffectiveViewport() const noexcept -> VkViewport {
        const auto& fb = graphResources.sceneColor.extent;
        if (viewportW <= 1 || viewportH <= 1) {
            return VkViewport {
                .x        = 0.0F,
                .y        = 0.0F,
                .width    = static_cast<float>(fb.width),
                .height   = static_cast<float>(fb.height),
                .minDepth = 0.0F,
                .maxDepth = 1.0F,
            };
        }
        const uint32_t x = std::min<uint32_t>(viewportX, fb.width);
        const uint32_t y = std::min<uint32_t>(viewportY, fb.height);
        const uint32_t w = std::min<uint32_t>(viewportW, fb.width - x);
        const uint32_t h = std::min<uint32_t>(viewportH, fb.height - y);
        return VkViewport {
            .x        = static_cast<float>(x),
            .y        = static_cast<float>(y),
            .width    = static_cast<float>(w),
            .height   = static_cast<float>(h),
            .minDepth = 0.0F,
            .maxDepth = 1.0F,
        };
    }

    [[nodiscard]] bool MeshShadingActive() const noexcept {
        return enableMeshShading && ctx.MeshShadersSupported();
    }

    [[nodiscard]] auto BufferAddress(VkBuffer buffer) const noexcept -> VkDeviceAddress {
        return ctx.BufferAddress(buffer);
    }

    [[nodiscard]] auto FrameHeapAddresses() const noexcept -> std::array<VkDeviceAddress, GpuAbi::kFrameAddressCount>;

    // A secondary command buffer's whole starting state, assembled from the
    // parts that own it: the rendering inheritance the caller's pass describes,
    // the heap inheritance the manager hands out by reference, and this frame's
    // push rows -- which are copied, because they are addresses of this frame's
    // buffers and the array that holds them is a local.
    //
    // Passes get one line here instead of three subsystems' worth of fields, and
    // no layer is asked for a struct it would then have to keep alive.
    [[nodiscard]] auto CreateSecondaryContext(const Vk::RenderingInheritance& rendering = {}) const noexcept -> Vk::SecondaryPassContext {
        const auto addresses = FrameHeapAddresses();
        return Vk::SecondaryPassContext {
            rendering, heapManager.GetInheritance(),
            Vk::FramePushState {GpuAbi::kScenePushLayout.UsedFrameAddresses(), std::span<const VkDeviceAddress> {addresses.data(), addresses.size()}}
        };
    }

    [[nodiscard]] auto BeginStream(VkCommandBuffer cmd) const noexcept -> std::expected<Vk::StreamCommandRecorder, ErrorCode>;

    [[nodiscard]] auto ActivePresentation() noexcept -> Vk::SwapchainPresenter& {
        if (const auto active = destinations.Active()) {
            return active->Presenter();
        }
        return presenter;
    }

    [[nodiscard]] auto ActiveDepthTarget() noexcept -> Vk::RenderTarget<Vk::Format::D32_SFloat_S8_UInt>& {
        if (auto active = destinations.Active(); active && !active->IsPrimary()) {
            return active->depthTarget;
        }
        return primaryDepthTarget;
    }

  private:
    void BindHeapsAndPushFrame(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN
