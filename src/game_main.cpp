#include "engine/FileWatcher.hpp"
#include "engine/Platform.hpp"
#include "imgui.h"

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

void LoadLevel(Engine& engine, const std::string& path, Material material);
} // namespace ZHLN

using namespace ZHLN;

namespace {
struct Scene {
	ZHLN::Entity playerEntity = ZHLN::NullEntity;

	void Setup(Engine& engine) {
		auto& rc = engine.GetRenderContext();
		auto& pc = engine.GetPhysicsContext();
		auto& reg = engine.GetRegistry();
		auto& assetMgr = engine.GetAssetManager();

		reg.RegisterComponent<MeshComponent>();
		reg.RegisterComponent<PhysicsComponent>();
		reg.RegisterComponent<MovementComponent>();
		reg.RegisterComponent<ALife::ALifeComponent>(); // <-- Register ALife Component

		// --- 1. Load CityPack GLB Assets ---
		ZHLN::Log("Loading City Scene Assets...");
		Mesh playerMesh = AssetFactory::LoadCookedMesh(rc, assetMgr, "Adventurer.glb");
		Mesh agentMesh = AssetFactory::LoadCookedMesh(rc, assetMgr, "Man.glb");
		Mesh treeMesh = AssetFactory::LoadCookedMesh(rc, assetMgr, "Tree.glb");
		Mesh benchMesh = AssetFactory::LoadCookedMesh(rc, assetMgr, "Bench.glb");
		Mesh dumpsterMesh = AssetFactory::LoadCookedMesh(rc, assetMgr, "Dumpster.glb");
		Mesh pizzaCornerMesh = AssetFactory::LoadCookedMesh(rc, assetMgr, "Pizza Corner.glb");
		Mesh buildingGreenMesh = AssetFactory::LoadCookedMesh(rc, assetMgr, "Building Green.glb");
		Mesh carMesh = AssetFactory::LoadCookedMesh(rc, assetMgr, "Car.glb");
		Mesh coneMesh = AssetFactory::LoadCookedMesh(rc, assetMgr, "Cone.glb");
		Mesh trashCanMesh = AssetFactory::LoadCookedMesh(rc, assetMgr, "Trash Can.glb");

		Material material = AssetFactory::CreateBasicMaterial(rc);

		// --- 2. Procedural Terrain Setup ---
		int terrainSize = 128;
		float terrainWorldSize = 250.0f;
		float terrainMaxHeight = 25.0f;
		std::vector<float> terrainHeights;

		Mesh terrainMesh = AssetFactory::CreateTerrain(rc, terrainSize, terrainWorldSize,
													   terrainMaxHeight, terrainHeights);
		auto terrainShape =
			Physics::CreateHeightFieldShape(terrainHeights, terrainSize, terrainWorldSize);

		// Spawn Terrain (Static)
		reg.Add(reg.Create(),
				MeshComponent{.mesh = terrainMesh, .material = material, .cullRadius = 300.0f},
				PhysicsComponent{Physics::CreateRigidBody(pc, terrainShape, {0.0f, 0.0f, 0.0f},
														  JPH::Quat::sIdentity(),
														  JPH::EMotionType::Static, 0)});

		// --- 3. Spawn Static Landmarks & Buildings ---
		// We approximate building footprints with simple static Jolt boxes so characters collide
		// with them.
		auto buildingGreenShape =
			Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, 4.0f, 10.0f, 4.0f);
		auto pizzaCornerShape =
			Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, 5.0f, 8.0f, 5.0f);

		// Place Green Building near the Hub
		reg.Add(reg.Create(),
				MeshComponent{.mesh = buildingGreenMesh, .material = material, .cullRadius = 25.0f},
				PhysicsComponent{
					Physics::CreateRigidBody(pc, buildingGreenShape, {50.0f, 13.0f, -50.0f},
											 JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0)});

		// Place Pizza Corner at the Hub
		reg.Add(reg.Create(),
				MeshComponent{.mesh = pizzaCornerMesh, .material = material, .cullRadius = 20.0f},
				PhysicsComponent{
					Physics::CreateRigidBody(pc, pizzaCornerShape, {60.0f, 13.0f, -60.0f},
											 JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0)});

