// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/MotorSettings.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>

#include "Physics.hpp"
#include "PhysicsDebug.hpp"
#include <Zahlen/Buffer.h>
#include "PhysicsWorld.hpp"
#include <Zahlen/Log.hpp>
#include "PhysicsContactEvents.hpp"
#include "Zahlen/Profiler.hpp"
#include "detail/ControlFlow.hpp"

// ZHLN Detail Utilities
#include <algorithm>
#include <cstddef>
#include <detail/Prefetch.hpp>
#include <detail/Span.hpp>
#include <detail/Loop.hpp>
// clang-format on

#include <cstring>
#include <memory>
#include <new>

namespace ZHLN {

struct ShapeKey {
	uint32_t type;
	float p1, p2, p3, p4;
};

struct ShapeEntry {
	ShapeKey key;
	JPH::ShapeRefC shape;
};

enum SlotState : uint8_t { SLOT_EMPTY = 0, SLOT_ALIVE = 1, SLOT_CHARACTER = 2 };

// =================================================================================================
// MEMORY UTILITIES
// =================================================================================================

template <typename T> [[nodiscard]] static T* AllocateAligned(size_t count, size_t alignment) {
	return static_cast<T*>(::operator new[](count * sizeof(T), std::align_val_t{alignment}));
}

template <typename T> static void DeallocateAligned(T* ptr, size_t alignment) {
	if (ptr) {
		::operator delete[](ptr, std::align_val_t{alignment});
	}
}

template <typename T>
static void ReallocateAligned(T*& ptr, size_t old_count, size_t new_count, size_t alignment) {
	static_assert(std::is_trivially_copyable_v<T>);
	T* new_ptr = AllocateAligned<T>(new_count, alignment);
	if (ptr && old_count > 0) {
		std::memcpy(new_ptr, ptr, old_count * sizeof(T));
		DeallocateAligned(ptr, alignment);
	} else if (ptr) {
		DeallocateAligned(ptr, alignment);
	}
	ptr = new_ptr;
}

template <typename T> static void ReallocateStandard(T*& ptr, size_t old_count, size_t new_count) {
	static_assert(std::is_trivially_copyable_v<T>);
	T* new_ptr = new T[new_count]();
	if (ptr && old_count > 0) {
		std::memcpy(new_ptr, ptr, old_count * sizeof(T));
		delete[] ptr;
	} else {
		delete[] ptr;
	}

	ptr = new_ptr;
}

// --- Jolt Boilerplate: Layers & Filters ---
namespace Layers {
enum ID : JPH::ObjectLayer { NON_MOVING = 0, MOVING = 1, NUM_LAYERS = 2 };
}
namespace BroadPhaseLayers {
enum ID : uint8_t { NON_MOVING = 0, MOVING = 1, NUM_LAYERS = 2 };
}

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
	JPH::BroadPhaseLayer mObjectToBroadPhase[Layers::NUM_LAYERS]{};

  public:
	BPLayerInterfaceImpl() {
		mObjectToBroadPhase[Layers::NON_MOVING] =
			JPH::BroadPhaseLayer(BroadPhaseLayers::NON_MOVING);
		mObjectToBroadPhase[Layers::MOVING] = JPH::BroadPhaseLayer(BroadPhaseLayers::MOVING);
	}
	[[nodiscard]] uint32_t GetNumBroadPhaseLayers() const override {
		return BroadPhaseLayers::NUM_LAYERS;
	}

	[[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
		return mObjectToBroadPhase[inLayer];
	}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
	[[nodiscard]] const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
		switch (static_cast<BroadPhaseLayers::ID>(static_cast<uint8_t>(inLayer))) {
			case BroadPhaseLayers::NON_MOVING:
				return "NON_MOVING";
			case BroadPhaseLayers::MOVING:
				return "MOVING";
			default:
				return "INVALID";
		}
	}
#endif
};

class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter {
  public:
	[[nodiscard]] bool ShouldCollide(JPH::ObjectLayer inLayer1,
									 JPH::BroadPhaseLayer inLayer2) const override {
		switch (inLayer1) {
			case Layers::NON_MOVING:
				return inLayer2 == JPH::BroadPhaseLayer(BroadPhaseLayers::MOVING);
			case Layers::MOVING:
				return true;
			default:
				return false;
		}
	}
};

