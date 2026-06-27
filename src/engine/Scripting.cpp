// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/Scripting.cpp

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
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

// ============================================================================
// PAYLOAD DEFINITIONS (Must exactly match ffi_cdef.lua)
// ============================================================================
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
using AddComponentArgs = GetComponentArgs; // Alias to resolve compiler type resolution

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
	float rx, ry, rz, rw; // Rotation (Quaternion) <-- Added
	float r, g, b;		  // Color
	float intensity;
	float radius;
	float dx, dy, dz; // Direction
	float range;
	ZHLN::LightType type; // 0=Dir, 1=Point, 2=Spot, 3=Area
	uint32_t twoSided;
};

#pragma pack(pop)

// Opaque declarations of ConsoleUI globals (defined in ConsoleUI.cpp)
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
static std::unordered_map<std::string_view, CommandHandler> s_CommandRegistry;

static void RegisterFFICommands() {
	if (!s_CommandRegistry.empty()) {
		return;
	}

	// --- Asset Spawning ---
	s_CommandRegistry["SpawnPrefab"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const SpawnPrefabArgs*>(args);
		auto& rc = engine->GetRenderContext();
		auto& reg = engine->GetRegistry();
		auto& pc = engine->GetPhysicsContext();

		auto* prefab = ZHLN::AssetFactory::LoadModelPrefab(rc, engine->GetAssetManager(), a->path);
		if (!prefab) {
			return 0;
		}

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
	};

	s_CommandRegistry["SetupRagdoll"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const SetupRagdollArgs*>(args);
		std::vector<ZHLN::Entity> parts(a->count);
		for (uint32_t i = 0; i < a->count; ++i) {
			parts[i] = ZHLN::Entity::Unpack(a->visualParts[i]);
		}
		ZHLN::AssetFactory::SetupPlayerRagdoll(engine->GetRenderContext(),
											   engine->GetPhysicsContext(), engine->GetRegistry(),
											   ZHLN::Entity::Unpack(a->playerEntity), parts);
		return 1;
	};

	s_CommandRegistry["CreateBox"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const CreateBoxArgs*>(args);
		ZHLN::Mesh mesh = ZHLN::AssetFactory::CreateBox(engine->GetRenderContext(),
														JPH::Vec3(a->hx, a->hy, a->hz),
														JPH::Vec4(a->r, a->g, a->b, a->a));
		return static_cast<uint64_t>(
			mesh.posBuffer); // Return the position stream handle as the key
	};

	s_CommandRegistry["CreateBasicMaterial"] = [](ZHLN::Engine* engine,
												  const void* args) -> uint64_t {
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
	};

	s_CommandRegistry["SpawnEntity"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
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
		// Configure DrawFlags based on transparency
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
					   desc->isStatic ? 0 : 1)});
		reg.Add(e, ZHLN::PhysicsStateComponent{.currPosition = {desc->px, desc->py, desc->pz},
											   .prevPosition = {desc->px, desc->py, desc->pz},
											   .currRotation = rotation,
											   .prevRotation = rotation});

		return e.Pack();
	};

	// --- Buffer Views ---
	s_CommandRegistry["GetPhysicsPositions"] = [](ZHLN::Engine* engine,
												  const void* args) -> uint64_t {
		const auto* a = static_cast<const GetBufferArgs*>(args);
		const auto& world = engine->GetPhysicsContext().GetWorld();
		*a->outView = ZHLN::ViewComposer::Build(
			&world, world.positions, (sizeof(JPH::Real) == 8) ? "d" : "f", world.count.load(), 4);
		return 0;
	};

	s_CommandRegistry["GetPhysicsLinearVelocities"] = [](ZHLN::Engine* engine,
														 const void* args) -> uint64_t {
		const auto* a = static_cast<const GetBufferArgs*>(args);
		const auto& world = engine->GetPhysicsContext().GetWorld();
		*a->outView =
			ZHLN::ViewComposer::Build(&world, world.linearVelocities, "f", world.count.load(), 4);
		return 0;
	};

	s_CommandRegistry["ReleaseBuffer"] = [](ZHLN::Engine*, const void* args) -> uint64_t {
		const auto* a = static_cast<const ReleaseBufferArgs*>(args);
		ZHLN::SyncPolicy::Release(static_cast<ZHLN::BufferSync*>(a->sync_ptr));
		return 0;
	};

	s_CommandRegistry["GetECSBuffer"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const GetECSBufferArgs*>(args);
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
			*a->outView = ZHLN::ViewComposer::Build(&reg, raw.data(), "B",
													raw.size()); // "B" for Bytes/Struct
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
	};

	s_CommandRegistry["GetECSEntities"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const GetECSBufferArgs*>(args);
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
	};

	s_CommandRegistry["GetPhysicsContactEvents"] = [](ZHLN::Engine* engine,
													  const void* args) -> uint64_t {
		const auto* a = static_cast<const GetBufferArgs*>(args);
		auto events = ZHLN::Physics::GetContactEvents(engine->GetPhysicsContext());
		const char* fmt = (sizeof(JPH::Real) == 8) ? "EvtD" : "EvtF";
		*a->outView = ZHLN::ViewComposer::Build(&engine->GetPhysicsContext().GetWorld(),
												events.first, fmt, events.second);
		return 0;
	};

	// --- ECS ---
	s_CommandRegistry["RegisterDynamicComponent"] = [](ZHLN::Engine* engine,
													   const void* args) -> uint64_t {
		const auto* a = static_cast<const RegisterDynamicComponentArgs*>(args);
		auto& reg = engine->GetRegistry();
		return reg.RegisterComponentDynamic(a->name, a->size, a->alignment);
	};

	s_CommandRegistry["GetComponent"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const GetComponentArgs*>(args);
		uint32_t familyID = ZHLN::ECS::Registry::GetFamilyIDFromName(a->componentName);
		if (familyID == 0xFFFFFFFF) {
			return 0;
		}
		return std::bit_cast<uint64_t>(
			engine->GetRegistry().GetRawByFamily(ZHLN::Entity::Unpack(a->entityRaw), familyID));
	};

	s_CommandRegistry["AddComponent"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const AddComponentArgs*>(args);
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
		} else {
			// 2. Fall back to dynamically registered types
			uint32_t familyID = ZHLN::ECS::Registry::GetFamilyIDFromName(name);
			if (familyID != 0xFFFFFFFF) {
				ptr = reg.AddDynamic(entity, familyID);
			}
		}
		return std::bit_cast<uint64_t>(ptr);
	};

	s_CommandRegistry["CreateEntity"] = [](ZHLN::Engine* engine, const void*) -> uint64_t {
		return engine->GetRegistry().Create().Pack();
	};

	s_CommandRegistry["DestroyEntity"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const EntityOnlyArgs*>(args);
		auto entity = ZHLN::Entity::Unpack(a->entityRaw);
		auto* text = engine->GetRegistry().Get<ZHLN::TextComponent>(entity);
		if (text != nullptr) {
			// Clean up both uniquely generated text mesh streams
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
		auto* mesh = engine->GetRegistry().Get<ZHLN::MeshComponent>(entity);
		if (mesh != nullptr) {
			if (mesh->skinnedVertexBuffer != ZHLN::BufferHandle::Invalid) {
				engine->GetRenderContext().DestroyBuffer(mesh->skinnedVertexBuffer);
			}
		}
		engine->GetRegistry().Destroy(entity);
		return 0;
	};

	// --- Input ---
	s_CommandRegistry["IsKeyDown"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const IsKeyDownArgs*>(args);
		return engine->GetInput().IsKeyDown(static_cast<ZHLN::KeyCode>(a->key)) ? 1 : 0;
	};

	s_CommandRegistry["GetMouseDelta"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const GetMouseDeltaArgs*>(args);
		*a->outX = engine->GetInput().GetMouse().deltaX;
		*a->outY = engine->GetInput().GetMouse().deltaY;
		return 0;
	};

	// --- Camera ---
	s_CommandRegistry["GetCameraYaw"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const CameraFloatArgs*>(args);
		*a->outVal = engine->GetCamera().yaw;
		return 0;
	};

	s_CommandRegistry["GetCameraFOV"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const CameraFloatArgs*>(args);
		*a->outVal = engine->GetCamera().fov;
		return 0;
	};

	s_CommandRegistry["SetCameraFOV"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const SetCameraFOVArgs*>(args);
		engine->GetCamera().fov = a->fov;
		return 0;
	};

	// --- Audio ---
	s_CommandRegistry["PlayOneShot"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const PlayOneShotArgs*>(args);
		engine->GetAudioContext().PlayOneShot(a->filepath, a->volume);
		return 0;
	};

	s_CommandRegistry["PlayOneShot3D"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const PlayOneShot3DArgs*>(args);
		engine->GetAudioContext().PlayOneShot3D(a->filepath, JPH::Vec3(a->x, a->y, a->z),
												a->volume);
		return 0;
	};

	s_CommandRegistry["PlayProceduralBeep"] = [](ZHLN::Engine* engine,
												 const void* args) -> uint64_t {
		const auto* a = static_cast<const PlayProceduralBeepArgs*>(args);
		engine->GetAudioContext().PlayProceduralBeep(a->frequency, a->duration, a->volume);
		return 0;
	};

	// --- Physics & Control ---
	s_CommandRegistry["SetCharacterVelocity"] = [](ZHLN::Engine* engine,
												   const void* args) -> uint64_t {
		const auto* a = static_cast<const SetCharVelArgs*>(args);
		ZHLN::Physics::SetCharacterVelocity(engine->GetPhysicsContext(),
											ZHLN::Entity::Unpack(a->entityRaw),
											JPH::Vec3(a->x, a->y, a->z));
		return 0;
	};

	s_CommandRegistry["IsCharacterOnGround"] = [](ZHLN::Engine* engine,
												  const void* args) -> uint64_t {
		const auto* a = static_cast<const EntityOnlyArgs*>(args);
		return ZHLN::Physics::IsCharacterOnGround(engine->GetPhysicsContext(),
												  ZHLN::Entity::Unpack(a->entityRaw))
				   ? 1
				   : 0;
	};

	s_CommandRegistry["SetLinearVelocity"] = [](ZHLN::Engine* engine,
												const void* args) -> uint64_t {
		const auto* a = static_cast<const SetCharVelArgs*>(args);
		ZHLN::Physics::SetLinearVelocity(engine->GetPhysicsContext(),
										 ZHLN::Entity::Unpack(a->entityRaw),
										 JPH::Vec3(a->x, a->y, a->z));
		return 0;
	};

	s_CommandRegistry["AddImpulse"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const SetCharVelArgs*>(args);
		ZHLN::Physics::AddImpulse(engine->GetPhysicsContext(), ZHLN::Entity::Unpack(a->entityRaw),
								  JPH::Vec3(a->x, a->y, a->z));
		return 0;
	};

	s_CommandRegistry["AddImpulseAt"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const AddImpulseAtArgs*>(args);
		ZHLN::Physics::AddImpulse(engine->GetPhysicsContext(), ZHLN::Entity::Unpack(a->entityRaw),
								  JPH::Vec3(a->ix, a->iy, a->iz), JPH::RVec3(a->px, a->py, a->pz));
		return 0;
	};

	s_CommandRegistry["Raycast"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
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
	};

	s_CommandRegistry["SetMovementInput"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const SetMoveInputArgs*>(args);
		if (auto* move = engine->GetRegistry().Get<ZHLN::MovementComponent>(
				ZHLN::Entity::Unpack(a->entityRaw))) {
			move->inputX = a->x;
			move->inputZ = a->z;
		}
		return 0;
	};

	s_CommandRegistry["SetJumpIntent"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const EntityOnlyArgs*>(args);
		if (auto* move = engine->GetRegistry().Get<ZHLN::MovementComponent>(
				ZHLN::Entity::Unpack(a->entityRaw))) {
			move->jumpRequested = true;
		}
		return 0;
	};

	s_CommandRegistry["UnprojectScreenToWorld"] = [](ZHLN::Engine* engine,
													 const void* args) -> uint64_t {
		const auto* a = static_cast<const UnprojectArgs*>(args);
		auto winSize = engine->GetWindow().GetSize();
		if (winSize.width == 0 || winSize.height == 0) {
			return 0;
		}
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
	};

	// --- Global State & Logging ---
	s_CommandRegistry["GetTotalTime"] = [](ZHLN::Engine*, const void* args) -> uint64_t {
		const auto* a = static_cast<const CameraFloatArgs*>(args);
		static auto start = std::chrono::high_resolution_clock::now();
		auto now = std::chrono::high_resolution_clock::now();
		*a->outVal = std::chrono::duration<float>(now - start).count();
		return 0;
	};

	s_CommandRegistry["LogInventoryShell"] = [](ZHLN::Engine*, const void* args) -> uint64_t {
		const auto* a = static_cast<const LogInventoryArgs*>(args);
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
	};

	s_CommandRegistry["SpawnLight"] = [](ZHLN::Engine* engine, const void* args) -> uint64_t {
		const auto* a = static_cast<const SpawnLightArgs*>(args);
		auto& reg = engine->GetRegistry();

		ZHLN::Entity e = reg.Create();

		// Apply the passed rotation to the Transform
		reg.Add(
			e, ZHLN::TransformComponent{
				   .position = {a->px, a->py, a->pz},
				   .rotation = JPH::Quat(a->rx, a->ry, a->rz, a->rw), // <-- Custom rotation applied
				   .scale = {1.0f, 1.0f, 1.0f}});

		reg.Add(e, ZHLN::NameComponent{.name = ZHLN::String64("SpawnedLight")});
		reg.Add(e, ZHLN::LightingSystem::LightComponent{.type = a->type,
														.color = JPH::Vec3(a->r, a->g, a->b),
														.intensity = a->intensity,
														.radius = a->radius,
														.direction = JPH::Vec3(a->dx, a->dy, a->dz),
														.range = a->range,
														.points = JPH::Mat44::sIdentity(),
														.twoSided = a->twoSided});

		return e.Pack();
	};
}

