// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/Scripting.cpp
#include "IScriptRuntime.hpp"
#include "LuaScriptRuntime.hpp"
#include "Zahlen/Camera.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Input.hpp"
#include "ecs/ECS.hpp"
#include "engine/system/AnimationSystem.hpp"
#include "engine/system/InputSystem.hpp"

#include <Zahlen/Audio.hpp>
#include <Zahlen/Buffer.h>
#include <Zahlen/Console.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Scripting.h>
#include <Zahlen/Scripting.hpp>
#include <Zahlen/Sync.hpp>
#include <Zahlen/Window.hpp>
#include <algorithm>
#include <cgltf.h>
#include <chrono>
#include <cstring>
#include <engine/system/LightingSystem.hpp>
#include <functional>
#include <physics/Physics.hpp>
#include <physics/PhysicsWorld.hpp>
#include <print>
#include <string_view>
#include <unordered_map>
#include <vector>

// ============================================================================
// PAYLOAD DEFINITIONS (Must exactly match ffi_cdef.lua)
// ============================================================================
namespace {
#pragma pack(push, 1)

struct ZHLN_RaycastResult {
	uint64_t entity;
	double px, py, pz;
	float nx, ny, nz;
	float fraction;
	int hasHit;
};

struct GetBufferArgs {
	ZHLN_BufferView* outView;
};
struct GetECSBufferArgs {
	const char* componentName;
	ZHLN_BufferView* outView;
};
struct ReleaseBufferArgs {
	void* sync_ptr;
};
struct GetComponentArgs {
	uint64_t entityRaw;
	const char* componentName;
};
using AddComponentArgs = GetComponentArgs;

struct EntityOnlyArgs {
	uint64_t entityRaw;
};
struct IsKeyDownArgs {
	uint8_t key;
};
struct GetMouseDeltaArgs {
	float* outX;
	float* outY;
};
struct CameraFloatArgs {
	float* outVal;
};
struct SetCameraFOVArgs {
	float fov;
};
struct PlayOneShotArgs {
	const char* filepath;
	float volume;
};
struct PlayOneShot3DArgs {
	const char* filepath;
	float x;
	float y;
	float z;
	float volume;
};
struct PlayProceduralBeepArgs {
	float frequency;
	float duration;
	float volume;
};
struct SetCharVelArgs {
	uint64_t entityRaw;
	float x;
	float y;
	float z;
};
struct AddImpulseAtArgs {
	uint64_t entityRaw;
	float ix;
	float iy;
	float iz;
	double px;
	double py;
	double pz;
};
struct RaycastArgs {
	double ox;
	double oy;
	double oz;
	float dx;
	float dy;
	float dz;
	float maxDist;
	uint64_t ignoreEntity;
	ZHLN_RaycastResult* outResult;
};
struct SetMoveInputArgs {
	uint64_t entityRaw;
	float x;
	float z;
};
struct UnprojectArgs {
	float ndcX;
	float ndcY;
	double* ox;
	double* oy;
	double* oz;
	float* dx;
	float* dy;
	float* dz;
};
struct LogInventoryArgs {
	const char* msg;
};

struct SpawnPrefabArgs {
	char path[256];
	float px, py, pz;
	int createPhysics;
	int isStatic;
	int isAnimated;
	uint32_t maxCount;
	uint64_t* outEntities;
};
struct SetupRagdollArgs {
	uint64_t playerEntity;
	uint32_t count;
	uint64_t* visualParts;
};
struct CreateBoxArgs {
	float hx, hy, hz;
	float r, g, b, a;
};
struct CreateMaterialArgs {
	float r, g, b, a;
	uint64_t* outPipeline;
	uint32_t* outAlbedo;
};
struct SpawnEntityArgs {
	uint8_t shapeType;
	float p1, p2, p3;
	float px, py, pz;
	float rx, ry, rz, rw;
	float r, g, b, a;
	uint8_t isStatic;
};
struct RegisterDynamicComponentArgs {
	const char* name;
	uint64_t size;
	uint64_t alignment;
};

struct SpawnLightArgs {
	float px, py, pz;	  // Position
	float rx, ry, rz, rw; // Rotation (Quaternion)
	float r, g, b;		  // Color
	float intensity;
	float radius;
	float dx, dy, dz; // Direction
	float range;
	ZHLN::LightType type; // 0=Dir, 1=Point, 2=Spot, 3=Area
	uint32_t twoSided;
};

struct CreateSoundInstanceArgs {
	const char* filepath;
	int spatialized;
};

struct SoundInstanceArgs {
	uint64_t handle;
};

struct PlayTrackArgs {
	uint64_t entityRaw;
	int32_t trackIndex;
	float blendDuration;
	int loop;
	float playbackSpeed;
};

struct GetTrackNameArgs {
	uint64_t entityRaw;
	int32_t trackIndex;
	char outName[64];
};

#pragma pack(pop)

void SafeDestroyEntity(ZHLN::Engine* engine, ZHLN::Entity entity) {
	auto& reg = engine->GetRegistry();

	std::vector<ZHLN::Entity> childrenToDestroy;

	uint32_t hierarchyID = ZHLN::ECS::ComponentFamily::GetTypeID<ZHLN::HierarchyComponent>();
	auto hEntities = reg.GetEntitiesByFamilyID(hierarchyID);
	for (ZHLN::Entity e : hEntities) {
		if (auto* hier = reg.Get<ZHLN::HierarchyComponent>(e)) {
			if (hier->parent == entity) {
				childrenToDestroy.push_back(e);
			}
		}
	}

	uint32_t uiRectID = ZHLN::ECS::ComponentFamily::GetTypeID<ZHLN::UIRectComponent>();
	auto uEntities = reg.GetEntitiesByFamilyID(uiRectID);
	for (ZHLN::Entity e : uEntities) {
		if (auto* rect = reg.Get<ZHLN::UIRectComponent>(e)) {
			if (rect->parentEntity == entity) {
				childrenToDestroy.push_back(e);
			}
		}
	}

	for (ZHLN::Entity child : childrenToDestroy) {
		SafeDestroyEntity(engine, child);
	}

	auto* text = reg.Get<ZHLN::TextComponent>(entity);
	if (text != nullptr) {
		if (text->mesh.posBuffer != ZHLN::BufferHandle::Invalid) {
			engine->GetRenderContext().DestroyBuffer(text->mesh.posBuffer);
		}
		if (text->mesh.attrBuffer != ZHLN::BufferHandle::Invalid) {
			engine->GetRenderContext().DestroyBuffer(text->mesh.attrBuffer);
		}
		if (text->mesh.indexBuffer != ZHLN::BufferHandle::Invalid) {
			engine->GetRenderContext().DestroyBuffer(text->mesh.indexBuffer);
		}
	}

	auto* panel = reg.Get<ZHLN::UIPanelComponent>(entity);
	if (panel != nullptr) {
		if (panel->mesh.posBuffer != ZHLN::BufferHandle::Invalid) {
			engine->GetRenderContext().DestroyBuffer(panel->mesh.posBuffer);
		}
		if (panel->mesh.attrBuffer != ZHLN::BufferHandle::Invalid) {
			engine->GetRenderContext().DestroyBuffer(panel->mesh.attrBuffer);
		}
	}

	auto* mesh = reg.Get<ZHLN::MeshComponent>(entity);
	if (mesh != nullptr) {
		if (mesh->skinnedVertexBuffer != ZHLN::BufferHandle::Invalid) {
			engine->GetRenderContext().DestroyBuffer(mesh->skinnedVertexBuffer);
		}
	}

	reg.Destroy(entity);
}
} // namespace

