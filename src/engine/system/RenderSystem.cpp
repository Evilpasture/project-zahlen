// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderSystem.hpp"

#include "CameraSystem.hpp"
#include "CullingSystem.hpp"
#include "LightingSystem.hpp"
#include "UIRenderSystem.hpp"
#include "ecs/ECS.hpp"

#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Render.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <engine/Resources.hpp>
#include <physics/Physics.hpp>
#include <physics/PhysicsDebug.hpp>

namespace ZHLN {

std::expected<void, RenderFrameResult> RenderSystem::Update(Engine& engine) {
	int physicsDrawMode = 0;
	JPH::Mat44 shadowProjView = JPH::Mat44::sIdentity();

	// 1. Process standard geometry draws and frame configurations
	auto mainResult = RenderMain(engine, physicsDrawMode, shadowProjView);
	if (!mainResult) {
		return mainResult;
	}

	// 2. Process secondary debug lines/solids drawing
	RenderDebug(engine, physicsDrawMode);

	// 3. Resolve frame boundaries
	auto& rc = engine.GetRenderContext();
	auto end_res = rc.EndFrame();
	if (!end_res) {
		return std::unexpected(end_res.error());
	}

	return {};
}

std::expected<void, RenderFrameResult>
RenderSystem::RenderMain(Engine& engine, int& outPhysicsDrawMode, JPH::Mat44& outShadowProjView) {
	auto& rc = engine.GetRenderContext();
	auto& reg = engine.GetRegistry();
	auto& cam = engine.GetCamera();
	const auto& visibleEntities = engine.GetVisibleEntities();

	JPH::Mat44 vp{};
	JPH::Mat44 unjitteredVp{};
	JPH::Mat44 prevUnjitteredVp{};

	auto cameraEntities = reg.GetEntitiesWith<MainCameraTagComponent>();
	if (cameraEntities.empty()) {
		return std::unexpected(RenderFrameResult::Error);
	}

	auto begin_res = rc.BeginFrame();
	if (!begin_res) {
		return std::unexpected(begin_res.error());
	}
	UIRenderSystem::Update(engine);
	Entity cameraEntity = cameraEntities[0];

	if (auto* cComp = reg.Get<CameraSystem::CameraComponent>(cameraEntity)) {
		vp = cComp->viewProj;
		unjitteredVp = cComp->unjitteredViewProj;
		prevUnjitteredVp = cComp->prevUnjitteredViewProj;
	} else {
		return std::unexpected(RenderFrameResult::Error);
	}

	int enableRTR = 0;
	JPH::Vec4 probeMin(0, 0, 0, 0);
	JPH::Vec4 probeMax(0, 0, 0, 0);
	JPH::Vec4 probePos(0, 0, 0, 0);
	outPhysicsDrawMode = 0;

	auto settingsEntities = reg.GetEntitiesWith<GlobalSettingsTagComponent>();
	if (!settingsEntities.empty()) {
		if (auto* pp = reg.Get<PostProcessSettingsComponent>(settingsEntities[0])) {
			enableRTR = pp->enableRTR;
			probeMin = JPH::Vec4(pp->probeMin.GetX(), pp->probeMin.GetY(), pp->probeMin.GetZ(),
								 pp->useLocalProbe ? 1.0f : 0.0f);
			probeMax =
				JPH::Vec4(pp->probeMax.GetX(), pp->probeMax.GetY(), pp->probeMax.GetZ(), 0.0f);
			probePos =
				JPH::Vec4(pp->probePos.GetX(), pp->probePos.GetY(), pp->probePos.GetZ(), 0.0f);
		}
		if (auto* dbg = reg.Get<DebugSettingsComponent>(settingsEntities[0])) {
			outPhysicsDrawMode = dbg->physicsDrawMode;
		}
	}

	JPH::Vec3 sunDirection = {0.5f, 1.0f, 0.2f};
	float sunIntensity = 0.0f;

	auto sunEntities = reg.GetEntitiesWith<SunTagComponent>();
	if (!sunEntities.empty()) {
		Entity sunEnt = sunEntities[0];
		if (auto* trans = reg.Get<TransformComponent>(sunEnt)) {
			JPH::Mat44 worldMat = trans->GetMatrix();
			sunDirection = worldMat.GetColumn3(2);
		}
		if (auto* light = reg.Get<LightingSystem::LightComponent>(sunEnt)) {
			sunIntensity = light->intensity;
		}
	}
	sunDirection = sunDirection.Normalized();

	float yawRad = JPH::DegreesToRadians(cam.yaw);
	float pitchRad = JPH::DegreesToRadians(cam.pitch);
	JPH::Vec3 forward(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad),
					  JPH::Sin(yawRad) * JPH::Cos(pitchRad));
	forward = forward.Normalized();

	float shadowWidth = 80.0f;
	uint32_t shadowResolution = 2048;

