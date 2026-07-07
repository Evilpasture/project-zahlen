// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "Zahlen/Camera.hpp"
#include "Zahlen/Math3D.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ZHLN::Renderer {

namespace {

JPH::Mat44 ComputeCascadeLightSpaceMatrix(const Camera& cam, const JPH::Mat44& lightView,
										  const JPH::Vec3& sunDir, float nearDist, float farDist,
										  float aspect, float tanHalfFov,
										  uint32_t shadowResolution) noexcept {
	float hNear = 2.0f * tanHalfFov * nearDist;
	float wNear = hNear * aspect;
	float hFar = 2.0f * tanHalfFov * farDist;
	float wFar = hFar * aspect;

	std::array<JPH::Vec3, 8> corners = {{{-wNear * 0.5f, hNear * 0.5f, -nearDist},
										 {wNear * 0.5f, hNear * 0.5f, -nearDist},
										 {wNear * 0.5f, -hNear * 0.5f, -nearDist},
										 {-wNear * 0.5f, -hNear * 0.5f, -nearDist},
										 {-wFar * 0.5f, hFar * 0.5f, -farDist},
										 {wFar * 0.5f, hFar * 0.5f, -farDist},
										 {wFar * 0.5f, -hFar * 0.5f, -farDist},
										 {-wFar * 0.5f, -hFar * 0.5f, -farDist}}};

	JPH::Mat44 invCamView = cam.GetViewMatrix().Inversed();
	JPH::Vec3 center = JPH::Vec3::sZero();
	for (auto& corner : corners) {
		corner = invCamView * corner;
		center += corner;
	}
	center /= 8.0f;

	float radius = 0.0f;
	for (const auto& corner : corners) {
		radius = std::max(radius, (corner - center).Length());
	}
	radius = std::ceil(radius * 16.0f) / 16.0f;

	JPH::Vec3 centerLight = lightView * center;
	float texelsPerUnit = static_cast<float>(shadowResolution) / (radius * 2.0f);

	centerLight.SetX(std::floor(centerLight.GetX() * texelsPerUnit) / texelsPerUnit);
	centerLight.SetY(std::floor(centerLight.GetY() * texelsPerUnit) / texelsPerUnit);

	center = lightView.Inversed() * centerLight;

	JPH::Vec3 cascadeLightPos = center + sunDir * 150.0f;
	JPH::Mat44 cascadeLightView = Math::CreateLookAt(cascadeLightPos, center, JPH::Vec3::sAxisY());
	JPH::Mat44 cascadeLightProj = Math::CreateOrtho(-radius, radius, -radius, radius, 0.1f, 300.0f);

	return cascadeLightProj * cascadeLightView;
}

} // namespace

void SetMatrices(RenderContext& ctx, const JPH::Mat44& viewProj,
				 const JPH::Mat44& unjitteredViewProj) {
	auto* impl = ctx.GetImpl();
	impl->current_view_proj = viewProj;
	impl->unjittered_view_proj = unjitteredViewProj;
}

void SetFrameData(RenderContext& ctx, const Camera& cam, const FrameUniforms& uniforms,
				  const JPH::Mat44& shadowProjView) {
	auto* impl = ctx.GetImpl();

	impl->shadowProjView = shadowProjView;
	impl->currentUniforms = uniforms;

	VkExtent2D res = impl->sceneColor.extent;
	float aspect = (res.height > 0) ? (float)res.width / res.height : 1.777f;

	// Check if projection bounds need an update
	if (aspect != impl->lastAspectRatio || cam.fov != impl->lastFov) {
		impl->lastAspectRatio = aspect;
		impl->lastFov = cam.fov;
		impl->UploadClusterBounds(cam.GetProjectionMatrix(aspect));
	}

	// Snapping and cascade projections
	float cascadeSplits[4];
	cascadeSplits[0] = cam.nearZ + (cam.farZ - cam.nearZ) * 0.08f;
	cascadeSplits[1] = cam.nearZ + (cam.farZ - cam.nearZ) * 0.22f;
	cascadeSplits[2] = cam.nearZ + (cam.farZ - cam.nearZ) * 0.55f;
	cascadeSplits[3] = cam.nearZ + (cam.farZ - cam.nearZ) * 1.0f;

	FrameUniforms gpuUniforms = uniforms;
	std::memcpy(gpuUniforms.cascadeSplits, cascadeSplits, sizeof(float) * 4);
	std::memcpy(gpuUniforms.sh, impl->iblPayload.shCoeffs.data(), sizeof(JPH::Vec4) * 9);

	// Update Cascade light space matrices
	JPH::Vec3 sunDir =
		JPH::Vec3(uniforms.lightDir[0], uniforms.lightDir[1], uniforms.lightDir[2]).Normalized();
	JPH::Mat44 lightView =
		Math::CreateLookAt(sunDir * 100.0f, JPH::Vec3::sZero(), JPH::Vec3::sAxisY());

	float tanHalfFov = std::tan(JPH::DegreesToRadians(cam.fov * 0.5f));

	for (uint32_t i = 0; i < RenderContext::Impl::NUM_CASCADES; ++i) {
		float nearDist = (i == 0) ? cam.nearZ : cascadeSplits[i - 1];
		float farDist = cascadeSplits[i];

		gpuUniforms.lightSpaceMatrices[i] =
			ComputeCascadeLightSpaceMatrix(cam, lightView, sunDir, nearDist, farDist, aspect,
										   tanHalfFov, uniforms.shadowResolution);
	}

	std::memcpy(impl->frameUniformBuffers->Map().data, &gpuUniforms, sizeof(FrameUniforms));
}

void SetGISettings(RenderContext& ctx, const GISettings& settings) {
	auto* impl = ctx.GetImpl();
	impl->giSettings = settings;
}

void SetLights(RenderContext& ctx, const GPULight* lights, uint32_t count) {
	auto* impl = ctx.GetImpl();
	uint32_t safeCount = std::min(count, 128u);
	if (safeCount > 0 && lights != nullptr) {
		std::memcpy(impl->lightStorageBuffers->Map().data, lights, sizeof(GPULight) * safeCount);
		impl->mappedLights.assign(lights, lights + safeCount);
	} else {
		impl->mappedLights.clear();
	}
}

} // namespace ZHLN::Renderer
