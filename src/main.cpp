#include "engine/FileWatcher.hpp"
#include "engine/Platform.hpp"
#include "imgui.h"

#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Scripting.hpp>
#include <algorithm>
#include <detail/ControlFlow.hpp>
#include <physics/PhysicsWorld.hpp>
#include <threading/Mutex.hpp>
#include <threading/TaskSystem.hpp>

namespace ZHLN {
void DrawConsole(ScriptRunner& runner);
void DrawProfiler(Engine& engine);
void MovementSystem(Engine& engine, float dt);
} // namespace ZHLN

using namespace ZHLN;

struct Scene {
	ZHLN::Entity playerEntity;

	void Setup(Engine& engine) {
		auto& rc = engine.GetRenderContext();
		auto& pc = engine.GetPhysicsContext();
		auto& reg = engine.GetRegistry();

		reg.RegisterComponent<MeshComponent>();
		reg.RegisterComponent<PhysicsComponent>();
		reg.RegisterComponent<MovementComponent>();
		reg.RegisterComponent<ALife::ALifeComponent>(); // <-- Register ALife Component

		// --- Procedural Terrain Setup ---
		int terrainSize = 128;
		float terrainWorldSize = 250.0f;
		float terrainMaxHeight = 25.0f;
		std::vector<float> terrainHeights;

		Mesh terrainMesh = AssetFactory::CreateTerrain(rc, terrainSize, terrainWorldSize,
													   terrainMaxHeight, terrainHeights);
		Mesh boxMesh = AssetFactory::CreateBox(rc, {0.5f, 0.5f, 0.5f}, {0.8f, 0.3f, 0.2f, 1.0f});
		Mesh playerMesh = AssetFactory::CreateBox(rc, {0.5f, 0.9f, 0.5f}, {0.2f, 0.6f, 0.9f, 1.0f});
		Material material = AssetFactory::CreateBasicMaterial(rc);

		auto terrainShape =
			Physics::CreateHeightFieldShape(terrainHeights, terrainSize, terrainWorldSize);
		auto boxShape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, 0.5f, 0.5f, 0.5f);

		// --- Spawn Terrain (Static) ---
		reg.Add(reg.Create(),
				MeshComponent{.mesh = terrainMesh, .material = material, .cullRadius = 300.0f},
				PhysicsComponent{Physics::CreateRigidBody(pc, terrainShape, {0.0f, 0.0f, 0.0f},
														  JPH::Quat::sIdentity(),
														  JPH::EMotionType::Static, 0)});

		// --- Spawn Dynamic Cascading Prop Boxes ---
		for (int i = 0; i < 2000; ++i) {
			float x = ((float)(i % 50) - 25.0f) * 4.0f;
			float z = ((float)(i / 50) - 20.0f) * 5.0f;
			float y = 32.0f + (i * 0.15f);

			Entity propPhys = Physics::CreateRigidBody(
				pc, boxShape, {x, y, z}, JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, 1);

			reg.Add(reg.Create(),
					MeshComponent{.mesh = boxMesh, .material = material, .cullRadius = 1.0f},
					PhysicsComponent{propPhys});
		}

		// Spawn player safely
		playerEntity = reg.Create();
		reg.Add(playerEntity,
				MeshComponent{.mesh = playerMesh, .material = material, .cullRadius = 1.5f});
		reg.Add(playerEntity, PhysicsComponent{Physics::CreateCharacter(pc, {0.0f, 35.0f, 0.0f})});
		reg.Add(playerEntity, MovementComponent{.speed = 8.0f});

		// =====================================================================
		// --- Configure Level Graph & Waypoints ---
		// =====================================================================
		auto& alife = engine.GetALife();
		alife.GetGraph() = ALife::LevelGraph(4);

		// Node 0: Campfire
		alife.GetGraph().GetNode(0).position = JPH::RVec3(-60.0, 12.0, -60.0);
		alife.GetGraph().GetNode(0).type = ALife::NodeType::Campfire;

		// Node 1: Camp / Hub
		alife.GetGraph().GetNode(1).position = JPH::RVec3(60.0, 12.0, -60.0);
		alife.GetGraph().GetNode(1).type = ALife::NodeType::Hub;

		// Node 2: Wilderness node
		alife.GetGraph().GetNode(2).position = JPH::RVec3(60.0, 12.0, 60.0);
		alife.GetGraph().GetNode(2).type = ALife::NodeType::Wilderness;

		// Node 3: Creature Lair
		alife.GetGraph().GetNode(3).position = JPH::RVec3(-60.0, 12.0, 60.0);
		alife.GetGraph().GetNode(3).type = ALife::NodeType::Lair;

		// Connect nodes in a cyclic route
		alife.GetGraph().Connect(0, 1);
		alife.GetGraph().Connect(1, 2);
		alife.GetGraph().Connect(2, 3);
		alife.GetGraph().Connect(3, 0);

		// =====================================================================
		// --- Set Up Simulator Callbacks ---
		// =====================================================================

