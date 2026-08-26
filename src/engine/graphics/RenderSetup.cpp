// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/RenderSetup.cpp
#include "RenderInternal.hpp"
#include "Zahlen/Camera.hpp"
#include "Zahlen/Math3D.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ZHLN {

namespace {

JPH::Mat44 ComputeCascadeLightSpaceMatrix(
    const Camera&     cam,
    const JPH::Mat44& lightView,
    const JPH::Vec3&  sunDir,
    float             nearDist,
    float             farDist,
    float             aspect,
    float             tanHalfFov,
    uint32_t          shadowResolution
) noexcept {
    float hNear = 2.0f * tanHalfFov * nearDist;
    float wNear = hNear * aspect;
    float hFar  = 2.0f * tanHalfFov * farDist;
    float wFar  = hFar * aspect;

    std::array<JPH::Vec3, 8> corners = {
        {{-wNear * 0.5f, hNear * 0.5f, -nearDist},
         {wNear * 0.5f, hNear * 0.5f, -nearDist},
         {wNear * 0.5f, -hNear * 0.5f, -nearDist},
         {-wNear * 0.5f, -hNear * 0.5f, -nearDist},
         {-wFar * 0.5f, hFar * 0.5f, -farDist},
         {wFar * 0.5f, hFar * 0.5f, -farDist},
         {wFar * 0.5f, -hFar * 0.5f, -farDist},
         {-wFar * 0.5f, -hFar * 0.5f, -farDist}}
    };

    JPH::Mat44 invCamView = cam.GetViewMatrix().Inversed();
    for (auto& corner: corners) {
        corner = invCamView * corner;
    }

    // Orientation-invariant fit: center the ortho on the frustum slice's
    // midpoint and take the radius as the farthest slice corner. Both depend
    // only on (fov, aspect, nearDist, farDist), so the ortho size is CONSTANT
    // under rigid camera motion. The previous AABB-centroid fit changed size
    // with camera orientation, and the 1/16 m radius quantization let every
    // texel edge crawl by up to half a texel (7.5-88 cm texels) on each step
    // - visible shadow-edge shimmer while moving at any distance, worst on
    // the far cascades.
    JPH::Vec3 nearCenter = JPH::Vec3::sZero();
    JPH::Vec3 farCenter  = JPH::Vec3::sZero();
    for (int i = 0; i < 4; ++i) {
        nearCenter += corners[static_cast<size_t>(i)];
        farCenter  += corners[4 + static_cast<size_t>(i)];
    }
    JPH::Vec3 center = (nearCenter + farCenter) * 0.125f;

    float radius = 0.0f;
    for (const auto& corner: corners) {
        radius = std::max(radius, (corner - center).Length());
    }
    radius = std::ceil(radius * 16.0f) / 16.0f;
    // Snap the ortho half-extent to a whole number of shadow-map texels so
    // split/far changes cannot shift the texel lattice by a sub-texel
    // remainder either.
    const float invTexels = 2.0f / static_cast<float>(shadowResolution);
    radius                = std::ceil(radius / invTexels) * invTexels;

    JPH::Vec3 centerLight   = lightView * center;
    float     texelsPerUnit = static_cast<float>(shadowResolution) / (radius * 2.0f);

    centerLight.SetX(std::floor(centerLight.GetX() * texelsPerUnit) / texelsPerUnit);
    centerLight.SetY(std::floor(centerLight.GetY() * texelsPerUnit) / texelsPerUnit);

    center = lightView.Inversed() * centerLight;

    float offset  = Shadows::BaseOffset;
    float farClip = Shadows::BaseDepth;

    if (farDist > 550.0f) {
        offset  = Shadows::FarOffset;
        farClip = Shadows::FarDepth;
    }

    JPH::Vec3  cascadeLightPos  = center + sunDir * offset;
    JPH::Mat44 cascadeLightView = Math::CreateLookAt(cascadeLightPos, center, JPH::Vec3::sAxisY());
    JPH::Mat44 cascadeLightProj = Math::CreateOrtho(-radius, radius, -radius, radius, Shadows::NearClip, farClip);

    return cascadeLightProj * cascadeLightView;
}

} // namespace

