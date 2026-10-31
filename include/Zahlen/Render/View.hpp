// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
#include <Zahlen/Math/ValueTypes.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <cstdint>

namespace ZHLN {

struct SceneView {
    ZHLN::Mat44  viewMatrix        = ZHLN::Mat44::sIdentity();
    ZHLN::Mat44  projMatrix        = ZHLN::Mat44::sIdentity();
    ZHLN::Mat44  viewProjMatrix    = ZHLN::Mat44::sIdentity();
    ZHLN::Mat44  invViewProjMatrix = ZHLN::Mat44::sIdentity();
    ZHLN::Float3 worldPosition {};
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