	auto shadowEntities = reg.GetEntitiesWith<ShadowSettingsComponent>();
	if (!shadowEntities.empty()) {
		auto* shadowSettings = reg.Get<ShadowSettingsComponent>(shadowEntities[0]);
		shadowWidth = shadowSettings->shadowWidth;
		shadowResolution = shadowSettings->shadowResolution;
	}

	float texelSize = shadowWidth / static_cast<float>(shadowResolution);

	JPH::Vec3 shadowCenter = JPH::Vec3::sZero();
	auto playerEntities = reg.GetEntitiesWith<PlayerTagComponent>();
	if (!playerEntities.empty()) {
		if (auto* trans = reg.Get<TransformComponent>(playerEntities[0])) {
			shadowCenter = trans->position;
		}
	}

	shadowCenter.SetX(std::round(shadowCenter.GetX() / texelSize) * texelSize);
	shadowCenter.SetY(std::round(shadowCenter.GetY() / texelSize) * texelSize);
	shadowCenter.SetZ(std::round(shadowCenter.GetZ() / texelSize) * texelSize);

	JPH::Vec3 lightPos = shadowCenter + sunDirection * 150.0f;
	JPH::Mat44 lightView = Math::CreateLookAt(lightPos, shadowCenter, JPH::Vec3::sAxisY());

	float halfWidth = shadowWidth * 0.5f;
	JPH::Mat44 lightProj =
		Math::CreateOrtho(-halfWidth, halfWidth, -halfWidth, halfWidth, 0.1f, 400.0f);
	outShadowProjView = lightProj * lightView;

	cam.shadowFrustum.Update(outShadowProjView);

	JPH::Mat44 biasMatrix = {JPH::Vec4(0.5f, 0.0f, 0.0f, 0.0f), JPH::Vec4(0.0f, -0.5f, 0.0f, 0.0f),
							 JPH::Vec4(0.0f, 0.0f, 1.0f, 0.0f), JPH::Vec4(0.5f, 0.5f, 0.0f, 1.0f)};

	AAState aaState{};
	if (auto* taaComp = reg.Get<AASettingsComponent>(cameraEntity)) {
		aaState = taaComp->state;
	}

	FrameUniforms uniforms{};
	uniforms.viewProj = vp;
	uniforms.unjitteredViewProj = unjitteredVp;
	uniforms.prevUnjitteredViewProj = prevUnjitteredVp;
	uniforms.invViewProj = unjitteredVp.Inversed();
	std::memcpy(&uniforms.camPos[0], &cam.position, sizeof(float) * 3);
	JPH::Vec3 shaderLightDir = sunDirection;
	std::memcpy(&uniforms.lightDir[0], &shaderLightDir, sizeof(float) * 3);
	uniforms.lightDir[3] = sunIntensity;
	uniforms.lightCount =
		static_cast<uint32_t>(reg.GetEntitiesWith<LightingSystem::LightComponent>().size());
	uniforms.probeMin = probeMin;
	uniforms.probeMax = probeMax;
	uniforms.probePos = probePos;
	uniforms.jitterParams =
		JPH::Vec4(aaState.jitterX, aaState.jitterY, aaState.prevJitterX, aaState.prevJitterY);
	uniforms.enableRTR = enableRTR;
	uniforms.shadowWidth = shadowWidth;
	uniforms.shadowResolution = shadowResolution;
	if (!settingsEntities.empty()) {
		if (auto* pp = reg.Get<PostProcessSettingsComponent>(settingsEntities[0])) {
			uniforms.ambientExposure = pp->ambientExposure;
		}
	}

	rc.SetAAState(aaState);
	Renderer::SetFrameData(rc, cam, uniforms, outShadowProjView);
	Renderer::SetMatrices(rc, vp, unjitteredVp);

	const auto& mainVisible = engine.GetVisibleEntities();
	const auto& shadowVisible = engine.GetVisibleShadowEntities();

	auto IsInList = [](const JPH::Array<Entity>& list, Entity e) -> bool {
		return std::ranges::find(list, e) != list.end();
	};

	if (outPhysicsDrawMode == 0) {
		for (Entity e : reg.GetEntitiesWith<MeshComponent>()) {
			bool inMain = IsInList(mainVisible, e);
			bool inShadow = IsInList(shadowVisible, e);

			if (inMain || inShadow) {
				auto* mesh = reg.Get<MeshComponent>(e);
				if (mesh == nullptr) {
					continue;
				}

				DrawFlags drawFlags = mesh->flags;
				if (inMain) {
					drawFlags |= DrawFlags::VisibleInMain;
				}
				if (inShadow) {
					drawFlags |= DrawFlags::VisibleInShadow;
				}

				float roughness = -1.0f;
				float metallic = -1.0f;
				if (auto* pbr = reg.Get<PBRComponent>(e)) {
					roughness = pbr->roughness;
					metallic = pbr->metallic;
				}

				Renderer::Draw(rc, mesh->material, mesh->mesh,
							   {.transform = mesh->worldTransform,
								.prevTransform = mesh->prevTransform,
								.cullRadius = mesh->cullRadius,
								.localCenter = {mesh->localCenter.GetX(), mesh->localCenter.GetY(),
												mesh->localCenter.GetZ()},
								.jointOffset = mesh->jointOffset,
								.morphOffset = mesh->morphOffset,
								.activeMorphCount = mesh->activeMorphCount,
								.morphWeights = mesh->morphWeights.data(),
								.flags = drawFlags,
								.skinnedVertexBuffer = mesh->skinnedVertexBuffer,
								.roughness = roughness,
								.metallic = metallic});
			}
		}
	}