class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter {
  public:
	[[nodiscard]] bool ShouldCollide(JPH::ObjectLayer inObject1,
									 JPH::ObjectLayer inObject2) const override {
		switch (inObject1) {
			case Layers::NON_MOVING:
				return inObject2 == Layers::MOVING;
			case Layers::MOVING:
				return true;
			default:
				return false;
		}
	}
};

// =================================================================================================
// CONTEXT IMPLEMENTATION
// =================================================================================================

struct PhysicsContext::Impl {
	JPH::PhysicsSystem physicsSystem;
	std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
	JPH::JobSystemThreadPool jobSystem{JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 1};

	BPLayerInterfaceImpl bpLayerInterface;
	ObjectVsBroadPhaseLayerFilterImpl objVsBpFilter;
	ObjectLayerPairFilterImpl objVsObjFilter;

	Physics::PhysicsWorld world{};

	Physics::ContactListener contactListener{&world};

	std::unique_ptr<Physics::PhysicsDebugRenderer> debugRenderer;

	JPH::Array<JPH::Ref<JPH::CharacterVirtual>> characterMap;
	JPH::Array<JPH::CharacterVirtual*> activeCharacters;

	// Character contact listener needs to be stored
	Physics::CharacterListener characterListener{&world};
	JPH::Array<ShapeEntry> shapeCache;
};

std::unique_ptr<Physics::PhysicsDebugRenderer> debugRenderer;

PhysicsContext::PhysicsContext(const PhysicsConfig& cfg) : _impl(std::make_unique<Impl>()) {
	_impl->tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(cfg.tempAllocatorSize);

	_impl->physicsSystem.SetContactListener(&_impl->contactListener);
	_impl->physicsSystem.Init(cfg.maxBodies, 0, cfg.maxBodyPairs, cfg.maxContactConstraints,
							  _impl->bpLayerInterface, _impl->objVsBpFilter, _impl->objVsObjFilter);

	_impl->debugRenderer = std::make_unique<Physics::PhysicsDebugRenderer>();
	_impl->characterListener = Physics::CharacterListener(&_impl->world);

	_impl->world.Init(cfg.maxBodies, &_impl->physicsSystem, &_impl->jobSystem,
					  _impl->tempAllocator.get());
}

PhysicsContext::~PhysicsContext() {
	// One-liner Shutdown!
	_impl->world.Shutdown();
}

void PhysicsContext::Step(float deltaTime) {
	ZHLN_PROFILE_SCOPE("Physics (Jolt)");
	auto& world = _impl->world;

	// --- 1. COMMAND FLUSH (Structural Phase) ---
	Physics::Command* capturedQueue = nullptr;
	size_t capturedCount = 0;

	ZHLN_LOCK(world.sync.shadowLock) {

		capturedQueue = world.commandQueue;
		capturedCount = world.commandCount;

		// Double-buffer swap
		world.commandQueue = world.commandQueueSpare;
		world.commandQueueSpare = capturedQueue;
		world.commandCount = 0;

		// Execute the specialized flusher
		world.FlushCommands(capturedQueue, capturedCount);
	}

	// Resets the counter so Jolt overwrites last frame's collisions
	world.contactCount.store(0, std::memory_order_relaxed);

	// --- 2. JOLT UPDATE PHASE ---
	world.isStepping.store(true, std::memory_order_release);

	_impl->physicsSystem.Update(deltaTime, 2, _impl->tempAllocator.get(), &_impl->jobSystem);

	for (auto* character : _impl->activeCharacters) {
		character->Update(deltaTime, _impl->physicsSystem.GetGravity(),
						  _impl->physicsSystem.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
						  _impl->physicsSystem.GetDefaultLayerFilter(Layers::MOVING), {}, {},
						  *_impl->tempAllocator);
	}

	// 3. Data Synchronization Pass (Jolt SoA -> Engine Shadow SoA)
	ZHLN_LOCK(world.sync.shadowLock) {
		world.Synchronize(&_impl->physicsSystem, _impl->activeCharacters);
	}

	world.isStepping.store(false, std::memory_order_release);
}

const Physics::PhysicsWorld& PhysicsContext::GetWorld() const {
	return _impl->world;
}

uint32_t PhysicsContext::GetActiveBodyCount() const {
	return _impl->physicsSystem.GetNumActiveBodies(JPH::EBodyType::RigidBody);
}

