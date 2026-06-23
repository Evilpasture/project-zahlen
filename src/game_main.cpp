// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/game_main.cpp

#include "Zahlen/Audio.hpp"
#include "Zahlen/CommandLine.hpp"
#include "Zahlen/Input.hpp"
#include "Zahlen/alife/Types.hpp"
#include "ecs/ECS.hpp"
#include "ecs/EntityCommandBuffer.hpp"
#include "ecs/SystemGraph.hpp"
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
#include <engine/system/InteractionSystem.hpp>
#include <expected>
#include <physics/PhysicsWorld.hpp>
#include <print>
#include <string>
#include <threading/Mutex.hpp>
#include <threading/TaskSystem.hpp>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

using namespace ZHLN;
using namespace ZHLN::ECS;

namespace ZHLN {
void DrawConsole(ScriptRunner& runner);
void DrawProfiler(Engine& engine);
void MovementSystem(Engine& engine, float dt);
void DrawOrientationGizmo(const ZHLN::Camera& cam);
void DrawInventoryShell(ScriptRunner& runner);
void DrawECSProfiler();
} // namespace ZHLN

namespace {

void WriteBenchmarkLog(std::vector<double> frameTimes) {
	if (frameTimes.empty()) {
		return;
	}

	double totalTime = 0.0;
	for (double t : frameTimes) {
		totalTime += t;
	}
	double avgFrameTime = totalTime / frameTimes.size();
	double avgFps = 1.0 / avgFrameTime;

	// Sort to calculate percentiles and lows (slowest frames are at the end)
	std::ranges::sort(frameTimes);

	// 1% Lows: Average of the slowest 1% of frames
	size_t count1Percent = std::max<size_t>(1, frameTimes.size() / 100);
	double sum1Percent = 0.0;
	for (size_t i = frameTimes.size() - count1Percent; i < frameTimes.size(); ++i) {
		sum1Percent += frameTimes[i];
	}
	double low1PercentFps = 1.0 / (sum1Percent / count1Percent);

	// 0.1% Lows: Average of the slowest 0.1% of frames
	size_t count01Percent = std::max<size_t>(1, frameTimes.size() / 1000);
	double sum01Percent = 0.0;
	for (size_t i = frameTimes.size() - count01Percent; i < frameTimes.size(); ++i) {
		sum01Percent += frameTimes[i];
	}
	double low01PercentFps = 1.0 / (sum01Percent / count01Percent);

	// Percentiles
	double p99 = frameTimes[static_cast<size_t>(frameTimes.size() * 0.99)] * 1000.0;
	double p999 = frameTimes[static_cast<size_t>(frameTimes.size() * 0.999)] * 1000.0;

	FILE* f = std::fopen("benchmark.log", "w");
	if (f != nullptr) {
		std::println(f, "=========================================");
		std::println(f, "         ZAHLEN BENCHMARK REPORT         ");
		std::println(f, "=========================================");
		std::println(f, "Total Frames:       {}", frameTimes.size());
		std::println(f, "Total Time (s):     {:.3f}", totalTime);
		std::println(f, "Average FPS:        {:.2f}", avgFps);
		std::println(f, "Average Frametime:  {:.2f} ms", avgFrameTime * 1000.0);
		std::println(f, "1% Low FPS:         {:.2f}", low1PercentFps);
		std::println(f, "0.1% Low FPS:       {:.2f}", low01PercentFps);
		std::println(f, "99.0% Percentile:   {:.2f} ms", p99);
		std::println(f, "99.9% Percentile:   {:.2f} ms", p999);
		std::println(f, "=========================================");
		std::fclose(f);
		ZHLN::Log("Benchmark report written to benchmark.log");
	} else {
		ZHLN::Log("Error: Failed to write benchmark.log");
	}
}

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

// ============================================================================
// Flattened System Wrappers (For 100% Predictable Function Pointers)
// ============================================================================

void Sys_VisualInterpolation(Engine& engine, float /*dt*/) {
	VisualInterpolationSystem::Update(engine, engine.GetCurrentAlpha());
}

void Sys_Animation(Engine& engine, float dt) {
	static AnimationSystem sys;
	sys.UpdateAnimations(engine.GetRenderContext(), engine.GetRegistry(), dt);
}

void Sys_Articulation(Engine& engine, float dt) {
	static ArticulationSystem sys;
	sys.Update(engine, dt);
}

void Sys_Transform(Engine& engine, float /*dt*/) {
	static TransformSystem sys;
	sys.ResolveTransforms(engine.GetRegistry());
}

void Sys_Audio(Engine& engine, float dt) {
	AudioSystem(engine, dt);
}

void Sys_TargetCamera(Engine& engine, float dt) {
	static TargetCameraSystem sys;
	sys.Update(engine, dt, engine.GetCurrentAlpha());
}

void Sys_Camera(Engine& engine, float dt) {
	static CameraSystem sys;
	sys.Update(engine, dt, engine.GetCurrentAlpha());
}

void Sys_Culling(Engine& engine, float /*dt*/) {
	engine.GetCullingSystem().Update<false>(engine, engine.GetVisibleEntities());
}

void Sys_Lighting(Engine& engine, float dt) {
	static LightingSystem sys;
	sys.Update(engine, dt);
}

void Sys_ParticleSpawner(Engine& engine, float /*dt*/) {}

void UISystem(Engine& engine, ScriptRunner& scriptRunner) {
	if (engine.GetWindow().IsTTY()) {
		return;
	}

	DrawConsole(scriptRunner);
	DrawInventoryShell(scriptRunner);
	DrawProfiler(engine);
	DrawOrientationGizmo(engine.GetCamera());
	DrawECSProfiler();

	auto& reg = engine.GetRegistry();

	// ============================================================================
	// NEW: semi-transparent corner HUD overlay for coordinate tracking
	// ============================================================================
	ImGui::SetNextWindowBgAlpha(0.35f); // Semi-transparent background
	ImGuiWindowFlags hudFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
								ImGuiWindowFlags_NoSavedSettings |
								ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

	// Position the HUD overlay near the top-left (just under the toolbar)
	ImGui::SetNextWindowPos({10.0f, 50.0f}, ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Coordinates HUD", nullptr, hudFlags)) {

		// 1. Fetch player entity position
		Entity playerEnt = NullEntity;
		for (Entity e : reg.GetEntitiesWith<MovementComponent>()) {
			playerEnt = e;
			break;
		}

		if (playerEnt != NullEntity) {
			if (auto* trans = reg.Get<TransformComponent>(playerEnt)) {
				ImGui::Text("Player Pos:  X: %.2f, Y: %.2f, Z: %.2f", trans->position.GetX(),
							trans->position.GetY(), trans->position.GetZ());
			}
		} else {
			ImGui::Text("Player Pos:  [Not Found]");
		}

		// 2. Fetch camera position & orientation
		auto& cam = engine.GetCamera();
		ImGui::Text("Camera Pos:  X: %.2f, Y: %.2f, Z: %.2f", cam.position.GetX(),
					cam.position.GetY(), cam.position.GetZ());
		ImGui::Text("Camera Rot:  Yaw: %.1f, Pitch: %.1f", cam.yaw, cam.pitch);
	}
	ImGui::End();
	// ============================================================================

	auto settingsEntities = reg.GetEntitiesWith<GlobalSettingsTagComponent>();
	if (settingsEntities.empty()) {
		return;
	}

	Entity settingsEnt = settingsEntities[0];
	auto* pp = reg.Get<PostProcessSettingsComponent>(settingsEnt);
	auto* dbg = reg.Get<DebugSettingsComponent>(settingsEnt);
	auto* aa = reg.Get<AASettingsComponent>(settingsEnt);

	if ((pp == nullptr) || (dbg == nullptr)) {
		return;
	}

	ImGui::Begin("Lighting Workspace Controller");
	ImGui::SeparatorText("Physics Debug");
	ImGui::RadioButton("Hidden", &dbg->physicsDrawMode, 0);
	ImGui::SameLine();
	ImGui::RadioButton("Wireframe", &dbg->physicsDrawMode, 1);
	ImGui::SameLine();
	ImGui::RadioButton("Solid", &dbg->physicsDrawMode, 2);
	ImGui::Text("PBR Materials & Lights Controller");
	ImGui::Separator();

	// Dynamically edit PBRComponent on any named floor/ground/lobby entity
	PBRComponent* floorPbr = nullptr;
	for (Entity e : reg.GetEntitiesWith<PBRComponent>()) {
		if (auto* nameComp = reg.Get<NameComponent>(e)) {
			std::string nameLower(nameComp->name.c_str());
			std::ranges::transform(nameLower, nameLower.begin(), ::tolower);
			if (nameLower.contains("floor") || nameLower.contains("ground") ||
				nameLower.contains("lobby")) {
				floorPbr = reg.Get<PBRComponent>(e);
				break;
			}
		}
	}

	if (floorPbr != nullptr) {
		ImGui::SliderFloat("Floor Roughness", &floorPbr->roughness, 0.0f, 1.0f);
		ImGui::SliderFloat("Floor Metallic", &floorPbr->metallic, 0.0f, 1.0f);
	}

	// Dynamically edit LightComponent on the point lights
	int lightIdx = 1;
	for (Entity e : reg.GetEntitiesWith<LightingSystem::LightComponent>()) {
		if (auto* light = reg.Get<LightingSystem::LightComponent>(e)) {
			if (light->type == 1) { // Point Light
				std::string labelInt = std::format("Point Light {} Intensity", lightIdx);
				std::string labelRad = std::format("Point Light {} Radius", lightIdx);
				ImGui::SliderFloat(labelInt.c_str(), &light->intensity, 0.0f, 500.0f);
				ImGui::SliderFloat(labelRad.c_str(), &light->radius, 0.0f, 5.0f);
				lightIdx++;
			}
		}
	}

	ImGui::SeparatorText("Parallax-Corrected Local Reflection Probe");
	bool useProbe = pp->useLocalProbe != 0;
	if (ImGui::Checkbox("Enable Box Projection", &useProbe)) {
		pp->useLocalProbe = useProbe ? 1 : 0;
	}
	if (pp->useLocalProbe != 0) {
		std::array<float, 3> minArr = {pp->probeMin.GetX(), pp->probeMin.GetY(),
									   pp->probeMin.GetZ()};
		std::array<float, 3> maxArr = {pp->probeMax.GetX(), pp->probeMax.GetY(),
									   pp->probeMax.GetZ()};
		std::array<float, 3> posArr = {pp->probePos.GetX(), pp->probePos.GetY(),
									   pp->probePos.GetZ()};

		if (ImGui::DragFloat3("Box Min", minArr.data(), 0.1f, -100.0f, 100.0f, "%.1fm")) {
			pp->probeMin = JPH::Vec3(minArr[0], minArr[1], minArr[2]);
		}
		if (ImGui::DragFloat3("Box Max", maxArr.data(), 0.1f, -100.0f, 100.0f, "%.1fm")) {
			pp->probeMax = JPH::Vec3(maxArr[0], maxArr[1], maxArr[2]);
		}
		if (ImGui::DragFloat3("Probe Position", posArr.data(), 0.1f, -100.0f, 100.0f, "%.1fm")) {
			pp->probePos = JPH::Vec3(posArr[0], posArr[1], posArr[2]);
		}
	}

	ImGui::SeparatorText("Ambient Occlusion & Global Illumination");
	constexpr std::array<const char*, 5> giModesList = {
		"Off", "SSAO (Ambient Occlusion)", "SSGI (Screen Space GI)", "HBAO (Horizon-Based AO)",
		"GTAO (Ground Truth AO)"};
	ImGui::Combo("GI Mode", &pp->giMode, giModesList.data(), static_cast<int>(giModesList.size()));

	if (pp->giMode == 1) {
		ImGui::SliderFloat("AO Radius", &pp->aoRadius, 0.05f, 2.5f, "%.2fm");
		ImGui::SliderFloat("AO Bias", &pp->aoBias, 0.001f, 0.2f, "%.3f");
		ImGui::SliderFloat("AO Contrast", &pp->aoPower, 0.5f, 5.0f, "%.1fx");
		ImGui::SliderInt("AO Samples", &pp->giSamples, 2, 32);
	} else if (pp->giMode == 2) {
		ImGui::SliderFloat("Bounce Radius", &pp->aoRadius, 0.05f, 2.5f, "%.2fm");
		ImGui::SliderFloat("Bounce Bias", &pp->aoBias, 0.001f, 0.2f, "%.3f");
		ImGui::SliderFloat("GI Bounce Intensity", &pp->giIntensity, 0.1f, 5.0f, "%.1fx");
		ImGui::SliderInt("GI Samples", &pp->giSamples, 2, 32);
	} else if (pp->giMode == 3 || pp->giMode == 4) {
		ImGui::SliderFloat("Search Radius", &pp->aoRadius, 0.05f, 3.0f, "%.2fm");
		ImGui::SliderFloat("Acne Bias", &pp->aoBias, 0.001f, 0.2f, "%.3f");
		ImGui::SliderFloat("Shadow Contrast", &pp->aoPower, 0.5f, 6.0f, "%.1fx");
		ImGui::SliderInt("Search Steps", &pp->giSamples, 4, 32);
	}

	ImGui::SeparatorText("Camera Vignette");
	ImGui::SliderFloat("Vignette Intensity", &pp->vignetteIntensity, 0.0f, 2.5f, "%.2f");
	if (pp->vignetteIntensity > 0.0f) {
		ImGui::SliderFloat("Vignette Power", &pp->vignettePower, 0.1f, 6.0f, "%.2f");
	}

	bool useSsr = pp->enableSSR != 0;
	if (ImGui::Checkbox("Enable SSR", &useSsr)) {
		pp->enableSSR = useSsr ? 1 : 0;
	}

	bool useRtr = pp->enableRTR != 0;
	if (ImGui::Checkbox("Enable Hardware RTR", &useRtr)) {
		pp->enableRTR = useRtr ? 1 : 0;
	}

	ImGui::End();
}

void Sys_PostProcess(Engine& engine, float /*dt*/) {
	auto& reg = engine.GetRegistry();
	auto& rc = engine.GetRenderContext();

	for (Entity e : reg.GetEntitiesWith<PostProcessSettingsComponent>()) {
		if (auto* pp = reg.Get<PostProcessSettingsComponent>(e)) {
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

void DebugDrawSystem(Engine& engine) {
	auto& cullingSystem = engine.GetCullingSystem();
	if (!CullingStats::FreezeFrustum) {
		return;
	}

	auto& rc = engine.GetRenderContext();
	auto& reg = engine.GetRegistry();

	auto settingsEntities = reg.GetEntitiesWith<GlobalSettingsTagComponent>();
	if (settingsEntities.empty()) {
		return;
	}

	auto* dbg = reg.Get<DebugSettingsComponent>(settingsEntities[0]);
	if ((dbg == nullptr) || dbg->debugLineVbo == 0) {
		return;
	}

	Mesh debugMesh = {.vertexBuffer = static_cast<BufferHandle>(dbg->debugLineVbo),
					  .vertexCount = 36};
	Material debugMat = {.pipeline = static_cast<PipelineHandle>(dbg->debugLinePipeline),
						 .albedoIndex = dbg->debugLineAlbedo};

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

std::expected<void, RenderFrameResult> RenderSystem(Engine& engine) {
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
	int physicsDrawMode = 0;

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
			physicsDrawMode = dbg->physicsDrawMode;
		}
	}

	JPH::Vec3 sunDirection = {0.5f, 1.0f, 0.2f};
	float sunIntensity = 0.0f; // Default off if no sun entity exists

	// Search the ECS for your custom Sun entity
	auto sunEntities = reg.GetEntitiesWith<SunTagComponent>();
	if (!sunEntities.empty()) {
		Entity sunEnt = sunEntities[0];
		if (auto* trans = reg.Get<TransformComponent>(sunEnt)) {
			// 1. Extract the direction (local -Z forward vector) from its rotation
			JPH::Mat44 worldMat = trans->GetMatrix();
			sunDirection = worldMat.GetColumn3(2);

			// 2. Extract the intensity from its standard LightComponent
			if (auto* light = reg.Get<LightingSystem::LightComponent>(sunEnt)) {
				sunIntensity = light->intensity;
			}
		}
	}
	sunDirection = sunDirection.Normalized();

	// 1. Calculate camera forward direction
	float yawRad = JPH::DegreesToRadians(cam.yaw);
	float pitchRad = JPH::DegreesToRadians(cam.pitch);
	JPH::Vec3 forward(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad),
					  JPH::Sin(yawRad) * JPH::Cos(pitchRad));
	forward = forward.Normalized();

	// 2. Keep the shadow map centered on the camera position to prevent orientation swings
	JPH::Vec3 shadowCenter = cam.position;

	// 3. Expand coverage to 200m (from -100 to 100) to cover the entire lobby scene
	float shadowWidth = 200.0f;
	float texelSize = shadowWidth / 2048.0f; // 200m total width / 2048 texels

	// 4. Snap the shadow center to prevent shadow shimmering
	shadowCenter.SetX(std::round(shadowCenter.GetX() / texelSize) * texelSize);
	shadowCenter.SetY(std::round(shadowCenter.GetY() / texelSize) * texelSize);
	shadowCenter.SetZ(std::round(shadowCenter.GetZ() / texelSize) * texelSize);

	JPH::Vec3 lightPos =
		shadowCenter + sunDirection * 150.0f; // Place light further back to fit larger bounds
	JPH::Mat44 lightView = Math::CreateLookAt(lightPos, shadowCenter, JPH::Vec3::sAxisY());

	// Orthographic projection covering 200x200 meters with depth up to 400m
	JPH::Mat44 lightProj = Math::CreateOrtho(-100.0f, 100.0f, -100.0f, 100.0f, 0.1f, 400.0f);
	JPH::Mat44 shadowProjView = lightProj * lightView;

	cam.shadowFrustum.Update(shadowProjView);

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
	uniforms.lightSpaceMatrix = shadowProjView;
	uniforms.invViewProj = unjitteredVp.Inversed();
	std::memcpy(&uniforms.camPos[0], &cam.position, sizeof(float) * 3);
	std::memcpy(&uniforms.lightDir[0], &sunDirection, sizeof(float) * 3);
	uniforms.lightDir[3] = sunIntensity;
	uniforms.lightCount =
		static_cast<uint32_t>(reg.GetEntitiesWith<LightingSystem::LightComponent>().size());
	uniforms.probeMin = probeMin;
	uniforms.probeMax = probeMax;
	uniforms.probePos = probePos;
	uniforms.jitterParams =
		JPH::Vec4(aaState.jitterX, aaState.jitterY, aaState.prevJitterX, aaState.prevJitterY);
	uniforms.enableRTR = enableRTR;

	rc.SetAAState(aaState);
	Renderer::SetFrameData(rc, uniforms, shadowProjView);
	Renderer::SetMatrices(rc, vp, unjitteredVp);

	if (physicsDrawMode == 0) { // Only draw standard world when debug is Off
		for (Entity e : visibleEntities) {
			auto* mesh = reg.Get<MeshComponent>(e);
			if (mesh == nullptr) {
				continue;
			}

			DrawFlags flags = DrawFlags::None;
			if (mesh->isSkinned) {
				flags |= DrawFlags::Skinned;
			}

			// Retrieve PBR factors from PBRComponent to use as the Single Source of Truth
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
							.jointOffset = mesh->jointOffset,
							.morphOffset = mesh->morphOffset,
							.activeMorphCount = mesh->activeMorphCount,
							.morphWeights = mesh->morphWeights.data(),
							.flags = flags,
							.skinnedVertexBuffer = mesh->skinnedVertexBuffer,
							.roughness = roughness,
							.metallic = metallic});
		}
	}