// ============================================================================
// ALL C-EXPORTS MUST BE INSIDE THIS BLOCK TO AVOID MANGLING
// ============================================================================
extern "C" {

ZHLN_API ZHLN_Engine* ZHLN_GetEngineContext() {
	return reinterpret_cast<ZHLN_Engine*>(ZHLN::GetEngineContext());
}

ZHLN_API uint64_t ZHLN_DispatchCommand(ZHLN_Engine* engine_handle, const char* cmd,
									   const void* args) {
	RegisterFFICommands();
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	auto it = s_CommandRegistry.find(std::string_view(cmd));
	if (it != s_CommandRegistry.end()) {
		return it->second(engine, args);
	}
	return 0;
}

// Lua internal Bridges
static int LuaBridge_Log(lua_State* L) {
	lua_Debug ar;
	std::memset(&ar, 0, sizeof(lua_Debug));

	if (lua_getstack(L, 1, &ar)) {
		lua_getinfo(L, "Sl", &ar);
	} else {
		std::strncpy(ar.short_src, "unknown", sizeof(ar.short_src) - 1);
		ar.currentline = 0;
	}

	int n = lua_gettop(L);
	std::string msg;

	lua_getglobal(L, "tostring");

	for (int i = 1; i <= n; i++) {
		lua_pushvalue(L, -1);
		lua_pushvalue(L, i);
		lua_call(L, 1, 1);

		size_t len = 0;
		const char* s = lua_tolstring(L, -1, &len);
		if (i > 1) {
			msg += "\t";
		}
		if (s != nullptr) {
			msg += std::string(s, len);
		}

		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	std::string_view file = ar.short_src;
	if (auto pos = file.find_last_of("/\\"); pos != std::string_view::npos) {
		file.remove_prefix(pos + 1);
	}

	ZHLN::LogManual(file, ar.currentline, msg, ZHLN::Color::Green);
	ZHLN::GameConsole::Log(msg, {.r = 0.4f, .g = 1.0f, .b = 0.4f, .a = 1.0f});

	return 0;
}

static int LuaBridge_Warn(lua_State* L) {
	lua_Debug ar;
	std::memset(&ar, 0, sizeof(lua_Debug));

	if (lua_getstack(L, 1, &ar)) {
		lua_getinfo(L, "Sl", &ar);
	} else {
		std::strncpy(ar.short_src, "unknown", sizeof(ar.short_src) - 1);
		ar.currentline = 0;
	}

	int n = lua_gettop(L);
	std::string msg;

	lua_getglobal(L, "tostring");

	for (int i = 1; i <= n; i++) {
		lua_pushvalue(L, -1);
		lua_pushvalue(L, i);
		lua_call(L, 1, 1);

		size_t len = 0;
		const char* s = lua_tolstring(L, -1, &len);
		if (i > 1) {
			msg += "\t";
		}
		if (s) {
			msg += std::string(s, len);
		}

		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	std::string_view file = ar.short_src;
	if (auto pos = file.find_last_of("/\\"); pos != std::string_view::npos) {
		file.remove_prefix(pos + 1);
	}

	ZHLN::LogManual(file, ar.currentline, msg, ZHLN::Color::Yellow);

	return 0;
}

} // End of extern "C"

namespace ZHLN {

ScriptRunner::ScriptRunner() : L(luaL_newstate()) {
	luaL_openlibs(L);

	lua_newtable(L);
	lua_pushcfunction(L, LuaBridge_Log);
	lua_setfield(L, -2, "log");
	lua_pushcfunction(L, LuaBridge_Warn);
	lua_setfield(L, -2, "warn");
	lua_setglobal(L, "zahlen");

	lua_pushcfunction(L, LuaBridge_Log);
	lua_setglobal(L, "print");
	lua_getglobal(L, "require");
	lua_pushstring(L, "scripts.core.memoryview");
	if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
		Panic("Failed to load core script: scripts/core/memoryview.lua. Error: {}",
			  lua_tostring(L, -1));
	}
	lua_pop(L, 1);
}

ScriptRunner::~ScriptRunner() {
	if (L != nullptr) {
		lua_close(L);
	}
}

void ScriptRunner::RunFile(std::string_view path) {
	std::string p(path);
	if (luaL_dofile(L, p.c_str()) != LUA_OK) {
		Log("Lua Error in {}: {}", path, lua_tostring(L, -1));
		lua_pop(L, 1);
	}
}

void ScriptRunner::CallUpdate(Engine* engine, float dt) {
	lua_getglobal(L, "update");
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 1);
		return;
	}

	lua_pushlightuserdata(L, engine);
	lua_pushnumber(L, dt);

	if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
		Log("Lua Error: {}", lua_tostring(L, -1));
		lua_pop(L, 1);
	}

	lua_getglobal(L, "require");
	lua_pushstring(L, "scripts.core.zahlen");
	lua_pcall(L, 1, 1, 0);
	lua_getfield(L, -1, "cleanup");
	lua_pcall(L, 0, 0, 0);
	lua_pop(L, 1);
}

void ScriptRunner::ExecuteString(std::string_view code) {
	if (luaL_dostring(L, code.data()) != LUA_OK) {
		std::string err = lua_tostring(L, -1);
		ZHLN::GameConsole::Log("Lua Error: " + err, {1.0f, 0.4f, 0.4f, 1.0f});
		lua_pop(L, 1);
	}
}

void ScriptRunner::ReloadFile(std::string_view path) {
	std::string moduleName = std::string(path);

	if (size_t pos = moduleName.find(".lua"); pos != std::string::npos) {
		moduleName.erase(pos);
	}
	std::ranges::replace(moduleName, '/', '.');

	std::string resetCode = std::format("package.loaded['{}'] = nil", moduleName);
	luaL_dostring(L, resetCode.c_str());

	RunFile(path);

	Log("Script Hot-Reloaded: {}", path);
	ZHLN::GameConsole::Log("Hot-Reloaded: " + std::string(path),
						   {.r = 0.2f, .g = 0.8f, .b = 1.0f, .a = 1.0f});
}

} // namespace ZHLN
