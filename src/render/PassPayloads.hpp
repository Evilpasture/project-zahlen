// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "GpuAbi.hpp"

namespace ZHLN {

using ObjectConstants = Slang::ObjectConstants;
using UIObjectConstants = Slang::UIObjectConstants;
using CullingConstants = Slang::CullingConstants;
using MeshParticleRenderPush = Slang::MeshParticleRenderPushConstants;
using SkinningConstants = Slang::SkinningConstants;
using BakePush = Slang::BakePush;

using RTShadowPushConstants = Slang::RTShadowPushConstants;
using GtaoPushConstants = Slang::GtaoPushConstants;
using RtgiCompositePushConstants = Slang::RtgiCompositePushConstants;
using RtgiAtrousPushConstants = Slang::RtgiAtrousPushConstants;
using KawasePushConstants = Slang::KawasePushConstants;
using ThresholdPushConstants = Slang::ThresholdPushConstants;
using HdrAtrousPushConstants = Slang::HdrAtrousPushConstants;
using BlitPushConstants = Slang::BlitPushConstants;

using ScenePassPushConstants = Slang::ScenePassPushConstants;

static_assert(
    (GpuAbi::ScenePassPayload<ObjectConstants> && GpuAbi::ScenePassPayload<UIObjectConstants> &&
     GpuAbi::ScenePassPayload<CullingConstants> && GpuAbi::ScenePassPayload<MeshParticleRenderPush> &&
     GpuAbi::ScenePassPayload<SkinningConstants> && GpuAbi::ScenePassPayload<BakePush> &&
     GpuAbi::ScenePassPayload<RTShadowPushConstants> && GpuAbi::ScenePassPayload<GtaoPushConstants> &&
     GpuAbi::ScenePassPayload<RtgiCompositePushConstants> && GpuAbi::ScenePassPayload<RtgiAtrousPushConstants> &&
     GpuAbi::ScenePassPayload<KawasePushConstants> && GpuAbi::ScenePassPayload<ThresholdPushConstants> &&
     GpuAbi::ScenePassPayload<HdrAtrousPushConstants> && GpuAbi::ScenePassPayload<BlitPushConstants> &&
     GpuAbi::ScenePassPayload<ScenePassPushConstants>),
    "a generated pass payload no longer fits the push blob's prefix in front of the frame addresses"
);

} // namespace ZHLN
