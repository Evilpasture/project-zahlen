#include <GLFW/glfw3.h> // Fixes: GLFWwindow, glfwGetMouseButton, etc.
#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/physics/Physics_C.h>
#include <algorithm>
#include <array>
#include <detail/ControlFlow.hpp> // Fixes: ZHLN_LOCK
#include <engine/Platform.hpp>	  // Fixes: Platform
#include <imgui.h>
#include <physics/PhysicsWorld.hpp>
#include <threading/TaskSystem.hpp>
#include <vector>

using namespace ZHLN;

namespace {

// ============================================================================
// Editor State & Global Systems
// ============================================================================
struct EditorState {
	EditorState() = default;
	EditorState(const EditorState&) = default;
	EditorState(EditorState&&) = default;
	auto operator=(const EditorState&) -> EditorState& = default;
	auto operator=(EditorState&&) -> EditorState& = default;
	~EditorState() = default;
	bool simulationRunning = false;		// Pauses/runs physics and ALife [c]
	Entity selectedEntity = NullEntity; // Currently selected ECS entity
	bool showPhysicsDebug = true;
};

EditorState g_EditorState;
JPH::Array<Entity> s_VisibleEntities;
JPH::Vec3 s_LastCullPos;
float s_LastCullYaw = 0.0f;

// ============================================================================
// Free-Fly Editor Camera
// ============================================================================
void UpdateEditorCamera(Camera& cam, const InputContext& input, float dt) {
	const float speed = 25.0f; // Movement speed (units per second)
	const float sensitivity = 0.15f;

	// 1. Mouse look (Active only when holding Right Click)
	if (input.IsMouseButtonDown(KeyCode::RButton)) {
		cam.yaw += input.GetMouse().deltaX * sensitivity;
		cam.pitch = std::clamp(cam.pitch - (input.GetMouse().deltaY * sensitivity), -89.0f, 89.0f);
	}

	// 2. Trigonometric calculations for move direction vectors
	float yawRad = JPH::DegreesToRadians(cam.yaw);
	float pitchRad = JPH::DegreesToRadians(cam.pitch);

	JPH::Vec3 forward(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad),
					  JPH::Sin(yawRad) * JPH::Cos(pitchRad));
	forward = forward.Normalized();
	JPH::Vec3 right = forward.Cross(JPH::Vec3::sAxisY()).Normalized();

	// 3. Collect keyboard movement inputs [c]
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

// ============================================================================
// Screen-Space Viewport Raycasting (3D Picking)
// ============================================================================
auto CastPickingRay(Engine& engine, const Camera& cam) -> Physics::RaycastResult {
	const auto& input = engine.GetInput();
	auto mouse = input.GetMouse();
	auto winSize = engine.GetWindow().GetSize();

	if (winSize.width == 0 || winSize.height == 0) {
		return {};
	}

	// 1. Map to Normalized Device Coordinates [-1, 1] [c]
	float ndcX = (2.0f * mouse.x) / (float)winSize.width - 1.0f;
	float ndcY = 1.0f - (2.0f * mouse.y) / (float)winSize.height;

	// 2. Unproject vectors through inverse View-Projection matrix
	float aspect = (float)winSize.width / (float)winSize.height;
	JPH::Mat44 invVP = (cam.GetProjectionMatrix(aspect) * cam.GetViewMatrix()).Inversed();

	JPH::Vec4 nearWorld = invVP * JPH::Vec4(ndcX, ndcY, 0.0f, 1.0f); // Near is 0.0 in Vulkan [c]
	JPH::Vec4 farWorld = invVP * JPH::Vec4(ndcX, ndcY, 1.0f, 1.0f);	 // Far is 1.0 in Vulkan [c]

	JPH::Vec3 pNear =
		JPH::Vec3(nearWorld.GetX() / nearWorld.GetW(), nearWorld.GetY() / nearWorld.GetW(),
				  nearWorld.GetZ() / nearWorld.GetW());
	JPH::Vec3 pFar = JPH::Vec3(farWorld.GetX() / farWorld.GetW(), farWorld.GetY() / farWorld.GetW(),
							   farWorld.GetZ() / farWorld.GetW());
	JPH::Vec3 dir = (pFar - pNear).Normalized();

	// 3. Execute narrowing phase raycast query via Jolt [c]
	return Physics::Raycast(engine.GetPhysicsContext(), JPH::RVec3(pNear), dir, 1000.0f);
}

// ============================================================================
// Editor Frustum Culling
// ============================================================================
void UpdateEditorCulling(Engine& engine) {
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

			if (auto* phys = reg.Get<PhysicsComponent>(e)) {
				uint32_t dense = world.slotToDense[phys->physicsHandle.index];
				const size_t base = static_cast<size_t>(dense) * 4;
				pos = JPH::Vec3((float)world.positions[base], (float)world.positions[base + 1],
								(float)world.positions[base + 2]);
			} else if (auto* alifeComp = reg.Get<ALife::ALifeComponent>(e)) {
				pos = JPH::Vec3(alifeComp->position);
			} else {
				pos = meshes[i].localTransform.GetTranslation();
			}

			if (cam.frustum.IsSphereVisible(pos, meshes[i].cullRadius)) {
				s_VisibleEntities.push_back(e);
			}
		}
	}

	s_LastCullPos = cam.position;
	s_LastCullYaw = cam.yaw;
}