	CullingStats::TotalObjects = reg.GetEntitiesWith<MeshComponent>().size();
	CullingStats::CulledObjects = CullingStats::TotalObjects - visibleEntities.size();

	auto uiSettingsEntities = reg.GetEntitiesWith<UISettingsComponent>();
	const FontAtlas* activeFont = nullptr;
	if (!uiSettingsEntities.empty()) {
		activeFont = &reg.Get<UISettingsComponent>(uiSettingsEntities[0])->fontAtlas;
	}

	for (Entity e : reg.GetEntitiesWith<TextComponent>()) {
		auto* text = reg.Get<TextComponent>(e);
		if (text->mesh.vertexBuffer == BufferHandle::Invalid && activeFont != nullptr) {
			text->mesh = GUI::CreateTextMesh(rc, *activeFont, text->text.c_str(), text->x, text->y,
											 text->scale, text->color);
		}
		Renderer::DrawUI(rc, text->mesh, text->fontIndex);
	}

	DebugDrawSystem(engine);

	if (physicsDrawMode > 0) {
		ZHLN_PROFILE_SCOPE("Physics Debug Extract & Upload");

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

		bool isWireframe = (physicsDrawMode == 1);
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
				v.normal = Math::PackNormal(0.0f, 1.0f, 0.0f);
				v.tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f);
				v.uv = Math::PackUV(0.0f, 0.0f);
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
				v.normal = Math::PackNormal(0.0f, 1.0f, 0.0f);
				v.tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f);
				v.uv = Math::PackUV(0.0f, 0.0f);
				debugVerts.push_back(v);
			}
		}

		if (!debugVerts.empty()) {
			rc.UploadDebugVertices(debugVerts.data(), debugVerts.size() * sizeof(Vertex),
								   static_cast<uint32_t>(debugVerts.size()));

			Mesh debugMesh = {.vertexBuffer = rc.GetDebugMeshBuffer(),
							  .vertexCount = static_cast<uint32_t>(debugVerts.size())};

			Renderer::Draw(rc, isWireframe ? debugLineMat : debugSolidMat, debugMesh,
						   {.transform = JPH::Mat44::sIdentity(),
							.prevTransform = JPH::Mat44::sIdentity(),
							.cullRadius = 10000.0f});
		}
	}

	auto end_res = rc.EndFrame();
	if (!end_res) {
		return std::unexpected(end_res.error());
	}

	return {};
}