size_t PhysicsContext::GetMemoryUsage() const {
	// Jolt doesn't have a single "Total" call, but we can track
	// the TempAllocator usage which is the most "active" memory.
	return _impl->tempAllocator->GetSize();
}

void PhysicsContext::OptimizeBroadphase() {
	_impl->physicsSystem.OptimizeBroadPhase();
}

namespace Physics {

// =================================================================================================
// CULVERIN SHAPE CACHING
// =================================================================================================

JPH::ShapeRefC GetOrCreateShape(PhysicsContext& ctx, Physics::ShapeType type, float p1, float p2,
								float p3, float p4) {
	auto* impl = ctx.GetImpl();

	// 1. Normalization (Matches Culverin logic for maximum cache hits)
	const float np1 = (p1 < 1e-3f && type != Physics::ShapeType::Plane) ? 1e-3f : p1;
	const float np2 = (p2 < 1e-3f && type != Physics::ShapeType::Plane) ? 1e-3f : p2;
	const float np3 = (p3 < 1e-3f && type != Physics::ShapeType::Plane) ? 1e-3f : p3;
	const float np4 = (type == Physics::ShapeType::Plane) ? p4 : 0.0f;

	// 2. Cache Lookup
	for (const auto& entry : impl->shapeCache) {
		if (entry.key.type == static_cast<uint32_t>(type) && entry.key.p1 == np1 &&
			entry.key.p2 == np2 && entry.key.p3 == np3 && entry.key.p4 == np4) {
			return entry.shape;
		}
	}

	// 3. Jolt Creation
	JPH::ShapeRefC shape;

	ZHLN_LOCK(impl->world.sync.shadowLock) {
		switch (type) {
			case Physics::ShapeType::Box: {
				JPH::BoxShapeSettings s(JPH::Vec3(np1, np2, np3), 0.05f);
				shape = s.Create().Get();
				break;
			}
			case Physics::ShapeType::Sphere: {
				JPH::SphereShapeSettings s(np1);
				shape = s.Create().Get();
				break;
			}
			case Physics::ShapeType::Capsule: {
				JPH::CapsuleShapeSettings s(np1, np2);
				shape = s.Create().Get();
				break;
			}
			case Physics::ShapeType::Cylinder: {
				JPH::CylinderShapeSettings s(np1, np2, 0.05f);
				shape = s.Create().Get();
				break;
			}
			case Physics::ShapeType::Plane: {
				JPH::Plane plane(JPH::Vec3(np1, np2, np3), np4);
				JPH::PlaneShapeSettings s(plane, nullptr, 1000.0f);
				shape = s.Create().Get();
				break;
			}
		}

		if (shape == nullptr) {
			ZHLN::Panic("Failed to create Jolt Shape! Degenerate parameters?");
		}

		// 4. Store in cache
		impl->shapeCache.push_back({.key = ShapeKey{.type = static_cast<uint32_t>(type),
													.p1 = np1,
													.p2 = np2,
													.p3 = np3,
													.p4 = np4},
									.shape = shape});
	}
	return shape;
}

// =================================================================================================
// PROCEDURAL API
// =================================================================================================

JPH::BodyID PhysicsWorld::GetBodyID(ZHLN::Entity handle) {
	if (handle.index >= slotCapacity) {
		return {};
	}

	// Check generation: Source of Truth check
	if (generations[handle.index].load(std::memory_order_acquire) != handle.generation) {
		return {}; // Stale handle
	}

	uint32_t dense = slotToDense[handle.index];
	return bodyIDs[dense];
}

JPH::BodyID GetBodyID(const PhysicsWorld& world, ZHLN::Entity handle) {
	if (handle.index >= world.slotCapacity) {
		return {};
	}

	if (world.generations[handle.index].load(std::memory_order_acquire) != handle.generation) {
		return {};
	}

	uint32_t dense = world.slotToDense[handle.index];
	return world.bodyIDs[dense];
}

void DestroyBody(PhysicsContext& ctx, ZHLN::Entity handle) {
	auto& world = ctx.GetImpl()->world;
	const uint32_t slot = handle.index;

	if (slot >= world.slotCapacity) {
		return;
	}

	// Source-of-truth generational check
	if (world.generations[slot].load(std::memory_order_acquire) != handle.generation) {
		return;
	}

	const uint8_t state = world.slotStates[slot].load(std::memory_order_acquire);
	const SlotPredicate pred = GetSlotPredicate(state);

	// If it's already EMPTY or PENDING_DESTROY, don't queue it again
	if (!pred.isDestructible) {
		return;
	}

	// Immediately mark as doomed so queries ignore it
	world.slotStates[slot].store(SLOT_PENDING_DESTROY, std::memory_order_release);

	// Lock the shadow buffer to queue the command safely
	ZHLN_LOCK(world.sync.shadowLock) {
		// Expand raw queue if needed
		if (world.commandCount >= world.commandCapacity) {
			size_t newCap = world.commandCapacity * 2;
			auto* newQ = new Physics::Command[newCap];
			auto* newQS = new Physics::Command[newCap];

			std::memcpy(newQ, world.commandQueue, world.commandCount * sizeof(Physics::Command));

			delete[] world.commandQueue;
			delete[] world.commandQueueSpare;

			world.commandQueue = newQ;
			world.commandQueueSpare = newQS;
			world.commandCapacity = newCap;
		}

		world.commandQueue[world.commandCount++] = {.type = CommandType::DestroyBody,
													.handle = handle};
	}
}

void RegisterMaterial(PhysicsContext& ctx, uint32_t id, float friction, float restitution) {
	auto& world = ctx.GetImpl()->world;
	ZHLN_LOCK(world.sync.shadowLock) {
		// 1. Update if exists
		for (size_t i = 0; i < world.materialCount; ++i) {
			if (world.materials[i].id == id) {
				world.materials[i].friction = friction;
				world.materials[i].restitution = restitution;
				return;
			}
		}

		// 2. Grow if needed
		if (world.materialCount >= world.materialCapacity) {
			size_t oldCap = world.materialCapacity;
			size_t newCap = oldCap * 2;

			auto* newMaterials = new MaterialData[newCap]();
			if ((world.materials != nullptr) && oldCap > 0) {
				std::memcpy(newMaterials, world.materials, oldCap * sizeof(MaterialData));
				delete[] world.materials;
			}
			world.materials = newMaterials;
			world.materialCapacity = newCap;
		}

		// 3. Insert new
		world.materials[world.materialCount++] = {
			.id = id, .friction = friction, .restitution = restitution};
	}
}

/**
 * @brief Resolves physics parameters from the registry.
 * This is a 'Cold' lookup used only during CreateRigidBody.
 */
static MaterialData ResolveMaterial(const PhysicsWorld& world, uint32_t id) {
	constexpr auto materialDefault = MaterialData{.id = 0, .friction = 0.2f, .restitution = 0.0f};
	if (id == 0) {
		return materialDefault; // Default
	}

	for (size_t i = 0; i < world.materialCount; ++i) {
		if (world.materials[i].id == id) {
			return world.materials[i];
		}
	}
	return materialDefault; // Fallback
}

/**
 * @brief Generalized Rigid Body creation.
 */
ZHLN::Entity CreateRigidBody(PhysicsContext& ctx, JPH::ShapeRefC shape, JPH::RVec3Arg pos,
							 JPH::QuatArg rot, JPH::EMotionType motion, JPH::ObjectLayer layer,
							 uint32_t materialID, uint32_t category, uint32_t mask) {
	auto& world = ctx.GetImpl()->world;
	MaterialData mat{};

	// Engine allocates the identity!
	ZHLN::Entity handle = world.AllocateHandle();
	ZHLN_LOCK(world.sync.shadowLock) {
		mat = ResolveMaterial(world, materialID);
		JPH::BodyCreationSettings settings(shape, pos, rot, motion, layer);
		settings.mUserData = handle.Pack();
		settings.mFriction = mat.friction;
		settings.mRestitution = mat.restitution;

		if (motion == JPH::EMotionType::Dynamic) {
			settings.mAllowSleeping = true;
		}

		JPH::BodyID id = world.bodyInterface->CreateAndAddBody(
			settings, (motion == JPH::EMotionType::Static) ? JPH::EActivation::DontActivate
														   : JPH::EActivation::Activate);

		// Map into the Dense SoA
		auto dense = static_cast<uint32_t>(world.count.fetch_add(1, std::memory_order_relaxed));
		world.bodyIDs[dense] = id;
		world.slotToDense[handle.index] = dense;
		world.denseToSlot[dense] = handle.index;
		world.slotStates[handle.index].store(SLOT_ALIVE, std::memory_order_release);

		// Update Fast-Mapping for callbacks
		const uint32_t j_idx = id.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;
		world.idToHandleMap[j_idx].store(handle.Pack(), std::memory_order_release);

		// Warm up shadow buffers so the very first render frame is correct
		world.positions[dense * 4 + 0] = pos.GetX();
		world.positions[dense * 4 + 1] = pos.GetY();
		world.positions[dense * 4 + 2] = pos.GetZ();
		world.positions[dense * 4 + 3] = 0.0;

		world.rotations[dense * 4 + 0] = rot.GetX();
		world.rotations[dense * 4 + 1] = rot.GetY();
		world.rotations[dense * 4 + 2] = rot.GetZ();
		world.rotations[dense * 4 + 3] = rot.GetW();

		// Store Material ID in the SoA for fast callback lookup
		world.materialIDs[dense] = materialID;
		world.categories[dense] = category;
		world.masks[dense] = mask;
	}
	return handle;
}

JPH::ShapeRefC CreateMeshShape(const Vertex* vertices, uint32_t vertexCount,
							   const uint32_t* indices, uint32_t indexCount) {
	if (vertexCount < 3 || indexCount < 3 || (indexCount % 3) != 0) {
		ZHLN::Log("WARNING: Cannot create MeshShape. Invalid bounds (vertices: {}, indices: {})",
				  vertexCount, indexCount);
		return nullptr;
	}

	// 1. Pack vertices into Jolt's VertexList (Array<Float3>)
	JPH::VertexList joltVertices;
	joltVertices.reserve(vertexCount);
	for (uint32_t i = 0; i < vertexCount; ++i) {
		joltVertices.push_back(
			JPH::Float3(vertices[i].position[0], vertices[i].position[1], vertices[i].position[2]));
	}

	// 2. Validate indices and pack into Jolt's IndexedTriangleList (Array<IndexedTriangle>) [6]
	JPH::IndexedTriangleList joltTriangles;
	joltTriangles.reserve(indexCount / 3);
	for (uint32_t i = 0; i < indexCount; i += 3) {
		uint32_t i1 = indices[i + 0];
		uint32_t i2 = indices[i + 1];
		uint32_t i3 = indices[i + 2];

		// Translated C23 Index Boundary Validation Guard [6]
		if (i1 >= vertexCount || i2 >= vertexCount || i3 >= vertexCount) [[unlikely]] {
			ZHLN::Panic("[Jolt Mesh Build] Index out of range: %u/%u/%u >= vertex_count (%u)", i1,
						i2, i3, vertexCount);
		}

		// Create native IndexedTriangle (materialIndex=0, userData=0)
		joltTriangles.push_back(JPH::IndexedTriangle(i1, i2, i3, 0, 0));
	}

	// 3. Build the BVH Accelerated Static Mesh Shape
	JPH::MeshShapeSettings settings(std::move(joltVertices), std::move(joltTriangles));
	JPH::Shape::ShapeResult result = settings.Create();

	if (result.HasError()) {
		ZHLN::Log("ERROR: Jolt Mesh BVH compilation failed: {}", result.GetError().c_str());
		return nullptr;
	}

	return result.Get(); // Returns safe JPH::ShapeRefC smart pointer [6]
}

ZHLN::Entity CreateMeshBody(PhysicsContext& ctx, const Vertex* vertices, uint32_t vertexCount,
							const uint32_t* indices, uint32_t indexCount, JPH::RVec3Arg pos,
							JPH::QuatArg rot, uint32_t category, uint32_t mask) {
	JPH::ShapeRefC shape = CreateMeshShape(vertices, vertexCount, indices, indexCount);
	if (shape == nullptr) {
		return ZHLN::NullEntity;
	}

	// Mesh shapes are strictly static in Jolt; spawn as a standard static rigid body [6]
	return CreateRigidBody(ctx, shape, pos, rot, JPH::EMotionType::Static, Layers::NON_MOVING, 0,
						   category, mask);
}

ZHLN::Entity CreateCharacter(PhysicsContext& ctx, JPH::RVec3Arg position, uint32_t category,
							 uint32_t mask) {
	auto* impl = ctx.GetImpl();
	auto& world = impl->world;
	// Using the cache for the character shape
	JPH::ShapeRefC charShape = GetOrCreateShape(ctx, ShapeType::Capsule, 0.5f, 0.3f);

	ZHLN::Entity handle = world.AllocateHandle();
	ZHLN_LOCK(world.sync.shadowLock) {
		JPH::CharacterVirtualSettings settings;
		settings.mShape = charShape;
		settings.mMaxStrength = 100.0f;

		auto character = new JPH::CharacterVirtual(&settings, position, JPH::Quat::sIdentity(),
												   &impl->physicsSystem);
		character->SetListener(&impl->characterListener);
		character->SetUserData(handle.Pack());

		// Character Management
		if (handle.index >= impl->characterMap.size()) {
			impl->characterMap.resize(handle.index + 1);
		}
		impl->characterMap[handle.index] = character;
		impl->activeCharacters.push_back(character);

		// SoA Sync
		auto dense = static_cast<uint32_t>(world.count.fetch_add(1, std::memory_order_relaxed));
		world.slotToDense[handle.index] = dense;
		world.denseToSlot[dense] = handle.index;
		world.bodyIDs[dense] = JPH::BodyID(); // Characters don't have Jolt BodyIDs
		world.slotStates[handle.index].store(SLOT_CHARACTER, std::memory_order_release);
		world.categories[dense] = category;
		world.masks[dense] = mask;
	}
	return handle;
}

void SetCollisionFilter(PhysicsContext& ctx, ZHLN::Entity handle, uint32_t category,
						uint32_t mask) {
	auto& world = ctx.GetImpl()->world;
	ZHLN_LOCK(world.sync.shadowLock) {
		Command cmd{};
		cmd.type = CommandType::SetCollisionFilter;
		cmd.setFilter.handle = handle;
		cmd.setFilter.category = category;
		cmd.setFilter.mask = mask;
		world.commandQueue[world.commandCount++] = cmd;
	}
}

DebugDrawData GetDebugDrawData(PhysicsContext& ctx, bool drawShapes, bool drawConstraints) {
	auto* impl = ctx.GetImpl();
	auto& world = impl->world;

	impl->debugRenderer->Clear();

	JPH::BodyManager::DrawSettings settings;
	settings.mDrawShape = drawShapes;
	settings.mDrawBoundingBox = false;
	settings.mDrawCenterOfMassTransform = false;

	if (drawShapes) {
		impl->physicsSystem.DrawBodies(settings, impl->debugRenderer.get());
	}
	if (drawConstraints) {
		impl->physicsSystem.DrawConstraints(impl->debugRenderer.get());
	}

	return {.lines = reinterpret_cast<const DebugVertex*>(impl->debugRenderer->lines.data()),
			.lineCount = impl->debugRenderer->lines.size(),
			.triangles =
				reinterpret_cast<const DebugVertex*>(impl->debugRenderer->triangles.data()),
			.triangleCount = impl->debugRenderer->triangles.size()};
}

void SetCharacterVelocity(PhysicsContext& ctx, ZHLN::Entity handle, JPH::Vec3Arg velocity) {
	auto* impl = ctx.GetImpl();

	// Generational Safety Check
	if (handle.index < impl->characterMap.size()) {
		auto& character = impl->characterMap[handle.index];
		if ((character != nullptr) &&
			ZHLN::Entity::Unpack(character->GetUserData()).generation == handle.generation) {
			character->SetLinearVelocity(velocity);
		}
	}
}

void SetLinearVelocity(PhysicsContext& ctx, ZHLN::Entity handle, JPH::Vec3Arg velocity) {
	auto* impl = ctx.GetImpl();
	JPH::BodyID id = impl->world.GetBodyID(handle);
	if (!id.IsInvalid()) {
		impl->world.bodyInterface->SetLinearVelocity(id, velocity);
	}
}

JPH::Vec3 GetCharacterVelocity(const PhysicsContext& ctx, ZHLN::Entity handle) {
	auto* impl = ctx.GetImpl();
	if (handle.index < impl->characterMap.size()) {
		auto& character = impl->characterMap[handle.index];
		if (character != nullptr) {
			return character->GetLinearVelocity();
		}
	}
	return JPH::Vec3::sZero();
}

bool IsCharacterOnGround(const PhysicsContext& ctx, ZHLN::Entity handle) {
	auto* impl = ctx.GetImpl();
	if (handle.index < impl->characterMap.size()) {
		auto& character = impl->characterMap[handle.index];
		if (character != nullptr) {
			// Jolt returns "Supported" if we are standing on something
			return character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
		}
	}
	return false;
}

auto GetPositionBuffer(const PhysicsContext& ctx) -> ZHLN::BufferView {
	const auto& world = ctx.GetWorld();
	BufferView view;
	view.buf = world.positions;
	view.itemsize = sizeof(JPH::Real);
	// Stride is 4 * itemsize because our SoA is packed [x,y,z,w]
	view.strides[0] = sizeof(JPH::Real) * 4;
	view.shape[0] = world.count.load();
	view.ndim = 1;
	view.format[0] = (sizeof(JPH::Real) == 8) ? 'd' : 'f';
	return view;
}

JPH::Quat GetRotation(const PhysicsContext& ctx, JPH::BodyID bodyID) {
	return ctx.GetImpl()->world.bodyInterface->GetRotation(bodyID);
}

ZHLN::Entity GetEntityHandle(const PhysicsContext& ctx, JPH::BodyID bodyID) {
	uint64_t rawData = ctx.GetImpl()->world.bodyInterface->GetUserData(bodyID);
	return ZHLN::Entity::Unpack(rawData);
}

std::pair<const ContactEvent*, size_t> GetContactEvents(const PhysicsContext& ctx) {
	const auto& world = ctx.GetImpl()->world;

	// Clamp the count to capacity in case the buffer overflowed
	size_t count = world.contactCount.load(std::memory_order_acquire);
	count = std::min(count, world.contactCapacity);

	return {world.contactBuffer, count};
}

ConstraintHandle CreateConstraint(PhysicsContext& ctx, ConstraintType type, ZHLN::Entity b1,
								  ZHLN::Entity b2, const ConstraintParams& params) {
	auto& world = ctx.GetImpl()->world;

	// 1. Allocate handle immediately so we can return it
	ConstraintHandle handle = world.AllocateConstraintHandle();

	ZHLN_LOCK(world.sync.shadowLock) { // 2. Queue Command
		Command cmd{};
		cmd.type = CommandType::CreateConstraint;
		cmd.cHandle = handle;
		cmd.createC.cType = type;
		cmd.createC.b1 = b1;
		cmd.createC.b2 = b2;
		cmd.createC.params = params;

		world.commandQueue[world.commandCount++] = cmd;
	}

	return handle;
}

void SetConstraintTarget(PhysicsContext& ctx, ConstraintHandle handle, float value) {
	auto& world = ctx.GetImpl()->world;
	ZHLN_LOCK(world.sync.shadowLock) {
		// Motors are often updated every frame, so we use a specialized command
		Command cmd{};
		cmd.type = CommandType::SetConstraintTarget;
		cmd.setTarget.targetCHandle = handle;
		cmd.setTarget.targetValue = value;
		world.commandQueue[world.commandCount++] = cmd;
	}
}

void AddImpulse(PhysicsContext& ctx, ZHLN::Entity handle, JPH::Vec3Arg impulse) {
	auto* impl = ctx.GetImpl();
	JPH::BodyID id = impl->world.GetBodyID(handle);

	if (!id.IsInvalid()) {
		// 1. Apply the force
		impl->world.bodyInterface->AddImpulse(id, impulse);

		// 2. IMPORTANT: Wake the body up!
		// Without this, impulses on stationary (sleeping) objects do nothing.
		impl->world.bodyInterface->ActivateBody(id);
	}
}

JPH::ShapeRefC CreateHeightFieldShape(const std::vector<float>& heights, int sampleCount,
									  float worldSize) {
	JPH::HeightFieldShapeSettings settings;
	settings.mSampleCount = sampleCount;
	settings.mHeightSamples.resize(static_cast<size_t>(sampleCount) * sampleCount);

	for (int i = 0; i < sampleCount * sampleCount; ++i) {
		settings.mHeightSamples[i] = heights[i];
	}

	// Offset to center the terrain over the origin (0, 0, 0)
	settings.mOffset = JPH::Vec3(-worldSize / 2.0f, 0.0f, -worldSize / 2.0f);

	// Map scale to stretch the local samples grid into world units
	settings.mScale = JPH::Vec3(worldSize / (sampleCount - 1), 1.0f, worldSize / (sampleCount - 1));

	JPH::Shape::ShapeResult result = settings.Create();
	if (result.HasError()) {
		ZHLN::Panic("Failed to build Jolt HeightFieldShape: {}", result.GetError().c_str());
	}
	return result.Get();
}

} // namespace Physics
} // namespace ZHLN