		// --- 4. Spawn Dynamic Street Clutter ---
		// We scatter dynamic Cones, Trash Cans, and Cars that react realistically to physics.
		auto coneShape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, 0.25f, 0.4f, 0.25f);
		auto trashCanShape =
			Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, 0.4f, 0.6f, 0.4f);
		auto carShape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, 1.0f, 0.8f, 2.0f);

		for (int i = 0; i < 150; ++i) {
			const int col = i % 15;
			const int row = i / 15;
			const float x = (static_cast<float>(col) - 7.5f) * 8.0f;
			const float z = (static_cast<float>(row) - 5.0f) * 10.0f;
			const float y =
				30.0f + (static_cast<float>(i) *
						 0.1f); // slightly stacked so they drop and settle on the terrain

			Entity prop = reg.Create();
			if (i % 3 == 0) {
				reg.Add(prop,
						MeshComponent{.mesh = coneMesh, .material = material, .cullRadius = 2.0f});
				reg.Add(prop, PhysicsComponent{Physics::CreateRigidBody(
								  pc, coneShape, {x, y, z}, JPH::Quat::sIdentity(),
								  JPH::EMotionType::Dynamic, 1)});
			} else if (i % 3 == 1) {
				reg.Add(prop, MeshComponent{
								  .mesh = trashCanMesh, .material = material, .cullRadius = 2.0f});
				reg.Add(prop, PhysicsComponent{Physics::CreateRigidBody(
								  pc, trashCanShape, {x, y, z}, JPH::Quat::sIdentity(),
								  JPH::EMotionType::Dynamic, 1)});
			} else {
				reg.Add(prop,
						MeshComponent{.mesh = carMesh, .material = material, .cullRadius = 5.0f});
				reg.Add(prop, PhysicsComponent{Physics::CreateRigidBody(
								  pc, carShape, {x, y, z}, JPH::Quat::sIdentity(),
								  JPH::EMotionType::Dynamic, 1)});
			}
		}

		// --- 5. Spawn Player (Adventurer GLB) ---
		playerEntity = reg.Create();
		reg.Add(playerEntity,
				MeshComponent{.mesh = playerMesh, .material = material, .cullRadius = 5.0f});
		reg.Add(playerEntity, PhysicsComponent{Physics::CreateCharacter(pc, {0.0f, 35.0f, 0.0f})});
		reg.Add(playerEntity, MovementComponent{.speed = 8.0f});

		// =====================================================================
		// --- 6. Configure Level Graph & Waypoints ---
		// =====================================================================
		auto& alife = engine.GetALife();
		alife.GetGraph() = ALife::LevelGraph(4);

		// Node 0: Campfire / Rest Spot
		alife.GetGraph().GetNode(0).position = JPH::RVec3(-60.0, 12.0, -60.0);
		alife.GetGraph().GetNode(0).type = ALife::NodeType::Campfire;

		// Spawn physical benches around the campfire spot
		auto benchShape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, 0.4f, 0.4f, 1.0f);
		for (int i = 0; i < 3; ++i) {
			float angle = i * 2.094f; // 120 degrees
			JPH::RVec3 pos = {-60.0f + std::cos(angle) * 3.0f, 13.0f,
							  -60.0f + std::sin(angle) * 3.0f};
			reg.Add(reg.Create(),
					MeshComponent{.mesh = benchMesh, .material = material, .cullRadius = 4.0f},
					PhysicsComponent{Physics::CreateRigidBody(
						pc, benchShape, pos, JPH::Quat::sRotation(JPH::Vec3::sAxisY(), angle),
						JPH::EMotionType::Static, 0)});
		}

		// Node 1: Camp / Pizza Hub
		alife.GetGraph().GetNode(1).position = JPH::RVec3(60.0, 12.0, -60.0);
		alife.GetGraph().GetNode(1).type = ALife::NodeType::Hub;

		// Node 2: Wilderness node (Now a forest)
		alife.GetGraph().GetNode(2).position = JPH::RVec3(60.0, 12.0, 60.0);
		alife.GetGraph().GetNode(2).type = ALife::NodeType::Wilderness;

		// Plant a cluster of trees near the Wilderness Node
		auto treeShape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, 0.5f, 4.0f, 0.5f);
		for (int i = 0; i < 15; ++i) {
			float rx = 60.0f + ((std::rand() % 100) - 50.0f) * 0.3f;
			float rz = 60.0f + ((std::rand() % 100) - 50.0f) * 0.3f;
			reg.Add(reg.Create(),
					MeshComponent{.mesh = treeMesh, .material = material, .cullRadius = 10.0f},
					PhysicsComponent{Physics::CreateRigidBody(pc, treeShape, {rx, 13.0f, rz},
															  JPH::Quat::sIdentity(),
															  JPH::EMotionType::Static, 0)});
		}

		// Node 3: Creature Lair (Now a junkyard)
		alife.GetGraph().GetNode(3).position = JPH::RVec3(-60.0, 12.0, 60.0);
		alife.GetGraph().GetNode(3).type = ALife::NodeType::Lair;

		// Spawn messy Dumpsters near the Junkyard Node
		auto dumpsterShape =
			Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, 0.8f, 0.8f, 1.2f);
		for (int i = 0; i < 4; ++i) {
			JPH::RVec3 pos = {-60.0f + (i * 2.0f), 13.0f, 60.0f + ((i % 2) ? 1.5f : -1.5f)};
			reg.Add(reg.Create(),
					MeshComponent{.mesh = dumpsterMesh, .material = material, .cullRadius = 5.0f},
					PhysicsComponent{Physics::CreateRigidBody(
						pc, dumpsterShape, pos, JPH::Quat::sRotation(JPH::Vec3::sAxisY(), (float)i),
						JPH::EMotionType::Static, 0)});
		}

		// Connect nodes in a cyclic route
		alife.GetGraph().Connect(0, 1);
		alife.GetGraph().Connect(1, 2);
		alife.GetGraph().Connect(2, 3);
		alife.GetGraph().Connect(3, 0);

		// =====================================================================
		// --- 7. Spawn Player LAST ---
		// =====================================================================
		// Placing this after all static scenery guarantees the Player is the
		// absolute last entity with a PhysicsComponent, letting Lua's `#entities - 1` grab it.
		playerEntity = reg.Create();
		reg.Add(playerEntity,
				MeshComponent{.mesh = playerMesh, .material = material, .cullRadius = 5.0f});
		reg.Add(playerEntity, PhysicsComponent{Physics::CreateCharacter(
								  pc, {0.0f, 15.0f, 0.0f})}); // Spawn closer to the ground
		reg.Add(playerEntity, MovementComponent{.speed = 8.0f});

		// =====================================================================
		// --- 8. Set Up Simulator Callbacks ---
		// =====================================================================
		// ... (Keep your on_think, on_interaction, on_event, on_task_completed callbacks unchanged)

		// Define friendly/hostile faction relations
		alife.GetFactions().SetRelation(0, 1, -0.9f); // Faction 0 and 1 are hostile
		alife.GetFactions().SetRelation(1, 2, -0.9f); // Faction 1 and 2 are hostile
		alife.GetFactions().SetRelation(0, 2, 0.4f);  // Faction 0 and 2 are neutral-friendly

		// --- 9. Spawn Citizens (No PhysicsComponent, won't interfere with Lua indexing) ---
		for (int i = 0; i < 30; ++i) {
			Entity agent = reg.Create();
			reg.Add(agent,
					MeshComponent{.mesh = agentMesh, .material = material, .cullRadius = 5.0f});

			float x = -50.0f + (i * 3.5f);
			float z = -50.0f + (i * 2.5f);
			float y = 14.0f;

			reg.Add(agent,
					ALife::ALifeComponent{
						.position = JPH::RVec3(x, y, z),
						.state = ALife::State::Offline,
						.travel_speed = 3.5f + ((std::rand() % 100) *
												0.02f), // Slightly randomized patrol speed
						.faction_id = static_cast<uint32_t>(i % 3), // Distribute among 3 factions
						.self_entity = agent});
		}

		pc.OptimizeBroadphase();
	}
};

