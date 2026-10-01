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
    std::memcpy(&_impl->currentUniforms.camPos[0], &cam.position, sizeof(float) * 3);

    auto        mapped = _impl->frames.frameUniformBuffers[_impl->presenter.frameIndex].Map(_impl->allocator.Get());
    auto* const gpu    = static_cast<FrameUniforms*>(mapped.data);
    if (gpu == nullptr) return;
    gpu->viewProj           = unjittered;
    gpu->unjitteredViewProj = unjittered;
    gpu->invViewProj        = unjittered.Inversed();
    gpu->invProj            = proj.Inversed();
    gpu->nearZ              = cam.nearZ;
    gpu->farZ               = cam.farZ;
    std::memcpy(&gpu->camPos[0], &cam.position, sizeof(float) * 3);
}

void RenderContext::ClearDrawQueues() noexcept {
    _impl->queues.Draws().clear();
    _impl->queues.CsgDraws().clear();
}

void RenderContext::SetFrameData(const Camera& cam, const FrameUniforms& uniforms, const JPH::Mat44& shadowProjView, float dt) noexcept {
    _impl->shadowProjView  = shadowProjView;
    _impl->currentUniforms = uniforms;
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

    FrameUniforms gpuUniforms       = uniforms;
    gpuUniforms.screenResolution[0] = static_cast<float>(res.width);
    gpuUniforms.screenResolution[1] = static_cast<float>(res.height);

    gpuUniforms.lightCount = _impl->packedLightCount;

    JPH::Mat44 viewmodelProj      = Math::CreatePerspective(JPH::DegreesToRadians(58.0f), aspect, cam.nearZ, cam.farZ);
    gpuUniforms.viewmodelViewProj = viewmodelProj * cam.GetViewMatrix();
    gpuUniforms.invProj           = cam.GetProjectionMatrix(vpAspect).Inversed();

    std::memcpy(gpuUniforms.cascadeSplits, cascadeSplits.data(), sizeof(float) * 4);
    std::memcpy(gpuUniforms.sh.data(), _impl->iblPayload.shCoeffs.data(), sizeof(JPH::Vec4) * 9);
    gpuUniforms.environmentMode = _impl->iblPayload.environmentMode;

    JPH::Vec3  sunDir    = JPH::Vec3(uniforms.lightDir[0], uniforms.lightDir[1], uniforms.lightDir[2]).Normalized();
    JPH::Mat44 lightView = Math::CreateLookAt(sunDir * 100.0f, JPH::Vec3::sZero(), JPH::Vec3::sAxisY());

    float tanHalfFov = std::tan(JPH::DegreesToRadians(cam.fov * 0.5f));

    for (uint32_t i = 0; i < RenderContext::Impl::NUM_CASCADES; ++i) {
        float nearDist = (i == 0) ? cam.nearZ : cascadeSplits[i - 1];
        float farDist  = cascadeSplits[i];

        gpuUniforms.lightSpaceMatrices[i] =
            ShadowRenderer::ComputeCascadeLightSpaceMatrix(
                cam, lightView, sunDir, nearDist, farDist, vpAspect, tanHalfFov, uniforms.shadowResolution
            );
    }

    gpuUniforms.nearZ = cam.nearZ;
    gpuUniforms.farZ  = cam.farZ;

    auto mappedUniforms = _impl->frames.frameUniformBuffers[_impl->presenter.frameIndex].Map(_impl->allocator.Get());
    if (mappedUniforms.data == nullptr) return;
    std::memcpy(mappedUniforms.data, &gpuUniforms, sizeof(FrameUniforms));

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
        auto mappedLights = _impl->frames.lightStorageBuffers[_impl->presenter.frameIndex].Map(_impl->allocator.Get());
        if (mappedLights.data == nullptr) {
            _impl->mappedLights.clear();
            _impl->packedLightCount = 0;
            return;
        }
        std::memcpy(mappedLights.data, visible.data(), visible.size_bytes());
        _impl->mappedLights.assign(visible.begin(), visible.end());
    } else {
        _impl->mappedLights.clear();
    }
    _impl->packedLightCount = static_cast<uint32_t>(visible.size());
}

}
