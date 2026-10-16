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

// Hand-written mirror of the shadow à-trous shader's push block. Unlike the
// RTGI/HDR payloads it is not in the generated Slang inventory, so it is
// declared here where every pass payload belongs; AtrousDenoisePass fills it.
struct ShadowAtrousPushConstants {
    uint32_t stepSize;
    float    phiDepth;
    float    phiNormal;
    float    temporalWeight;
};
static_assert(GpuAbi::ScenePassPayload<ShadowAtrousPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

// The AA filters' hand-written payloads, formerly declared in their pass
// files. Each mirrors the push block its shader declares.
struct FXAAPushConstants {
    float rcpFrameX;
    float rcpFrameY;
    float subpix;
    float edgeThreshold;
    float edgeThresholdMin;
    float _pad;
};
static_assert(GpuAbi::ScenePassPayload<FXAAPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

struct MLAAPushConstants {
    float    rcpFrameX;
    float    rcpFrameY;
    float    threshold;
    uint32_t maxSearchSteps;
};
static_assert(GpuAbi::ScenePassPayload<MLAAPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

struct TAAPushConstants {
    float feedback;
};
static_assert(GpuAbi::ScenePassPayload<TAAPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

// The single-dispatch compute passes' payloads, formerly declared in their
// pass files. Each mirrors the push block its shader declares.
struct RtrHalfPushConstants {
    uint32_t halfRes[2];
    uint32_t _pad[2];
};
static_assert(GpuAbi::ScenePassPayload<RtrHalfPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

struct RtgiHalfPushConstants {
    uint32_t halfRes[2];
    uint32_t _pad[2];
};
static_assert(GpuAbi::ScenePassPayload<RtgiHalfPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

struct DdgiTracePushConstants {
    uint32_t totalProbes;
    uint32_t raysPerProbe;
    float    maxRayDistance;
    uint32_t _pad;
};
static_assert(GpuAbi::ScenePassPayload<DdgiTracePushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

struct DdgiUpdatePushConstants {
    uint32_t totalProbes;
    uint32_t raysPerProbe;
    float    hysteresis;
    float    depthSharpness;
};
static_assert(GpuAbi::ScenePassPayload<DdgiUpdatePushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

struct alignas(16) VolumetricFogPushConstants {
    float density;
    float heightFalloff;
    float heightOffset;
    float anisotropy;

    float scatteringColor[3];
    float noiseScale;

    float absorptionColor[3];
    float noiseSpeed;

    float emissiveColor[3];
    float noiseIntensity;

    uint32_t _pad0;
    uint32_t enableNoise;
    uint32_t _pad1;
    uint32_t _pad2;
};
static_assert(sizeof(VolumetricFogPushConstants) == 80);
static_assert(GpuAbi::ScenePassPayload<VolumetricFogPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

struct alignas(16) VolumetricLightInjectPushConstants {
    float    scatteringIntensity;
    float    ambientIntensity;
    float    phaseAnisotropy;
    uint32_t enableShadows;
};
static_assert(sizeof(VolumetricLightInjectPushConstants) == 16);
static_assert(
    GpuAbi::ScenePassPayload<VolumetricLightInjectPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared"
);

struct alignas(16) VolumetricTemporalPushConstants {
    float    temporalWeight;
    float    clampStrength;
    uint32_t resetHistory;
    uint32_t _pad;
};
static_assert(sizeof(VolumetricTemporalPushConstants) == 16);
static_assert(GpuAbi::ScenePassPayload<VolumetricTemporalPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

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