JPH::Array<ZHLN::Entity> s_VisibleEntities;
JPH::Vec3 s_LastCullPos;
float s_LastCullYaw = 0.0f;

void UpdateCameraSystem(Camera& cam, InputContext& input, Entity player, ECS::Registry& reg,
						const Physics::PhysicsWorld& world) {
	if (input.IsMouseButtonDown(KeyCode::RButton)) {
		cam.yaw += input.GetMouse().deltaX * 0.2f;
		cam.pitch = std::clamp(cam.pitch - (input.GetMouse().deltaY * 0.2f), -89.0f, 89.0f);
	}

	ZHLN_LOCK(world.sync.shadowLock) {
		if (auto* pComp = reg.Get<PhysicsComponent>(player)) {
			uint32_t dense = world.slotToDense[pComp->physicsHandle.index];
			const size_t base = static_cast<size_t>(dense) * 4;
			JPH::Vec3 target = {(float)world.positions[base],
								(float)world.positions[base + 1] + 1.0f,
								(float)world.positions[base + 2]};

			float yR = JPH::DegreesToRadians(cam.yaw);
			float pR = JPH::DegreesToRadians(cam.pitch);
			JPH::Vec3 dir(JPH::Cos(yR) * JPH::Cos(pR), JPH::Sin(pR), JPH::Sin(yR) * JPH::Cos(pR));
			cam.position = target - (dir.Normalized() * 10.0f);
		}
	}
}

