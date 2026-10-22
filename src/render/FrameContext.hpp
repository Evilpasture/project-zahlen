// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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
#include "features/ShadowAtlas.hpp"
#include "features/ShadowRenderer.hpp"
#include "features/VolumetricFogSystem.hpp"
#include "passes/lighting/LightingResources.hpp"
#include "passes/postprocess/PostProcessResources.hpp"
#include "pipelines/ReflectionPipeline.hpp"
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/GraphicsSettings.hpp>
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

    Vk::Image                visualSkyImage;
    Vk::ImageView            visualSkyView;
    std::array<JPH::Vec4, 9> shCoeffs {};
    VkFormat                 prefilteredFormat = VK_FORMAT_R8G8B8A8_UNORM;
    uint64_t                 contentHash       = 0;
    int                      environmentMode   = 0;

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

struct RenderFrameContext {
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

    FrameResources                                            frames;
    // Both halves share the same physical type; the explicit graph resolvers
    // identify whichever half is Previous or Current on a given frame.
    PingPong<Vk::RenderTarget<Res_AccumCurrent>> accumulationHistory;
    Vk::Buffer                                                clusterBoundsBuffer;
    Vk::Buffer                                                morphDeltasBuffer;

    Vk::SwapchainPresenter presenter;
    // Primary scene depth is a renderer resource, not part of presentation state.
    Vk::FormatRenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT> primaryDepthTarget;
    FrameDestinations      destinations;

    DrawQueueManager queues;
    GraphicsSettings settings {};

    TextureManager      textureManager;
    GeometryManager     geometry;
    ShadowRenderer      shadows;
    ShadowAtlas         shadowAtlas;
    VolumetricFogSystem fog;
    // Ray-traced and screen-space lighting: shadows, reflections, GI, DDGI, GTAO
    // and their denoisers, each a pipeline bundled with its heap bindings.
    LightingResources lighting;
    // What runs after the scene is lit: the two fullscreen passes, the LTC
    // lookup-table views, and the bloom/HDR chain. Both groups are addressed the
    // same way -- one member per pass, pipeline and bindings together.
    PostProcessResources post;
    ReflectionPipeline  reflectionPipeline;

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

    bool enableMeshShading = true;
    bool rtHistoryValid    = false;
    // DDGI atlas texels are indexed by probe coordinate, so moving the grid
    // changes the world position represented by every texel. Track that
    // separately from FrameUniforms (which is rebuilt from FrameData each
    // frame) so relocation can invalidate temporal probe accumulation.
    bool      ddgiGridOriginValid = false;
    JPH::Vec3 ddgiGridOrigin      = JPH::Vec3::sZero();
    uint32_t  blueNoiseTexIdx     = 0;

    uint32_t activeLineVertexCount = 0;

    static constexpr size_t       kParallelRecordingSlots = 2;
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

    IBLPayload iblPayload;

    RenderFrameContext() noexcept:
        targets(ctx, allocator, graphicsCmdRing), textureManager(ctx, allocator, stagingRingBuffer, graphicsCmdRing, heapManager),
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

    [[nodiscard]] auto FrameHeapAddresses() const noexcept -> std::array<VkDeviceAddress, GpuAbi::kFrameAddressCount>;

    // Establishes the descriptor-heap state of one command stream: binds both
    // heap segments and pushes the device-address block that the shaders'
    // `ParameterBlock<GlobalSceneRegistry> scene` dereferences directly, with no
    // descriptor table behind it.
    //
    // This is state of the STREAM, not of any pass in it, so it is called once
    // per command stream, at the one moment the stream begins recording:
    //
    //   * RenderContext::Impl::AcquireTarget  -- a window's graphics stream, the
    //     one the scene graph, the Blit pass and the UI pass all record into;
    //   * Pipelines::ComputeSimPipeline::Submit -- the async-compute stream.
    //
    // A pass must never call this. A pass that re-establishes state its
    // predecessors already established is a pass that only works because of
    // what happens to run before it: it keeps working when the graph is
    // reordered or the pass is forked onto another queue, until the day it
    // does not. configure/check_heap_state_roots.py rejects any call outside
    // the two roots above.
    //
    // Secondary command buffers inherit neither the heap bindings nor the push
    // range from the primary they are executed in, so the parallel recorders
    // re-establish this state for every slot they begin, from
    // FrameHeapAddresses() (see Vk::ParallelCommandRecorder::SetHeapState and
    // RenderContext::Impl::ForkReplayer::ExecuteFork).
    void BindHeapsAndPushFrame(VkCommandBuffer cmd) const noexcept;

    [[nodiscard]] auto ActivePresentation() noexcept -> Vk::SwapchainPresenter& {
        if (const auto active = destinations.Active()) {
            return active->Presenter();
        }
        return presenter;
    }

    [[nodiscard]] auto ActiveDepthTarget() noexcept -> Vk::FormatRenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT>& {
        if (auto active = destinations.Active(); active && !active->IsPrimary()) {
            return active->depthTarget;
        }
        return primaryDepthTarget;
    }
};

} // namespace ZHLN
