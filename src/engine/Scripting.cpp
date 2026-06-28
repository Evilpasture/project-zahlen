// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/Scripting.cpp

#include "IScriptRuntime.hpp"
#include "LuaScriptRuntime.hpp"
#include "Zahlen/Camera.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Input.hpp"
#include "ecs/ECS.hpp"

#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Buffer.h>
#include <Zahlen/Console.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Scripting.h>
#include <Zahlen/Scripting.hpp>
#include <Zahlen/Sync.hpp>
#include <Zahlen/Window.hpp>
#include <algorithm>
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

} // namespace ZHLN

using CommandHandler = std::function<uint64_t(ZHLN::Engine*, const void*)>;

// 1. The O(1) Fast Path arrays
static std::vector<CommandHandler> s_JumpTable;
static std::unordered_map<std::string_view, uint32_t> s_StringToIntMap;

// Helper to push lambdas into the flat array
static void RegisterCmd(std::string_view name, CommandHandler handler) {
	uint32_t id = static_cast<uint32_t>(s_JumpTable.size());
	s_JumpTable.push_back(std::move(handler));
	s_StringToIntMap[name] = id;
}

static void RegisterFFICommands() {
	if (!s_JumpTable.empty()) {
		return;
	}

	// --- Asset Spawning ---
	RegisterCmd("SpawnPrefab", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const SpawnPrefabArgs*>(args);
		auto& rc = engine->GetRenderContext();
		auto& reg = engine->GetRegistry();
		auto& pc = engine->GetPhysicsContext();

		auto* prefab = ZHLN::AssetFactory::LoadModelPrefab(rc, engine->GetAssetManager(), a->path);
		if (!prefab)
			return 0;

		ZHLN::AssetFactory::SpawnParams params;
		params.position = JPH::RVec3(a->px, a->py, a->pz);
		params.createPhysics = a->createPhysics != 0;
		params.isStaticPhysics = a->isStatic != 0;
		params.isAnimated = a->isAnimated != 0;
		params.useBoxColliders = false;

		std::vector<ZHLN::Entity> temp_buffer(a->maxCount);
		uint32_t count = ZHLN::AssetFactory::InstantiatePrefab(rc, reg, pc, *prefab, params,
															   temp_buffer.data(), a->maxCount);

		uint32_t writtenCount = std::min(count, a->maxCount);
		for (uint32_t i = 0; i < writtenCount; ++i) {
			a->outEntities[i] = temp_buffer[i].Pack();
		}
		return writtenCount;
	});

	RegisterCmd("SetupRagdoll", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const SetupRagdollArgs*>(args);
		std::vector<ZHLN::Entity> parts(a->count);
		for (uint32_t i = 0; i < a->count; ++i) {
			parts[i] = ZHLN::Entity::Unpack(a->visualParts[i]);
		}
		ZHLN::AssetFactory::SetupPlayerRagdoll(engine->GetRenderContext(),
											   engine->GetPhysicsContext(), engine->GetRegistry(),
											   ZHLN::Entity::Unpack(a->playerEntity), parts);
		return 1;
	});

	RegisterCmd("CreateBox", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const CreateBoxArgs*>(args);
		ZHLN::Mesh mesh = ZHLN::AssetFactory::CreateBox(engine->GetRenderContext(),
														JPH::Vec3(a->hx, a->hy, a->hz),
														JPH::Vec4(a->r, a->g, a->b, a->a));
		return static_cast<uint64_t>(mesh.posBuffer);
	});

	RegisterCmd("CreateBasicMaterial", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const CreateMaterialArgs*>(args);
		bool isTransparent = (a->a < 1.0f);
		ZHLN::Material mat = ZHLN::AssetFactory::CreateBasicMaterial(engine->GetRenderContext(),
																	 false, isTransparent);
		mat.baseColorFactor[0] = a->r;
		mat.baseColorFactor[1] = a->g;
		mat.baseColorFactor[2] = a->b;
		mat.baseColorFactor[3] = a->a;
		*a->outPipeline = static_cast<uint64_t>(mat.pipeline);
		*a->outAlbedo = mat.albedoIndex;
		return 1;
	});

	RegisterCmd("SpawnEntity", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* desc = static_cast<const SpawnEntityArgs*>(args);
		auto& rc = engine->GetRenderContext();
		auto& pc = engine->GetPhysicsContext();
		auto& reg = engine->GetRegistry();

		ZHLN::Mesh mesh;
		JPH::ShapeRefC shape;
		float cullRadius = 1.0f;
		auto type = static_cast<ZHLN::Physics::ShapeType>(desc->shapeType);

		switch (type) {
			case ZHLN::Physics::ShapeType::Sphere:
				mesh = ZHLN::AssetFactory::CreateBox(rc, JPH::Vec3(desc->p1, desc->p1, desc->p1),
													 JPH::Vec4(desc->r, desc->g, desc->b, desc->a));
				shape = ZHLN::Physics::GetOrCreateShape(pc, type, desc->p1);
				cullRadius = desc->p1 * 2.0f;
				break;
			case ZHLN::Physics::ShapeType::Plane:
				mesh = ZHLN::AssetFactory::CreatePlane(
					rc, desc->p1, JPH::Vec4(desc->r, desc->g, desc->b, desc->a));
				shape = ZHLN::Physics::GetOrCreateShape(pc, type, 0.0f, 1.0f, 0.0f, 0.0f);
				cullRadius = desc->p1 * 2.0f;
				break;
			case ZHLN::Physics::ShapeType::Box:
			default:
				mesh = ZHLN::AssetFactory::CreateBox(rc, JPH::Vec3(desc->p1, desc->p2, desc->p3),
													 JPH::Vec4(desc->r, desc->g, desc->b, desc->a));
				shape = ZHLN::Physics::GetOrCreateShape(pc, ZHLN::Physics::ShapeType::Box, desc->p1,
														desc->p2, desc->p3);
				cullRadius = std::max({desc->p1, desc->p2, desc->p3}) * 2.0f;
				break;
		}

		bool isTransparent = (desc->a < 1.0f);

		ZHLN::Material mat = ZHLN::AssetFactory::CreateBasicMaterial(rc, false, isTransparent);
		mat.baseColorFactor[0] = desc->r;
		mat.baseColorFactor[1] = desc->g;
		mat.baseColorFactor[2] = desc->b;
		mat.baseColorFactor[3] = desc->a;
		ZHLN::Entity e = reg.Create();
		reg.Add(e, ZHLN::TransformComponent{.position = {desc->px, desc->py, desc->pz},
											.rotation = {desc->rx, desc->ry, desc->rz, desc->rw},
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

		JPH::Quat rotation(desc->rx, desc->ry, desc->rz, desc->rw);
		reg.Add(e, ZHLN::PhysicsComponent{ZHLN::Physics::CreateRigidBody(
					   pc, shape, JPH::RVec3(desc->px, desc->py, desc->pz), rotation,
					   desc->isStatic ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic,
					   desc->isStatic ? static_cast<JPH::ObjectLayer>(0)
									  : static_cast<JPH::ObjectLayer>(1),
					   0)});
		reg.Add(e, ZHLN::PhysicsStateComponent{.currPosition = {desc->px, desc->py, desc->pz},
											   .prevPosition = {desc->px, desc->py, desc->pz},
											   .currRotation = rotation,
											   .prevRotation = rotation});

		return e.Pack();
	});

	// --- Buffer Views ---
	RegisterCmd("GetPhysicsPositions", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const GetBufferArgs*>(args);
		const auto& world = engine->GetPhysicsContext().GetWorld();
		*a->outView = ZHLN::ViewComposer::Build(
			&world, world.positions, (sizeof(JPH::Real) == 8) ? "d" : "f", world.count.load(), 4);
		return 0;
	});

	RegisterCmd("GetPhysicsLinearVelocities",
				[](ZHLN::Engine* engine, const void* args) -> uint64_t {
					if (!engine || !args)
						return 0;
					const auto* a = static_cast<const GetBufferArgs*>(args);
					const auto& world = engine->GetPhysicsContext().GetWorld();
					*a->outView = ZHLN::ViewComposer::Build(&world, world.linearVelocities, "f",
															world.count.load(), 4);
					return 0;
				});

	RegisterCmd("ReleaseBuffer", [](ZHLN::Engine*, const void* args) -> uint64_t {
		if (!args)
			return 0;
		const auto* a = static_cast<const ReleaseBufferArgs*>(args);
		if (a->sync_ptr == nullptr)
			return 0;
		ZHLN::SyncPolicy::Release(static_cast<ZHLN::BufferSync*>(a->sync_ptr));
		return 0;
	});

	RegisterCmd("GetECSBuffer", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const GetECSBufferArgs*>(args);
		if (!a->componentName)
			return 0;
		auto& reg = engine->GetRegistry();
		std::string_view name(a->componentName);

		if (name == "PhysicsComponent") {
			auto raw = reg.GetRawArray<ZHLN::PhysicsComponent>();
			*a->outView = ZHLN::ViewComposer::Build(&reg, raw.data(), "Q", raw.size());
		} else if (name == "PBRComponent") {
			auto raw = reg.GetRawArray<ZHLN::PBRComponent>();
			*a->outView = ZHLN::ViewComposer::Build(&reg, raw.data(), "f", raw.size(), 2);
		} else if (name == "PostProcessSettingsComponent") {
			auto raw = reg.GetRawArray<ZHLN::PostProcessSettingsComponent>();
			*a->outView = ZHLN::ViewComposer::Build(&reg, raw.data(), "B", raw.size());
		} else if (name == "ShadowSettingsComponent") {
			auto raw = reg.GetRawArray<ZHLN::ShadowSettingsComponent>();
			*a->outView = ZHLN::ViewComposer::Build(&reg, raw.data(), "B", raw.size());
		} else if (name == "AASettingsComponent") {
			auto raw = reg.GetRawArray<ZHLN::AASettingsComponent>();
			*a->outView = ZHLN::ViewComposer::Build(&reg, raw.data(), "B", raw.size());
		} else if (name == "DebugSettingsComponent") {
			auto raw = reg.GetRawArray<ZHLN::DebugSettingsComponent>();
			*a->outView = ZHLN::ViewComposer::Build(&reg, raw.data(), "B", raw.size());
		} else {
			*a->outView = {};
		}
		return 0;
	});

	RegisterCmd("GetECSEntities", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const GetECSBufferArgs*>(args);
		if (!a->componentName)
			return 0;
		auto& reg = engine->GetRegistry();
		uint32_t familyID = ZHLN::ECS::Registry::GetFamilyIDFromName(a->componentName);
		if (familyID != 0xFFFFFFFF) {
			auto entities = reg.GetEntitiesByFamilyID(familyID);
			*a->outView = ZHLN::ViewComposer::Build(
				&reg, const_cast<ZHLN::Entity*>(entities.data()), "Q", entities.size());
		} else {
			*a->outView = {};
		}
		return 0;
	});

	RegisterCmd("GetPhysicsContactEvents", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const GetBufferArgs*>(args);
		auto events = ZHLN::Physics::GetContactEvents(engine->GetPhysicsContext());
		const char* fmt = (sizeof(JPH::Real) == 8) ? "EvtD" : "EvtF";
		*a->outView = ZHLN::ViewComposer::Build(&engine->GetPhysicsContext().GetWorld(),
												events.first, fmt, events.second);
		return 0;
	});

	RegisterCmd("RegisterDynamicComponent", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const RegisterDynamicComponentArgs*>(args);
		if (!a->name)
			return 0;
		return engine->GetRegistry().RegisterComponentDynamic(a->name, a->size, a->alignment);
	});

	RegisterCmd("GetComponent", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const GetComponentArgs*>(args);
		if (!a->componentName)
			return 0;
		uint32_t familyID = ZHLN::ECS::Registry::GetFamilyIDFromName(a->componentName);
		if (familyID == 0xFFFFFFFF)
			return 0;
		return std::bit_cast<uint64_t>(
			engine->GetRegistry().GetRawByFamily(ZHLN::Entity::Unpack(a->entityRaw), familyID));
	});

	RegisterCmd("AddComponent", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const AddComponentArgs*>(args);
		if (!a->componentName)
			return 0;
		auto entity = ZHLN::Entity::Unpack(a->entityRaw);
		auto& reg = engine->GetRegistry();
		std::string_view name(a->componentName);
		void* ptr = nullptr;
		if (name == "HierarchyComponent") {
			ptr = &reg.Add(entity, ZHLN::HierarchyComponent{});
		} else if (name == "TransformComponent") {
			ptr = &reg.Add(entity, ZHLN::TransformComponent{});
		} else if (name == "MovementComponent") {
			ptr = &reg.Add(entity, ZHLN::MovementComponent{});
		} else if (name == "PhysicsStateComponent") {
			ptr = &reg.Add(entity, ZHLN::PhysicsStateComponent{});
		} else if (name == "TargetCameraComponent") {
			ptr = &reg.Add(entity, ZHLN::TargetCameraComponent{});
		} else if (name == "PBRComponent") {
			ptr = &reg.Add(entity, ZHLN::PBRComponent{});
		} else if (name == "TextComponent") {
			ptr = &reg.Add(entity, ZHLN::TextComponent{});
		} else if (name == "PostProcessSettingsComponent") {
			ptr = &reg.Add(entity, ZHLN::PostProcessSettingsComponent{});
		} else if (name == "DebugSettingsComponent") {
			ptr = &reg.Add(entity, ZHLN::DebugSettingsComponent{});
		} else if (name == "AASettingsComponent") {
			ptr = &reg.Add(entity, ZHLN::AASettingsComponent{});
		} else if (name == "SunTagComponent") {
			ptr = &reg.Add(entity, ZHLN::SunTagComponent{});
		} else if (name == "ShadowSettingsComponent") {
			ptr = &reg.Add(entity, ZHLN::ShadowSettingsComponent{});
		} else if (name == "UIRectComponent") {
			ptr = &reg.Add(entity, ZHLN::UIRectComponent{});
		} else if (name == "UIPanelComponent") {
			ptr = &reg.Add(entity, ZHLN::UIPanelComponent{});
		} else if (name == "UIButtonComponent") {
			ptr = &reg.Add(entity, ZHLN::UIButtonComponent{});
		} else if (name == "UIDragComponent") {
			ptr = &reg.Add(entity, ZHLN::UIDragComponent{});
		} else if (name == "UIStackComponent") {
			ptr = &reg.Add(entity, ZHLN::UIStackComponent{});
		} else {
			uint32_t familyID = ZHLN::ECS::Registry::GetFamilyIDFromName(name);
			if (familyID != 0xFFFFFFFF) {
				ptr = reg.AddDynamic(entity, familyID);
			}
		}
		return std::bit_cast<uint64_t>(ptr);
	});

	RegisterCmd("CreateEntity", [](ZHLN::Engine* engine, const void*) -> uint64_t {
		if (!engine)
			return 0;
		return engine->GetRegistry().Create().Pack();
	});

	RegisterCmd("DestroyEntity", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const EntityOnlyArgs*>(args);
		SafeDestroyEntity(engine, ZHLN::Entity::Unpack(a->entityRaw));
		return 0;
	});

	// --- Input ---
	RegisterCmd("IsKeyDown", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const IsKeyDownArgs*>(args);
		return engine->GetInput().IsKeyDown(static_cast<ZHLN::KeyCode>(a->key)) ? 1 : 0;
	});

	RegisterCmd("GetMouseDelta", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const GetMouseDeltaArgs*>(args);
		*a->outX = engine->GetInput().GetMouse().deltaX;
		*a->outY = engine->GetInput().GetMouse().deltaY;
		return 0;
	});

	// --- Camera ---
	RegisterCmd("GetCameraYaw", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const CameraFloatArgs*>(args);
		*a->outVal = engine->GetCamera().yaw;
		return 0;
	});

	RegisterCmd("GetCameraFOV", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const CameraFloatArgs*>(args);
		*a->outVal = engine->GetCamera().fov;
		return 0;
	});

	RegisterCmd("SetCameraFOV", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const SetCameraFOVArgs*>(args);
		engine->GetCamera().fov = a->fov;
		return 0;
	});

	// --- Audio ---
	RegisterCmd("PlayOneShot", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const PlayOneShotArgs*>(args);
		if (!a->filepath)
			return 0;
		engine->GetAudioContext().PlayOneShot(a->filepath, a->volume);
		return 0;
	});

	RegisterCmd("PlayOneShot3D", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const PlayOneShot3DArgs*>(args);
		if (!a->filepath)
			return 0;
		engine->GetAudioContext().PlayOneShot3D(a->filepath, JPH::Vec3(a->x, a->y, a->z),
												a->volume);
		return 0;
	});

	RegisterCmd("PlayProceduralBeep", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const PlayProceduralBeepArgs*>(args);
		engine->GetAudioContext().PlayProceduralBeep(a->frequency, a->duration, a->volume);
		return 0;
	});

	// --- Physics & Control ---
	RegisterCmd("SetCharacterVelocity", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const SetCharVelArgs*>(args);
		ZHLN::Physics::SetCharacterVelocity(engine->GetPhysicsContext(),
											ZHLN::Entity::Unpack(a->entityRaw),
											JPH::Vec3(a->x, a->y, a->z));
		return 0;
	});

	RegisterCmd("IsCharacterOnGround", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const EntityOnlyArgs*>(args);
		return ZHLN::Physics::IsCharacterOnGround(engine->GetPhysicsContext(),
												  ZHLN::Entity::Unpack(a->entityRaw))
				   ? 1
				   : 0;
	});

	RegisterCmd("SetLinearVelocity", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const SetCharVelArgs*>(args);
		ZHLN::Physics::SetLinearVelocity(engine->GetPhysicsContext(),
										 ZHLN::Entity::Unpack(a->entityRaw),
										 JPH::Vec3(a->x, a->y, a->z));
		return 0;
	});

	RegisterCmd("AddImpulse", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const SetCharVelArgs*>(args);
		ZHLN::Physics::AddImpulse(engine->GetPhysicsContext(), ZHLN::Entity::Unpack(a->entityRaw),
								  JPH::Vec3(a->x, a->y, a->z));
		return 0;
	});

	RegisterCmd("AddImpulseAt", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const AddImpulseAtArgs*>(args);
		ZHLN::Physics::AddImpulse(engine->GetPhysicsContext(), ZHLN::Entity::Unpack(a->entityRaw),
								  JPH::Vec3(a->ix, a->iy, a->iz), JPH::RVec3(a->px, a->py, a->pz));
		return 0;
	});

	RegisterCmd("Raycast", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const RaycastArgs*>(args);
		ZHLN::Entity ignore =
			a->ignoreEntity != 0 ? ZHLN::Entity::Unpack(a->ignoreEntity) : ZHLN::Entity{};
		auto res =
			ZHLN::Physics::Raycast(engine->GetPhysicsContext(), JPH::RVec3(a->ox, a->oy, a->oz),
								   JPH::Vec3(a->dx, a->dy, a->dz), a->maxDist, ignore);
		a->outResult->hasHit = res.hasHit ? 1 : 0;
		if (res.hasHit) {
			a->outResult->entity = res.handle.Pack();
			a->outResult->px = res.position.GetX();
			a->outResult->py = res.position.GetY();
			a->outResult->pz = res.position.GetZ();
			a->outResult->nx = res.normal.GetX();
			a->outResult->ny = res.normal.GetY();
			a->outResult->nz = res.normal.GetZ();
			a->outResult->fraction = res.fraction;
		}
		return 0;
	});

	RegisterCmd("SetMovementInput", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const SetMoveInputArgs*>(args);
		if (auto* move = engine->GetRegistry().Get<ZHLN::MovementComponent>(
				ZHLN::Entity::Unpack(a->entityRaw))) {
			move->inputX = a->x;
			move->inputZ = a->z;
		}
		return 0;
	});

	RegisterCmd("SetJumpIntent", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const EntityOnlyArgs*>(args);
		if (auto* move = engine->GetRegistry().Get<ZHLN::MovementComponent>(
				ZHLN::Entity::Unpack(a->entityRaw))) {
			move->jumpRequested = true;
		}
		return 0;
	});

	RegisterCmd("UnprojectScreenToWorld", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const UnprojectArgs*>(args);
		auto winSize = engine->GetWindow().GetSize();
		if (winSize.width == 0 || winSize.height == 0)
			return 0;

		float aspect = (float)winSize.width / (float)winSize.height;
		const auto& cam = engine->GetCamera();
		JPH::Mat44 invVP = (cam.GetProjectionMatrix(aspect) * cam.GetViewMatrix()).Inversed();
		JPH::Vec4 nearWorld = invVP * JPH::Vec4(a->ndcX, a->ndcY, 0.0f, 1.0f);
		JPH::Vec4 farWorld = invVP * JPH::Vec4(a->ndcX, a->ndcY, 1.0f, 1.0f);
		JPH::Vec3 pNear =
			JPH::Vec3(nearWorld.GetX() / nearWorld.GetW(), nearWorld.GetY() / nearWorld.GetW(),
					  nearWorld.GetZ() / nearWorld.GetW());
		JPH::Vec3 pFar =
			JPH::Vec3(farWorld.GetX() / farWorld.GetW(), farWorld.GetY() / farWorld.GetW(),
					  farWorld.GetZ() / farWorld.GetW());
		JPH::Vec3 dir = (pFar - pNear).Normalized();

		*a->ox = pNear.GetX();
		*a->oy = pNear.GetY();
		*a->oz = pNear.GetZ();
		*a->dx = dir.GetX();
		*a->dy = dir.GetY();
		*a->dz = dir.GetZ();
		return 0;
	});

	RegisterCmd("GetTotalTime", [](ZHLN::Engine*, const void* args) -> uint64_t {
		if (!args)
			return 0;
		const auto* a = static_cast<const CameraFloatArgs*>(args);
		static auto start = std::chrono::high_resolution_clock::now();
		auto now = std::chrono::high_resolution_clock::now();
		*a->outVal = std::chrono::duration<float>(now - start).count();
		return 0;
	});

	RegisterCmd("LogInventoryShell", [](ZHLN::Engine*, const void* args) -> uint64_t {
		if (!args)
			return 0;
		const auto* a = static_cast<const LogInventoryArgs*>(args);
		if (!a->msg)
			return 0;
		std::string str(a->msg);
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
		std::println(stdout, "[InvShell Output]\n{}", a->msg);
		std::fflush(stdout);
		return 0;
	});

	RegisterCmd("SpawnLight", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args) {
			return 0;
		}
		const auto* a = static_cast<const SpawnLightArgs*>(args);
		auto& reg = engine->GetRegistry();

		ZHLN::Entity e = reg.Create();
		reg.Add(e, ZHLN::TransformComponent{.position = {a->px, a->py, a->pz},
											.rotation = JPH::Quat(a->rx, a->ry, a->rz, a->rw),
											.scale = {1.0f, 1.0f, 1.0f}});
		reg.Add(e, ZHLN::NameComponent{.name = ZHLN::String64("SpawnedLight")});
		reg.Add(e, ZHLN::LightingSystem::LightComponent{.type = a->type,
														.color = JPH::Vec3(a->r, a->g, a->b),
														.intensity = a->intensity,
														.radius = a->radius,
														.direction = JPH::Vec3(a->dx, a->dy, a->dz),
														.range = a->range,
														.points = {},
														.twoSided = a->twoSided});
		return e.Pack();
	});

	RegisterCmd("CreateSoundInstance", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const CreateSoundInstanceArgs*>(args);
		if (!a->filepath)
			return 0;
		void* handle =
			engine->GetAudioContext().CreateSoundInstance(a->filepath, a->spatialized != 0);
		return reinterpret_cast<uint64_t>(handle);
	});

	RegisterCmd("PlaySoundInstance", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const SoundInstanceArgs*>(args);
		engine->GetAudioContext().PlaySoundInstance(reinterpret_cast<void*>(a->handle));
		return 0;
	});

	RegisterCmd("StopSoundInstance", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const SoundInstanceArgs*>(args);
		engine->GetAudioContext().StopSoundInstance(reinterpret_cast<void*>(a->handle));
		return 0;
	});

	RegisterCmd("DestroySoundInstance", [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		if (!engine || !args)
			return 0;
		const auto* a = static_cast<const SoundInstanceArgs*>(args);
		engine->GetAudioContext().DestroySoundInstance(reinterpret_cast<void*>(a->handle));
		return 0;
	});

	RegisterCmd("ProvokeDeviceLost", [](ZHLN::Engine* engine, const void*) -> uint64_t {
		if (!engine) {
			return 0;
		}
		engine->ProvokeDeviceLost();
		return 1;
	});
}

extern "C" {

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
