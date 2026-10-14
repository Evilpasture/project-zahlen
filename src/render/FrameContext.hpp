// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What a pass is allowed to see.
//
// RenderContext::Impl is ~160 data members and ~80 member functions covering
// five unrelated lifetimes: device, swapchain, frame, init, and host-side
// resource registries. RenderFrameContext is the slice of it that a recording
// pass legitimately needs -- the device, the descriptor heap, the render
// targets, the per-frame GPU buffers, and the shared subsystems (shadows, fog,
// post-process, textures, geometry) whose passes read state rather than own it.
//
// RenderContext::Impl derives from this, so `Impl&` still binds to
// `RenderFrameContext&` and every `Passes::XPass {.impl = self}` in
// RenderGraphBuild.hpp keeps working unchanged. That is what lets the passes
// migrate one at a time: a pass whose member is declared `RenderFrameContext&`
// physically cannot reach the renderer's init functions, its presentation
// internals, its diagnostics, or another pass's pipelines, and the compiler
// proves it.
//
// Not named FrameContext: ZHLN::FrameContext already exists in
// <Zahlen/FrameScheduler.hpp> and is the engine's per-frame scheduler struct.

#include "DrawQueueManager.hpp"
#include "FrameDestinations.hpp"
#include "FrameResources.hpp"
#include "GeometryManager.hpp"
#include "GpuAbi.hpp"
#include "GpuLayout.hpp"
#include "Rendering.hpp"
#include "TargetManager.hpp"
#include "TextureManager.hpp"
#include "core/FrameStorage.hpp"
#include "features/PostProcessFeature.hpp"
#include "features/ShadowAtlas.hpp"
#include "features/ShadowRenderer.hpp"
#include "features/VolumetricFogSystem.hpp"
#include "pipelines/ReflectionPipeline.hpp"
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Render/Types.hpp>
#include <algorithm>
#include <array>
#include <cstdint>

namespace ZHLN::Vk {

struct IBLPayload {
    // Baked textures keep no VkImageView handles at all: only the plain view
    // descriptions their heap writes are built from.
    Image                 brdfLutImage;
    VkImageViewCreateInfo brdfLutInfo {};
    Image                 prefilteredImage;
    VkImageViewCreateInfo prefilteredInfo {};
    // Original RGBA32F sky, only retained when the lighting cube was cooked
    // from a different (sunless) panorama and the skybox is visible.
    Image                    visualSkyImage;
    VkImageViewCreateInfo    visualSkyInfo {};
    std::array<JPH::Vec4, 9> shCoeffs {};
    VkFormat                 prefilteredFormat = VK_FORMAT_R8G8B8A8_UNORM;
    uint64_t                 contentHash       = 0;
    int                      environmentMode   = 0;

    void Destroy(Allocator& allocator) noexcept {
        brdfLutInfo     = {};
        prefilteredInfo = {};
        visualSkyInfo   = {};
        allocator.DestroyImage(brdfLutImage);
        allocator.DestroyImage(prefilteredImage);
        allocator.DestroyImage(visualSkyImage);
    }
};

} // namespace ZHLN::Vk

namespace ZHLN {

struct WorkerCmdContext {
    std::array<Vk::CommandPool<Vk::QueueType::Graphics>, Vk::kFramesInFlight> pools;
    std::array<ZHLN::Atomic<uint32_t>, Vk::kFramesInFlight>                   cmdCount {};
};

struct RenderFrameContext {
    using GraphResources = TargetManager::GraphResources;

    // ---------------------------------------------------------------------
    // Device. Declared first: everything below is built from it.
    // ---------------------------------------------------------------------
    Vk::Context   ctx;
    Vk::Allocator allocator;

    // Command rings, staging and deferred destruction. These are here rather
    // than in Impl because targets, textureManager and geometry are
    // constructed from them, and a base class is initialized before any
    // derived member exists.
    mutable Vk::CommandRing<Vk::QueueType::Graphics, 8> graphicsCmdRing;
    mutable Vk::CommandRing<Vk::QueueType::Transfer, 8> transferCmdRing;
    Vk::StagingRingBuffer                               stagingRingBuffer;
    mutable Vk::StagingRingBuffer                       transferRingBuffer;
    Vk::DeletionQueue                                   deletionQueue;

    // ---------------------------------------------------------------------
    // Descriptor heap.
    // ---------------------------------------------------------------------
    Vk::HeapManager heapManager;

    // ---------------------------------------------------------------------
    // Render targets and the per-frame GPU buffers.
    // ---------------------------------------------------------------------
    TargetManager   targets;
    GraphResources& graphResources = targets.Graph();

