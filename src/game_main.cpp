// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/game_main.cpp

#include "Zahlen/Audio.hpp"
#include "Zahlen/CommandLine.hpp"
#include "Zahlen/Input.hpp"
#include "Zahlen/alife/Types.hpp"
#include "ecs/ECS.hpp"
#include "engine/FileWatcher.hpp"
#include "engine/Platform.hpp"
#include "engine/Resources.hpp"
#include "engine/system/CameraSystem.hpp"
#include "engine/system/InputSystem.hpp"
#include "engine/system/LightingSystem.hpp"
#include "engine/system/PhysicsStateSystem.hpp"
#include "engine/system/TargetCameraSystem.hpp"
#include "engine/system/TransformSystem.hpp"
#include "imgui.h"
#include "physics/Physics.hpp"
#include "physics/PhysicsDebug.hpp"

#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Scripting.h>
#include <Zahlen/Scripting.hpp>
#include <algorithm>
#include <chrono>
#include <detail/ControlFlow.hpp>
#include <engine/system/AnimationSystem.hpp>
#include <engine/system/ArticulationSystem.hpp>
#include <engine/system/CullingSystem.hpp>
#include <expected>
#include <physics/PhysicsWorld.hpp>
#include <string>
#include <thread>
#include <threading/Mutex.hpp>
#include <threading/TaskSystem.hpp>
#include <utility> // Required for std::move

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace ZHLN {

// ============================================================================
// ECS Components
// ============================================================================

struct PostProcessComponent {
	int giMode;
	float aoRadius;
	float aoBias;
	float aoPower;
	float giIntensity;
	int giSamples;
	int useLocalProbe;
	JPH::Vec3 probeMin;
	JPH::Vec3 probeMax;
	JPH::Vec3 probePos;
	float vignetteIntensity;
	float vignettePower;
	int enableSSR;
	int enableRTR;
};

void DrawConsole(ScriptRunner& runner);
void DrawProfiler(Engine& engine);
void MovementSystem(Engine& engine, float dt);
void AudioSystem(Engine& engine, float dt);
void DrawOrientationGizmo(const ZHLN::Camera& cam);
void DrawInventoryShell(ScriptRunner& runner);
void DrawECSProfiler();

namespace {

struct FrustumEdge {
	int start;
	int end;
};

constexpr std::array<FrustumEdge, 12> s_FrustumEdges = {{
	{.start = 0, .end = 1},
	{.start = 1, .end = 2},
	{.start = 2, .end = 3},
	{.start = 3, .end = 0}, // Near Plane loop
	{.start = 4, .end = 5},
	{.start = 5, .end = 6},
	{.start = 6, .end = 7},
	{.start = 7, .end = 4}, // Far Plane loop
	{.start = 0, .end = 4},
	{.start = 1, .end = 5},
	{.start = 2, .end = 6},
	{.start = 3, .end = 7} // Near-to-Far connection lines
}};

} // namespace

// ============================================================================
// Modular Systems
// ============================================================================

void UISystem(Engine& engine, ScriptRunner& scriptRunner) {
	if (engine.GetWindow().IsTTY()) {
		return;
	}

	ZHLN_GameState state =
		*static_cast<ZHLN_GameState*>(ZHLN_GetGameState(reinterpret_cast<ZHLN_Engine*>(&engine)));

	ZHLN::DrawConsole(scriptRunner);
	ZHLN::DrawInventoryShell(scriptRunner);
	ZHLN::DrawProfiler(engine);
	ZHLN::DrawOrientationGizmo(engine.GetCamera());
	ZHLN::DrawECSProfiler();

	ImGui::Begin("Lighting Workspace Controller");
	ImGui::SeparatorText("Physics Debug");
	ImGui::RadioButton("Hidden", &state.physicsDrawMode, 0);
	ImGui::SameLine();
	ImGui::RadioButton("Wireframe", &state.physicsDrawMode, 1);
	ImGui::SameLine();
	ImGui::RadioButton("Solid", &state.physicsDrawMode, 2);
	ImGui::Text("Specular Mips & Area Lights Debugger");
	ImGui::Separator();
	ImGui::SliderFloat("Sphere Light Radius", &state.sphereLightRadius, 0.0f, 5.0f);
	ImGui::SliderFloat("Cyan Intensity", &state.light1Intensity, 0.0f, 500.0f);
	ImGui::SliderFloat("Magenta Intensity", &state.light2Intensity, 0.0f, 500.0f);
	ImGui::Separator();
	ImGui::SliderFloat("Floor Roughness", &state.floorRoughness, 0.0f, 1.0f);
	ImGui::SliderFloat("Floor Metallic", &state.floorMetallic, 0.0f, 1.0f);

	ImGui::SeparatorText("Parallax-Corrected Local Reflection Probe");
	bool useProbe = state.useLocalProbe != 0;
	if (ImGui::Checkbox("Enable Box Projection", &useProbe)) {
		state.useLocalProbe = useProbe ? 1 : 0;
	}
	if (state.useLocalProbe != 0) {
		std::array<float, 3> minArr = {state.probeMin[0], state.probeMin[1], state.probeMin[2]};
		std::array<float, 3> maxArr = {state.probeMax[0], state.probeMax[1], state.probeMax[2]};
		std::array<float, 3> posArr = {state.probePos[0], state.probePos[1], state.probePos[2]};

		if (ImGui::DragFloat3("Box Min", minArr.data(), 0.1f, -100.0f, 100.0f, "%.1fm")) {
			state.probeMin[0] = minArr[0];
			state.probeMin[1] = minArr[1];
			state.probeMin[2] = minArr[2];
		}
		if (ImGui::DragFloat3("Box Max", maxArr.data(), 0.1f, -100.0f, 100.0f, "%.1fm")) {
			state.probeMax[0] = maxArr[0];
			state.probeMax[1] = maxArr[1];
			state.probeMax[2] = maxArr[2];
		}
		if (ImGui::DragFloat3("Probe Position", posArr.data(), 0.1f, -100.0f, 100.0f, "%.1fm")) {
			state.probePos[0] = posArr[0];
			state.probePos[1] = posArr[1];
			state.probePos[2] = posArr[2];
		}
	}

	ImGui::SeparatorText("Ambient Occlusion & Global Illumination");
	const char* giModesList[] = {"Off", "SSAO (Ambient Occlusion)", "SSGI (Screen Space GI)",
								 "HBAO (Horizon-Based AO)", "GTAO (Ground Truth AO)"};
	ImGui::Combo("GI Mode", &state.giMode, giModesList, IM_ARRAYSIZE(giModesList));

	if (state.giMode == 1) {
		ImGui::SliderFloat("AO Radius", &state.aoRadius, 0.05f, 2.5f, "%.2fm");
		ImGui::SliderFloat("AO Bias", &state.aoBias, 0.001f, 0.2f, "%.3f");
		ImGui::SliderFloat("AO Contrast", &state.aoPower, 0.5f, 5.0f, "%.1fx");
		ImGui::SliderInt("AO Samples", &state.giSamples, 2, 32);
	} else if (state.giMode == 2) {
		ImGui::SliderFloat("Bounce Radius", &state.aoRadius, 0.05f, 2.5f, "%.2fm");
		ImGui::SliderFloat("Bounce Bias", &state.aoBias, 0.001f, 0.2f, "%.3f");
		ImGui::SliderFloat("GI Bounce Intensity", &state.giIntensity, 0.1f, 5.0f, "%.1fx");
		ImGui::SliderInt("GI Samples", &state.giSamples, 2, 32);
	} else if (state.giMode == 3 || state.giMode == 4) {
		ImGui::SliderFloat("Search Radius", &state.aoRadius, 0.05f, 3.0f, "%.2fm");
		ImGui::SliderFloat("Acne Bias", &state.aoBias, 0.001f, 0.2f, "%.3f");
		ImGui::SliderFloat("Shadow Contrast", &state.aoPower, 0.5f, 6.0f, "%.1fx");
		ImGui::SliderInt("Search Steps", &state.giSamples, 4, 32);
	}

	ImGui::SeparatorText("Camera Vignette");
	ImGui::SliderFloat("Vignette Intensity", &state.vignetteIntensity, 0.0f, 2.5f, "%.2f");
	if (state.vignetteIntensity > 0.0f) {
		ImGui::SliderFloat("Vignette Power", &state.vignettePower, 0.1f, 6.0f, "%.2f");
	}

	bool useSsr = state.enableSSR != 0;
	if (ImGui::Checkbox("Enable SSR", &useSsr)) {
		state.enableSSR = useSsr ? 1 : 0;
	}

	bool useRtr = state.enableRTR != 0;
	if (ImGui::Checkbox("Enable Hardware RTR", &useRtr)) {
		state.enableRTR = useRtr ? 1 : 0;
	}

	ImGui::End();

	ZHLN_SetGameState(reinterpret_cast<ZHLN_Engine*>(&engine), &state);

	auto& reg = engine.GetRegistry();
	for (Entity e : reg.GetEntitiesWith<PostProcessComponent>()) {
		if (auto* pp = reg.Get<PostProcessComponent>(e)) {
			pp->giMode = state.giMode;
			pp->aoRadius = state.aoRadius;
			pp->aoBias = state.aoBias;
			pp->aoPower = state.aoPower;
			pp->giIntensity = state.giIntensity;
			pp->giSamples = state.giSamples;
			pp->useLocalProbe = state.useLocalProbe;
			pp->probeMin = JPH::Vec3(state.probeMin[0], state.probeMin[1], state.probeMin[2]);
			pp->probeMax = JPH::Vec3(state.probeMax[0], state.probeMax[1], state.probeMax[2]);
			pp->probePos = JPH::Vec3(state.probePos[0], state.probePos[1], state.probePos[2]);
			pp->vignetteIntensity = state.vignetteIntensity;
			pp->vignettePower = state.vignettePower;
			pp->enableSSR = state.enableSSR;
			pp->enableRTR = state.enableRTR;
		}
	}
}

void PostProcessSystem(Engine& engine) {
	auto& reg = engine.GetRegistry();
	auto& rc = engine.GetRenderContext();

	for (Entity e : reg.GetEntitiesWith<PostProcessComponent>()) {
		if (auto* pp = reg.Get<PostProcessComponent>(e)) {
			Renderer::SetGISettings(rc, {.mode = pp->giMode,
										 .aoRadius = pp->aoRadius,
										 .aoBias = pp->aoBias,
										 .aoPower = pp->aoPower,
										 .giIntensity = pp->giIntensity,
										 .giSamples = pp->giSamples,
										 .vignetteIntensity = pp->vignetteIntensity,
										 .vignettePower = pp->vignettePower,
										 .enableSSR = pp->enableSSR ? 1 : 0,
										 .enableRTR = pp->enableRTR ? 1 : 0});
		}
	}
}

void DebugDrawSystem(Engine& engine, CullingSystem& cullingSystem) {
	if (!CullingStats::FreezeFrustum) {
		return;
	}

	auto& rc = engine.GetRenderContext();
	ZHLN_GameState state =
		*static_cast<ZHLN_GameState*>(ZHLN_GetGameState(reinterpret_cast<ZHLN_Engine*>(&engine)));

	if (state.debugLineVbo != 0) {
		Mesh debugMesh = {.vertexBuffer = static_cast<BufferHandle>(state.debugLineVbo),
						  .vertexCount = 36};
		Material debugMat = {.pipeline = static_cast<PipelineHandle>(state.debugLinePipeline),
							 .albedoIndex = state.debugLineAlbedo};

		debugMat.baseColorFactor[0] = 0.0f;
		debugMat.baseColorFactor[1] = 1.0f;
		debugMat.baseColorFactor[2] = 1.0f;
		debugMat.baseColorFactor[3] = 1.0f;

		auto frustumCorners = cullingSystem.GetFrustumCorners();
		for (auto s_FrustumEdge : s_FrustumEdges) {
			JPH::Vec3 pA = frustumCorners[s_FrustumEdge.start];
			JPH::Vec3 pB = frustumCorners[s_FrustumEdge.end];

			JPH::Vec3 v = pB - pA;
			float len = v.Length();
			if (len < 1e-4f) {
				continue;
			}

			JPH::Vec3 dir = v / len;
			JPH::Vec3 mid = (pA + pB) * 0.5f;

			JPH::Quat rot = JPH::Quat::sFromTo(JPH::Vec3::sAxisZ(), dir);
			JPH::Mat44 lineTransform = Math::CreateTransform(mid, rot, JPH::Vec3(1.0f, 1.0f, len));

			Renderer::Draw(
				rc, debugMat, debugMesh,
				{.transform = lineTransform, .prevTransform = lineTransform, .cullRadius = len});
		}
	}
}

std::expected<void, RenderFrameResult> RenderSystem(Engine& engine, CullingSystem& cullingSystem,
													const JPH::Array<Entity>& visibleEntities) {
	auto& rc = engine.GetRenderContext();
	auto& reg = engine.GetRegistry();
	auto& cam = engine.GetCamera();

	JPH::Mat44 vp{};
	JPH::Mat44 unjitteredVp{};
	JPH::Mat44 prevUnjitteredVp{};

	auto cameraEntities = reg.GetEntitiesWith<MainCameraTagComponent>();
	if (cameraEntities.empty()) {
		return std::unexpected(
			RenderFrameResult::Error); // <-- FIXED: Early exit returning expected error
	}
	// Capture monadic results on BeginFrame
	auto begin_res = rc.BeginFrame();
	if (!begin_res) {
		return std::unexpected(begin_res.error());
	}
	Entity cameraEntity = cameraEntities[0];

	if (auto* cComp = reg.Get<CameraSystem::CameraComponent>(cameraEntity)) {
		vp = cComp->viewProj;
		unjitteredVp = cComp->unjitteredViewProj;
		prevUnjitteredVp = cComp->prevUnjitteredViewProj;
	} else {
		return std::unexpected(
			RenderFrameResult::Error); // <-- FIXED: Early exit returning expected error
	}

	// 1. Fetch Game State at the top so we can use the debug flag early
	ZHLN_GameState state =
		*static_cast<ZHLN_GameState*>(ZHLN_GetGameState(reinterpret_cast<ZHLN_Engine*>(&engine)));

	int enableRTR = 0;
	JPH::Vec4 probeMin(0, 0, 0, 0);
	JPH::Vec4 probeMax(0, 0, 0, 0);
	JPH::Vec4 probePos(0, 0, 0, 0);

	auto settingsEntities = reg.GetEntitiesWith<GlobalSettingsTagComponent>();
	if (!settingsEntities.empty()) {
		if (auto* pp = reg.Get<PostProcessComponent>(settingsEntities[0])) {
			enableRTR = pp->enableRTR;
			probeMin = JPH::Vec4(pp->probeMin.GetX(), pp->probeMin.GetY(), pp->probeMin.GetZ(),
								 pp->useLocalProbe ? 1.0f : 0.0f);
			probeMax =
				JPH::Vec4(pp->probeMax.GetX(), pp->probeMax.GetY(), pp->probeMax.GetZ(), 0.0f);
			probePos =
				JPH::Vec4(pp->probePos.GetX(), pp->probePos.GetY(), pp->probePos.GetZ(), 0.0f);
		}
	}

	JPH::Vec3 sunDirection = {0.5f, 1.0f, 0.2f};
	JPH::Mat44 lightView =
		Math::CreateLookAt(sunDirection * 100.0f, {0.0f, 0.0f, 0.0f}, JPH::Vec3::sAxisY());
	JPH::Mat44 lightProj = Math::CreateOrtho(-50.0f, 50.0f, -50.0f, 50.0f, 0.1f, 200.0f);
	JPH::Mat44 shadowProjView = lightProj * lightView;

	JPH::Mat44 biasMatrix = {JPH::Vec4(0.5f, 0.0f, 0.0f, 0.0f), JPH::Vec4(0.0f, -0.5f, 0.0f, 0.0f),
							 JPH::Vec4(0.0f, 0.0f, 1.0f, 0.0f), JPH::Vec4(0.5f, 0.5f, 0.0f, 1.0f)};
	JPH::Mat44 lightSpaceBiased = biasMatrix * shadowProjView;

	AAState aaState{};
	if (auto* taaComp = reg.Get<AASettingsComponent>(cameraEntity)) {
		aaState = taaComp->state;
	}

	FrameUniforms uniforms{};
	uniforms.viewProj = vp;
	uniforms.unjitteredViewProj = unjitteredVp;
	uniforms.prevUnjitteredViewProj = prevUnjitteredVp;
	uniforms.lightSpaceMatrix = lightSpaceBiased;
	uniforms.invViewProj = unjitteredVp.Inversed();
	std::memcpy(&uniforms.camPos[0], &cam.position, sizeof(float) * 3);
	std::memcpy(&uniforms.lightDir[0], &sunDirection, sizeof(float) * 3);
	uniforms.lightCount = reg.GetEntitiesWith<LightingSystem::LightComponent>().size();
	uniforms.probeMin = probeMin;
	uniforms.probeMax = probeMax;
	uniforms.probePos = probePos;
	uniforms.jitterParams =
		JPH::Vec4(aaState.jitterX, aaState.jitterY, aaState.prevJitterX, aaState.prevJitterY);
	uniforms.enableRTR = enableRTR;

	rc.SetAAState(aaState);
	Renderer::SetFrameData(rc, uniforms, shadowProjView);
	Renderer::SetMatrices(rc, vp, unjitteredVp);

	// 2. Wrap normal entity rendering with a conditional block
	if (state.physicsDrawMode == 0) { // Only draw standard world when debug is Off
		for (Entity e : visibleEntities) {
			auto* mesh = reg.Get<MeshComponent>(e);
			if (mesh == nullptr) {
				continue;
			}

			DrawFlags flags = DrawFlags::None;
			if (mesh->isSkinned) {
				flags |= DrawFlags::Skinned;
			}

			Renderer::Draw(rc, mesh->material, mesh->mesh,
						   {.transform = mesh->worldTransform,
							.prevTransform = mesh->prevTransform,
							.cullRadius = mesh->cullRadius,
							.jointOffset = mesh->jointOffset,
							.morphOffset = mesh->morphOffset,
							.activeMorphCount = mesh->activeMorphCount,
							.morphWeights = mesh->morphWeights.data(),
							.flags = flags});
		}
	}

	CullingStats::TotalObjects = reg.GetEntitiesWith<MeshComponent>().size();
	CullingStats::CulledObjects = CullingStats::TotalObjects - visibleEntities.size();

	for (Entity e : reg.GetEntitiesWith<TextComponent>()) {
		auto* text = reg.Get<TextComponent>(e);
		if (text->mesh.vertexBuffer == BufferHandle::Invalid) {
			text->mesh = GUI::CreateTextMesh(rc, text->text.c_str(), text->x, text->y, text->scale,
											 text->color);
		}
		Renderer::DrawUI(rc, text->mesh, text->fontIndex);
	}

	DebugDrawSystem(engine, cullingSystem);
	// -------------------------------------------------------------------------
	// [FAST PERSISTENTLY MAPPED PHYSICS DEBUG RENDERER]
	// -------------------------------------------------------------------------

	if (state.physicsDrawMode > 0) {
		ZHLN_PROFILE_SCOPE("Physics Debug Extract & Upload"); // Now tracked on the CPU profiler!

		static Material debugLineMat = {.pipeline = PipelineHandle::Invalid};
		static Material debugSolidMat = {.pipeline = PipelineHandle::Invalid};

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

		bool isWireframe = (state.physicsDrawMode == 1);
		auto debugData =
			Physics::GetDebugDrawData(engine.GetPhysicsContext(), true, true, isWireframe);

		std::vector<Vertex> debugVerts;

		if (isWireframe && debugData.lineCount > 0) {
			debugVerts.reserve(debugData.lineCount);
			for (size_t i = 0; i < debugData.lineCount; ++i) {
				const auto& jv = debugData.lines[i];
				Vertex v{};
				v.position[0] = jv.x;
				v.position[1] = jv.y;
				v.position[2] = jv.z;
				v.color.data = jv.color;
				v.normal = Math::PackNormal(0, 1, 0);
				debugVerts.push_back(v);
			}
		} else if (!isWireframe && debugData.triangleCount > 0) {
			debugVerts.reserve(debugData.triangleCount);
			for (size_t i = 0; i < debugData.triangleCount; ++i) {
				const auto& jv = debugData.triangles[i];
				Vertex v{};
				v.position[0] = jv.x;
				v.position[1] = jv.y;
				v.position[2] = jv.z;
				v.color.data = jv.color;
				v.normal = Math::PackNormal(0, 1, 0);
				debugVerts.push_back(v);
			}
		}

		if (!debugVerts.empty()) {
			// Instantly upload vertices directly without triggering any Vulkan queue submissions
			rc.UploadDebugVertices(debugVerts.data(), debugVerts.size() * sizeof(Vertex),
								   (uint32_t)debugVerts.size());

			Mesh debugMesh = {.vertexBuffer = rc.GetDebugMeshBuffer(),
							  .vertexCount = (uint32_t)debugVerts.size()};

			Renderer::Draw(rc, isWireframe ? debugLineMat : debugSolidMat, debugMesh,
						   {.transform = JPH::Mat44::sIdentity(),
							.prevTransform = JPH::Mat44::sIdentity(),
							.cullRadius = 10000.0f});
		}
	}
	// -------------------------------------------------------------------------

	auto end_res = rc.EndFrame();
	if (!end_res) {
		return std::unexpected(end_res.error());
	}

	return {}; // <-- FIXED: Added final success return statement
}

// ============================================================================
// GAME APPLICATION INTERFACE IMPLEMENTATION
// ============================================================================

bool InitializeGame(Engine& engine, ScriptRunner& scriptRunner) {
	auto& rc = engine.GetRenderContext();
	auto& reg = engine.GetRegistry();
	auto& pc = engine.GetPhysicsContext();

	// 1. Create a thin procedural box mesh (0.02m x 0.02m x 1.0m) to represent our lines.
	// A half-extent of 0.5 on the Z-axis gives a total length of 1.0, which scales correctly with
	// line lengths.
	Mesh lineMesh = AssetFactory::CreateBox(rc, {0.01f, 0.01f, 0.5f}, {0.0f, 1.0f, 1.0f, 1.0f});
	Material lineMat = AssetFactory::CreateBasicMaterial(rc);

	ZHLN_GameState defaultState{};
	defaultState.giMode = 1;
	defaultState.aoRadius = 0.5f;
	defaultState.aoBias = 0.05f;
	defaultState.aoPower = 1.8f;
	defaultState.giIntensity = 1.2f;
	defaultState.giSamples = 8;
	defaultState.useLocalProbe = 1;
	defaultState.probeMin[0] = -22.0f;
	defaultState.probeMin[1] = 0.0f;
	defaultState.probeMin[2] = -22.0f;
	defaultState.probeMax[0] = 22.0f;
	defaultState.probeMax[1] = 12.0f;
	defaultState.probeMax[2] = 22.0f;
	defaultState.probePos[0] = 0.0f;
	defaultState.probePos[1] = 4.0f;
	defaultState.probePos[2] = 0.0f;
	defaultState.vignetteIntensity = 1.10f;
	defaultState.vignettePower = 1.50f;
	defaultState.enableSSR = 1;
	defaultState.floorRoughness = 0.15f;
	defaultState.floorMetallic = 0.95f;
	defaultState.sphereLightRadius = 1.5f;
	defaultState.light1Intensity = 180.0f;
	defaultState.light2Intensity = 180.0f;
	defaultState.enableTAA = 1;
	defaultState.taaFeedback = 0.95f;

	// 2. Populate debug line state parameters
	defaultState.debugLineVbo = static_cast<uint64_t>(lineMesh.vertexBuffer);
	defaultState.debugLinePipeline = static_cast<uint64_t>(lineMat.pipeline);
	defaultState.debugLineAlbedo = lineMat.albedoIndex;
	defaultState.physicsDrawMode = 0;

	ZHLN_SetGameState(reinterpret_cast<ZHLN_Engine*>(&engine), &defaultState);

	reg.RegisterComponents<TransformComponent, MeshComponent, PhysicsComponent,
						   PhysicsStateComponent, MovementComponent, ALife::ALifeComponent,
						   RagdollComponent, NameComponent, TargetCameraComponent,
						   InputSystem::InputComponent, LightingSystem::LightComponent,
						   PostProcessComponent, CameraSystem::CameraComponent, PlayerTagComponent,
						   MainCameraTagComponent, GlobalSettingsTagComponent, AASettingsComponent,
						   TextComponent, UISettingsComponent, AudioSourceComponent>();

	auto groundShape =
		Physics::GetOrCreateShape(pc, Physics::ShapeType::Plane, 0.0f, 1.0f, 0.0f, 0.0f);
	Entity ground = reg.Create();
	reg.Add(ground,
			PhysicsComponent{Physics::CreateRigidBody(
				pc, groundShape, {0, 0, 0}, JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0)});
	reg.Add(ground, PhysicsStateComponent{});

	Entity playerEntity = reg.Create();
	reg.Add(playerEntity, PlayerTagComponent{});
	reg.Add(playerEntity, TransformComponent{.position = {0.0f, 3.0f, 0.0f}});
	reg.Add(playerEntity, MovementComponent{});
	reg.Add(playerEntity, InputSystem::InputComponent{});
	Entity charPhys = Physics::CreateCharacter(pc, JPH::RVec3(0.0f, 3.0f, 0.0f));
	reg.Add(playerEntity, PhysicsComponent{charPhys});
	reg.Add(playerEntity, PhysicsStateComponent{.currPosition = {0.0f, 3.0f, 0.0f},
												.prevPosition = {0.0f, 3.0f, 0.0f}});

	Entity cameraEntity = reg.Create();
	reg.Add(cameraEntity, MainCameraTagComponent{});
	reg.Add(cameraEntity, TargetCameraComponent{.target = playerEntity,
												.distance = 4.5f,
												.targetDistance = 4.5f,
												.yaw = -90.0f,
												.pitch = -10.0f,
												.stiffness = 15.0f,
												.vignetteIntensity = 1.10f,
												.vignettePower = 1.50f,
												.fov = 45.0f,
												.targetFov = 45.0f});
	reg.Add(cameraEntity, InputSystem::InputComponent{});
	reg.Add(cameraEntity, CameraSystem::CameraComponent{});
	reg.Add(cameraEntity,
			AASettingsComponent{.state = {.mode = AAMode::TAA, .taaFeedback = 0.95f}});

	Entity settingsEntity = reg.Create();
	reg.Add(settingsEntity, GlobalSettingsTagComponent{});
	reg.Add(settingsEntity,
			PostProcessComponent{
				.giMode = defaultState.giMode,
				.aoRadius = defaultState.aoRadius,
				.aoBias = defaultState.aoBias,
				.aoPower = defaultState.aoPower,
				.giIntensity = defaultState.giIntensity,
				.giSamples = defaultState.giSamples,
				.useLocalProbe = defaultState.useLocalProbe,
				.probeMin = JPH::Vec3(defaultState.probeMin[0], defaultState.probeMin[1],
									  defaultState.probeMin[2]),
				.probeMax = JPH::Vec3(defaultState.probeMax[0], defaultState.probeMax[1],
									  defaultState.probeMax[2]),
				.probePos = JPH::Vec3(defaultState.probePos[0], defaultState.probePos[1],
									  defaultState.probePos[2]),
				.vignetteIntensity = defaultState.vignetteIntensity,
				.vignettePower = defaultState.vignettePower,
				.enableSSR = defaultState.enableSSR,
				.enableRTR = defaultState.enableRTR});

	Entity areaLight = reg.Create();
	reg.Add(areaLight, LightingSystem::LightComponent{.type = 3,
													  .color = {1.0f, 0.8f, 0.6f},
													  .intensity = 5.0f,
													  .radius = 0.0f,
													  .direction = {0.0f, -1.0f, 0.0f},
													  .range = 0.0f,
													  .points = {{-2.0f, 5.0f, -2.0f, 0.0f},
																 {2.0f, 5.0f, -2.0f, 0.0f},
																 {2.0f, 5.0f, 2.0f, 0.0f},
																 {-2.0f, 5.0f, 2.0f, 0.0f}},
													  .twoSided = 0});

	Entity pt1 = reg.Create();
	reg.Add(pt1,
			LightingSystem::LightComponent{.type = 1,
										   .color = {0.0f, 0.5f, 1.0f},
										   .intensity = defaultState.light1Intensity,
										   .radius = defaultState.sphereLightRadius,
										   .direction = {0.0f, 0.0f, 0.0f},
										   .range = 0.0f,
										   .points = {},
										   .twoSided = 0},
			TransformComponent{.position = {-5.0f, 4.0f, 0.0f}});

	Entity pt2 = reg.Create();
	reg.Add(pt2,
			LightingSystem::LightComponent{.type = 1,
										   .color = {1.0f, 0.0f, 0.5f},
										   .intensity = defaultState.light2Intensity,
										   .radius = defaultState.sphereLightRadius,
										   .direction = {0.0f, 0.0f, 0.0f},
										   .range = 0.0f,
										   .points = {},
										   .twoSided = 0},
			TransformComponent{.position = {5.0f, 4.0f, 0.0f}});

	Entity uiSettings = reg.Create();
	reg.Add(uiSettings,
			UISettingsComponent{.defaultFontAtlasIdx = AssetFactory::CreateFontAtlasTexture(rc)});

	Entity textEnt = reg.Create();
	reg.Add(
		textEnt,
		TextComponent{.text = "Zahlen Engine - TADC Dorm Showcase",
					  .x = 25.0f,
					  .y = 25.0f,
					  .scale = 2.5f,
					  .color = {0.9f, 0.1f, 0.1f, 1.0f},
					  .fontIndex = reg.Get<UISettingsComponent>(uiSettings)->defaultFontAtlasIdx});

	return true;
}

void UpdateGame(Engine& engine, float dt, float& physicsAccumulator, ScriptRunner& scriptRunner,
				FileWatcher& gameplayWatcher, InputSystem& inputSystem,
				AnimationSystem& animationSystem, ArticulationSystem& articulationSystem,
				TransformSystem& transformSystem) {
	inputSystem.Update(engine);
	UISystem(engine, scriptRunner);
	PostProcessSystem(engine);
	inputSystem.PlayerInputTranslate(engine, engine.GetCamera());

	if constexpr (isDev) {
		static float watcherAccumulator = 0.0f;
		watcherAccumulator += dt;
		if (watcherAccumulator >= 2.0f) {
			watcherAccumulator = 0.0f;
			if (gameplayWatcher.CheckModified()) {
				scriptRunner.ReloadFile("scripts/gameplay.lua");
			}
		}
	}

	float cappedDt = std::min(dt, 0.1f);
	physicsAccumulator += cappedDt;
	constexpr float targetDt = 1.0f / 60.0f;

	physicsAccumulator = std::min(physicsAccumulator, targetDt * 4.0f);

	{
		ZHLN_PROFILE_SCOPE("ECS System: Physics & Movement");
		while (physicsAccumulator >= targetDt) {
			ZHLN::MovementSystem(engine, targetDt);
			engine.GetPhysicsContext().Step(targetDt);
			ZHLN::PhysicsStateSystem::WriteBack(engine);
			physicsAccumulator -= targetDt;
		}
	}

	float alpha = physicsAccumulator / targetDt;

	{
		ZHLN_PROFILE_SCOPE("ECS System: Visual Interpolation");
		ZHLN::VisualInterpolationSystem::Update(engine, alpha);
	}

	{
		ZHLN_PROFILE_SCOPE("ECS System: Script/Lua Update");
		scriptRunner.CallUpdate(&engine, dt);
	}

	{
		ZHLN_PROFILE_SCOPE("ECS System: Animation Update");
		animationSystem.UpdateAnimations(engine.GetRenderContext(), engine.GetRegistry(), dt);
	}

	{
		ZHLN_PROFILE_SCOPE("ECS System: Articulation/Ragdoll");
		articulationSystem.Update(engine, dt);
	}

	{
		ZHLN_PROFILE_SCOPE("ECS System: Resolve Transforms");
		transformSystem.ResolveTransforms(engine.GetRegistry());
	}
}

std::expected<void, RenderFrameResult> RenderGame(Engine& engine, float frameTime,
												  float physicsAccumulator,
												  CullingSystem& cullingSystem,
												  LightingSystem& lightingSystem) {
	constexpr float targetDt = 1.0f / 60.0f;
	float alpha = physicsAccumulator / targetDt;

	TargetCameraSystem camSys;
	camSys.Update(engine, frameTime, alpha);

	CameraSystem standardCamSys;
	standardCamSys.Update(engine, frameTime, alpha);

	JPH::Array<Entity> visibleEntities;
	cullingSystem.Update<false>(engine, visibleEntities);
	lightingSystem.Update(engine, frameTime);

	// Capture the monadic frame result
	auto render_res = RenderSystem(engine, cullingSystem, visibleEntities);
	if (!render_res) {
		return std::unexpected(render_res.error());
	}

	{
		ZHLN_PROFILE_SCOPE("ECS System: Audio Update");
		ZHLN::AudioSystem(engine, frameTime);
	}

	{
		ZHLN_PROFILE_SCOPE("ECS System: Update Transform History");
		TransformSystem ts;
		ts.UpdateTransformHistory(engine.GetRegistry());
	}

	return {};
}

} // namespace ZHLN