// ============================================================================
// UI Draw Panels
// ============================================================================
void DrawEditorPanels(Engine& engine) {
	auto& reg = engine.GetRegistry();
	auto& pc = engine.GetPhysicsContext();
	const auto& world = pc.GetWorld();

	// 1. Control Toolbar
	ImGui::Begin("Toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
	ImGui::SetWindowPos({0, 0});
	ImGui::SetWindowSize({(float)engine.GetWindow().GetSize().width, 42.0f});

	if (ImGui::Button(g_EditorState.simulationRunning ? "⏸ PAUSE" : "▶ PLAY")) {
		g_EditorState.simulationRunning = !g_EditorState.simulationRunning;
	}
	ImGui::SameLine();
	if (ImGui::Button("⏵❘ Step Frame")) {
		pc.Step(1.0f / 60.0f);
	}

	// Fix: Replaced internal ImGui::SeparatorEx with standard, portable layout calls [c]
	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();

	ImGui::Checkbox("Show Colliders", &g_EditorState.showPhysicsDebug);
	ImGui::End();

	// 2. Scene Hierarchy Window
	ImGui::Begin("Scene Hierarchy");
	auto entities = reg.GetEntitiesWith<MeshComponent>();
	for (size_t i = 0; i < entities.size(); ++i) {
		Entity e = entities[i];
		std::string label = std::format("Entity [Index: {}, Gen: {}]", e.index, e.generation);
		bool isSelected = (g_EditorState.selectedEntity == e);
		if (ImGui::Selectable(label.c_str(), isSelected)) {
			g_EditorState.selectedEntity = e;
		}
	}
	ImGui::End();

	// 3. Component Inspector Window [c]
	ImGui::Begin("Component Inspector");
	if (g_EditorState.selectedEntity != NullEntity && reg.IsAlive(g_EditorState.selectedEntity)) {
		Entity e = g_EditorState.selectedEntity;
		ImGui::TextUnformatted(std::format("Active Entity ID: {}", e.index).c_str());
		ImGui::Separator();

		// Mesh Component Panel
		if (auto* mesh = reg.Get<MeshComponent>(e)) {
			if (ImGui::CollapsingHeader("Mesh Component", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::TextUnformatted(std::format("Vertices: {}", mesh->mesh.vertexCount).c_str());
				ImGui::DragFloat("Cull Radius", &mesh->cullRadius, 0.1f, 0.5f, 200.0f);
			}
		}

		// Physics Component Panel (Direct Jolt Teleportation) [c]
		if (auto* phys = reg.Get<PhysicsComponent>(e)) {
			if (ImGui::CollapsingHeader("Physics Component", ImGuiTreeNodeFlags_DefaultOpen)) {
				// Fix: Access Jolt BodyIDs using the const-safe free function [c]
				JPH::BodyID bid = Physics::GetBodyID(world, phys->physicsHandle);
				if (!bid.IsInvalid()) {
					JPH::RVec3 p = world.bodyInterface->GetPosition(bid);
					JPH::Quat r = world.bodyInterface->GetRotation(bid);
					JPH::Vec3 euler = Math::QuatToEulerDegrees(r);

					std::array<float, 3> posArr = {(float)p.GetX(), (float)p.GetY(),
												   (float)p.GetZ()};
					std::array<float, 3> rotArr = {euler.GetX(), euler.GetY(), euler.GetZ()};

					bool pMod = ImGui::DragFloat3("Position", posArr.data(), 0.05f);
					bool rMod = ImGui::DragFloat3("Rotation", rotArr.data(), 0.2f);

					if (pMod || rMod) {
						JPH::RVec3 nextPos(posArr[0], posArr[1], posArr[2]);
						JPH::Quat nextRot =
							Math::EulerDegreesToQuat(JPH::Vec3(rotArr[0], rotArr[1], rotArr[2]));
						world.bodyInterface->SetPositionAndRotation(bid, nextPos, nextRot,
																	JPH::EActivation::Activate);
					}
				}
			}
		}

		// ALife Component Panel
		if (auto* alife = reg.Get<ALife::ALifeComponent>(e)) {
			if (ImGui::CollapsingHeader("ALife Component", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::SliderInt("Health", &alife->health, 0, 100);
				ImGui::SliderInt("Power", &alife->power, 0, 100);

				const char* stateStr = "Offline";
				if (alife->state == ALife::State::Dead) {
					stateStr = "Dead";
				} else if (alife->state == ALife::State::Online) {
					stateStr = "Online";
				}
				ImGui::TextUnformatted(std::format("Current State: {}", stateStr).c_str());
			}
		}
	} else {
		ImGui::TextUnformatted("No entity selected. Left click elements in viewport to select.");
	}
	ImGui::End();
}

} // namespace

// ============================================================================
// Main Execution Entrypoint
// ============================================================================
int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
	Platform::Init();
	ZHLN::SetupSignalHandler();
	TaskSystem::Init();
	Clock clock;

	EngineConfig config{
		.physics = {.maxBodies = 10000, .maxBodyPairs = 20000, .maxContactConstraints = 20000},
		.render = {.appName = "Zahlen World Editor", .width = 1600, .height = 900, .vsync = true},
	};

	Engine engine(config);
	engine.GetWindow().Focus();

	auto& rc = engine.GetRenderContext();
	auto& pc = engine.GetPhysicsContext();
	auto& reg = engine.GetRegistry();
	auto& cam = engine.GetCamera();

	// Spawn a temporary workspace terrain and basic landmarks (Copying Setup from your old
	// main.cpp)
	reg.RegisterComponent<MeshComponent>("MeshComponent");
	reg.RegisterComponent<PhysicsComponent>("PhysicsComponent");
	reg.RegisterComponent<MovementComponent>("MovementComponent");
	reg.RegisterComponent<ALife::ALifeComponent>("ALifeComponent");

	ZHLN::Log("Initializing Editor Workspace Scene...");
	int terrainSize = 128;
	float terrainWorldSize = 250.0f;
	float terrainMaxHeight = 25.0f;
	std::vector<float> terrainHeights;

	Mesh terrainMesh = AssetFactory::CreateTerrain(rc, terrainSize, terrainWorldSize,
												   terrainMaxHeight, terrainHeights);
	auto terrainShape =
		Physics::CreateHeightFieldShape(terrainHeights, terrainSize, terrainWorldSize);
	Material material = AssetFactory::CreateBasicMaterial(rc);

	reg.Add(reg.Create(),
			MeshComponent{.mesh = terrainMesh, .material = material, .cullRadius = 300.0f},
			PhysicsComponent{Physics::CreateRigidBody(pc, terrainShape, {0.0f, 0.0f, 0.0f},
													  JPH::Quat::sIdentity(),
													  JPH::EMotionType::Static, 0)});

	// Place some test boxes for 3D selection and transformation [c]
	auto boxShape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, 2.0f, 2.0f, 2.0f);
	for (int i = 0; i < 5; ++i) {
		Entity box = reg.Create();
		reg.Add(box, MeshComponent{.mesh = AssetFactory::CreateBox(rc, {2, 2, 2}),
								   .material = material,
								   .cullRadius = 10.f});
		reg.Add(box, PhysicsComponent{Physics::CreateRigidBody(
						 pc, boxShape, {(float)i * 6.0f, 15.0f, 0.0f}, JPH::Quat::sIdentity(),
						 JPH::EMotionType::Dynamic, 1)});
	}

	pc.OptimizeBroadphase();

	// Position the camera initially above our workspace
	cam.position = {0.0f, 20.0f, 40.0f};
	cam.yaw = -90.0f;
	cam.pitch = -20.0f;

	float accumulator = 0.0f;
	const float targetDt = 1.0f / 60.0f;

	while (engine.IsRunning()) {
		float frameTime = clock.GetDeltaTime();
		engine.ProcessEvents();

		// Left Click selection logic when not interacting with ImGui Windows
		if (engine.GetInput().IsKeyDown(KeyCode::Escape)) {
			engine.GetWindow().Close();
		}

		if (!engine.GetInput().IsKeyDown(KeyCode::Unknown) &&
			!engine.GetInput().IsMouseButtonDown(KeyCode::RButton) &&
			!ImGui::GetIO().WantCaptureMouse) {

			// Perform 3D viewport raycast selection on mouse press
			static bool wasMouseDown = false;
			bool isMouseDown =
				glfwGetMouseButton(static_cast<GLFWwindow*>(engine.GetWindow().GetNativeHandle()),
								   GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

			if (isMouseDown && !wasMouseDown) {
				auto hit = CastPickingRay(engine, cam);
				if (hit.hasHit) {
					g_EditorState.selectedEntity = hit.handle;
				} else {
					g_EditorState.selectedEntity = NullEntity;
				}
			}
			wasMouseDown = isMouseDown;
		}

		// Draw HUDs, Hierarchies, and Inspectors
		DrawEditorPanels(engine);

		if (engine.GetInput().NeedsResize()) {
			rc.SetResolution(engine.GetInput().GetNewSize());
			engine.GetInput().ClearResizeFlag();
			continue;
		}

		// 1. Only tick simulation systems if explicitly clicked "PLAY" [c]
		if (g_EditorState.simulationRunning) {
			accumulator += frameTime;
			while (accumulator >= targetDt) {
				pc.Step(targetDt);
				accumulator -= targetDt;
			}
			engine.GetALife().Update(engine, frameTime, JPH::RVec3(cam.position));
		}

		// 2. Editor free camera moves regardless of whether simulation is running [c]
		UpdateEditorCamera(cam, engine.GetInput(), frameTime);

		// 3. Render Viewport
		const auto& worldState = pc.GetWorld();
		auto res = engine.GetWindow().GetSize();

		if (res.width > 0 && res.height > 0) {
			if (g_TAAState.enabled) {
				g_TAAState.frameIndex++;
			} else {
				g_TAAState.frameIndex = 0;
			}

			JPH::Mat44 unjitteredProj = cam.GetProjectionMatrix((float)res.width / res.height);
			JPH::Mat44 unjitteredVp = unjitteredProj * cam.GetViewMatrix();

			JPH::Mat44 vp = unjitteredVp;
			if (g_TAAState.enabled) {
				vp = cam.GetJitteredProjectionMatrix((float)res.width / res.height, res.width,
													 res.height) *
					 cam.GetViewMatrix();
			}

			cam.frustum.Update(vp);
			UpdateEditorCulling(engine);

			// Shadow Matrices
			JPH::Vec3 sunDirection = {0.5f, 1.0f, 0.2f};
			JPH::Mat44 lightView =
				Math::CreateLookAt(sunDirection * 100.0f, {0.0f, 0.0f, 0.0f}, JPH::Vec3::sAxisY());
			JPH::Mat44 lightProj = Math::CreateOrtho(-50.0f, 50.0f, -50.0f, 50.0f, 0.1f, 200.0f);
			JPH::Mat44 shadowProjView = lightProj * lightView;

			// Change the second column's Y component from 0.5f to -0.5f
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

			// Draw objects [c]
			ZHLN_LOCK(worldState.sync.shadowLock) {
				for (Entity e : s_VisibleEntities) {
					auto* mesh = reg.Get<MeshComponent>(e);
					if (mesh == nullptr) {
						continue;
					}

					JPH::Mat44 currentTransform{};
					auto* phys = reg.Get<PhysicsComponent>(e);

					if (phys != nullptr) {
						uint32_t dense = worldState.slotToDense[phys->physicsHandle.index];
						const size_t base = static_cast<size_t>(dense) * 4;
						JPH::Vec3 pos((float)worldState.positions[base],
									  (float)worldState.positions[base + 1],
									  (float)worldState.positions[base + 2]);
						JPH::Quat rot(worldState.rotations[base], worldState.rotations[base + 1],
									  worldState.rotations[base + 2],
									  worldState.rotations[base + 3]);
						currentTransform = Math::CreateTransform(pos, rot) * mesh->localTransform;
					} else if (auto* alifeComp = reg.Get<ALife::ALifeComponent>(e)) {
						currentTransform = Math::CreateTransform(JPH::Vec3(alifeComp->position),
																 JPH::Quat::sIdentity()) *
										   mesh->localTransform;
					} else {
						currentTransform = mesh->localTransform;
					}

					Renderer::Draw(rc, mesh->material, mesh->mesh, currentTransform,
								   mesh->prevTransform, mesh->cullRadius, mesh->jointOffset,
								   mesh->isSkinned);
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

	ZHLN::Log("Shutting down Editor...");
	return 0;
}