	CullingStats::TotalObjects = reg.GetEntitiesWith<MeshComponent>().size();
	CullingStats::CulledObjects = CullingStats::TotalObjects - visibleEntities.size();

	return {};
}

void RenderSystem::RenderDebug(Engine& engine, int physicsDrawMode) {
	auto& rc = engine.GetRenderContext();

	engine.GetCullingSystem().DrawDebugFrustum(engine);

	if (physicsDrawMode > 0) {
		ZHLN_PROFILE_SCOPE("Physics Debug Extract & Upload");

		static Material debugLineMat = {.pipeline = PipelineHandle::Invalid};
		static Material debugSolidMat = {.pipeline = PipelineHandle::Invalid};

		static RenderContext* s_LastContext = nullptr;
		if (&rc != s_LastContext) {
			debugLineMat.pipeline = PipelineHandle::Invalid;
			debugSolidMat.pipeline = PipelineHandle::Invalid;
			s_LastContext = &rc;
		}

		if (debugLineMat.pipeline == PipelineHandle::Invalid) {
			PipelineDesc lineDesc = {.vertexShaderData = ZHLN_Resource_BasicVertSpv,
									 .vertexShaderSize = ZHLN_Resource_BasicVertSpv_Len,
									 .fragShaderData = ZHLN_Resource_BasicFragSpv,
									 .fragShaderSize = ZHLN_Resource_BasicFragSpv_Len,
									 .doubleSided = true,
									 .alphaBlend = true,
									 .isLineList = true};
			debugLineMat = rc.CreateMaterial(lineDesc);
			debugLineMat.albedoIndex = 1;

			PipelineDesc solidDesc = lineDesc;
			solidDesc.isLineList = false;
			debugSolidMat = rc.CreateMaterial(solidDesc);
			debugSolidMat.albedoIndex = 1;
		}

		bool isWireframe = (physicsDrawMode == 1);
		auto debugData =
			Physics::GetDebugDrawData(engine.GetPhysicsContext(), true, true, isWireframe);

		std::vector<VertexPosition> debugPos;
		std::vector<VertexAttributes> debugAttr;

		if (isWireframe && debugData.lineCount > 0) {
			debugPos.reserve(debugData.lineCount);
			debugAttr.reserve(debugData.lineCount);
			for (size_t i = 0; i < debugData.lineCount; ++i) {
				const auto& jv = debugData.lines[i];
				debugPos.push_back({.position = {jv.x, jv.y, jv.z}});
				debugAttr.push_back({.normal = Math::PackNormal(0.0f, 1.0f, 0.0f),
									 .tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f),
									 .uv = Math::PackUV(0.0f, 0.0f),
									 .color = {.data = jv.color}});
			}
		} else if (!isWireframe && debugData.triangleCount > 0) {
			debugPos.reserve(debugData.triangleCount);
			debugAttr.reserve(debugData.triangleCount);
			for (size_t i = 0; i < debugData.triangleCount; ++i) {
				const auto& jv = debugData.triangles[i];
				debugPos.push_back({.position = {jv.x, jv.y, jv.z}});
				debugAttr.push_back({.normal = Math::PackNormal(0.0f, 1.0f, 0.0f),
									 .tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f),
									 .uv = Math::PackUV(0.0f, 0.0f),
									 .color = {.data = jv.color}});
			}
		}

		if (!debugPos.empty()) {
			rc.UploadDebugVertices(debugPos.data(), debugPos.size() * sizeof(VertexPosition),
								   debugAttr.data(), debugAttr.size() * sizeof(VertexAttributes),
								   static_cast<uint32_t>(debugPos.size()));

			Mesh debugMesh = {.posBuffer = rc.GetDebugMeshBuffer(),
							  .attrBuffer = rc.GetDebugMeshBuffer(),
							  .skinBuffer = BufferHandle::Invalid,
							  .indexBuffer = BufferHandle::Invalid,
							  .vertexCount = static_cast<uint32_t>(debugPos.size()),
							  .indexCount = 0};

			Renderer::Draw(rc, isWireframe ? debugLineMat : debugSolidMat, debugMesh,
						   {.transform = JPH::Mat44::sIdentity(),
							.prevTransform = JPH::Mat44::sIdentity(),
							.cullRadius = 10000.0f});
		}
	}
}

} // namespace ZHLN
