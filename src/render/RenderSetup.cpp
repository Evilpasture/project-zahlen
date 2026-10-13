// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "GpuPack.hpp"
#include "Zahlen/Camera.hpp"
#include "Zahlen/Math3D.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <span>

namespace ZHLN {

void RenderContext::ClearDrawQueues() noexcept {
    _impl->queues.Draws().clear();
    _impl->queues.CsgDraws().clear();
}

void RenderContext::SetFrameData(
    const Camera& cam, const FrameData& frame, const JPH::Mat44& shadowProjView, std::span<const LightDesc> lights, float dt
) noexcept {
    _impl->shadowProjView       = shadowProjView;
    _impl->view_matrix          = cam.GetViewMatrix();
    _impl->current_view_proj    = frame.viewProj;
    _impl->unjittered_view_proj = frame.unjitteredViewProj;
    _impl->currentUniforms      = GpuPack::PackFrameData(frame);
    _impl->currentDt            = std::clamp(dt, 0.0001f, 0.1f);
    // kMaxSceneLights, not 128: see RenderInternal.hpp. The truncation stays --
    // it is what keeps the memcpy below inside the buffer -- but LightingSystem
    // now publishes the list in ranked order, so if a map ever does exceed the
    // budget the lights that survive are the nearest and brightest rather than
    // the first ones the entity lump happened to list.
    _impl->submittedLights.assign(lights.begin(), lights.first(std::min(lights.size(), size_t {kMaxSceneLights})).end());
    _impl->UploadSubmittedLights(cam);

    VkExtent2D res    = _impl->graphResources.sceneColor.extent;
    float      aspect = (res.height > 0) ? static_cast<float>(res.width) / res.height : 1.777f;
    const auto  sceneVp  = _impl->EffectiveViewport();
    const float vpAspect = (sceneVp.height > 0.0F) ? sceneVp.width / sceneVp.height : aspect;

    std::array<float, 4> cascadeSplits {};
    cascadeSplits[0] = cam.nearZ + (cam.farZ - cam.nearZ) * 0.08f;
    cascadeSplits[1] = cam.nearZ + (cam.farZ - cam.nearZ) * 0.22f;
    cascadeSplits[2] = cam.nearZ + (cam.farZ - cam.nearZ) * 0.55f;
    cascadeSplits[3] = cam.nearZ + (cam.farZ - cam.nearZ) * 1.0f;

    FrameUniforms gpuUniforms    = _impl->currentUniforms;
    gpuUniforms.screenResolution = JPH::Float2 {static_cast<float>(res.width), static_cast<float>(res.height)};

    gpuUniforms.lightCount = _impl->packedLightCount;

    JPH::Mat44 viewmodelProj      = Math::CreatePerspective(JPH::DegreesToRadians(58.0f), aspect, cam.nearZ, cam.farZ);
    gpuUniforms.viewmodelViewProj = viewmodelProj * cam.GetViewMatrix();
    gpuUniforms.invProj           = cam.GetProjectionMatrix(vpAspect).Inversed();

    gpuUniforms.cascadeSplits = JPH::Float4 {cascadeSplits[0], cascadeSplits[1], cascadeSplits[2], cascadeSplits[3]};
    std::memcpy(gpuUniforms.sh.data(), _impl->iblPayload.shCoeffs.data(), sizeof(JPH::Float4) * 9);
    gpuUniforms.environmentMode = _impl->iblPayload.environmentMode;

    // The sun's direction and intensity ride lightDir's lanes, which is the
    // shader's own layout: read them where the shader reads them.
    JPH::Vec3  sunDir    = frame.sunDirection;
    if (sunDir.LengthSq() > 1e-6f) sunDir = sunDir.Normalized();
    JPH::Mat44 lightView = Math::CreateLookAt(sunDir * 100.0f, JPH::Vec3::sZero(), JPH::Vec3::sAxisY());

    float tanHalfFov = std::tan(JPH::DegreesToRadians(cam.fov * 0.5f));

    for (uint32_t i = 0; i < RenderContext::Impl::NUM_CASCADES; ++i) {
        float nearDist = (i == 0) ? cam.nearZ : cascadeSplits[i - 1];
        float farDist  = cascadeSplits[i];

        gpuUniforms.lightSpaceMatrices[i] =
            ShadowRenderer::ComputeCascadeLightSpaceMatrix(
                cam, lightView, sunDir, nearDist, farDist, vpAspect, tanHalfFov, frame.shadowResolution
            );
    }

    gpuUniforms.nearZ = cam.nearZ;
    gpuUniforms.farZ  = cam.farZ;
    if (gpuUniforms.frameIndex == 0) {
        gpuUniforms.frameIndex            = static_cast<uint32_t>(_impl->frameSerial);
        _impl->currentUniforms.frameIndex = gpuUniforms.frameIndex;
    }

    constexpr float kProbeSpacing = 3.2f;
    const JPH::Vec3 halfSpan      = JPH::Vec3(15.0f * 0.5f, 7.0f * 0.5f, 15.0f * 0.5f) * kProbeSpacing;
    JPH::Vec3       gridCenter    = JPH::Vec3(0.0f, 6.0f, 0.0f);
    if ((frame.probeMax - frame.probeMin).LengthSq() > 1.0f) {
        gridCenter = (frame.probeMin + frame.probeMax) * 0.5f;
    }
    const JPH::Vec3 camOffset = cam.position - gridCenter;
    if (std::abs(camOffset.GetX()) > halfSpan.GetX() * 0.75f || std::abs(camOffset.GetY()) > halfSpan.GetY() * 0.75f ||
        std::abs(camOffset.GetZ()) > halfSpan.GetZ() * 0.75f) {
        constexpr float kSnap = kProbeSpacing * 4.0f;
        gridCenter            = JPH::Vec3(
            std::floor(cam.position.GetX() / kSnap + 0.5f) * kSnap,
            std::floor(cam.position.GetY() / kSnap + 0.5f) * kSnap,
            std::floor(cam.position.GetZ() / kSnap + 0.5f) * kSnap
        );
    }
    const JPH::Vec3 gridOrigin    = gridCenter - halfSpan;
    const uint32_t  fIdx          = _impl->presenter.frameIndex;
    const bool      ddgiEnabled   = _impl->ctx.RayTracingSupported() && _impl->settings.environment.fullBright == 0 &&
                                    static_cast<bool>(_impl->frames.tlas[fIdx]) &&
                                    (_impl->settings.rayTracing.enableDiffuseGI || _impl->settings.rayTracing.enableReflections ||
                                     _impl->settings.rayTracing.enableShadows);
    const float ddgiHysteresis    = _impl->rtHistoryValid ? 0.96f : 0.0f;
    gpuUniforms.ddgiOriginSpacing = JPH::Float4 {gridOrigin.GetX(), gridOrigin.GetY(), gridOrigin.GetZ(), kProbeSpacing};
    gpuUniforms.ddgiGridCounts    = JPH::Float4 {16.0f, 8.0f, 16.0f, 64.0f};
    gpuUniforms.ddgiParams        = JPH::Float4 {ddgiHysteresis, 0.1f, 0.2f, ddgiEnabled ? 1.0f : 0.0f};
    _impl->currentUniforms.ddgiOriginSpacing = gpuUniforms.ddgiOriginSpacing;
    _impl->currentUniforms.ddgiGridCounts    = gpuUniforms.ddgiGridCounts;
    _impl->currentUniforms.ddgiParams        = gpuUniforms.ddgiParams;

    auto mappedUniforms = _impl->frames.frameUniformBuffers[_impl->presenter.frameIndex].Map(_impl->allocator);
    if (!mappedUniforms) return;
    std::memcpy(mappedUniforms->Data(), &gpuUniforms, sizeof(FrameUniforms));

    if (vpAspect != _impl->lastAspectRatio || cam.fov != _impl->lastFov || cam.nearZ != _impl->lastNearZ || cam.farZ != _impl->lastFarZ) {
        _impl->lastAspectRatio               = vpAspect;
        _impl->lastFov                       = cam.fov;
        _impl->lastNearZ                     = cam.nearZ;
        _impl->lastFarZ                      = cam.farZ;
        _impl->frameState.clusterBoundsDirty = true;
    }
}

void RenderContext::SetGISettings(const GISettings& settings) noexcept {
    _impl->settings.post = settings;
}

// Hands out this frame's shadow tiles.
//
// The lights arrive already ranked -- pinned first, then nearest-and-brightest
// (LightingSystem) -- so the atlas fills in that order and the lights that lose
// are the ones furthest away and least worth the texels. What a loser loses is
// tile *size*, not shadows: `TileForDistance` steps a light down as it recedes
// long before the atlas is full enough to refuse it.
void RenderContext::Impl::AllocateShadowTiles(const JPH::Vec3& viewPos) noexcept {
    shadowAtlas.BeginFrame();

    std::array<JPH::Mat44, ShadowAtlas::kPointFaces> faces {};
    for (size_t index = 0; index < submittedLights.size(); ++index) {
        const LightDesc& desc = submittedLights[index];
        const bool isPoint    = desc.type == LightType::Point;
        const bool isSpot     = desc.type == LightType::Spot;
        if (!desc.castsShadow || (!isPoint && !isSpot)) {
            lightTileBases.push_back(-1);
            continue;
        }

        const uint32_t faceCount = ShadowAtlas::MakeFaceMatrices(
            desc.position, desc.direction, desc.range, isPoint, desc.outerConeRadians, faces.data()
        );
        ShadowAtlas::Request req {};
        req.key        = ShadowAtlas::MakeLightKey(desc.type, desc.position, desc.intensity, desc.range);
        req.lightIndex = static_cast<int32_t>(index);
        req.faceCount  = faceCount;
        req.tile       = ShadowAtlas::TileForDistance((desc.position - viewPos).Length());
        for (uint32_t face = 0; face < faceCount; ++face) {
            req.faces[face] = faces[face];
        }
        lightTileBases.push_back(shadowAtlas.Acquire(req).tileBase);
    }

    // Drops the lights that stopped asking, so the cache cannot grow with a
    // scene's history.
    shadowAtlas.EndFrame();
}

// Packed from SetFrameData once the view matrix and light list exist.
void RenderContext::Impl::UploadSubmittedLights(const Camera& cam) noexcept {
    if (submittedLights.empty()) {
        mappedLights.clear();
        lightTileBases.clear();
        shadowAtlas.BeginFrame();
        shadowAtlas.EndFrame();
        packedLightCount = 0;
        return;
    }

    lightTileBases.clear();
    AllocateShadowTiles(cam.position);

    gpuLights.clear();
    gpuLights.reserve(submittedLights.size());
    for (size_t index = 0; index < submittedLights.size(); ++index) {
        const int32_t tileBase = (index < lightTileBases.size()) ? lightTileBases[index] : -1;
        gpuLights.push_back(GpuPack::PackLight(submittedLights[index], view_matrix, tileBase));
    }

    auto mapped = frames.lightStorageBuffers[presenter.frameIndex].Map(allocator);
    if (!mapped) {
        mappedLights.clear();
        packedLightCount = 0;
        return;
    }
    std::memcpy(mapped->Data(), gpuLights.data(), gpuLights.size() * sizeof(Light));
    mappedLights.assign(gpuLights.begin(), gpuLights.end());
    packedLightCount = static_cast<uint32_t>(gpuLights.size());

    // The tiles go up with the lights: the lighting pass reconstructs a
    // shadow-atlas tile from the same matrices the shadow pass is about to
    // rasterize with, so the two are written in the same place in the frame.
    if (const auto tiles = frames.shadowTileBuffers[presenter.frameIndex].Map(allocator); tiles && shadowAtlas.TileCount() > 0) {
        std::memcpy(tiles->Data(), shadowAtlas.Tiles().data(), shadowAtlas.TileCount() * sizeof(ShadowTile));
    }
}

}
