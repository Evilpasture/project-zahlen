// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Render/View.hpp
//
// Render parameters, split from the renderer's dispatch surface.
//
// A view is pure spatial/optical data plus a destination subresource. It does
// not classify *what kind of view* it is (no `Scene3D` / `UIOnly` / `ShadowCascade`
// enumerator), because every such enumerator is a feature sink: adding OpenXR,
// a cubemap probe face or a minimap would mean a new value and a new switch in
// every pass. A caller instead fills in the optics it needs and points `target`
// at the subresource it wants written.

#pragma once
#include <Zahlen/Camera.hpp>
#include <Zahlen/Types.hpp>
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>
#include <cstdint>

namespace ZHLN {

// Optical, geometric and destination parameters for rendering a 3D scene.
//
// The matrices are what the renderer pushes into its pass constants. History
// that a *frame* owns rather than a view -- previous-frame matrices, TAA
// jitter, sun/sky/probe uniforms -- stays with the renderer's frame data
// (`RenderContext::SetFrameData` / `ApplySettings`), because a second view in
// the same frame must not overwrite another view's history.
struct SceneView {
    JPH::Mat44       viewMatrix        = JPH::Mat44::sIdentity();
    JPH::Mat44       projMatrix        = JPH::Mat44::sIdentity();
    JPH::Mat44       viewProjMatrix    = JPH::Mat44::sIdentity();
    JPH::Mat44       invViewProjMatrix = JPH::Mat44::sIdentity();
    JPH::Vec3        worldPosition     = JPH::Vec3::sZero();
    ViewportRect     viewport          = {};
    RenderAttachment target            = {}; // Output subresource
    Frustum          frustum           = {};
    uint64_t         visibilityMask    = ~0ULL;
    uint32_t         frameIndex        = 0;
    float            time              = 0.0f;
};

// Destination and layout bounds for rendering 2D UI.
struct UIView {
    ViewportRect     viewport   = {};
    RenderAttachment target     = {}; // Output subresource
    uint32_t         frameIndex = 0;
};

} // namespace ZHLN
