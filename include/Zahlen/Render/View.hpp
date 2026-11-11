// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
#include <Zahlen/Math/Vec2.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Render/RenderIDs.hpp>
#include <cstdint>

namespace ZHLN {

struct SceneView {
    ZHLN::Mat4x4  viewMatrix        = ZHLN::Mat4x4::Identity();
    ZHLN::Mat4x4  projMatrix        = ZHLN::Mat4x4::Identity();
    ZHLN::Mat4x4  viewProjMatrix    = ZHLN::Mat4x4::Identity();
    ZHLN::Mat4x4  invViewProjMatrix = ZHLN::Mat4x4::Identity();
    ZHLN::FVec3 worldPosition {};
    ViewportRect viewport       = {};
    FrameTarget  target         = {};
    Frustum      frustum        = {};
    uint64_t     visibilityMask = ~0ULL;
    uint32_t     frameIndex     = 0;
    float        time           = 0.0f;
};

struct UIView {
    ViewportRect viewport   = {};
    FrameTarget  target     = {};
    uint32_t     frameIndex = 0;
};
}