void BuildSystemGraphs(Engine& engine) {
	auto& updateGraph = engine.GetUpdateGraph();
	auto& renderGraph = engine.GetRenderGraph();

	// --- COMPILE UPDATE GRAPH (Variable Timestep Logic) ---
	updateGraph.AddSystem(
		{.update_func = Sys_VisualInterpolation,
		 .name = "VisualInterpolationSystem",
		 .access_pattern = {Read<PhysicsStateComponent>(), Write<TransformComponent>()},
		 .enabled = true});

	updateGraph.AddSystem({.update_func = Sys_Animation,
						   .name = "AnimationSystem",
						   .access_pattern = {Read<MovementComponent>(), Write<MeshComponent>()},
						   .enabled = true});

	updateGraph.AddSystem(
		{.update_func = Sys_Articulation,
		 .name = "ArticulationSystem",
		 .access_pattern = {Read<PhysicsComponent>(), Read<MeshComponent>(),
							Write<RagdollComponent>(), Write<TransformComponent>()},
		 .enabled = true});

	updateGraph.AddSystem({.update_func = Sys_Transform,
						   .name = "TransformSystem",
						   .access_pattern = {Read<HierarchyComponent>(),
											  Read<TransformComponent>(), Write<MeshComponent>()},
						   .enabled = true});

	updateGraph.AddSystem({.update_func = Sys_PostProcess,
						   .name = "PostProcessSystem",
						   .access_pattern = {Read<PostProcessSettingsComponent>()},
						   .enabled = true});

	updateGraph.AddSystem(
		{.update_func = Sys_Audio,
		 .name = "AudioSystem",
		 .access_pattern = {Read<PhysicsComponent>(), Read<ALife::ALifeComponent>(),
							Write<AudioSourceComponent>()},
		 .enabled = true});

	updateGraph.AddSystem({.update_func = Sys_ParticleSpawner,
						   .name = "ParticleSpawnerExample",
						   .access_pattern = {},
						   .enabled = true});

	updateGraph.AddSystem(
		{.update_func =
			 [](Engine& eng, float dt) {
				 static InteractionSystem sys;
				 sys.Update(eng, dt);
			 },
		 .name = "InteractionSystem",
		 .access_pattern = {Write<TriggerComponent>(), Write<ContainerComponent>(),
							Write<PickupComponent>(), Read<ItemBaseComponent>(),
							Read<UsableComponent>(), Read<MovementComponent>()},
		 .enabled = true});

	updateGraph.Compile();

	// --- COMPILE RENDER GRAPH (Lighting & Culling) ---

	renderGraph.AddSystem(
		{.update_func = Sys_Culling,
		 .name = "CullingSystem",
		 .access_pattern = {Read<MeshComponent>(), Read<CameraSystem::CameraComponent>()},
		 .enabled = true});

	renderGraph.AddSystem(
		{.update_func = Sys_Lighting,
		 .name = "LightingSystem",
		 .access_pattern = {Read<LightingSystem::LightComponent>(), Read<TransformComponent>(),
							Read<NameComponent>(), Write<MeshComponent>()},
		 .enabled = true});

	renderGraph.Compile();
}