void RenderContext::SetMatrices(const JPH::Mat44& viewProj, const JPH::Mat44& unjitteredViewProj) noexcept {
    _impl->current_view_proj    = viewProj;
    _impl->unjittered_view_proj = unjitteredViewProj;
}

void RenderContext::SetFrameData(const Camera& cam, const FrameUniforms& uniforms, const JPH::Mat44& shadowProjView, float dt) noexcept {
    _impl->shadowProjView  = shadowProjView;
    _impl->currentUniforms = uniforms;
    _impl->currentDt       = std::clamp(dt, 0.0001f, 0.1f);

    VkExtent2D res    = _impl->graphResources.sceneColor.extent;
    float      aspect = (res.height > 0) ? static_cast<float>(res.width) / res.height : 1.777f;

    std::array<float, 4> cascadeSplits {};
    cascadeSplits[0] = cam.nearZ + (cam.farZ - cam.nearZ) * 0.08f;
    cascadeSplits[1] = cam.nearZ + (cam.farZ - cam.nearZ) * 0.22f;
    cascadeSplits[2] = cam.nearZ + (cam.farZ - cam.nearZ) * 0.55f;
    cascadeSplits[3] = cam.nearZ + (cam.farZ - cam.nearZ) * 1.0f;

    FrameUniforms gpuUniforms       = uniforms;
    gpuUniforms.screenResolution[0] = static_cast<float>(res.width);
    gpuUniforms.screenResolution[1] = static_cast<float>(res.height);

    JPH::Mat44 viewmodelProj      = Math::CreatePerspective(JPH::DegreesToRadians(58.0f), aspect, cam.nearZ, cam.farZ);
    gpuUniforms.viewmodelViewProj = viewmodelProj * cam.GetViewMatrix();
    gpuUniforms.invProj           = cam.GetProjectionMatrix(aspect).Inversed();

    std::memcpy(gpuUniforms.cascadeSplits, cascadeSplits.data(), sizeof(float) * 4);
    std::memcpy(gpuUniforms.sh, _impl->iblPayload.shCoeffs.data(), sizeof(JPH::Vec4) * 9);

    JPH::Vec3  sunDir    = JPH::Vec3(uniforms.lightDir[0], uniforms.lightDir[1], uniforms.lightDir[2]).Normalized();
    JPH::Mat44 lightView = Math::CreateLookAt(sunDir * 100.0f, JPH::Vec3::sZero(), JPH::Vec3::sAxisY());

    float tanHalfFov = std::tan(JPH::DegreesToRadians(cam.fov * 0.5f));

    for (uint32_t i = 0; i < RenderContext::Impl::NUM_CASCADES; ++i) {
        float nearDist = (i == 0) ? cam.nearZ : cascadeSplits[i - 1];
        float farDist  = cascadeSplits[i];

        gpuUniforms.lightSpaceMatrices[i] =
            ComputeCascadeLightSpaceMatrix(cam, lightView, sunDir, nearDist, farDist, aspect, tanHalfFov, uniforms.shadowResolution);
    }

    std::memcpy(_impl->frames.frameUniformBuffers->Map().data, &gpuUniforms, sizeof(FrameUniforms));

    if (aspect != _impl->lastAspectRatio || cam.fov != _impl->lastFov) {
        _impl->lastAspectRatio    = aspect;
        _impl->lastFov            = cam.fov;
        _impl->clusterBoundsDirty = true;
    }
}

void RenderContext::SetGISettings(const GISettings& settings) noexcept {
    _impl->giSettings = settings;
}

void RenderContext::SetLights(const Light* lights, uint32_t count) noexcept {
    uint32_t safeCount = std::min(count, 128u);
    if (safeCount > 0 && lights != nullptr) {
        std::memcpy(_impl->frames.lightStorageBuffers->Map().data, lights, sizeof(Light) * safeCount);
        _impl->mappedLights.assign(lights, lights + safeCount);
    } else {
        _impl->mappedLights.clear();
    }
}

} // namespace ZHLN
