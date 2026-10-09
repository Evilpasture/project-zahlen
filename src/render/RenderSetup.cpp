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
    _impl->submittedLights.assign(lights.begin(), lights.first(std::min(lights.size(), size_t {128})).end());
    _impl->UploadSubmittedLights();

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

// Packed from SetFrameData once the view matrix and light list exist.
void RenderContext::Impl::UploadSubmittedLights() noexcept {
    if (submittedLights.empty()) {
        mappedLights.clear();
        packedLightCount = 0;
        return;
    }

    gpuLights.clear();
    gpuLights.reserve(submittedLights.size());
    for (const LightDesc& desc: submittedLights) {
        gpuLights.push_back(GpuPack::PackLight(desc, view_matrix));
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
}

}