bool InitializeGame(Engine& engine) {
	auto& rc = engine.GetRenderContext();
	auto& reg = engine.GetRegistry();
	auto& pc = engine.GetPhysicsContext();

	Mesh lineMesh = AssetFactory::CreateBox(rc, {0.01f, 0.01f, 0.5f}, {0.0f, 1.0f, 1.0f, 1.0f});
	Material lineMat = AssetFactory::CreateBasicMaterial(rc);

	reg.RegisterComponents<
		TransformComponent, MeshComponent, PhysicsComponent, PhysicsStateComponent,
		MovementComponent, ALife::ALifeComponent, RagdollComponent, NameComponent,
		TargetCameraComponent, InputSystem::InputComponent, LightingSystem::LightComponent,
		PostProcessSettingsComponent, CameraSystem::CameraComponent, PlayerTagComponent,
		MainCameraTagComponent, GlobalSettingsTagComponent, AASettingsComponent, TextComponent,
		UISettingsComponent, AudioSourceComponent, PBRComponent, ItemBaseComponent, PickupComponent,
		UsableComponent, ContainerComponent, TriggerComponent, DebugSettingsComponent,
		SunTagComponent>();

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
	reg.Add(settingsEntity, PostProcessSettingsComponent{});
	reg.Add(settingsEntity,
			DebugSettingsComponent{.debugLineVbo = static_cast<uint64_t>(lineMesh.vertexBuffer),
								   .debugLinePipeline = static_cast<uint64_t>(lineMat.pipeline),
								   .debugLineAlbedo = lineMat.albedoIndex,
								   .physicsDrawMode = 0});

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
										   .intensity = 180.0f,
										   .radius = 1.5f,
										   .direction = {0.0f, 0.0f, 0.0f},
										   .range = 0.0f,
										   .points = {},
										   .twoSided = 0},
			TransformComponent{.position = {-5.0f, 4.0f, 0.0f}});

	Entity pt2 = reg.Create();
	reg.Add(pt2,
			LightingSystem::LightComponent{.type = 1,
										   .color = {1.0f, 0.0f, 0.5f},
										   .intensity = 180.0f,
										   .radius = 1.5f,
										   .direction = {0.0f, 0.0f, 0.0f},
										   .range = 0.0f,
										   .points = {},
										   .twoSided = 0},
			TransformComponent{.position = {5.0f, 4.0f, 0.0f}});

	Entity uiSettings = reg.Create();
	reg.Add(uiSettings, UISettingsComponent{});
	AssetFactory::CreateFontAtlasTexture(rc);

	// Entity textEnt = reg.Create();
	// reg.Add(
	// 	textEnt,
	// 	TextComponent{.text = "Zahlen Engine - TADC Dorm Showcase",
	// 				  .x = 25.0f,
	// 				  .y = 25.0f,
	// 				  .scale = 2.5f,
	// 				  .color = {0.9f, 0.1f, 0.1f, 1.0f},
	// 				  .fontIndex = reg.Get<UISettingsComponent>(uiSettings)->defaultFontAtlasIdx});

	BuildSystemGraphs(engine);

	return true;
}

