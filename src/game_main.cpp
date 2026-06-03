#include "engine/FileWatcher.hpp"
#include "engine/Platform.hpp"

#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Scripting.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <detail/ControlFlow.hpp>
#include <physics/PhysicsWorld.hpp>
#include <string>
#include <threading/Mutex.hpp>
#include <threading/TaskSystem.hpp>

namespace ZHLN {
void DrawConsole(ScriptRunner& runner);
void DrawProfiler(Engine& engine);
void MovementSystem(Engine& engine, float dt);
} // namespace ZHLN

using namespace ZHLN;

namespace {

// ============================================================================
// Free-Fly Camera Controller
// ============================================================================
void UpdateFreeCamera(Camera& cam, const InputContext& input, float dt) {
	const float speed = 10.0f; // Flight speed (meters per second)
	const float sensitivity = 0.15f;

	// Mouse look (Active only when holding Right Click)
	if (input.IsMouseButtonDown(KeyCode::RButton)) {
		cam.yaw += input.GetMouse().deltaX * sensitivity;
		cam.pitch = std::clamp(cam.pitch - (input.GetMouse().deltaY * sensitivity), -89.0f, 89.0f);
	}

	float yawRad = JPH::DegreesToRadians(cam.yaw);
	float pitchRad = JPH::DegreesToRadians(cam.pitch);

	JPH::Vec3 forward(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad),
					  JPH::Sin(yawRad) * JPH::Cos(pitchRad));
	forward = forward.Normalized();
	JPH::Vec3 right = forward.Cross(JPH::Vec3::sAxisY()).Normalized();

	JPH::Vec3 moveDirection = JPH::Vec3::sZero();
	if (input.IsKeyDown(KeyCode::W)) {
		moveDirection += forward;
	}
	if (input.IsKeyDown(KeyCode::S)) {
		moveDirection -= forward;
	}
	if (input.IsKeyDown(KeyCode::A)) {
		moveDirection -= right;
	}
	if (input.IsKeyDown(KeyCode::D)) {
		moveDirection += right;
	}

	if (moveDirection.LengthSq() > 0.0f) {
		cam.position += moveDirection.Normalized() * speed * dt;
	}
}

struct Scene {
	void Setup(Engine& engine) {
		auto& rc = engine.GetRenderContext();
		auto& reg = engine.GetRegistry();

		ZHLN::Log("Assembling TADC Scene with Pure Runtime glTF Parsing...");

		// 1. Spawns the entire Room and automatically loads all internal textures natively
		// std::vector<Entity> roomParts = AssetFactory::SpawnGLB(rc, reg, "Pomnis Room V2.glb");

		// 2. Spawns Pomni and automatically loads internal textures natively
		std::vector<Entity> pomniParts = AssetFactory::SpawnGLB(rc, reg, "tadc_models/POMNI.glb");
		// std::vector<Entity> cesiumParts =
		// 	AssetFactory::SpawnGLB(rc, reg, "CesiumMan.glb");
	}
};

JPH::Array<ZHLN::Entity> s_VisibleEntities;
JPH::Vec3 s_LastCullPos;
float s_LastCullYaw = 0.0f;

void UpdateCulling(Engine& engine) {
	ZHLN_PROFILE_SCOPE("Culling (ECS O(N))");
	auto& cam = engine.GetCamera();
	auto& reg = engine.GetRegistry();

	auto entities = reg.GetEntitiesWith<MeshComponent>();

	if (!CullingStats::EnableCulling) {
		s_VisibleEntities.assign(entities.begin(), entities.end());
		return;
	}

	bool moved = (cam.position - s_LastCullPos).LengthSq() > 0.01f ||
				 std::abs(cam.yaw - s_LastCullYaw) > 0.5f;
	if (!moved && !s_VisibleEntities.empty()) {
		return;
	}

	s_VisibleEntities.clear();
	auto meshes = reg.GetRawArray<MeshComponent>();

	for (size_t i = 0; i < entities.size(); ++i) {
		Entity e = entities[i];
		JPH::Vec3 pos = meshes[i].localTransform.GetTranslation();

		// Position offsets for inspection layout
		if (e.index == 2) {
			pos += JPH::Vec3(0.0f, 0.0f, 0.0f); // Pomni at center
		} else if (e.index == 3) {
			pos += JPH::Vec3(2.5f, 0.5f, -1.0f); // Caine hovering right
		} else if (e.index == 4) {
			pos += JPH::Vec3(-2.5f, 0.0f, 1.0f); // Kinger left
		}

		// The room (index 1) remains at origin
		if (cam.frustum.IsSphereVisible(pos, meshes[i].cullRadius)) {
			s_VisibleEntities.push_back(e);
		}
	}

	s_LastCullPos = cam.position;
	s_LastCullYaw = cam.yaw;
}
} // namespace