    FrameResources                                            frames;
    PingPong<Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>> accumulationHistory;
    Vk::Buffer                                                clusterBoundsBuffer;
    Vk::Buffer                                                morphDeltasBuffer;

    // ---------------------------------------------------------------------
    // Presentation. `destinations` is here because ActivePresentation() has
    // to consult it; the aux-window machinery itself stays on Impl.
    // ---------------------------------------------------------------------
    Vk::SwapchainPresenter presenter;
    FrameDestinations      destinations;

    // ---------------------------------------------------------------------
    // This frame's inputs.
    // ---------------------------------------------------------------------
    DrawQueueManager queues;
    GraphicsSettings settings {};

    // ---------------------------------------------------------------------
    // Shared multi-pass subsystems. Each owns the pipelines and scratch
    // assets its passes need, so a pass is a description of recording work
    // rather than a holder of GPU state.
    // ---------------------------------------------------------------------
    TextureManager      textureManager;
    GeometryManager     geometry;
    ShadowRenderer      shadows;
    ShadowAtlas         shadowAtlas;
    VolumetricFogSystem fog;
    PostProcessFeature  postProcess;
    ReflectionPipeline  reflectionPipeline;

    // ---------------------------------------------------------------------
    // Per-frame scalars and host-side frame state.
    // ---------------------------------------------------------------------
    uint32_t viewportX = 0;
    uint32_t viewportY = 0;
    uint32_t viewportW = 0;
    uint32_t viewportH = 0;

    JPH::Mat44 current_view_proj    = JPH::Mat44::sIdentity();
    JPH::Mat44 unjittered_view_proj = JPH::Mat44::sIdentity();
    // Kept for the pack boundary: light packing needs a world-to-view matrix, and
    // nothing else in the renderer holds one separately from the projection.
    JPH::Mat44    view_matrix    = JPH::Mat44::sIdentity();
    JPH::Mat44    shadowProjView = JPH::Mat44::sIdentity();
    FrameUniforms currentUniforms {};
    float         currentDt = 0.0166f;

    bool     enableMeshShading = true;
    bool     rtHistoryValid    = false;
    uint32_t blueNoiseTexIdx   = 0;

    uint32_t activeLineVertexCount = 0;
    uint32_t lineInstanceId        = 0;

    static constexpr size_t       kParallelRecordingSlots = 2; // concurrent secondary recordings, not frames in flight
    ZHLN::Array<WorkerCmdContext> workerCmds;

    ZHLN::Array<Light> mappedLights;

    struct FrameTransientState {
        bool computeSubmitted = false;

        bool inForkSecondary = false;

        bool hasSkinned = false;

        bool resized            = true;
        bool clusterBoundsDirty = true;

        void Reset() noexcept {
            computeSubmitted = false;
            inForkSecondary  = false;
            hasSkinned       = false;
        }
    };

    FrameTransientState frameState;

    Vk::IBLPayload iblPayload;


    // Pipelines and heap bindings the passes reach for directly. These sit on the
    // frame context rather than on Impl because the passes that use them -- both
    // GBuffer passes for the GPU cull, SceneDraw for CSG and mesh particles --
    // are already written against RenderFrameContext&, and leaving the members
    // behind would strand those passes on Impl&. Step 1 (per-pass pipeline
    // ownership) is where they move on to the passes that actually own them.
    Vk::Pipeline           meshParticleRenderPipeline;
    Vk::Pipeline           meshParticleShadowPipeline;
    Vk::Pipeline           csgWritePipeline;
    Vk::Pipeline           csgDifferencePipeline;
    Vk::Pipeline           csgIntersectionPipeline;

    RenderFrameContext() noexcept:
        targets(ctx, allocator, graphicsCmdRing),
        textureManager(ctx, allocator, stagingRingBuffer, graphicsCmdRing, heapManager),
        geometry(ctx, allocator, transferRingBuffer, transferCmdRing, deletionQueue), reflectionPipeline(*this) {
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

    // The six frame addresses the scene pushes ahead of every pass payload.
    [[nodiscard]] auto FrameHeapAddresses() const noexcept -> std::array<VkDeviceAddress, GpuAbi::kFrameAddressCount>;

    void BindHeapsAndPushFrame(VkCommandBuffer cmd) const noexcept;

    // The presenter the frame is being recorded for: the active aux window's,
    // if the frame is rendering into one, otherwise the swapchain's.
    [[nodiscard]] auto ActivePresentation() noexcept -> Vk::SwapchainPresenter& {
        if (const auto active = destinations.Active()) {
            return active->Presenter();
        }
        return presenter;
    }

    [[nodiscard]] auto InheritsHeaps() const noexcept -> bool {
        return frameState.inForkSecondary;
    }
};

} // namespace ZHLN