// --- Modified Culling for Hybrid Rendering (Physics + pure ALife) ---
void UpdateCulling(Engine& engine) {
	ZHLN_PROFILE_SCOPE("Culling (ECS O(N))");
	auto& cam = engine.GetCamera();
	auto& reg = engine.GetRegistry();
	const auto& world = engine.GetPhysicsContext().GetWorld();

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

	ZHLN_LOCK(world.sync.shadowLock) {
		for (size_t i = 0; i < entities.size(); ++i) {
			Entity e = entities[i];

			JPH::Vec3 pos{};
			auto* phys = reg.Get<PhysicsComponent>(e);
			if (phys != nullptr) {
				uint32_t dense = world.slotToDense[phys->physicsHandle.index];
				const size_t base = static_cast<size_t>(dense) * 4;
				pos = JPH::Vec3((float)world.positions[base], (float)world.positions[base + 1],
								(float)world.positions[base + 2]);
			} else if (auto* alifeComp = reg.Get<ALife::ALifeComponent>(e)) {
				pos = JPH::Vec3(alifeComp->position);
			} else {
				continue;
			}

			if (cam.frustum.IsSphereVisible(pos, meshes[i].cullRadius)) {
				s_VisibleEntities.push_back(e);
			}
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
		.physics = {.maxBodies = 10000, .maxBodyPairs = 20000, .maxContactConstraints = 20000},
		.render = {.appName = "ZahlenEngine",
				   .width = 1280,
				   .height = 720,
				   .vsync = false,
				   .enableValidation = true},
	};

	Engine engine(config);
	engine.GetWindow().Focus();

	auto& rc = engine.GetRenderContext();
	auto& pc = engine.GetPhysicsContext();
	auto& reg = engine.GetRegistry();
	auto& cam = engine.GetCamera();

	ScriptRunner scriptRunner;
	scriptRunner.RunFile("scripts/gameplay.lua");
	FileWatcher gameplayWatcher("scripts/gameplay.lua");
	uint32_t frameCounter = 0;

	Scene scene{};
	scene.Setup(engine);

	float accumulator = 0.0f;
	const float targetDt = 1.0f / 60.0f;

	uint32_t fontAtlasIdx = AssetFactory::CreateFontAtlasTexture(rc);

	Mesh helloText = GUI::CreateTextMesh(rc, "Hello Custom GUI World!", 25.0f, 25.0f, 3.0f,
										 {0.2f, 0.8f, 0.4f, 1.0f});

	while (engine.IsRunning()) {
		float frameTime = clock.GetDeltaTime();
		accumulator += frameTime;

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
			if (!ImGui::GetIO().WantCaptureKeyboard) {
				scriptRunner.CallUpdate(&engine, frameTime);
			}
			MovementSystem(engine, frameTime);

			// =================================================================
			// --- Tick the ALife Subsystem ---
			// =================================================================
			engine.GetALife().Update(engine, frameTime, JPH::RVec3(cam.position));
		}

		while (accumulator >= targetDt) {
			pc.Step(targetDt);
			accumulator -= targetDt;
		}

		// --- Core Orchestration ---
		const auto& world = pc.GetWorld();
		auto res = engine.GetWindow().GetSize();

		if (res.width > 0 && res.height > 0) {
			if (g_TAAState.enabled) {
				g_TAAState.frameIndex++;
			} else {
				g_TAAState.frameIndex = 0;
			}

			UpdateCameraSystem(cam, engine.GetInput(), scene.playerEntity, reg, world);

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

			// Compute Light View-Projection (Shadow) matrix
			JPH::Vec3 sunDirection = {0.5f, 1.0f, 0.2f}; // Matching shader sun direction
			JPH::Mat44 lightView =
				Math::CreateLookAt(sunDirection * 100.0f, {0.0f, 0.0f, 0.0f}, JPH::Vec3::sAxisY());
			JPH::Mat44 lightProj = Math::CreateOrtho(-50.0f, 50.0f, -50.0f, 50.0f, 0.1f, 200.0f);
			JPH::Mat44 shadowProjView = lightProj * lightView;

			// Compute bias matrix for shadow map sampling projection
			JPH::Mat44 biasMatrix = {
				JPH::Vec4(0.5f, 0.0f, 0.0f, 0.0f), JPH::Vec4(0.0f, 0.5f, 0.0f, 0.0f),
				JPH::Vec4(0.0f, 0.0f, 1.0f, 0.0f), JPH::Vec4(0.5f, 0.5f, 0.0f, 1.0f)};
			JPH::Mat44 lightSpaceBiased = biasMatrix * shadowProjView;

			// Prepare Frame Uniforms payload
			FrameUniforms uniforms{};
			uniforms.viewProj = vp;
			uniforms.prevViewProj = unjitteredVp;
			uniforms.lightSpaceMatrix = lightSpaceBiased;
			std::memcpy(&uniforms.camPos[0], &cam.position, sizeof(float) * 3);
			std::memcpy(&uniforms.lightDir[0], &sunDirection, sizeof(float) * 3);
			uniforms.lightCount = 0; // Populate this with active dynamic lights from scene query

			// Upload Uniforms and Light lists to the GPU via the public API [c]
			Renderer::SetFrameData(rc, uniforms, shadowProjView);

			// Renderer call now executes with two internal passes [c]
			engine.BeginFrame();
			Renderer::SetMatrices(rc, vp, unjitteredVp);

			// --- Modified Rendering Logic: Support pure simulated positions ---
			ZHLN_LOCK(world.sync.shadowLock) {
				for (Entity e : s_VisibleEntities) {
					auto* mesh = reg.Get<MeshComponent>(e);
					if (mesh == nullptr) {
						continue;
					}

					JPH::Mat44 currentTransform{};
					auto* phys = reg.Get<PhysicsComponent>(e);

					if (phys != nullptr) {
						uint32_t dense = world.slotToDense[phys->physicsHandle.index];
						const size_t base = static_cast<size_t>(dense) * 4;
						JPH::Vec3 pos((float)world.positions[base],
									  (float)world.positions[base + 1],
									  (float)world.positions[base + 2]);
						JPH::Quat rot(world.rotations[base], world.rotations[base + 1],
									  world.rotations[base + 2], world.rotations[base + 3]);
						currentTransform = Math::CreateTransform(pos, rot);
					} else if (auto* alifeComp = reg.Get<ALife::ALifeComponent>(e)) {
						// Render purely simulated ALife agents using their own simulated
						// coordinates
						currentTransform = Math::CreateTransform(JPH::Vec3(alifeComp->position),
																 JPH::Quat::sIdentity());
					} else {
						continue;
					}

					Renderer::Draw(rc, mesh->material, mesh->mesh, currentTransform,
								   mesh->prevTransform);

					mesh->prevTransform = currentTransform;
				}
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
