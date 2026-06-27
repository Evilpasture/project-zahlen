// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/editor_main.cpp
#include "Zahlen/Camera.hpp"
#include "Zahlen/CommandLine.hpp"
#include "Zahlen/Input.hpp"
#include "Zahlen/Render.hpp"
#include "Zahlen/Window.hpp"
#include "Zahlen/alife/Simulator.hpp"
#include "Zahlen/alife/Types.hpp"
#include "ecs/ECS.hpp"
#include "engine/Resources.hpp"
#include "physics/Physics.hpp"
#include "physics/PhysicsDebug.hpp"

#include <GLFW/glfw3.h>
#include <Jolt/Physics/Collision/CastResult.h>
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
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <detail/ControlFlow.hpp>
#include <engine/Platform.hpp>
#include <engine/system/CullingSystem.hpp>
#include <engine/system/PhysicsStateSystem.hpp>
#include <expected>
#include <imgui.h>
#include <physics/PhysicsWorld.hpp>
#include <span>
#include <thread>
#include <threading/TaskSystem.hpp>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

using namespace ZHLN;

namespace {

// ============================================================================
// Editor State & Global Systems
// ============================================================================
struct EditorState {
	bool simulationRunning = false;		// Pauses/runs physics and ALife
	Entity selectedEntity = NullEntity; // Currently selected ECS entity
	bool showPhysicsDebug = true;		// Keep this exactly as it was
};

EditorState g_EditorState;
JPH::Array<Entity> s_VisibleEntities;
JPH::Array<Entity> s_VisibleShadowEntities;
AAState s_AAState;

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

	// 1. Map to Normalized Device Coordinates [-1, 1]
	float ndcX = (2.0f * mouse.x) / (float)winSize.width - 1.0f;
	float ndcY = 1.0f - (2.0f * mouse.y) / (float)winSize.height;

	// 2. Unproject vectors through inverse View-Projection matrix
	float aspect = (float)winSize.width / (float)winSize.height;
	JPH::Mat44 invVP = (cam.GetProjectionMatrix(aspect) * cam.GetViewMatrix()).Inversed();

	JPH::Vec4 nearWorld = invVP * JPH::Vec4(ndcX, ndcY, 0.0f, 1.0f); // Near is 0.0 in Vulkan
	JPH::Vec4 farWorld = invVP * JPH::Vec4(ndcX, ndcY, 1.0f, 1.0f);	 // Far is 1.0 in Vulkan

	JPH::Vec3 pNear =
		JPH::Vec3(nearWorld.GetX() / nearWorld.GetW(), nearWorld.GetY() / nearWorld.GetW(),
				  nearWorld.GetZ() / nearWorld.GetW());
	JPH::Vec3 pFar = JPH::Vec3(farWorld.GetX() / farWorld.GetW(), farWorld.GetY() / farWorld.GetW(),
							   farWorld.GetZ() / farWorld.GetW());
	JPH::Vec3 dir = (pFar - pNear).Normalized();

	// 3. Execute narrowing phase raycast query via Jolt
	return Physics::Raycast(engine.GetPhysicsContext(), JPH::RVec3(pNear), dir, 1000.0f);
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
	ImGui::SetWindowPos({0, 0});
	ImGui::SetWindowSize({(float)engine.GetWindow().GetSize().width, 42.0f});

	if (ImGui::Button(g_EditorState.simulationRunning ? "⏸ PAUSE" : "▶ PLAY")) {
		g_EditorState.simulationRunning = !g_EditorState.simulationRunning;
	}
	ImGui::SameLine();
	if (ImGui::Button("⏵❘ Step Frame")) {
		pc.Step(1.0f / 60.0f);
	}

	ImGui::SameLine();
	ImGui::TextDisabled("|");
	ImGui::SameLine();

	// 2. Scene Hierarchy Window
	ImGui::Begin("Scene Hierarchy");
	auto entities = reg.GetEntitiesWith<MeshComponent>();
	for (auto e : entities) {
		std::string label = std::format("Entity [Index: {}, Gen: {}]", e.index, e.generation);
		bool isSelected = (g_EditorState.selectedEntity == e);
		if (ImGui::Selectable(label.c_str(), isSelected)) {
			g_EditorState.selectedEntity = e;
		}
	}
	ImGui::End();

	// 3. Component Inspector Window
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

