// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "GpuPack.hpp"
#include "Zahlen/Math/Camera.hpp"
#include "Zahlen/Math/Math3D.hpp"
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

void RenderContext::SetGISettings(GISettings settings) noexcept {
    _impl->settings.post = settings;
}

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

    shadowAtlas.EndFrame();
}

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

    if (const auto tiles = frames.shadowTileBuffers[presenter.frameIndex].Map(allocator); tiles && shadowAtlas.TileCount() > 0) {
        std::memcpy(tiles->Data(), shadowAtlas.Tiles().data(), shadowAtlas.TileCount() * sizeof(ShadowTile));
    }
}

}