extern std::vector<std::string> s_InvShellLog;
extern bool s_InvScrollToBottom;

namespace ZHLN {

namespace {

struct SyncPolicy {
	static void Acquire(BufferSync* sync) {
		if (sync->viewExportCount.fetch_add(1, std::memory_order_acquire) == 0) {
			sync->shadowLock.lock();
		}
	}
	static void Release(BufferSync* sync) {
		if (sync->viewExportCount.fetch_sub(1, std::memory_order_release) == 1) {
			sync->shadowLock.unlock();
		}
	}
};

struct ViewComposer {
	template <typename TOwner, typename TData, typename... Dims>
	static ZHLN_BufferView Build(const TOwner* owner, TData* data, const char* format,
								 Dims... dims) {
		auto* sync = reinterpret_cast<BufferSync*>(const_cast<TOwner*>(owner));
		SyncPolicy::Acquire(sync);

		ZHLN_BufferView view = {};
		view.buf = (void*)data;
		view.obj = (void*)sync;
		view.itemsize = sizeof(TData);
		std::strncpy(view.format, format, 7);
		view.readonly = 0;

		view.ndim = sizeof...(dims);
		size_t d_array[] = {static_cast<size_t>(dims)...};

		size_t stride = sizeof(TData);
		for (int i = (int)view.ndim - 1; i >= 0; --i) {
			view.shape[i] = d_array[i];
			view.strides[i] = stride;
			stride *= d_array[i];
		}

		view.len = stride;
		view.flags = ZHLN_BUFFER_CONTIGUOUS | ZHLN_BUFFER_WRITABLE;
		if (((uintptr_t)view.buf % 32) == 0) {
			view.flags |= ZHLN_BUFFER_ALIGNED_32;
		}

		return view;
	}
};

using CommandHandler = std::function<uint64_t(ZHLN::Engine*, const void*)>;

// 1. The O(1) Fast Path arrays
static std::vector<CommandHandler> s_JumpTable;
static std::unordered_map<std::string_view, uint32_t> s_StringToIntMap;

// Helper to push lambdas into the flat array
static void RegisterCmd(std::string_view name, CommandHandler handler) {
	auto id = static_cast<uint32_t>(s_JumpTable.size());
	s_JumpTable.push_back(std::move(handler));
	s_StringToIntMap[name] = id;
}

// ============================================================================
// UNIFIED COMMAND WRAPPER TEMPLATE
// ============================================================================

template <typename TArgs = void, bool RequireEngine = true, typename Fn>
CommandHandler MakeCmd(Fn fn) {
	return [fn = std::move(fn)](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if constexpr (RequireEngine) {
			if (!engine) {
				return 0;
			}
		}
		if constexpr (std::is_void_v<TArgs>) {
			return fn(engine);
		} else {
			if (!args) {
				return 0;
			}
			return fn(engine, *static_cast<const TArgs*>(args));
		}
	};
}

// ============================================================================
// GENERALIZED TYPE-SAFE ECS COMPONENT REGISTRY
// ============================================================================
struct ComponentRegistryEntry {
	void* (*add)(ZHLN::ECS::Registry&, ZHLN::Entity) = nullptr;
	std::function<ZHLN_BufferView(ZHLN::ECS::Registry&)> getBuffer;
};

std::unordered_map<std::string_view, ComponentRegistryEntry> s_ComponentRegistry;

template <typename T, typename... Dims>
void RegisterComponentType(std::string_view name, const char* format, Dims... dims) {
	s_ComponentRegistry[name] = ComponentRegistryEntry{
		.add = [](ZHLN::ECS::Registry& reg, ZHLN::Entity entity) -> void* {
			return &reg.template Add<T>(entity, T{});
		},
		.getBuffer = [format, ... dims = dims](ZHLN::ECS::Registry& reg) -> ZHLN_BufferView {
			auto raw = reg.GetRawArray<T>();
			if constexpr (sizeof...(Dims) > 0) {
				return ZHLN::ViewComposer::Build(&reg, raw.data(), format, raw.size(), dims...);
			} else {
				return ZHLN::ViewComposer::Build(&reg, raw.data(), format, raw.size());
			}
		}};
}

template <typename T, typename... Dims>
void RegisterComponentTypeReadOnly(std::string_view name, const char* format, Dims... dims) {
	s_ComponentRegistry[name] = ComponentRegistryEntry{
		.add = nullptr, // Block manual generic instantiations (for Physics components)
		.getBuffer = [format, ... dims = dims](ZHLN::ECS::Registry& reg) -> ZHLN_BufferView {
			auto raw = reg.GetRawArray<T>();
			if constexpr (sizeof...(Dims) > 0) {
				return ZHLN::ViewComposer::Build(&reg, raw.data(), format, raw.size(), dims...);
			} else {
				return ZHLN::ViewComposer::Build(&reg, raw.data(), format, raw.size());
			}
		}};
}

void InitComponentRegistry() {
	if (!s_ComponentRegistry.empty()) {
		return;
	}

	RegisterComponentType<ZHLN::HierarchyComponent>("HierarchyComponent", "B");
	RegisterComponentType<ZHLN::TransformComponent>("TransformComponent", "B");
	RegisterComponentType<ZHLN::MovementComponent>("MovementComponent", "B");
	RegisterComponentType<ZHLN::PhysicsStateComponent>("PhysicsStateComponent", "B");
	RegisterComponentType<ZHLN::TargetCameraComponent>("TargetCameraComponent", "B");
	RegisterComponentType<ZHLN::PBRComponent>("PBRComponent", "f", 2); // 2D array [size, 2]
	RegisterComponentType<ZHLN::TextComponent>("TextComponent", "B");
	RegisterComponentType<ZHLN::PostProcessSettingsComponent>("PostProcessSettingsComponent", "B");
	RegisterComponentType<ZHLN::DebugSettingsComponent>("DebugSettingsComponent", "B");
	RegisterComponentType<ZHLN::AASettingsComponent>("AASettingsComponent", "B");
	RegisterComponentType<ZHLN::SunTagComponent>("SunTagComponent", "B");
	RegisterComponentType<ZHLN::ShadowSettingsComponent>("ShadowSettingsComponent", "B");
	RegisterComponentType<ZHLN::UIRectComponent>("UIRectComponent", "B");
	RegisterComponentType<ZHLN::UIPanelComponent>("UIPanelComponent", "B");
	RegisterComponentType<ZHLN::UIButtonComponent>("UIButtonComponent", "B");
	RegisterComponentType<ZHLN::UIDragComponent>("UIDragComponent", "B");
	RegisterComponentType<ZHLN::UIStackComponent>("UIStackComponent", "B");
	RegisterComponentTypeReadOnly<ZHLN::PhysicsComponent>("PhysicsComponent", "Q");
}

// ============================================================================
// DECOUPLED SUBSYSTEM COMMAND REGISTERS
// ============================================================================

void RegisterCreativeWorkCommands() {
	RegisterCmd("SpawnPrefab", MakeCmd<SpawnPrefabArgs>([](ZHLN::Engine* engine,
														   const SpawnPrefabArgs& a) -> uint64_t {
					auto& rc = engine->GetRenderContext();
					auto& reg = engine->GetRegistry();
					auto& pc = engine->GetPhysicsContext();

					auto* prefab = ZHLN::CreativeWorksFactory::LoadModelPrefab(
						rc, engine->GetCreativeWorksManager(), a.path);
					if (!prefab) {
						return 0;
					}

					ZHLN::CreativeWorksFactory::SpawnParams params;
					params.position = JPH::RVec3(a.px, a.py, a.pz);
					params.createPhysics = (a.createPhysics != 0);
					params.isStaticPhysics = (a.isStatic != 0);
					params.isAnimated = (a.isAnimated != 0);
					params.useBoxColliders = false;

					std::vector<ZHLN::Entity> temp_buffer(a.maxCount);
					uint32_t count = ZHLN::CreativeWorksFactory::InstantiatePrefab(
						rc, reg, pc, *prefab, params, temp_buffer.data(), a.maxCount);

					uint32_t writtenCount = std::min(count, a.maxCount);
					for (uint32_t i = 0; i < writtenCount; ++i) {
						a.outEntities[i] = temp_buffer[i].Pack();
					}
					return writtenCount;
				}));

	RegisterCmd(
		"SetupRagdoll",
		MakeCmd<SetupRagdollArgs>([](ZHLN::Engine* engine, const SetupRagdollArgs& a) -> uint64_t {
			std::vector<ZHLN::Entity> parts(a.count);
			for (uint32_t i = 0; i < a.count; ++i) {
				parts[i] = ZHLN::Entity::Unpack(a.visualParts[i]);
			}
			ZHLN::CreativeWorksFactory::SetupPlayerRagdoll(
				engine->GetRenderContext(), engine->GetPhysicsContext(), engine->GetRegistry(),
				ZHLN::Entity::Unpack(a.playerEntity), parts);
			return 1;
		}));

	RegisterCmd("CreateBox", MakeCmd<CreateBoxArgs>(
								 [](ZHLN::Engine* engine, const CreateBoxArgs& a) -> uint64_t {
									 ZHLN::Mesh mesh = ZHLN::CreativeWorksFactory::CreateBox(
										 engine->GetRenderContext(), JPH::Vec3(a.hx, a.hy, a.hz),
										 JPH::Vec4(a.r, a.g, a.b, a.a));
									 return static_cast<uint64_t>(mesh.posBuffer);
								 }));

	RegisterCmd("CreateBasicMaterial",
				MakeCmd<CreateMaterialArgs>(
					[](ZHLN::Engine* engine, const CreateMaterialArgs& a) -> uint64_t {
						ZHLN::Material mat = ZHLN::CreativeWorksFactory::CreateBasicMaterial(
							engine->GetRenderContext(), false, a.a < 1.0f);
						mat.baseColorFactor[0] = a.r;
						mat.baseColorFactor[1] = a.g;
						mat.baseColorFactor[2] = a.b;
						mat.baseColorFactor[3] = a.a;
						*a.outPipeline = static_cast<uint64_t>(mat.pipeline);
						*a.outAlbedo = mat.albedoIndex;
						return 1;
					}));

	RegisterCmd(
		"SpawnEntity",
		MakeCmd<SpawnEntityArgs>([](ZHLN::Engine* engine, const SpawnEntityArgs& a) -> uint64_t {
			auto& rc = engine->GetRenderContext();
			auto& pc = engine->GetPhysicsContext();
			auto& reg = engine->GetRegistry();

			ZHLN::Mesh mesh;
			JPH::ShapeRefC shape;
			float cullRadius = 1.0f;
			auto type = static_cast<ZHLN::Physics::ShapeType>(a.shapeType);

			switch (type) {
				case ZHLN::Physics::ShapeType::Sphere:
					mesh = ZHLN::CreativeWorksFactory::CreateBox(rc, JPH::Vec3(a.p1, a.p1, a.p1),
																{a.r, a.g, a.b, a.a});
					shape = ZHLN::Physics::GetOrCreateShape(pc, type, a.p1);
					cullRadius = a.p1 * 2.0f;
					break;
				case ZHLN::Physics::ShapeType::Plane:
					mesh = ZHLN::CreativeWorksFactory::CreatePlane(rc, a.p1, {a.r, a.g, a.b, a.a});
					shape = ZHLN::Physics::GetOrCreateShape(pc, type, 0.0f, 1.0f, 0.0f, 0.0f);
					cullRadius = a.p1 * 2.0f;
					break;
				case ZHLN::Physics::ShapeType::Box:
				default:
					mesh = ZHLN::CreativeWorksFactory::CreateBox(rc, JPH::Vec3(a.p1, a.p2, a.p3),
																{a.r, a.g, a.b, a.a});
					shape = ZHLN::Physics::GetOrCreateShape(pc, ZHLN::Physics::ShapeType::Box, a.p1,
															a.p2, a.p3);
					cullRadius = std::max({a.p1, a.p2, a.p3}) * 2.0f;
					break;
			}

			bool isTransparent = (a.a < 1.0f);

			ZHLN::Material mat =
				ZHLN::CreativeWorksFactory::CreateBasicMaterial(rc, false, isTransparent);
			mat.baseColorFactor[0] = a.r;
			mat.baseColorFactor[1] = a.g;
			mat.baseColorFactor[2] = a.b;
			mat.baseColorFactor[3] = a.a;
			ZHLN::Entity e = reg.Create();
			reg.Add(e, ZHLN::TransformComponent{.position = {a.px, a.py, a.pz},
												.rotation = {a.rx, a.ry, a.rz, a.rw},
												.scale = {1.0f, 1.0f, 1.0f}});
			ZHLN::DrawFlags flags = ZHLN::DrawFlags::None;
			if (isTransparent) {
				flags |= ZHLN::DrawFlags::ExcludeFromTLAS;
			}

			reg.Add(e, ZHLN::MeshComponent{.mesh = mesh,
										   .material = mat,
										   .cullRadius = cullRadius,
										   .localTransform = JPH::Mat44::sIdentity(),
										   .prevTransform = JPH::Mat44::sIdentity(),
										   .flags = flags});

			JPH::Quat rotation(a.rx, a.ry, a.rz, a.rw);
			reg.Add(e, ZHLN::PhysicsComponent{ZHLN::Physics::CreateRigidBody(
						   pc, shape, JPH::RVec3(a.px, a.py, a.pz), rotation,
						   a.isStatic ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic,
						   a.isStatic ? static_cast<JPH::ObjectLayer>(0)
									  : static_cast<JPH::ObjectLayer>(1),
						   0)});
			reg.Add(e, ZHLN::PhysicsStateComponent{.currPosition = {a.px, a.py, a.pz},
												   .prevPosition = {a.px, a.py, a.pz},
												   .currRotation = rotation,
												   .prevRotation = rotation});

			return e.Pack();
		}));

	RegisterCmd(
		"SpawnLight",
		MakeCmd<SpawnLightArgs>([](ZHLN::Engine* engine, const SpawnLightArgs& a) -> uint64_t {
			auto& reg = engine->GetRegistry();

			ZHLN::Entity e = reg.Create();
			reg.Add(e, ZHLN::TransformComponent{.position = {a.px, a.py, a.pz},
												.rotation = JPH::Quat(a.rx, a.ry, a.rz, a.rw),
												.scale = {1.0f, 1.0f, 1.0f}});
			reg.Add(e, ZHLN::NameComponent{.name = ZHLN::String64("SpawnedLight")});
			reg.Add(e,
					ZHLN::LightingSystem::LightComponent{.type = a.type,
														 .color = JPH::Vec3(a.r, a.g, a.b),
														 .intensity = a.intensity,
														 .radius = a.radius,
														 .direction = JPH::Vec3(a.dx, a.dy, a.dz),
														 .range = a.range,
														 .points = {},
														 .twoSided = a.twoSided});
			return e.Pack();
		}));
}

void RegisterPhysicsCommands() {
	RegisterCmd(
		"GetPhysicsPositions",
		MakeCmd<GetBufferArgs>([](ZHLN::Engine* engine, const GetBufferArgs& a) -> uint64_t {
			const auto& world = engine->GetPhysicsContext().GetWorld();
			*a.outView = ZHLN::ViewComposer::Build(&world, world.positions,
												   (sizeof(JPH::Real) == 8) ? "d" : "f",
												   world.count.load(), 4);
			return 0;
		}));

	RegisterCmd(
		"GetPhysicsLinearVelocities",
		MakeCmd<GetBufferArgs>([](ZHLN::Engine* engine, const GetBufferArgs& a) -> uint64_t {
			const auto& world = engine->GetPhysicsContext().GetWorld();
			*a.outView = ZHLN::ViewComposer::Build(&world, world.linearVelocities, "f",
												   world.count.load(), 4);
			return 0;
		}));

	RegisterCmd(
		"GetPhysicsContactEvents",
		MakeCmd<GetBufferArgs>([](ZHLN::Engine* engine, const GetBufferArgs& a) -> uint64_t {
			auto events = ZHLN::Physics::GetContactEvents(engine->GetPhysicsContext());
			const char* fmt = (sizeof(JPH::Real) == 8) ? "EvtD" : "EvtF";
			*a.outView = ZHLN::ViewComposer::Build(&engine->GetPhysicsContext().GetWorld(),
												   events.first, fmt, events.second);
			return 0;
		}));

	RegisterCmd(
		"SetCharacterVelocity",
		MakeCmd<SetCharVelArgs>([](ZHLN::Engine* engine, const SetCharVelArgs& a) -> uint64_t {
			ZHLN::Physics::SetCharacterVelocity(engine->GetPhysicsContext(),
												ZHLN::Entity::Unpack(a.entityRaw),
												JPH::Vec3(a.x, a.y, a.z));
			return 0;
		}));

	RegisterCmd(
		"IsCharacterOnGround",
		MakeCmd<EntityOnlyArgs>([](ZHLN::Engine* engine, const EntityOnlyArgs& a) -> uint64_t {
			return ZHLN::Physics::IsCharacterOnGround(engine->GetPhysicsContext(),
													  ZHLN::Entity::Unpack(a.entityRaw))
					   ? 1
					   : 0;
		}));

	RegisterCmd(
		"SetLinearVelocity",
		MakeCmd<SetCharVelArgs>([](ZHLN::Engine* engine, const SetCharVelArgs& a) -> uint64_t {
			ZHLN::Physics::SetLinearVelocity(engine->GetPhysicsContext(),
											 ZHLN::Entity::Unpack(a.entityRaw),
											 JPH::Vec3(a.x, a.y, a.z));
			return 0;
		}));

	RegisterCmd(
		"AddImpulse",
		MakeCmd<SetCharVelArgs>([](ZHLN::Engine* engine, const SetCharVelArgs& a) -> uint64_t {
			ZHLN::Physics::AddImpulse(engine->GetPhysicsContext(),
									  ZHLN::Entity::Unpack(a.entityRaw), JPH::Vec3(a.x, a.y, a.z));
			return 0;
		}));

	RegisterCmd(
		"AddImpulseAt",
		MakeCmd<AddImpulseAtArgs>([](ZHLN::Engine* engine, const AddImpulseAtArgs& a) -> uint64_t {
			ZHLN::Physics::AddImpulse(engine->GetPhysicsContext(),
									  ZHLN::Entity::Unpack(a.entityRaw),
									  JPH::Vec3(a.ix, a.iy, a.iz), JPH::RVec3(a.px, a.py, a.pz));
			return 0;
		}));

	RegisterCmd(
		"Raycast", MakeCmd<RaycastArgs>([](ZHLN::Engine* engine, const RaycastArgs& a) -> uint64_t {
			ZHLN::Entity ignore =
				a.ignoreEntity != 0 ? ZHLN::Entity::Unpack(a.ignoreEntity) : ZHLN::Entity{};
			auto res =
				ZHLN::Physics::Raycast(engine->GetPhysicsContext(), JPH::RVec3(a.ox, a.oy, a.oz),
									   JPH::Vec3(a.dx, a.dy, a.dz), a.maxDist, ignore);
			a.outResult->hasHit = res.hasHit ? 1 : 0;
			if (res.hasHit) {
				a.outResult->entity = res.handle.Pack();
				a.outResult->px = res.position.GetX();
				a.outResult->py = res.position.GetY();
				a.outResult->pz = res.position.GetZ();
				a.outResult->nx = res.normal.GetX();
				a.outResult->ny = res.normal.GetY();
				a.outResult->nz = res.normal.GetZ();
				a.outResult->fraction = res.fraction;
			}
			return 0;
		}));

	RegisterCmd(
		"SetMovementInput",
		MakeCmd<SetMoveInputArgs>([](ZHLN::Engine* engine, const SetMoveInputArgs& a) -> uint64_t {
			if (auto* move = engine->GetRegistry().Get<ZHLN::MovementComponent>(
					ZHLN::Entity::Unpack(a.entityRaw))) {
				move->inputX = a.x;
				move->inputZ = a.z;
			}
			return 0;
		}));

	RegisterCmd("SetJumpIntent", MakeCmd<EntityOnlyArgs>([](ZHLN::Engine* engine,
															const EntityOnlyArgs& a) -> uint64_t {
					if (auto* move = engine->GetRegistry().Get<ZHLN::MovementComponent>(
							ZHLN::Entity::Unpack(a.entityRaw))) {
						move->jumpRequested = true;
					}
					return 0;
				}));

	RegisterCmd(
		"UnprojectScreenToWorld",
		MakeCmd<UnprojectArgs>([](ZHLN::Engine* engine, const UnprojectArgs& a) -> uint64_t {
			auto winSize = engine->GetWindow().GetSize();
			if (winSize.width == 0 || winSize.height == 0)
				return 0;

			float aspect = (float)winSize.width / (float)winSize.height;
			const auto& cam = engine->GetCamera();
			JPH::Mat44 invVP = (cam.GetProjectionMatrix(aspect) * cam.GetViewMatrix()).Inversed();
			JPH::Vec4 nearWorld = invVP * JPH::Vec4(a.ndcX, a.ndcY, 0.0f, 1.0f);
			JPH::Vec4 farWorld = invVP * JPH::Vec4(a.ndcX, a.ndcY, 1.0f, 1.0f);
			JPH::Vec3 pNear =
				JPH::Vec3(nearWorld.GetX() / nearWorld.GetW(), nearWorld.GetY() / nearWorld.GetW(),
						  nearWorld.GetZ() / nearWorld.GetW());
			JPH::Vec3 pFar =
				JPH::Vec3(farWorld.GetX() / farWorld.GetW(), farWorld.GetY() / farWorld.GetW(),
						  farWorld.GetZ() / farWorld.GetW());
			JPH::Vec3 dir = (pFar - pNear).Normalized();

			*a.ox = pNear.GetX();
			*a.oy = pNear.GetY();
			*a.oz = pNear.GetZ();
			*a.dx = dir.GetX();
			*a.dy = dir.GetY();
			*a.dz = dir.GetZ();
			return 0;
		}));
}

void RegisterInputAndCameraCommands() {
	RegisterCmd("IsKeyDown", MakeCmd<IsKeyDownArgs>([](ZHLN::Engine* engine,
													   const IsKeyDownArgs& a) -> uint64_t {
					return engine->GetInput().IsKeyDown(static_cast<ZHLN::KeyCode>(a.key)) ? 1 : 0;
				}));

	RegisterCmd("GetMouseDelta",
				MakeCmd<GetMouseDeltaArgs>(
					[](ZHLN::Engine* engine, const GetMouseDeltaArgs& a) -> uint64_t {
						*a.outX = engine->GetInput().GetMouse().deltaX;
						*a.outY = engine->GetInput().GetMouse().deltaY;
						return 0;
					}));

	RegisterCmd("GetCameraYaw", MakeCmd<CameraFloatArgs>(
									[](ZHLN::Engine* engine, const CameraFloatArgs& a) -> uint64_t {
										*a.outVal = engine->GetCamera().yaw;
										return 0;
									}));

	RegisterCmd("GetCameraFOV", MakeCmd<CameraFloatArgs>(
									[](ZHLN::Engine* engine, const CameraFloatArgs& a) -> uint64_t {
										*a.outVal = engine->GetCamera().fov;
										return 0;
									}));

	RegisterCmd(
		"SetCameraFOV",
		MakeCmd<SetCameraFOVArgs>([](ZHLN::Engine* engine, const SetCameraFOVArgs& a) -> uint64_t {
			engine->GetCamera().fov = a.fov;
			return 0;
		}));

	RegisterCmd("GetTotalTime",
				MakeCmd<CameraFloatArgs>([](ZHLN::Engine*, const CameraFloatArgs& a) -> uint64_t {
					static auto start = std::chrono::high_resolution_clock::now();
					auto now = std::chrono::high_resolution_clock::now();
					*a.outVal = std::chrono::duration<float>(now - start).count();
					return 0;
				}));
}

void RegisterAudioCommands() {
	RegisterCmd("PlayOneShot", MakeCmd<PlayOneShotArgs>(
								   [](ZHLN::Engine* engine, const PlayOneShotArgs& a) -> uint64_t {
									   if (!a.filepath) {
										   return 0;
									   }
									   engine->GetAudioContext().PlayOneShot(a.filepath, a.volume);
									   return 0;
								   }));

	RegisterCmd(
		"PlayOneShot3D", MakeCmd<PlayOneShot3DArgs>([](ZHLN::Engine* engine,
													   const PlayOneShot3DArgs& a) -> uint64_t {
			if (!a.filepath) {
				return 0;
			}
			engine->GetAudioContext().PlayOneShot3D(a.filepath, JPH::Vec3(a.x, a.y, a.z), a.volume);
			return 0;
		}));

	RegisterCmd("PlayProceduralBeep",
				MakeCmd<PlayProceduralBeepArgs>([](ZHLN::Engine* engine,
												   const PlayProceduralBeepArgs& a) -> uint64_t {
					engine->GetAudioContext().PlayProceduralBeep(a.frequency, a.duration, a.volume);
					return 0;
				}));

	RegisterCmd("CreateSoundInstance",
				MakeCmd<CreateSoundInstanceArgs>(
					[](ZHLN::Engine* engine, const CreateSoundInstanceArgs& a) -> uint64_t {
						if (!a.filepath)
							return 0;
						void* handle = engine->GetAudioContext().CreateSoundInstance(
							a.filepath, a.spatialized != 0);
						return reinterpret_cast<uint64_t>(handle);
					}));

	RegisterCmd("PlaySoundInstance",
				MakeCmd<SoundInstanceArgs>([](ZHLN::Engine* engine,
											  const SoundInstanceArgs& a) -> uint64_t {
					engine->GetAudioContext().PlaySoundInstance(reinterpret_cast<void*>(a.handle));
					return 0;
				}));

	RegisterCmd("StopSoundInstance",
				MakeCmd<SoundInstanceArgs>([](ZHLN::Engine* engine,
											  const SoundInstanceArgs& a) -> uint64_t {
					engine->GetAudioContext().StopSoundInstance(reinterpret_cast<void*>(a.handle));
					return 0;
				}));

	RegisterCmd("DestroySoundInstance",
				MakeCmd<SoundInstanceArgs>(
					[](ZHLN::Engine* engine, const SoundInstanceArgs& a) -> uint64_t {
						engine->GetAudioContext().DestroySoundInstance(
							reinterpret_cast<void*>(a.handle));
						return 0;
					}));
}

void RegisterECSCommands() {
	RegisterCmd("CreateEntity", MakeCmd<void>([](ZHLN::Engine* engine) -> uint64_t {
					return engine->GetRegistry().Create().Pack();
				}));

	RegisterCmd("DestroyEntity", MakeCmd<EntityOnlyArgs>([](ZHLN::Engine* engine,
															const EntityOnlyArgs& a) -> uint64_t {
					SafeDestroyEntity(engine, ZHLN::Entity::Unpack(a.entityRaw));
					return 0;
				}));

	RegisterCmd(
		"GetComponent",
		MakeCmd<GetComponentArgs>([](ZHLN::Engine* engine, const GetComponentArgs& a) -> uint64_t {
			if (!a.componentName)
				return 0;
			uint32_t familyID = ZHLN::ECS::Registry::GetFamilyIDFromName(a.componentName);
			if (familyID == 0xFFFFFFFF)
				return 0;
			return std::bit_cast<uint64_t>(
				engine->GetRegistry().GetRawByFamily(ZHLN::Entity::Unpack(a.entityRaw), familyID));
		}));

	RegisterCmd(
		"AddComponent",
		MakeCmd<AddComponentArgs>([](ZHLN::Engine* engine, const AddComponentArgs& a) -> uint64_t {
			if (!a.componentName)
				return 0;
			auto entity = ZHLN::Entity::Unpack(a.entityRaw);
			auto& reg = engine->GetRegistry();
			std::string_view name(a.componentName);
			void* ptr = nullptr;

			auto it = s_ComponentRegistry.find(name);
			if (it != s_ComponentRegistry.end() && it->second.add != nullptr) {
				ptr = it->second.add(reg, entity);
			} else {
				uint32_t familyID = ZHLN::ECS::Registry::GetFamilyIDFromName(name);
				if (familyID != 0xFFFFFFFF) {
					ptr = reg.AddDynamic(entity, familyID);
				}
			}
			return std::bit_cast<uint64_t>(ptr);
		}));

	RegisterCmd("RegisterDynamicComponent",
				MakeCmd<RegisterDynamicComponentArgs>(
					[](ZHLN::Engine* engine, const RegisterDynamicComponentArgs& a) -> uint64_t {
						if (!a.name)
							return 0;
						return engine->GetRegistry().RegisterComponentDynamic(a.name, a.size,
																			  a.alignment);
					}));

	RegisterCmd(
		"GetECSBuffer",
		MakeCmd<GetECSBufferArgs>([](ZHLN::Engine* engine, const GetECSBufferArgs& a) -> uint64_t {
			if (!a.componentName)
				return 0;
			auto& reg = engine->GetRegistry();
			std::string_view name(a.componentName);

			auto it = s_ComponentRegistry.find(name);
			if (it != s_ComponentRegistry.end()) {
				*a.outView = it->second.getBuffer(reg);
			} else {
				*a.outView = {};
			}
			return 0;
		}));

	RegisterCmd(
		"GetECSEntities",
		MakeCmd<GetECSBufferArgs>([](ZHLN::Engine* engine, const GetECSBufferArgs& a) -> uint64_t {
			if (!a.componentName)
				return 0;
			auto& reg = engine->GetRegistry();
			uint32_t familyID = ZHLN::ECS::Registry::GetFamilyIDFromName(a.componentName);
			if (familyID != 0xFFFFFFFF) {
				auto entities = reg.GetEntitiesByFamilyID(familyID);
				*a.outView = ZHLN::ViewComposer::Build(
					&reg, const_cast<ZHLN::Entity*>(entities.data()), "Q", entities.size());
			} else {
				*a.outView = {};
			}
			return 0;
		}));

	RegisterCmd("ReleaseBuffer",
				MakeCmd<ReleaseBufferArgs, false>(
					[](ZHLN::Engine*, const ReleaseBufferArgs& a) -> uint64_t {
						if (a.sync_ptr == nullptr) {
							return 0;
						}
						ZHLN::SyncPolicy::Release(static_cast<ZHLN::BufferSync*>(a.sync_ptr));
						return 0;
					}));

	RegisterCmd("LogInventoryShell",
				MakeCmd<LogInventoryArgs>([](ZHLN::Engine*, const LogInventoryArgs& a) -> uint64_t {
					if (!a.msg)
						return 0;
					std::string str(a.msg);
					size_t pos = 0;
					while (pos < str.size()) {
						size_t next_nl = str.find('\n', pos);
						if (next_nl == std::string::npos) {
							s_InvShellLog.push_back(str.substr(pos));
							break;
						}
						s_InvShellLog.push_back(str.substr(pos, next_nl - pos));
						pos = next_nl + 1;
					}
					s_InvScrollToBottom = true;
					std::println(stdout, "[InvShell Output]\n{}", a.msg);
					std::fflush(stdout);
					return 0;
				}));
}

void RegisterSystemCommands() {
	RegisterCmd("ProvokeDeviceLost", MakeCmd<void>([](ZHLN::Engine* engine) -> uint64_t {
					engine->ProvokeDeviceLost();
					return 1;
				}));

	RegisterCmd("InitPlayer", MakeCmd<void>([](ZHLN::Engine* engine) -> uint64_t {
					using namespace ZHLN;
					auto& reg = engine->GetRegistry();
					auto& pc = engine->GetPhysicsContext();

					// 1. Spawn infinite physical ground plane
					auto groundShape = ZHLN::Physics::GetOrCreateShape(
						pc, ZHLN::Physics::ShapeType::Plane, 0.0f, 1.0f, 0.0f, 0.0f);
					ZHLN::Entity ground = reg.Create();
					reg.Add(ground, PhysicsComponent{Physics::CreateRigidBody(
										pc, groundShape, {0, 0, 0}, JPH::Quat::sIdentity(),
										JPH::EMotionType::Static, 0)});
					reg.Add(ground, PhysicsStateComponent{});

					// 2. Spawn the Player Character Controller
					ZHLN::Entity playerEntity = reg.Create();
					reg.Add(playerEntity, PlayerTagComponent{});
					reg.Add(playerEntity, TransformComponent{.position = {0.0f, 3.0f, 0.0f}});
					reg.Add(playerEntity, MovementComponent{});
					reg.Add(playerEntity, ZHLN::InputSystem::InputComponent{});
					ZHLN::Entity charPhys =
						ZHLN::Physics::CreateCharacter(pc, JPH::RVec3(0.0f, 3.0f, 0.0f));
					reg.Add(playerEntity, PhysicsComponent{charPhys});
					reg.Add(playerEntity,
							PhysicsStateComponent{.currPosition = {0.0f, 3.0f, 0.0f},
												  .prevPosition = {0.0f, 3.0f, 0.0f}});

					// 3. Attach Camera Tracking logic to the blank menu camera
					auto camEnts = reg.GetEntitiesWith<ZHLN::MainCameraTagComponent>();
					if (!camEnts.empty()) {
						ZHLN::Entity camEnt = camEnts[0];
						reg.Add(camEnt, TargetCameraComponent{.target = playerEntity,
															  .distance = 4.5f,
															  .targetDistance = 4.5f,
															  .yaw = -90.0f,
															  .pitch = -10.0f,
															  .stiffness = 15.0f,
															  .vignetteIntensity = 1.10f,
															  .vignettePower = 1.50f,
															  .fov = 45.0f,
															  .targetFov = 45.0f});
						reg.Add(camEnt, ZHLN::InputSystem::InputComponent{});
					}
					return playerEntity.Pack();
				}));

	RegisterCmd(
		"GetAnimationTrackCount",
		MakeCmd<EntityOnlyArgs>([](ZHLN::Engine* engine, const EntityOnlyArgs& a) -> uint64_t {
			auto& reg = engine->GetRegistry();

			auto entity = ZHLN::Entity::Unpack(a.entityRaw);
			if (auto* anim = reg.Get<ZHLN::AnimatorComponent>(entity)) {
				if (anim->gltfData != nullptr) {
					auto* data = static_cast<cgltf_data*>(anim->gltfData);
					return static_cast<uint64_t>(data->animations_count);
				}
			}
			return 0;
		}));

	RegisterCmd(
		"GetAnimationTrackName",
		MakeCmd<GetTrackNameArgs>([](ZHLN::Engine* engine, const GetTrackNameArgs& a) -> uint64_t {
			auto& reg = engine->GetRegistry();

			auto entity = ZHLN::Entity::Unpack(a.entityRaw);
			if (auto* anim = reg.Get<ZHLN::AnimatorComponent>(entity)) {
				if (anim->gltfData != nullptr) {
					auto* data = static_cast<cgltf_data*>(anim->gltfData);
					if (a.trackIndex >= 0 &&
						a.trackIndex < static_cast<int32_t>(data->animations_count)) {
						const char* name = data->animations[a.trackIndex].name;
						// Safely write to the flat array
						std::strncpy(const_cast<char*>(a.outName), name ? name : "Unnamed", 63);
						const_cast<char*>(a.outName)[63] = '\0';
						return 1;
					}
				}
			}
			return 0;
		}));

	RegisterCmd(
		"PlayAnimationTrack",
		MakeCmd<PlayTrackArgs>([](ZHLN::Engine* engine, const PlayTrackArgs& a) -> uint64_t {
			auto& reg = engine->GetRegistry();

			auto entity = ZHLN::Entity::Unpack(a.entityRaw);
			if (auto* anim = reg.Get<ZHLN::AnimatorComponent>(entity)) {
				if (anim->gltfData != nullptr) {
					auto* data = static_cast<cgltf_data*>(anim->gltfData);
					if (a.trackIndex >= 0 &&
						a.trackIndex < static_cast<int32_t>(data->animations_count)) {
						// Manage the crossfading transition state machine
						if (anim->currentTrackIdx != a.trackIndex) {
							anim->prevTrackIdx = anim->currentTrackIdx;
							anim->prevTrackTime = anim->currentTrackTime;
							anim->prevPlaybackSpeed = anim->currentPlaybackSpeed;

							anim->currentTrackIdx = a.trackIndex;
							anim->currentTrackTime = 0.0f;
							anim->currentPlaybackSpeed = a.playbackSpeed;
							anim->currentLoop = (a.loop != 0);

							anim->blendFactor = 0.0f;
							anim->blendDuration = a.blendDuration;
							anim->isFinished = false;
						} else {
							// If targeting the same track, update playback properties
							anim->currentLoop = (a.loop != 0);
							anim->currentPlaybackSpeed = a.playbackSpeed;
							if (anim->isFinished && anim->currentLoop) {
								anim->isFinished = false;
								anim->currentTrackTime = 0.0f;
							}
						}
						return 1;
					}
				}
			}
			return 0;
		}));
}

void RegisterFFICommands() {
	if (!s_JumpTable.empty()) {
		return;
	}
	InitComponentRegistry();

	RegisterCreativeWorkCommands();
	RegisterPhysicsCommands();
	RegisterInputAndCameraCommands();
	RegisterAudioCommands();
	RegisterECSCommands();
	RegisterSystemCommands();
}
} // namespace
} // namespace ZHLN

