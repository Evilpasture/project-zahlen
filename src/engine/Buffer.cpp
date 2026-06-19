// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Zahlen/Engine.hpp"
#include "Zahlen/Render.hpp"
#include "Zahlen/Sync.hpp"
#include "ecs/ECS.hpp"
#include "physics/Physics.hpp"

#include <Zahlen/Buffer.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Scripting.h>
#include <physics/PhysicsWorld.hpp>

namespace ZHLN {
/**
 * @brief Policy that defines how to "lock" an engine object for script access.
 */
struct SyncPolicy {
	// The "Universal" entry point
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
		// 1. Thread Synchronization via Policy
		// Since sync is the first member, we can safely cast the owner to BufferSync
		auto* sync = reinterpret_cast<BufferSync*>(const_cast<TOwner*>(owner));
		SyncPolicy::Acquire(sync);

		ZHLN_BufferView view = {};
		view.buf = (void*)data;
		view.obj = (void*)sync; // Store the SYNC pointer, not the object pointer
		view.itemsize = sizeof(TData);
		std::strncpy(view.format, format, 7);
		view.readonly = 0;

		// 2. Setup Dimensions
		view.ndim = sizeof...(dims);
		size_t d_array[] = {static_cast<size_t>(dims)...};

		// 3. Recursive Stride Calculation (C-Contiguous / Row-Major)
		size_t stride = sizeof(TData);
		for (int i = (int)view.ndim - 1; i >= 0; --i) {
			view.shape[i] = d_array[i];
			view.strides[i] = stride;
			stride *= d_array[i];
		}

		view.len = stride; // Total footprint in bytes
		view.flags = ZHLN_BUFFER_CONTIGUOUS | ZHLN_BUFFER_WRITABLE;
		if (((uintptr_t)view.buf % 32) == 0) {
			view.flags |= ZHLN_BUFFER_ALIGNED_32;
		}

		return view;
	}
};
} // namespace ZHLN

extern "C" {

ZHLN_BufferView ZHLN_GetPhysicsPositions(ZHLN_Engine* engine_handle) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	const auto& world = engine->GetPhysicsContext().GetWorld();

	return ZHLN::ViewComposer::Build(&world, world.positions, (sizeof(JPH::Real) == 8) ? "d" : "f",
									 world.count.load(), 4);
}

ZHLN_BufferView ZHLN_GetPhysicsLinearVelocities(ZHLN_Engine* engine_handle) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	const auto& world = engine->GetPhysicsContext().GetWorld();

	return ZHLN::ViewComposer::Build(&world, world.linearVelocities, "f", world.count.load(), 4);
}

void ZHLN_ReleaseBuffer(void* sync_ptr) {
	ZHLN::SyncPolicy::Release(static_cast<ZHLN::BufferSync*>(sync_ptr));
}

ZHLN_BufferView ZHLN_GetECSBuffer(struct ZHLN_Engine* engine_handle, const char* componentName) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	auto& reg = engine->GetRegistry();
	std::string_view name(componentName);

	if (name == "PhysicsComponent") {
		auto raw = reg.GetRawArray<ZHLN::PhysicsComponent>();
		return ZHLN::ViewComposer::Build(&reg, raw.data(), "Q", raw.size());
	}
	if (name == "PBRComponent") {
		auto raw = reg.GetRawArray<ZHLN::PBRComponent>();
		return ZHLN::ViewComposer::Build(&reg, raw.data(), "f", raw.size(),
										 2); // 2 floats: roughness, metallic
	}

	return {};
}

ZHLN_BufferView ZHLN_GetECSEntities(struct ZHLN_Engine* engine_handle, const char* componentName) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	auto& reg = engine->GetRegistry();

	uint32_t familyID = ZHLN::ECS::Registry::GetFamilyIDFromName(componentName);
	if (familyID == 0xFFFFFFFF) {
		return {};
	}

	auto entities = reg.GetEntitiesByFamilyID(familyID);
	return ZHLN::ViewComposer::Build(&reg, const_cast<ZHLN::Entity*>(entities.data()), "Q",
									 entities.size());
}

ZHLN_BufferView ZHLN_GetPhysicsContactEvents(ZHLN_Engine* engine_handle) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	auto events = ZHLN::Physics::GetContactEvents(engine->GetPhysicsContext());

	const char* fmt = (sizeof(JPH::Real) == 8) ? "EvtD" : "EvtF";

	return ZHLN::ViewComposer::Build(&engine->GetPhysicsContext().GetWorld(), events.first, fmt,
									 events.second);
}

void* ZHLN_GetComponent(ZHLN_Engine* engine_handle, uint64_t entityRaw, const char* componentName) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	auto entity = ZHLN::Entity::Unpack(entityRaw);
	auto& reg = engine->GetRegistry();

	uint32_t familyID = ZHLN::ECS::Registry::GetFamilyIDFromName(componentName);
	if (familyID == 0xFFFFFFFF) {
		return nullptr; // Not a registered C++ component
	}

	return reg.GetRawByFamily(entity, familyID);
}

void* ZHLN_AddComponent(ZHLN_Engine* handle, uint64_t entityRaw, const char* componentName) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(handle);
	auto entity = ZHLN::Entity::Unpack(entityRaw);
	auto& reg = engine->GetRegistry();

	std::string_view name(componentName);
	if (name == "HierarchyComponent") {
		return &reg.Add(entity, ZHLN::HierarchyComponent{});
	}
	if (name == "TransformComponent") {
		return &reg.Add(entity, ZHLN::TransformComponent{});
	}
	if (name == "MovementComponent") {
		return &reg.Add(entity, ZHLN::MovementComponent{});
	}
	if (name == "PhysicsStateComponent") {
		return &reg.Add(entity, ZHLN::PhysicsStateComponent{});
	}
	if (name == "TargetCameraComponent") {
		return &reg.Add(entity, ZHLN::TargetCameraComponent{});
	}
	if (name == "PBRComponent") {
		return &reg.Add(entity, ZHLN::PBRComponent{});
	}
	if (name == "TextComponent") {
		return &reg.Add(entity, ZHLN::TextComponent{});
	}
	return nullptr;
}

static_assert(sizeof(ZHLN::TextComponent) == 336);
static_assert(offsetof(ZHLN::TextComponent, color) == 288);
static_assert(offsetof(ZHLN::TextComponent, mesh) == 312);

uint64_t ZHLN_CreateEntity(ZHLN_Engine* engine_handle) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	return engine->GetRegistry().Create().Pack();
}

void ZHLN_DestroyEntity(ZHLN_Engine* engine_handle, uint64_t entityRaw) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	auto entity = ZHLN::Entity::Unpack(entityRaw);

	// AUTOMATIC VULKAN CLEANUP:
	// If the entity carries a TextComponent, destroy its GPU buffers in Vulkan to prevent
	// memory/pool leaks!
	auto* text = engine->GetRegistry().Get<ZHLN::TextComponent>(entity);
	if (text != nullptr) {
		if (text->mesh.vertexBuffer != ZHLN::BufferHandle::Invalid) {
			engine->GetRenderContext().DestroyBuffer(text->mesh.vertexBuffer);
		}
		if (text->mesh.indexBuffer != ZHLN::BufferHandle::Invalid) {
			engine->GetRenderContext().DestroyBuffer(text->mesh.indexBuffer);
		}
	}

	engine->GetRegistry().Destroy(entity);
}
}