		// PBR Component Panel
		if (auto* pbr = reg.Get<PBRComponent>(e)) {
			if (ImGui::CollapsingHeader("PBR Component", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::SliderFloat("Roughness", &pbr->roughness, 0.0f, 1.0f);
				ImGui::SliderFloat("Metallic", &pbr->metallic, 0.0f, 1.0f);
			}
		}

		// Physics Component Panel (Direct Jolt Teleportation)
		if (auto* phys = reg.Get<PhysicsComponent>(e)) {
			if (ImGui::CollapsingHeader("Physics Component", ImGuiTreeNodeFlags_DefaultOpen)) {
				// Fix: Access Jolt BodyIDs using the const-safe free function
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

// ============================================================================
// STAGE 2: SUBSYSTEM INITIALIZATION & WORKSPACE WINDOW CREATION
// ============================================================================

std::expected<std::unique_ptr<Engine>, EngineError> InitializeEditor(CommandLineOptions options) {
	Platform::Init();
	ZHLN::SetupSignalHandler();
	TaskSystem::Init();

	EngineConfig config{
		.physics = {.maxBodies = 10000, .maxBodyPairs = 20000, .maxContactConstraints = 20000},
		.render = {.appName = "Zahlen World Editor",
				   .width = 1600,
				   .height = 900,
				   .vsync = true,
				   .enableValidation = options.enableValidation},
	};

	const char* initError = nullptr;
	auto engine = Engine::Create(config, &initError);

	if (!engine) {
		return std::unexpected(EngineError{
			.msg = (initError != nullptr) ? initError : "Unknown editor initialization error.",
			.code = EXIT_FAILURE});
	}

	engine->GetWindow().Focus();
	return engine;
}

// ============================================================================
// STAGE 3: WORKSPACE COMPONENT & GEOMETRY GENERATION
// ============================================================================

bool InitializeEditorScene(Engine& engine) {
	auto& rc = engine.GetRenderContext();
	auto& pc = engine.GetPhysicsContext();
	auto& reg = engine.GetRegistry();
	auto& cam = engine.GetCamera();

	// Register visual and physical components
	reg.RegisterComponent<MeshComponent>("MeshComponent");
	reg.RegisterComponent<PhysicsComponent>("PhysicsComponent");
	reg.RegisterComponent<PhysicsStateComponent>("PhysicsStateComponent");
	reg.RegisterComponent<MovementComponent>("MovementComponent");
	reg.RegisterComponent<ALife::ALifeComponent>("ALifeComponent");
	reg.RegisterComponent<NameComponent>("NameComponent");
	reg.RegisterComponent<TargetCameraComponent>("TargetCameraComponent");
	reg.RegisterComponent<PBRComponent>("PBRComponent");

	ZHLN::Log("Initializing Editor Workspace Scene...");
	int terrainSize = 128;
	float terrainWorldSize = 250.0f;
	float terrainMaxHeight = 25.0f;
	std::vector<float> terrainHeights(static_cast<size_t>(terrainSize * terrainSize));

	Mesh terrainMesh = AssetFactory::CreateTerrain(rc, terrainSize, terrainWorldSize,
												   terrainMaxHeight, terrainHeights.data());
	auto terrainShape =
		Physics::CreateHeightFieldShape(terrainHeights, terrainSize, terrainWorldSize);
	Material material = AssetFactory::CreateBasicMaterial(rc);

	Entity terrainEnt = reg.Create();
	reg.Add(terrainEnt,
			MeshComponent{.mesh = terrainMesh, .material = material, .cullRadius = 300.0f},
			PhysicsComponent{Physics::CreateRigidBody(pc, terrainShape, {0.0f, 0.0f, 0.0f},
													  JPH::Quat::sIdentity(),
													  JPH::EMotionType::Static, 0)},
			PhysicsStateComponent{});

	// Place 3D selection test boxes
	auto boxShape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, 2.0f, 2.0f, 2.0f);
	for (int i = 0; i < 5; ++i) {
		Entity box = reg.Create();
		reg.Add(box, MeshComponent{.mesh = AssetFactory::CreateBox(rc, {2, 2, 2}),
								   .material = material,
								   .cullRadius = 10.f});
		reg.Add(box, PhysicsComponent{Physics::CreateRigidBody(
						 pc, boxShape, {i * 6.0f, 15.0f, 0.0f}, JPH::Quat::sIdentity(),
						 JPH::EMotionType::Dynamic, 1)});
		reg.Add(box, PhysicsStateComponent{.currPosition = {i * 6.0f, 15.0f, 0.0f},
										   .prevPosition = {i * 6.0f, 15.0f, 0.0f},
										   .currRotation = JPH::Quat::sIdentity(),
										   .prevRotation = JPH::Quat::sIdentity()});
	}

	pc.OptimizeBroadphase();

	// Set starting viewport transform
	cam.position = {0.0f, 20.0f, 40.0f};
	cam.yaw = -90.0f;
	cam.pitch = -20.0f;

	return true;
}

// ============================================================================
// STAGE 4: MAIN WORKSPACE INTERACTION LOOP
// ============================================================================

std::expected<int, EngineError> RunEditorLoop(std::unique_ptr<Engine> engine, uint32_t fpsLimit) {
	Clock clock;

	if (!InitializeEditorScene(*engine)) {
		return std::unexpected(
			EngineError{.msg = "Editor scene failed to initialize.", .code = EXIT_FAILURE});
	}

	auto& rc = engine->GetRenderContext();
	auto& pc = engine->GetPhysicsContext();
	auto& reg = engine->GetRegistry();
	auto& cam = engine->GetCamera();

	float accumulator = 0.0f;
	const float targetDt = 1.0f / 60.0f;
	const double targetFrameTime = fpsLimit > 0 ? 1.0 / static_cast<double>(fpsLimit) : 0.0;
	auto frameStart = std::chrono::high_resolution_clock::now();

	while (engine->IsRunning()) {
		float frameTime = clock.GetDeltaTime();
		engine->ProcessEvents();

		if (engine->GetInput().IsKeyDown(KeyCode::Escape)) {
			engine->GetWindow().Close();
		}

		if (!engine->GetInput().IsKeyDown(KeyCode::Unknown) &&
			!engine->GetInput().IsMouseButtonDown(KeyCode::RButton) &&
			!ImGui::GetIO().WantCaptureMouse) {

			static bool wasMouseDown = false;
			bool isMouseDown =
				glfwGetMouseButton(static_cast<GLFWwindow*>(engine->GetWindow().GetNativeHandle()),
								   GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

			if (isMouseDown && !wasMouseDown) {
				auto hit = CastPickingRay(*engine, cam);
				if (hit.hasHit) {
					g_EditorState.selectedEntity = hit.handle;
				} else {
					g_EditorState.selectedEntity = NullEntity;
				}
			}
			wasMouseDown = isMouseDown;
		}

		// UI render pass
		DrawEditorPanels(*engine);

		if (engine->GetInput().NeedsResize()) {
			rc.SetResolution(engine->GetInput().GetNewSize());
			engine->GetInput().ClearResizeFlag();
			ImGui::EndFrame();
			continue;
		}

		// Physics simulation step
		if (g_EditorState.simulationRunning) {
			accumulator += frameTime;
			while (accumulator >= targetDt) {
				pc.Step(targetDt);
				ZHLN::PhysicsStateSystem::WriteBack(*engine);
				accumulator -= targetDt;
			}
			engine->GetALife().Update(*engine, frameTime, JPH::RVec3(cam.position));
		}

		ZHLN::VisualInterpolationSystem::Update(
			*engine, g_EditorState.simulationRunning ? (accumulator / targetDt) : 1.0f);

		UpdateEditorCamera(cam, engine->GetInput(), frameTime);

		const auto& worldState = pc.GetWorld();
		auto res = engine->GetWindow().GetSize();

		if (res.width > 0 && res.height > 0) {
			if (s_AAState.mode == AAMode::TAA) {
				s_AAState.frameIndex++;
			} else {
				s_AAState.frameIndex = 0;
			}

			JPH::Mat44 unjitteredProj = cam.GetProjectionMatrix((float)res.width / res.height);
			JPH::Mat44 unjitteredVp = unjitteredProj * cam.GetViewMatrix();

			JPH::Mat44 vp = unjitteredVp;
			if (s_AAState.mode == AAMode::TAA) {
				vp = cam.GetJitteredProjectionMatrix((float)res.width / res.height, res.width,
													 res.height, s_AAState) *
					 cam.GetViewMatrix();
			}

			cam.frustum.Update(vp);
			static auto* cullingSystem = new CullingSystem();
			cullingSystem->Update<true>(*engine, s_VisibleEntities, s_VisibleShadowEntities);

			// 1. Retrieve the sun direction and current camera position
			JPH::Vec3 sunDirection = {0.5f, 1.0f, 0.2f};
			sunDirection = sunDirection.Normalized();

			JPH::Vec3 shadowCenter = cam.position;

			// 2. Snapping: Align the shadow center to texel increments to prevent edge shimmering.
			// Total orthographic width is 100.0f (-50 to 50), and shadow map resolution is 2048.
			float texelSize = 100.0f / 2048.0f;
			shadowCenter.SetX(std::round(shadowCenter.GetX() / texelSize) * texelSize);
			shadowCenter.SetY(std::round(shadowCenter.GetY() / texelSize) * texelSize);
			shadowCenter.SetZ(std::round(shadowCenter.GetZ() / texelSize) * texelSize);

			// 3. Build the camera-centric shadow View-Projection matrices
			JPH::Vec3 lightPos = shadowCenter + sunDirection * 100.0f;
			JPH::Mat44 lightView = Math::CreateLookAt(lightPos, shadowCenter, JPH::Vec3::sAxisY());

			// Orthographic projection bounding volume (adjust width/height based on clip distance)
			JPH::Mat44 lightProj = Math::CreateOrtho(-50.0f, 50.0f, -50.0f, 50.0f, 0.1f, 200.0f);
			JPH::Mat44 shadowProjView = lightProj * lightView;
			cam.shadowFrustum.Update(shadowProjView);

			JPH::Mat44 biasMatrix = {
				JPH::Vec4(0.5f, 0.0f, 0.0f, 0.0f), JPH::Vec4(0.0f, -0.5f, 0.0f, 0.0f),
				JPH::Vec4(0.0f, 0.0f, 1.0f, 0.0f), JPH::Vec4(0.5f, 0.5f, 0.0f, 1.0f)};
			JPH::Mat44 lightSpaceBiased = biasMatrix * shadowProjView;

			static JPH::Mat44 s_PrevUnjitteredVp = unjitteredVp;
			static bool s_FirstFrame = true;
			if (s_FirstFrame) {
				s_PrevUnjitteredVp = unjitteredVp;
				s_FirstFrame = false;
			}

			FrameUniforms uniforms{};
			uniforms.viewProj = vp;
			uniforms.unjitteredViewProj = unjitteredVp;
			uniforms.prevUnjitteredViewProj = s_PrevUnjitteredVp;
			uniforms.invViewProj = unjitteredVp.Inversed();
			std::memcpy(&uniforms.camPos[0], &cam.position, sizeof(float) * 3);
			std::memcpy(&uniforms.lightDir[0], &sunDirection, sizeof(float) * 3);
			uniforms.lightDir[3] = 10.0f;
			uniforms.lightCount = 0;
			uniforms.jitterParams = JPH::Vec4(s_AAState.jitterX, s_AAState.jitterY,
											  s_AAState.prevJitterX, s_AAState.prevJitterY);

			rc.SetAAState(s_AAState);
			Renderer::SetFrameData(rc, cam, uniforms, shadowProjView);

			auto begin_res = rc.BeginFrame(); // <-- FIXED: Capture expected
			if (!begin_res) {
				if (begin_res.error() == RenderFrameResult::DeviceLost) {
					ZHLN::Panic("Vulkan Device Lost in Editor Loop!");
				}
				continue; // Skip rendering if the swapchain is out of date or suboptimal
			}

			Renderer::SetMatrices(rc, vp, unjitteredVp);

			// -------------------------------------------------------------------------
			// [FAST PERSISTENTLY MAPPED PHYSICS DEBUG RENDERER]
			// -------------------------------------------------------------------------
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

			if (g_EditorState.simulationRunning && g_EditorState.showPhysicsDebug) {
				ZHLN_PROFILE_SCOPE("Physics Debug Extract & Upload"); // Profiled!
				bool isWireframe = true;
				auto debugData = Physics::GetDebugDrawData(pc, true, true, isWireframe);

				std::vector<VertexPosition> debugPos;
				std::vector<VertexAttributes> debugAttr;
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

				if (!debugPos.empty()) {
					rc.UploadDebugVertices(
						debugPos.data(), debugPos.size() * sizeof(VertexPosition), debugAttr.data(),
						debugAttr.size() * sizeof(VertexAttributes),
						static_cast<uint32_t>(debugPos.size()));

					Mesh debugMesh = {.posBuffer = rc.GetDebugMeshBuffer(),
									  .attrBuffer = rc.GetDebugMeshBuffer(),
									  .skinBuffer = BufferHandle::Invalid,
									  .indexBuffer = BufferHandle::Invalid,
									  .vertexCount = static_cast<uint32_t>(debugPos.size()),
									  .indexCount = 0};

					Renderer::Draw(rc, debugLineMat, debugMesh,
								   {.transform = JPH::Mat44::sIdentity(),
									.prevTransform = JPH::Mat44::sIdentity(),
									.cullRadius = 10000.0f});
				}
			}
			// -------------------------------------------------------------------------

			// --- Wrap with conditional check to hide standard meshes ---
			if (!g_EditorState.showPhysicsDebug) {
				ZHLN_LOCK(worldState.sync.shadowLock) {
					for (Entity e : s_VisibleEntities) {
						auto* mesh = reg.Get<MeshComponent>(e);
						if (mesh == nullptr) {
							continue;
						}

						JPH::Mat44 currentTransform{};
						auto* trans = reg.Get<TransformComponent>(e);

						if (trans != nullptr) {
							currentTransform = trans->GetMatrix() * mesh->localTransform;
						} else if (auto* alifeComp = reg.Get<ALife::ALifeComponent>(e)) {
							currentTransform = Math::CreateTransform(JPH::Vec3(alifeComp->position),
																	 JPH::Quat::sIdentity()) *
											   mesh->localTransform;
						} else {
							currentTransform = mesh->localTransform;
						}

						Renderer::Draw(
							rc, mesh->material, mesh->mesh,
							{.transform = currentTransform,
							 .prevTransform = mesh->prevTransform,
							 .cullRadius = mesh->cullRadius,
							 .localCenter = {mesh->localCenter.GetX(), mesh->localCenter.GetY(),
											 mesh->localCenter.GetZ()},
							 .jointOffset = mesh->jointOffset,
							 .morphOffset = mesh->morphOffset,
							 .activeMorphCount = mesh->activeMorphCount,
							 .morphWeights = mesh->morphWeights.data(),
							 .flags = mesh->flags});
					}
				}
			}

			CullingStats::TotalObjects = reg.GetEntitiesWith<MeshComponent>().size();
			CullingStats::CulledObjects = CullingStats::TotalObjects - s_VisibleEntities.size();

			auto end_res = rc.EndFrame(); // <-- FIXED: Capture expected
			if (!end_res) {
				if (end_res.error() == RenderFrameResult::DeviceLost) {
					ZHLN::Panic("Vulkan Device Lost in Editor Loop!");
				}
			}

			s_PrevUnjitteredVp = unjitteredVp;

			auto allEntities = reg.GetEntitiesWith<MeshComponent>();
			for (Entity e : allEntities) {
				auto* mesh = reg.Get<MeshComponent>(e);
				if (mesh != nullptr) {
					JPH::Mat44 currentTransform{};
					auto* phys = reg.Get<PhysicsComponent>(e);
					if (phys != nullptr) {
						uint32_t dense = worldState.slotToDense[phys->physicsHandle.index];
						const size_t base = static_cast<size_t>(dense) * 4;
						JPH::Vec3 pos(worldState.positions[base], worldState.positions[base + 1],
									  worldState.positions[base + 2]);
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
					mesh->prevTransform = currentTransform;
				}
			}
			// Frame Rate Limiter
			if (fpsLimit > 0) {
				auto frameEnd = std::chrono::high_resolution_clock::now();
				double elapsed = std::chrono::duration<double>(frameEnd - frameStart).count();
				if (elapsed < targetFrameTime) {
					double sleepTime = targetFrameTime - elapsed;
					if (sleepTime > 0.002) {
						std::this_thread::sleep_for(std::chrono::microseconds(
							static_cast<int64_t>((sleepTime - 0.001) * 1e6)));
					}
					while (std::chrono::duration<double>(std::chrono::high_resolution_clock::now() -
														 frameStart)
							   .count() < targetFrameTime) {
						CPURelax();
					}
				}
			}
			frameStart = std::chrono::high_resolution_clock::now();
		} else {
			Platform::Sleep(10);
		}
	}

	TaskSystem::Shutdown();
	ZHLN::Log("Shutting down Editor...");

	return EXIT_SUCCESS;
}

} // namespace

extern auto RunEditor(const CommandLineOptions& options) {
	auto result = InitializeEditor(options)
					  .and_then([&options](std::unique_ptr<Engine> engine) {
						  return RunEditorLoop(std::move(engine), options.fpsLimit);
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
