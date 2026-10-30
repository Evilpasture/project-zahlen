// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "FrameResources.hpp"
#include "GpuLayout.hpp"
#include "TargetManager.hpp"
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Math3D.hpp>
#include <cstdint>

namespace ZHLN {

/// The renderer, declared in Renderer.hpp. A frame reaches it by reference and
/// nothing else: whatever outlives the frame is reached through `frame.renderer`,
/// so a pass can see where the frame ends.
struct Renderer;

/// State that belongs to the frame in flight rather than to the renderer: reset
/// between frames, read by whatever part of the recording asks about it.
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

/// One recorded frame.
///
/// Three kinds of thing, and nothing else:
///
///   * its identity -- which frames-in-flight slot it occupies;
///   * its storage  -- FrameResources::Slice: this frame's line VBO, light array,
///                     cluster grid, TLAS and joint-ring slots as plain
///                     references, so `frame.lightStorageBuffers` is the whole
///                     access path;
///   * its inputs   -- the matrices, uniforms, timestep, viewport and mesh
///                     shading decision the frame was started with, plus the
///                     targets it draws into and the settings it runs under.
///
/// Everything else -- the device, the allocator, command rings, heaps, the
/// managers, the pipelines, the presenters -- is the renderer's, and is reached
/// through `renderer`.
///
/// Lifetime: created by the frame loop when a frame begins, gone when its
/// recording ends. It is a struct of references, so a temporary cannot bind to
/// the `RenderFrame&` a pass takes: the frame owns one, and passes borrow it.
struct RenderFrame: FrameResources::Slice {
    RenderFrame(Renderer& renderer, uint32_t inFlight) noexcept;

    Renderer& renderer;

    /// The slot in every PerFrame<> this frame owns. This is what call sites used
    /// to fetch as `presenter.frameIndex` -- a frames-in-flight counter that lives
    /// on presentation state for reasons that have nothing to do with buffers. The
    /// swapchain image this frame presents into is a different number and stays
    /// with the destination.
    uint32_t inFlight = 0;

    /// The render targets this frame draws into (TargetManager::Graph()).
    TargetManager::GraphResources& graphResources;

    /// The configuration this frame is recorded under, as it stood when the frame
    /// began.
    const GraphicsSettings& settings;

    /// computeSubmitted / inForkSecondary / hasSkinned / resized / clusterBoundsDirty.
    FrameTransientState& state;

    /// Frame inputs. The renderer holds the one live copy -- they are written by
    /// the app-facing entry points before and while the frame records -- and the
    /// frame sees them as its own; binding them here keeps that single source of
    /// truth while giving pass code frame language instead of a walk into the
    /// renderer.
    const JPH::Mat44&    viewProj;
    const JPH::Mat44&    unjitteredViewProj;
    const JPH::Mat44&    view;
    const JPH::Mat44&    shadowProjView;
    const FrameUniforms& uniforms;
    const float&         dt;

    /// Resolved once when the frame begins instead of recomputed by every pass
    /// that asks: EffectiveViewport() over the scene-color extent, and whether
    /// mesh shading is available and enabled this frame.
    VkViewport viewport {};
    bool       meshShading = false;

    const uint32_t& activeLineVertexCount;

    /// The depth target of the destination this frame presents into.
    [[nodiscard]] auto ActiveDepthTarget() noexcept -> Vk::FormatRenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT>&;
};

} // namespace ZHLN