void UpdateGame(Engine& engine, float dt, float& physicsAccumulator, ScriptRunner& scriptRunner,
				FileWatcher& gameplayWatcher) {
	static InputSystem inputSystem;
	inputSystem.Update(engine);
	UISystem(engine, scriptRunner);

	// --- DYNAMIC HOT-RELOAD PUMP ---
	if (gameplayWatcher.CheckModified()) {
		scriptRunner.ReloadFile("scripts/gameplay.lua");
	}
	engine.GetRenderContext().CheckShaderReload(); // Checks and re-builds stale pipelines

	static TargetCameraSystem targetCamSys;
	static CameraSystem camSys;
	targetCamSys.Update(engine, dt, engine.GetCurrentAlpha());
	camSys.Update(engine, dt, engine.GetCurrentAlpha());

	inputSystem.PlayerInputTranslate(engine, engine.GetCamera());

	float cappedDt = std::min(dt, 0.1f);
	physicsAccumulator += cappedDt;
	constexpr float targetDt = 1.0f / 60.0f;

	physicsAccumulator = std::min(physicsAccumulator, targetDt * 4.0f);

	{
		ZHLN_PROFILE_SCOPE("ECS System: Physics & Movement");
		while (physicsAccumulator >= targetDt) {
			MovementSystem(engine, targetDt);
			engine.GetPhysicsContext().Step(targetDt);
			ZHLN::PhysicsStateSystem::WriteBack(engine);
			physicsAccumulator -= targetDt;
		}
	}

	engine.GetCurrentAlpha() = physicsAccumulator / targetDt;

	{
		ZHLN_PROFILE_SCOPE("ECS System: Script/Lua Update");
		scriptRunner.CallUpdate(&engine, dt);
	}

	engine.GetUpdateGraph().Execute(engine, dt);
	engine.GetMainECB().Playback();
}