		// Think Callback (Runs when an agent is idle)
		alife.on_think = [](ALife::Simulator& sim, Entity e) {
			auto& r = GetEngineContext()->GetRegistry();
			auto* comp = r.Get<ALife::ALifeComponent>(e);
			if (!comp)
				return;

			if (comp->current_node == ALife::INVALID_GRAPH_NODE) {
				comp->current_node = sim.GetGraph().FindClosest(comp->position);
			}

			if (comp->current_node != ALife::INVALID_GRAPH_NODE) {
				const auto& node = sim.GetGraph().GetNode(comp->current_node);
				if (node.neighbor_count > 0) {
					uint32_t next = node.neighbors[std::rand() % node.neighbor_count];
					ALife::PathWorkspace ws(sim.GetGraph().GetNodeCount(),
											std::pmr::new_delete_resource());

					comp->path_count =
						sim.GetGraph().FindPath(comp->current_node, next, comp->path, ws);
					comp->path_index = 0;
					comp->target_node = next;
				}
			}
		};

		// Interaction Callback (Triggered offline when two agents are near each other)
		alife.on_interaction = [](ALife::Simulator& sim, Entity e1, Entity e2) {
			auto& r = GetEngineContext()->GetRegistry();
			sim.ResolveOfflineInteraction(r, e1, e2);
		};

		// Event Callback (Pipes state changes, battles, and deaths to the console)
		alife.on_event = [](ALife::Simulator&, const ALife::Event& ev) {
			if (ev.type == ALife::EventType::StateChange) {
				const char* states[] = {"Offline", "Online", "Dead"};
				ZHLN::Log("ALife State Change: Entity {} became {}", ev.subject.index,
						  states[static_cast<int>(ev.state_change.new_state)]);
			} else if (ev.type == ALife::EventType::Death) {
				ZHLN::Log("ALife Death Event: Entity {} was killed!", ev.subject.index);
			}
		};

		// Task Completed Callback (Rest at waypoints)
		alife.on_task_completed = [](ALife::Simulator&, Entity e) {
			auto& r = GetEngineContext()->GetRegistry();
			auto* comp = r.Get<ALife::ALifeComponent>(e);
			if (comp) {
				comp->wait_time = 1000 + (std::rand() % 2000); // Wait 1 to 3 simulated seconds
				ZHLN::Log("ALife Agent {} arrived at waypoint {} and is resting.", e.index,
						  comp->current_node);
			}
		};

		// Define friendly/hostile faction relations
		alife.GetFactions().SetRelation(0, 1, -0.9f); // Faction 0 and 1 are hostile
		alife.GetFactions().SetRelation(1, 2, -0.9f); // Faction 1 and 2 are hostile
		alife.GetFactions().SetRelation(0, 2, 0.4f);  // Faction 0 and 2 are neutral-friendly

		// --- Spawn Distinct Green ALife Agents ---
		Mesh agentMesh = AssetFactory::CreateBox(rc, {0.6f, 0.6f, 0.6f}, {0.1f, 0.8f, 0.3f, 1.0f});
		for (int i = 0; i < 30; ++i) {
			Entity agent = reg.Create();
			reg.Add(agent,
					MeshComponent{.mesh = agentMesh, .material = material, .cullRadius = 1.0f});

			float x = -50.0f + (i * 3.5f);
			float z = -50.0f + (i * 2.5f);
			float y = 14.0f;

			reg.Add(agent,
					ALife::ALifeComponent{
						.position = JPH::RVec3(x, y, z),
						.state = ALife::State::Offline,
						.travel_speed =
							3.5f + (std::rand() % 100) * 0.02f, // Slightly randomized patrol speed
						.faction_id = static_cast<uint32_t>(i % 3), // Distribute among 3 factions
						.self_entity = agent});
		}

		pc.OptimizeBroadphase();
	}
};

namespace {

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
			JPH::Real* p = &world.positions[dense * 4];
			JPH::Vec3 target = {(float)p[0], (float)p[1] + 1.0f, (float)p[2]};

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

			JPH::Vec3 pos;
			auto* phys = reg.Get<PhysicsComponent>(e);
			if (phys) {
				uint32_t dense = world.slotToDense[phys->physicsHandle.index];
				pos = JPH::Vec3((float)world.positions[dense * 4],
								(float)world.positions[dense * 4 + 1],
								(float)world.positions[dense * 4 + 2]);
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

			engine.BeginFrame();
			Renderer::Clear(rc, {0.08f, 0.09f, 0.12f, 1.0f});

			Renderer::SetMatrices(rc, vp, unjitteredVp);

			// --- Modified Rendering Logic: Support pure simulated positions ---
			ZHLN_LOCK(world.sync.shadowLock) {
				for (Entity e : s_VisibleEntities) {
					auto* mesh = reg.Get<MeshComponent>(e);
					if (!mesh) {
						continue;
					}

					JPH::Mat44 currentTransform;
					auto* phys = reg.Get<PhysicsComponent>(e);

					if (phys) {
						uint32_t dense = world.slotToDense[phys->physicsHandle.index];
						JPH::Vec3 pos((float)world.positions[dense * 4],
									  (float)world.positions[dense * 4 + 1],
									  (float)world.positions[dense * 4 + 2]);
						JPH::Quat rot(world.rotations[dense * 4], world.rotations[dense * 4 + 1],
									  world.rotations[dense * 4 + 2],
									  world.rotations[dense * 4 + 3]);
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

			engine.EndFrame();
		} else {
			Platform::Sleep(10);
		}
	}

	ZHLN::Log("Shutting down engine...");
	return 0;
}
