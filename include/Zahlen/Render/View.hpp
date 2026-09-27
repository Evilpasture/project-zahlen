// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include <Zahlen/Camera.hpp>
#include <Zahlen/Geometry2D.hpp>
#include <Zahlen/Render/Handles.hpp>
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>
#include <cstdint>

namespace ZHLN {

struct SceneView {
    JPH::Mat44       viewMatrix        = JPH::Mat44::sIdentity();
    JPH::Mat44       projMatrix        = JPH::Mat44::sIdentity();
    JPH::Mat44       viewProjMatrix    = JPH::Mat44::sIdentity();
    JPH::Mat44       invViewProjMatrix = JPH::Mat44::sIdentity();
    JPH::Vec3        worldPosition     = JPH::Vec3::sZero();
    ViewportRect     viewport          = {};
    RenderAttachment target            = {};
    Frustum          frustum           = {};
    uint64_t         visibilityMask    = ~0ULL;
    uint32_t         frameIndex        = 0;
    float            time              = 0.0f;
};

struct UIView {
    ViewportRect     viewport   = {};
    RenderAttachment target     = {};
    uint32_t         frameIndex = 0;
};

}