std::expected<void, RenderFrameResult> RenderGame(Engine& engine, float frameTime) {
	engine.GetRenderGraph().Execute(engine, frameTime);

	auto render_res = RenderSystem(engine);
	if (!render_res) {
		return std::unexpected(render_res.error());
	}

	{
		ZHLN_PROFILE_SCOPE("ECS System: Update Transform History");
		static TransformSystem ts;
		ts.UpdateTransformHistory(engine.GetRegistry());
	}

	return {};
}

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

std::expected<int, EngineError> RunEngineLoop(std::unique_ptr<Engine> engine,
											  const CommandLineOptions& options) {
	ScriptRunner scriptRunner;
	FileWatcher gameplayWatcher("scripts/gameplay.lua");

	if (!InitializeGame(*engine)) {
		return std::unexpected(
			EngineError{.msg = "Game failed to initialize.", .code = EXIT_FAILURE});
	}

	for (int i = 0; i < 3; ++i) {
		engine->ProcessEvents();

		auto res = RenderGame(*engine, 0.016f);
		if (!res) {
			if (res.error() == RenderFrameResult::DeviceLost) {
				engine->HandleDeviceLost();
			}
		}
	}

	ZHLN::Log("Window active and presenting. Loading scene assets...");
	scriptRunner.RunFile("scripts/gameplay.lua");

	float physicsAccumulator = 0.0f;
	const double targetFrameTime =
		options.fpsLimit > 0 ? 1.0 / static_cast<double>(options.fpsLimit) : 0.0;

	std::vector<double> frameTimes;
	if (options.benchmark) {
		frameTimes.reserve(10000); // Pre-allocate memory to avoid allocation spikes during gameplay
	}

	auto frameStart = std::chrono::high_resolution_clock::now();

	while (engine->IsRunning()) {
		engine->ProcessEvents();

		if (engine->GetInput().IsKeyDown(KeyCode::Escape)) {
			engine->GetWindow().Close();
			break;
		}

		auto frameEnd = std::chrono::high_resolution_clock::now();
		double elapsed = std::chrono::duration<double>(frameEnd - frameStart).count();
		frameStart = std::chrono::high_resolution_clock::now();

		if (options.benchmark) {
			frameTimes.push_back(elapsed);
		}

		float rawDt = std::min(static_cast<float>(elapsed), 0.1f);

		double target = elapsed;
		constexpr std::array<double, 7> snapTargets = {{1.0 / 60.0, 1.0 / 75.0, 1.0 / 90.0,
														1.0 / 120.0, 1.0 / 144.0, 1.0 / 240.0,
														1.0 / 360.0}};
		for (double t : snapTargets) {
			if (std::abs(elapsed - t) < 0.001) {
				target = t;
				break;
			}
		}

		static double smoothedElapsed = 0.0166667;
		smoothedElapsed = (smoothedElapsed * 0.9) + (target * 0.1);
		float frameTime = std::min(static_cast<float>(smoothedElapsed), 0.1f);

		if (engine->GetInput().NeedsResize()) {
			engine->GetRenderContext().SetResolution(engine->GetInput().GetNewSize());
			engine->GetInput().ClearResizeFlag();
			if (!engine->GetWindow().IsTTY()) {
				ImGui::EndFrame();
			}
			continue;
		}

		UpdateGame(*engine, rawDt, physicsAccumulator, scriptRunner, gameplayWatcher);

		auto render_res = RenderGame(*engine, rawDt);
		if (!render_res) {
			if (render_res.error() == RenderFrameResult::DeviceLost) {
				engine->HandleDeviceLost();
				scriptRunner.ReloadFile("scripts/gameplay.lua");
			}
		}

		if (options.fpsLimit > 0) {
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

	if (options.benchmark && !frameTimes.empty()) {
		WriteBenchmarkLog(frameTimes);
	}

	return EXIT_SUCCESS;
}

} // namespace

extern auto RunGame(const ZHLN::CommandLineOptions& options) {
	auto result = InitializeEngine(options)
					  .and_then([&options](std::unique_ptr<Engine> engine) {
						  return RunEngineLoop(std::move(engine), options);
					  })
					  .transform_error([](const EngineError& err) -> int {
						  if (!err.msg.empty() && !err.silent) {
							  ZHLN::Log("Error: {}", err.msg);
						  }
						  return err.code;
					  });

	return result.has_value() ? result.value() : result.error();
}