using namespace ZHLN;

namespace {

std::expected<std::unique_ptr<Engine>, EngineError> InitializeEngine(CommandLineOptions options) {
	Platform::Init();
	ZHLN::SetupSignalHandler();
	TaskSystem::Init();

	uint32_t w = options.fullscreen ? 0 : 1280;
	uint32_t h = options.fullscreen ? 0 : 720;

	EngineConfig config{
		.physics = {.maxBodies = 5000,
					.maxBodyPairs = 10000,
					.maxContactConstraints = 10000,
					.tempAllocatorSize = 64 * 1024 * 1024},
		.render = {.appName = "Zahlen Engine",
				   .width = w,
				   .height = h,
				   .vsync = options.vsync,
				   .fullscreen = options.fullscreen,
				   .enableValidation = options.enableValidation},
	};

	const char* initError = nullptr;
	auto engine = Engine::Create(config, &initError);

	if (!engine) {
		return std::unexpected(EngineError{
			.msg = (initError != nullptr) ? initError : "Unknown engine initialization error.",
			.code = EXIT_FAILURE});
	}

	engine->GetWindow().Focus();
	return engine;
}

std::expected<int, EngineError> RunEngineLoop(std::unique_ptr<Engine> engine, uint32_t fpsLimit) {
	ScriptRunner scriptRunner;
	FileWatcher gameplayWatcher("scripts/gameplay.lua");
	ArticulationSystem articulationSystem;
	AnimationSystem animationSystem;
	TransformSystem transformSystem;
	LightingSystem lightingSystem;
	CullingSystem cullingSystem;
	InputSystem inputSystem;

	if (!ZHLN::InitializeGame(*engine, scriptRunner)) {
		return std::unexpected(
			EngineError{.msg = "Game failed to initialize.", .code = EXIT_FAILURE});
	}

	// 1. Original safe warm-up frames (balanced with rc.BeginFrame inside RenderSystem)
	for (int i = 0; i < 3; ++i) {
		engine->ProcessEvents();

		// Capture and check the monadic result during warm-up
		auto res = ZHLN::RenderGame(*engine, 0.016f, 0.0f, cullingSystem, lightingSystem);
		if (!res) {
			if (res.error() == RenderFrameResult::DeviceLost) {
				engine->HandleDeviceLost(); // Safe, parameterless rebuild
			}
		}
	}

	ZHLN::Log("Window active and presenting. Loading scene assets...");
	scriptRunner.RunFile("scripts/gameplay.lua");

	float physicsAccumulator = 0.0f;
	const double targetFrameTime = fpsLimit > 0 ? 1.0 / static_cast<double>(fpsLimit) : 0.0;

	auto frameStart = std::chrono::high_resolution_clock::now();

	while (engine->IsRunning()) {
		engine->ProcessEvents();

		if (engine->GetInput().IsKeyDown(KeyCode::Escape)) {
			engine->GetWindow().Close();
			break;
		}

		// Calculate raw elapsed time
		auto frameEnd = std::chrono::high_resolution_clock::now();
		double elapsed = std::chrono::duration<double>(frameEnd - frameStart).count();
		frameStart = std::chrono::high_resolution_clock::now();

		// 1. Identify which refresh rate target we are actually near using the raw elapsed time.
		// This prevents the low-pass filter from getting trapped in a feedback loop at high frame
		// rates.
		double target = elapsed;
		constexpr double snapTargets[] = {1.0 / 60.0,  1.0 / 75.0,	1.0 / 90.0, 1.0 / 120.0,
										  1.0 / 144.0, 1.0 / 240.0, 1.0 / 360.0};
		for (double t : snapTargets) {
			if (std::abs(elapsed - t) < 0.001) { // 1.0ms threshold against raw elapsed
				target = t;
				break;
			}
		}

		// 2. Smooth the detected target with a low-pass filter to eliminate swapchain presentation
		// jitter
		static double smoothedElapsed = 0.0166667; // Fallback to 60fps
		smoothedElapsed = (smoothedElapsed * 0.9) + (target * 0.1);

		float frameTime = std::min(static_cast<float>(smoothedElapsed), 0.1f);

		if (engine->GetInput().NeedsResize()) {
			engine->GetRenderContext().SetResolution(engine->GetInput().GetNewSize());
			engine->GetInput().ClearResizeFlag();
			if (!engine->GetWindow().IsTTY()) {
				ImGui::EndFrame(); // <--- Add this!
			}
			continue;
		}

		ZHLN::UpdateGame(*engine, frameTime, physicsAccumulator, scriptRunner, gameplayWatcher,
						 inputSystem, animationSystem, articulationSystem, transformSystem);
		auto render_res =
			ZHLN::RenderGame(*engine, frameTime, physicsAccumulator, cullingSystem, lightingSystem);
		if (!render_res) {
			if (render_res.error() == RenderFrameResult::DeviceLost) {
				engine->HandleDeviceLost();

				// Re-run the scene setup script to reload and upload meshes/textures to the fresh
				// Vulkan Context
				scriptRunner.ReloadFile("scripts/gameplay.lua");
			} else {
				// Handle other non-fatal errors (OutOfDate, Suboptimal, resizing, etc.)
			}
		}

		if (fpsLimit > 0) {
			auto now = std::chrono::high_resolution_clock::now();
			double frameElapsed = std::chrono::duration<double>(now - frameStart).count();
			if (frameElapsed < targetFrameTime) {
				double sleepTime = targetFrameTime - frameElapsed;
				if (sleepTime > 0.002) {
					std::this_thread::sleep_for(
						std::chrono::microseconds(static_cast<int64_t>((sleepTime - 0.001) * 1e6)));
				}
				while (std::chrono::duration<double>(std::chrono::high_resolution_clock::now() -
													 frameStart)
						   .count() < targetFrameTime) {
#if defined(__x86_64__) || defined(_M_X64)
					_mm_pause();
#elif defined(__aarch64__)
					__asm__ __volatile__("yield" ::: "memory");
#else
					std::this_thread::yield();
#endif
				}
			}
		}
	}

	TaskSystem::Shutdown();

	return EXIT_SUCCESS;
}

} // namespace

extern auto RunGame(const ZHLN::CommandLineOptions& options) {
	auto result = InitializeEngine(options)
					  .and_then([&options](std::unique_ptr<Engine> engine) {
						  return RunEngineLoop(std::move(engine), options.fpsLimit);
					  })
					  .transform_error([](const EngineError& err) -> int {
						  if (!err.msg.empty() && !err.silent) {
							  ZHLN::Log("Error: {}", err.msg);
						  }
						  return err.code;
					  });

	// Fix: Prevent eager evaluation of .error() on successful runs
	return result.has_value() ? result.value() : result.error();
}