extern "C" {

using namespace ZHLN;

ZHLN_API ZHLN_Engine* ZHLN_GetEngineContext() {
	return reinterpret_cast<ZHLN_Engine*>(ZHLN::GetEngineContext());
}

ZHLN_API uint32_t ZHLN_GetCommandID(const char* cmdName) {
	if (cmdName == nullptr) {
		return 0xFFFFFFFF;
	}
	RegisterFFICommands(); // Lazily ensure the jump table is built on first request

	std::string_view view(cmdName);
	auto it = s_StringToIntMap.find(view);
	if (it != s_StringToIntMap.end()) {
		return it->second;
	}

	ZHLN::Log("WARNING: ZHLN_GetCommandID could not resolve '{}' to a known command.", cmdName);
	return 0xFFFFFFFF;
}

ZHLN_API uint64_t ZHLN_DispatchCommand(ZHLN_Engine* engine_handle, uint32_t cmdID,
									   const void* args) {
	if (cmdID >= s_JumpTable.size()) [[unlikely]] {
		return 0; // Out of bounds
	}

	// 100% Branchless O(1) jump directly into the closure.
	// Null pointer safety checks are handled individually by the lambda's inner closure scopes.
	return s_JumpTable[cmdID](reinterpret_cast<ZHLN::Engine*>(engine_handle), args);
}

} // extern "C"

namespace ZHLN {

ScriptRunner::ScriptRunner() : _runtime(std::make_unique<LuaScriptRuntime>()) {}

ScriptRunner::~ScriptRunner() = default;

void ScriptRunner::RunFile(std::string_view path) {
	_runtime->RunFile(path);
}

void ScriptRunner::CallUpdate(Engine* engine, float dt) {
	if (engine != nullptr) {
		_runtime->Initialize(engine);
		_runtime->TickUpdate(engine, dt);
	}
}

void ScriptRunner::ExecuteString(std::string_view code) {
	_runtime->ExecuteString(code);
}

void ScriptRunner::ReloadFile(std::string_view path) {
	_runtime->ReloadFile(path);
}

} // namespace ZHLN
