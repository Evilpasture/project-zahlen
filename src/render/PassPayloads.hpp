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

using GtaoPushConstants = Slang::GtaoPushConstants;
using RtgiCompositePushConstants = Slang::RtgiCompositePushConstants;
using RtgiAtrousPushConstants = Slang::RtgiAtrousPushConstants;
using KawasePushConstants = Slang::KawasePushConstants;
using ThresholdPushConstants = Slang::ThresholdPushConstants;
using HdrAtrousPushConstants = Slang::HdrAtrousPushConstants;
using BlitPushConstants = Slang::BlitPushConstants;

using ScenePassPushConstants = Slang::ScenePassPushConstants;

// Slang owns these payloads (resources/shaders/gpu_push_constants.slang); the
// generated definitions are the ABI. The aliases keep the pass-side names.
using FXAAPushConstants = Slang::FXAAPushConstants;
static_assert(GpuAbi::ScenePassPayload<FXAAPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

using MLAAPushConstants = Slang::MLAAPushConstants;
static_assert(GpuAbi::ScenePassPayload<MLAAPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

using TAAPushConstants = Slang::TAAPushConstants;
static_assert(GpuAbi::ScenePassPayload<TAAPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

using RtgiHalfPushConstants = Slang::RtgiHalfPushConstants;
static_assert(GpuAbi::ScenePassPayload<RtgiHalfPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

using RcgiProbeSetupPushConstants = Slang::RcgiProbeSetupPushConstants;
static_assert(GpuAbi::ScenePassPayload<RcgiProbeSetupPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

using RcgiCascadePushConstants = Slang::RcgiCascadePushConstants;
static_assert(GpuAbi::ScenePassPayload<RcgiCascadePushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

using RcgiResolvePushConstants = Slang::RcgiResolvePushConstants;
static_assert(GpuAbi::ScenePassPayload<RcgiResolvePushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

using DdgiTracePushConstants = Slang::DdgiTracePushConstants;
static_assert(GpuAbi::ScenePassPayload<DdgiTracePushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

using DdgiUpdatePushConstants = Slang::DdgiUpdatePushConstants;
static_assert(GpuAbi::ScenePassPayload<DdgiUpdatePushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

using VolumetricFogPushConstants = Slang::VolumetricFogPushConstants;
static_assert(GpuAbi::ScenePassPayload<VolumetricFogPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

using VolumetricLightInjectPushConstants = Slang::VolumetricLightInjectPushConstants;
static_assert(
    GpuAbi::ScenePassPayload<VolumetricLightInjectPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared"
);

using VolumetricTemporalPushConstants = Slang::VolumetricTemporalPushConstants;
static_assert(GpuAbi::ScenePassPayload<VolumetricTemporalPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

static_assert(
    (GpuAbi::ScenePassPayload<ObjectConstants> && GpuAbi::ScenePassPayload<UIObjectConstants> &&
     GpuAbi::ScenePassPayload<CullingConstants> && GpuAbi::ScenePassPayload<MeshParticleRenderPush> &&
     GpuAbi::ScenePassPayload<SkinningConstants> && GpuAbi::ScenePassPayload<BakePush> &&
     GpuAbi::ScenePassPayload<GtaoPushConstants> &&
     GpuAbi::ScenePassPayload<RtgiCompositePushConstants> && GpuAbi::ScenePassPayload<RtgiAtrousPushConstants> &&
     GpuAbi::ScenePassPayload<KawasePushConstants> && GpuAbi::ScenePassPayload<ThresholdPushConstants> &&
     GpuAbi::ScenePassPayload<HdrAtrousPushConstants> && GpuAbi::ScenePassPayload<BlitPushConstants> &&
     GpuAbi::ScenePassPayload<ScenePassPushConstants>),
    "a generated pass payload no longer fits the push blob's prefix in front of the frame addresses"
);

} // namespace ZHLN