auto main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) -> int {
	Platform::Init();
	ZHLN::SetupSignalHandler();
	TaskSystem::Init();
	Clock clock;

	ZHLN::EngineConfig config{
		.physics = {.maxBodies = 1000, .maxBodyPairs = 2000, .maxContactConstraints = 2000},
		.render = {.appName = "Zahlen Engine - Digital Circus Showcase",
				   .width = 1280,
				   .height = 720,
				   .vsync = false,
				   .enableValidation = true},
	};

	Engine engine(config);
	engine.GetWindow().Focus();

	auto& rc = engine.GetRenderContext();
	auto& reg = engine.GetRegistry();
	auto& cam = engine.GetCamera();

	ScriptRunner scriptRunner;
	scriptRunner.RunFile("scripts/gameplay.lua");
	FileWatcher gameplayWatcher("scripts/gameplay.lua");
	uint32_t frameCounter = 0;

	Scene scene{};
	scene.Setup(engine);

	// Position camera to look directly at the character lineup
	cam.position = {0.0f, 1.8f, 5.5f};
	cam.yaw = -90.0f;
	cam.pitch = -10.0f;

	uint32_t fontAtlasIdx = AssetFactory::CreateFontAtlasTexture(rc);

	Mesh helloText = GUI::CreateTextMesh(rc, "Zahlen Engine - TADC Dorm Showcase", 25.0f, 25.0f,
										 2.5f, {0.9f, 0.1f, 0.1f, 1.0f});

	while (engine.IsRunning()) {
		float frameTime = clock.GetDeltaTime();

		engine.ProcessEvents();

		if (engine.GetInput().IsKeyDown(KeyCode::Escape)) {
			engine.GetWindow().Close();
		}

		ZHLN::DrawConsole(scriptRunner);
		ZHLN::DrawProfiler(engine);

		if (engine.GetInput().NeedsResize()) {
			rc.SetResolution(engine.GetInput().GetNewSize());
			engine.GetInput().ClearResizeFlag();
			continue;
		}

		if (++frameCounter % 60 == 0 && gameplayWatcher.CheckModified()) {
			scriptRunner.ReloadFile("scripts/gameplay.lua");
		}

		{
			ZHLN_PROFILE_SCOPE("Logic");
			UpdateFreeCamera(cam, engine.GetInput(), frameTime);

			// --- STEP 4C: Play embedded skeletal keyframes over time ---
			AssetFactory::UpdateAnimations(rc, reg, frameTime);
		}

		auto res = engine.GetWindow().GetSize();

		if (res.width > 0 && res.height > 0) {
			if (g_TAAState.enabled) {
				g_TAAState.frameIndex++;
			} else {
				g_TAAState.frameIndex = 0;
			}

			JPH::Mat44 unjitteredProj = cam.GetProjectionMatrix((float)res.width / res.height);
			JPH::Mat44 unjitteredVp = unjitteredProj * cam.GetViewMatrix();

			JPH::Mat44 vp{};
			if (g_TAAState.enabled) {
				vp = cam.GetJitteredProjectionMatrix((float)res.width / res.height, res.width,
													 res.height) *
					 cam.GetViewMatrix();
			} else {
				vp = unjitteredVp;
			}

			cam.frustum.Update(vp);
			UpdateCulling(engine);

			JPH::Vec3 sunDirection = {-0.6f, 0.4f, -0.7f}; // Shines from front-right-above
			JPH::Mat44 lightView =
				Math::CreateLookAt(sunDirection * 100.0f, {0.0f, 0.0f, 0.0f}, JPH::Vec3::sAxisY());
			JPH::Mat44 lightProj = Math::CreateOrtho(-50.0f, 50.0f, -50.0f, 50.0f, 0.1f, 200.0f);
			JPH::Mat44 shadowProjView = lightProj * lightView;

			JPH::Mat44 biasMatrix = {
				JPH::Vec4(0.5f, 0.0f, 0.0f, 0.0f), JPH::Vec4(0.0f, -0.5f, 0.0f, 0.0f),
				JPH::Vec4(0.0f, 0.0f, 1.0f, 0.0f), JPH::Vec4(0.5f, 0.5f, 0.0f, 1.0f)};
			JPH::Mat44 lightSpaceBiased = biasMatrix * shadowProjView;

			FrameUniforms uniforms{};
			uniforms.viewProj = vp;
			uniforms.prevViewProj = unjitteredVp;
			uniforms.lightSpaceMatrix = lightSpaceBiased;
			std::memcpy(&uniforms.camPos[0], &cam.position, sizeof(float) * 3);
			std::memcpy(&uniforms.lightDir[0], &sunDirection, sizeof(float) * 3);
			uniforms.lightCount = 0;

			Renderer::SetFrameData(rc, uniforms, shadowProjView);

			engine.BeginFrame();
			Renderer::SetMatrices(rc, vp, unjitteredVp);

			for (Entity e : s_VisibleEntities) {
				auto* mesh = reg.Get<MeshComponent>(e);
				if (mesh == nullptr) {
					continue;
				}

				// Offset character matrices in the world for an organized lineup
				JPH::Mat44 currentTransform = mesh->localTransform;
				if (e.index == 2) {
					currentTransform =
						Math::CreateTransform(JPH::Vec3(0.0f, 0.0f, 0.0f), JPH::Quat::sIdentity()) *
						mesh->localTransform;
				} else if (e.index == 3) {
					currentTransform = Math::CreateTransform(JPH::Vec3(2.5f, 0.5f, -1.0f),
															 JPH::Quat::sIdentity()) *
									   mesh->localTransform;
				} else if (e.index == 4) {
					currentTransform = Math::CreateTransform(JPH::Vec3(-2.5f, 0.0f, 1.0f),
															 JPH::Quat::sIdentity()) *
									   mesh->localTransform;
				}

				Renderer::Draw(rc, mesh->material, mesh->mesh, currentTransform,
							   mesh->prevTransform, mesh->cullRadius, mesh->jointOffset,
							   mesh->isSkinned, mesh->morphOffset, mesh->activeMorphCount,
							   mesh->morphWeights);
				mesh->prevTransform = currentTransform;
			}

			CullingStats::TotalObjects = (uint32_t)reg.GetEntitiesWith<MeshComponent>().size();
			CullingStats::CulledObjects =
				CullingStats::TotalObjects - (uint32_t)s_VisibleEntities.size();

			Renderer::DrawUI(rc, helloText, fontAtlasIdx);

			engine.EndFrame();
		} else {
			Platform::Sleep(10);
		}
	}

	ZHLN::Log("Shutting down engine...");
	return 0;
}
