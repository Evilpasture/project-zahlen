// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "Zahlen/Camera.hpp"
#include "Zahlen/Math3D.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace ZHLN {

void RenderContext::SetMatrices(const JPH::Mat44& viewProj, const JPH::Mat44& unjitteredViewProj) noexcept {
    _impl->current_view_proj    = viewProj;
    _impl->unjittered_view_proj = unjitteredViewProj;
}

void RenderContext::BindCamera(const Camera& cam, Extent2D viewSize) noexcept {
    const float      aspect     = (viewSize.height > 0) ? static_cast<float>(viewSize.width) / static_cast<float>(viewSize.height) : 1.777f;
    const JPH::Mat44 view       = cam.GetViewMatrix();
    const JPH::Mat44 proj       = cam.GetProjectionMatrix(aspect);
    const JPH::Mat44 unjittered = proj * view;
    _impl->current_view_proj                  = unjittered;
    _impl->unjittered_view_proj               = unjittered;
    _impl->currentUniforms.viewProj           = unjittered;
    _impl->currentUniforms.unjitteredViewProj = unjittered;
    _impl->currentUniforms.invViewProj        = unjittered.Inversed();
    _impl->currentUniforms.invProj            = proj.Inversed();
    _impl->currentUniforms.nearZ              = cam.nearZ;
    _impl->currentUniforms.farZ               = cam.farZ;
    _impl->currentUniforms.camPos.x = cam.position.GetX();
    _impl->currentUniforms.camPos.y = cam.position.GetY();
    _impl->currentUniforms.camPos.z = cam.position.GetZ();

    auto mapped = _impl->frames.frameUniformBuffers[_impl->presenter.frameIndex].Map(_impl->allocator);
    if (!mapped) return;
    auto* const gpu = mapped->As<FrameUniforms>();
    gpu->viewProj           = unjittered;
    gpu->unjitteredViewProj = unjittered;
    gpu->invViewProj        = unjittered.Inversed();
    gpu->invProj            = proj.Inversed();
    gpu->nearZ              = cam.nearZ;
    gpu->farZ               = cam.farZ;
    gpu->camPos.x = cam.position.GetX();
    gpu->camPos.y = cam.position.GetY();
    gpu->camPos.z = cam.position.GetZ();
}

void RenderContext::ClearDrawQueues() noexcept {
    _impl->queues.Draws().clear();
    _impl->queues.CsgDraws().clear();
}

void RenderContext::SetFrameData(const Camera& cam, const FrameUniforms& view, const JPH::Mat44& shadowProjView, float dt) noexcept {
    // The engine authored this struct: one type, no packing step. What follows
    // fills the lanes only the renderer knows -- resolution, light count, the
    // cascade matrices, the SH payload, the viewmodel matrix.
    _impl->shadowProjView  = shadowProjView;
    _impl->currentUniforms = view;
    _impl->currentDt       = std::clamp(dt, 0.0001f, 0.1f);

    VkExtent2D res    = _impl->graphResources.sceneColor.extent;
    float      aspect = (res.height > 0) ? static_cast<float>(res.width) / res.height : 1.777f;
    const auto  sceneVp  = _impl->EffectiveViewport();
    const float vpAspect = (sceneVp.height > 0.0F) ? sceneVp.width / sceneVp.height : aspect;

    std::array<float, 4> cascadeSplits {};
    cascadeSplits[0] = cam.nearZ + (cam.farZ - cam.nearZ) * 0.08f;
    cascadeSplits[1] = cam.nearZ + (cam.farZ - cam.nearZ) * 0.22f;
    cascadeSplits[2] = cam.nearZ + (cam.farZ - cam.nearZ) * 0.55f;
    cascadeSplits[3] = cam.nearZ + (cam.farZ - cam.nearZ) * 1.0f;

    FrameUniforms gpuUniforms    = view;
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
    JPH::Vec3  sunDir    = JPH::Vec3 {view.lightDir.x, view.lightDir.y, view.lightDir.z};
    if (sunDir.LengthSq() > 1e-6f) sunDir = sunDir.Normalized();
    JPH::Mat44 lightView = Math::CreateLookAt(sunDir * 100.0f, JPH::Vec3::sZero(), JPH::Vec3::sAxisY());

    float tanHalfFov = std::tan(JPH::DegreesToRadians(cam.fov * 0.5f));

    for (uint32_t i = 0; i < RenderContext::Impl::NUM_CASCADES; ++i) {
        float nearDist = (i == 0) ? cam.nearZ : cascadeSplits[i - 1];
        float farDist  = cascadeSplits[i];

        gpuUniforms.lightSpaceMatrices[i] =
            ShadowRenderer::ComputeCascadeLightSpaceMatrix(
                cam, lightView, sunDir, nearDist, farDist, vpAspect, tanHalfFov, view.shadowResolution
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

void RenderContext::SetLights(std::span<const Light> lights) noexcept {
    const auto visible = lights.first(std::min(lights.size(), size_t {128}));
    if (!visible.empty()) {
        // The engine fills the shader's own struct, so this is a copy:
        // nothing to keep in sync with anything but the struct itself.
        _impl->gpuLights.assign(visible.begin(), visible.end());
        auto mappedLights = _impl->frames.lightStorageBuffers[_impl->presenter.frameIndex].Map(_impl->allocator);
        if (!mappedLights) {
            _impl->mappedLights.clear();
            _impl->packedLightCount = 0;
            return;
        }
        std::memcpy(mappedLights->Data(), _impl->gpuLights.data(), _impl->gpuLights.size() * sizeof(Light));
        _impl->mappedLights.assign(_impl->gpuLights.begin(), _impl->gpuLights.end());
    } else {
        _impl->mappedLights.clear();
    }
    _impl->packedLightCount = static_cast<uint32_t>(visible.size());
}

}
