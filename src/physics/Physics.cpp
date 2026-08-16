// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsContactEvents.hpp"
#include "PhysicsDebug.hpp"
#include "PhysicsWorld.hpp"
#include "Zahlen/Profiler.hpp"
#include <Jolt/Core/Color.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/TempAllocator.h>
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/MotorSettings.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Zahlen/Buffer.h>
#include <Zahlen/Core/ControlFlow.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <alloca.h>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

namespace ZHLN {

struct ShapeKey {
    uint32_t type;
    float    p1, p2, p3, p4;
};

struct ShapeEntry {
    ShapeKey       key;
    JPH::ShapeRefC shape;
};

enum SlotState : uint8_t { SLOT_EMPTY = 0, SLOT_ALIVE = 1, SLOT_CHARACTER = 2 };

// =================================================================================================
// MEMORY UTILITIES
// =================================================================================================
namespace {
template <typename T>
[[nodiscard]] auto AllocateAligned(size_t count, size_t alignment) -> T* {
    return static_cast<T*>(::operator new[](count * sizeof(T), std::align_val_t {alignment}));
}

template <typename T>
void DeallocateAligned(T* ptr, size_t alignment) {
    if (ptr) {
        ::operator delete[](ptr, std::align_val_t {alignment});
    }
}

template <typename T>
void ReallocateAligned(T*& ptr, size_t old_count, size_t new_count, size_t alignment) {
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
} // namespace

// --- Jolt Boilerplate: Layers & Filters ---

class BPLayerInterfaceImpl final: public JPH::BroadPhaseLayerInterface {
    JPH::BroadPhaseLayer mObjectToBroadPhase[Layers::NUM_LAYERS] {};

  public:
    BPLayerInterfaceImpl() {
        mObjectToBroadPhase[Layers::NON_MOVING] = JPH::BroadPhaseLayer(BroadPhaseLayers::NON_MOVING);
        mObjectToBroadPhase[Layers::MOVING]     = JPH::BroadPhaseLayer(BroadPhaseLayers::MOVING);
    }
    [[nodiscard]] auto GetNumBroadPhaseLayers() const -> uint32_t override {
        return BroadPhaseLayers::NUM_LAYERS;
    }

    [[nodiscard]] auto GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const -> JPH::BroadPhaseLayer override {
        return mObjectToBroadPhase[inLayer];
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    [[nodiscard]] auto GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const -> const char* override {
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

class ObjectVsBroadPhaseLayerFilterImpl: public JPH::ObjectVsBroadPhaseLayerFilter {
  public:
    [[nodiscard]] auto ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const -> bool override {
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

class ObjectLayerPairFilterImpl: public JPH::ObjectLayerPairFilter {
  public:
    [[nodiscard]] auto ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const -> bool override {
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

namespace {

class JobSystemFiber final: public JPH::JobSystemWithBarrier {
  public:
    JPH_OVERRIDE_NEW_DELETE

    static void JoltJobThunk(void* arg) {
        auto* job = static_cast<JPH::JobSystem::Job*>(arg);
        job->Execute();
        job->Release();
    }

    JobSystemFiber(uint inMaxJobs, uint inMaxBarriers): JPH::JobSystemWithBarrier(inMaxBarriers) {
        mJobs.Init(inMaxJobs, inMaxJobs);
    }

    ~JobSystemFiber() override = default;

    [[nodiscard]] auto GetMaxConcurrency() const -> int override {
        return static_cast<int>(ZHLN::TaskSystem::GetWorkerCount());
    }

    auto CreateJob(const char* inName, JPH::ColorArg inColor, const JPH::JobSystem::JobFunction& inJobFunction, JPH::uint32 inNumDependencies = 0)
        -> JPH::JobSystem::JobHandle override {
        JPH::uint32 index = mJobs.ConstructObject(inName, inColor, this, inJobFunction, inNumDependencies);
        if (index == JPH::FixedSizeFreeList<JPH::JobSystem::Job>::cInvalidObjectIndex) {
            ZHLN::Panic("Jolt: JobSystemFiber exceeded the maximum number of jobs!");
        }
        return JPH::JobSystem::JobHandle(&mJobs.Get(index));
    }

  protected:
    void QueueJob(JPH::JobSystem::Job* inJob) override {
        inJob->AddRef();

        ZHLN::TaskSystem::Task task = {.func = JoltJobThunk, .arg = inJob};
        ZHLN::TaskSystem::Dispatch(std::span<const ZHLN::TaskSystem::Task>(&task, 1), nullptr);
    }

    void QueueJobs(JPH::JobSystem::Job** inJobs, uint inNumJobs) override {
        if (inNumJobs == 0) {
            return;
        }

        auto* tasks = static_cast<ZHLN::TaskSystem::Task*>(alloca(inNumJobs * sizeof(ZHLN::TaskSystem::Task)));

        for (uint i = 0; i < inNumJobs; ++i) {
            inJobs[i]->AddRef();
            tasks[i] = {.func = JoltJobThunk, .arg = inJobs[i]};
        }

        ZHLN::TaskSystem::Dispatch(std::span<const ZHLN::TaskSystem::Task>(tasks, inNumJobs), nullptr);
    }

    void FreeJob(JPH::JobSystem::Job* inJob) override {
        mJobs.DestructObject(inJob);
    }

  private:
    using AvailableJobs = JPH::FixedSizeFreeList<JPH::JobSystem::Job>;
    AvailableJobs mJobs;
};
} // namespace

// =================================================================================================
// CONTEXT IMPLEMENTATION
// =================================================================================================

struct PhysicsContext::Impl {
    JPH::PhysicsSystem                      physicsSystem;
    std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
    JobSystemFiber                          jobSystem {JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers};

    BPLayerInterfaceImpl              bpLayerInterface;
    ObjectVsBroadPhaseLayerFilterImpl objVsBpFilter;
    ObjectLayerPairFilterImpl         objVsObjFilter;

    Physics::PhysicsWorld world {};

    Physics::ContactListener contactListener {&world};

    std::unique_ptr<Physics::PhysicsDebugRenderer> debugRenderer;

    JPH::Array<JPH::Ref<JPH::CharacterVirtual>> characterMap;
    JPH::Array<JPH::CharacterVirtual*>          activeCharacters;

    Physics::CharacterListener characterListener {&world};
    JPH::Array<ShapeEntry>     shapeCache;
};

PhysicsContext::PhysicsContext(const PhysicsConfig& cfg): _impl(std::make_unique<Impl>()) {
    _impl->tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(cfg.tempAllocatorSize);

    _impl->physicsSystem.SetContactListener(&_impl->contactListener);
    _impl->physicsSystem.Init(
        cfg.maxBodies, 0, cfg.maxBodyPairs, cfg.maxContactConstraints, _impl->bpLayerInterface, _impl->objVsBpFilter, _impl->objVsObjFilter
    );

    _impl->debugRenderer     = std::make_unique<Physics::PhysicsDebugRenderer>();
    _impl->characterListener = Physics::CharacterListener(&_impl->world);

    _impl->world.Init(cfg.maxBodies, &_impl->physicsSystem, &_impl->jobSystem, _impl->tempAllocator.get());
}

PhysicsContext::~PhysicsContext() {
    _impl->world.Shutdown();
}

void PhysicsContext::Step(float deltaTime) {
    ZHLN::ScopedTimer profTimer("Physics (JPH)");
    auto&             world = _impl->world;

    size_t capturedCount = 0;

    ZHLN::Lock(world.sync.shadowLock, [&] -> void {
        capturedCount = world.commandCount;
        if (capturedCount > 0) {
            world.commandQueue.swap(world.commandQueueSpare);
            world.commandCount = 0;
        }
    });

    if (capturedCount > 0) {
        world.FlushCommands(world.commandQueueSpare.data(), capturedCount);
    }

    world.contactCount.store(0, std::memory_order::relaxed);
    world.isStepping.store(true, std::memory_order::release);

    _impl->physicsSystem.Update(deltaTime, 2, _impl->tempAllocator.get(), &_impl->jobSystem);

    for (auto* character: _impl->activeCharacters) {
        // Disable floor-sticking when character has upward velocity (jumping)
        JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings {
            .mStickToFloorStepDown = (character->GetLinearVelocity().GetY() <= 0.01f) ? JPH::Vec3(0.0f, -0.25f, 0.0f) : JPH::Vec3::sZero(),
            .mWalkStairsStepUp     = JPH::Vec3(0.0f, 0.40f, 0.0f),
        };

        character->ExtendedUpdate(
            deltaTime, _impl->physicsSystem.GetGravity(), updateSettings, _impl->physicsSystem.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
            _impl->physicsSystem.GetDefaultLayerFilter(Layers::MOVING), {}, {}, *_impl->tempAllocator
        );
    }

    ZHLN::Lock(world.sync.shadowLock, [&] -> void { world.Synchronize(&_impl->physicsSystem, _impl->activeCharacters); });
    world.isStepping.store(false, std::memory_order::release);
}

auto PhysicsContext::GetWorld() const -> const Physics::PhysicsWorld& {
    return _impl->world;
}

auto PhysicsContext::GetActiveBodyCount() const -> uint32_t {
    return _impl->physicsSystem.GetNumActiveBodies(JPH::EBodyType::RigidBody);
}

auto PhysicsContext::GetMemoryUsage() const -> size_t {
    return _impl->tempAllocator->GetSize();
}

void PhysicsContext::OptimizeBroadphase() {
    _impl->physicsSystem.OptimizeBroadPhase();
}

auto PhysicsContext::GetOrCreateShape(Physics::ShapeType type, float p1, float p2, float p3, float p4) -> JPH::ShapeRefC {
    auto* impl = _impl.get();

    const float np1 = (p1 < 1e-3f && type != Physics::ShapeType::Plane) ? 1e-3f : p1;
    const float np2 = (p2 < 1e-3f && type != Physics::ShapeType::Plane) ? 1e-3f : p2;
    const float np3 = (p3 < 1e-3f && type != Physics::ShapeType::Plane) ? 1e-3f : p3;
    const float np4 = (type == Physics::ShapeType::Plane) ? p4 : 0.0f;

    for (const auto& entry: impl->shapeCache) {
        if (entry.key.type == static_cast<uint32_t>(type) && entry.key.p1 == np1 && entry.key.p2 == np2 && entry.key.p3 == np3 && entry.key.p4 == np4) {
            return entry.shape;
        }
    }

    // Lock section returns the created shape directly into a const variable
    return ZHLN::Lock(impl->world.sync.shadowLock, [&]() -> JPH::ShapeRefC {
        JPH::ShapeRefC newShape;

        switch (type) {
            case Physics::ShapeType::Box: {
                JPH::BoxShapeSettings s(JPH::Vec3(np1, np2, np3), 0.05f);
                newShape = s.Create().Get();
                break;
            }
            case Physics::ShapeType::Sphere: {
                JPH::SphereShapeSettings s(np1);
                newShape = s.Create().Get();
                break;
            }
            case Physics::ShapeType::Capsule: {
                JPH::CapsuleShapeSettings s(np1, np2);
                newShape = s.Create().Get();
                break;
            }
            case Physics::ShapeType::Cylinder: {
                JPH::CylinderShapeSettings s(np1, np2, 0.05f);
                newShape = s.Create().Get();
                break;
            }
            case Physics::ShapeType::Plane: {
                JPH::Plane              plane(JPH::Vec3(np1, np2, np3), np4);
                JPH::PlaneShapeSettings s(plane, nullptr, 1000.0f);
                newShape = s.Create().Get();
                break;
            }
        }

        if (newShape == nullptr) {
            ZHLN::Log("Failed to create Jolt Shape! Degenerate parameters?");
            return nullptr;
        }

        impl->shapeCache.push_back({.key = ShapeKey {.type = static_cast<uint32_t>(type), .p1 = np1, .p2 = np2, .p3 = np3, .p4 = np4}, .shape = newShape});

        return newShape;
    });
}

static auto ResolveMaterial(const Physics::PhysicsWorld& world, uint32_t id) -> Physics::MaterialData {
    constexpr auto materialDefault = Physics::MaterialData {.id = 0, .friction = 0.2f, .restitution = 0.0f};
    if (id == 0) {
        return materialDefault;
    }

    for (size_t i = 0; i < world.materialCount; ++i) {
        if (world.materials[i].id == id) {
            return world.materials[i];
        }
    }
    return materialDefault;
}

auto PhysicsContext::CreateRigidBody(
    const JPH::ShapeRefC& shape,
    JPH::RVec3Arg         pos,
    JPH::QuatArg          rot,
    JPH::EMotionType      motion,
    JPH::ObjectLayer      layer,
    uint32_t              materialID,
    uint32_t              category,
    uint32_t              mask
) -> ZHLN::Entity {
    auto&                 world = _impl->world;
    Physics::MaterialData mat {};

    ZHLN::Entity handle = world.AllocateHandle();
    ZHLN::Lock(world.sync.shadowLock, [&] -> void {
        mat = ResolveMaterial(world, materialID);
        JPH::BodyCreationSettings settings(shape, pos, rot, motion, layer);
        settings.mUserData    = handle.Pack();
        settings.mFriction    = mat.friction;
        settings.mRestitution = mat.restitution;

        if (motion == JPH::EMotionType::Dynamic) {
            settings.mAllowSleeping = true;
        }

        JPH::BodyID id =
            world.bodyInterface->CreateAndAddBody(settings, (motion == JPH::EMotionType::Static) ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);

        auto dense                      = static_cast<uint32_t>(world.count.fetch_add(1, std::memory_order::relaxed));
        world.bodyIDs[dense]            = id;
        world.slotToDense[handle.index] = dense;
        world.denseToSlot[dense]        = handle.index;
        world.slotStates[handle.index].store(SLOT_ALIVE, std::memory_order::release);

        const uint32_t j_idx = id.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;
        world.idToHandleMap[j_idx].store(handle.Pack(), std::memory_order::release);

        world.positions[dense * 4 + 0] = pos.GetX();
        world.positions[dense * 4 + 1] = pos.GetY();
        world.positions[dense * 4 + 2] = pos.GetZ();
        world.positions[dense * 4 + 3] = 0.0;

        world.prevPositions[dense * 4 + 0] = pos.GetX();
        world.prevPositions[dense * 4 + 1] = pos.GetY();
        world.prevPositions[dense * 4 + 2] = pos.GetZ();
        world.prevPositions[dense * 4 + 3] = 0.0;

        world.rotations[dense * 4 + 0] = rot.GetX();
        world.rotations[dense * 4 + 1] = rot.GetY();
        world.rotations[dense * 4 + 2] = rot.GetZ();
        world.rotations[dense * 4 + 3] = rot.GetW();

        world.prevRotations[dense * 4 + 0] = rot.GetX();
        world.prevRotations[dense * 4 + 1] = rot.GetY();
        world.prevRotations[dense * 4 + 2] = rot.GetZ();
        world.prevRotations[dense * 4 + 3] = rot.GetW();

        world.materialIDs[dense] = materialID;
        world.categories[dense]  = category;
        world.masks[dense]       = mask;
    });
    return handle;
}

namespace Physics {

// ADDED: Missing GetBodyID implementation that operates on the core struct
auto GetBodyID(const PhysicsWorld& world, ZHLN::Entity handle) -> JPH::BodyID {
    if (handle.index >= world.slotCapacity) {
        return {};
    }

    if (world.generations[handle.index].load(std::memory_order::acquire) != handle.generation) {
        return {};
    }

    uint32_t dense = world.slotToDense[handle.index];
    return world.bodyIDs[dense];
}

auto CreateMeshShape(const VertexPosition* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount) -> JPH::ShapeRefC {
    if (vertexCount < 3 || indexCount < 3 || (indexCount % 3) != 0) {
        return nullptr;
    }

    JPH::VertexList joltVertices;
    joltVertices.reserve(vertexCount);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        joltVertices.push_back(JPH::Float3(vertices[i].position[0], vertices[i].position[1], vertices[i].position[2]));
    }

    JPH::IndexedTriangleList joltTriangles;
    joltTriangles.reserve(indexCount / 3);
    for (uint32_t i = 0; i < indexCount; i += 3) {
        uint32_t i1 = indices[i + 0];
        uint32_t i2 = indices[i + 1];
        uint32_t i3 = indices[i + 2];

        if (i1 == i2 || i2 == i3 || i1 == i3) {
            continue;
        }

        joltTriangles.push_back(JPH::IndexedTriangle(i1, i2, i3, 0, 0));
    }

    if (joltTriangles.empty()) {
        return nullptr;
    }

    JPH::MeshShapeSettings  settings(std::move(joltVertices), std::move(joltTriangles));
    JPH::Shape::ShapeResult result = settings.Create();
    return result.HasError() ? nullptr : result.Get();
}

auto CreateHeightFieldShape(const float* heights, int sampleCount, float worldSize) -> JPH::ShapeRefC {
    JPH::HeightFieldShapeSettings settings;
    settings.mSampleCount = sampleCount;
    settings.mHeightSamples.resize(static_cast<size_t>(sampleCount) * sampleCount);

    for (int i = 0; i < sampleCount * sampleCount; ++i) {
        settings.mHeightSamples[i] = heights[i];
    }

    settings.mOffset = JPH::Vec3(-worldSize / 2.0f, 0.0f, -worldSize / 2.0f);
    settings.mScale  = JPH::Vec3(worldSize / (sampleCount - 1), 1.0f, worldSize / (sampleCount - 1));

    JPH::Shape::ShapeResult result = settings.Create();
    return result.HasError() ? nullptr : result.Get();
}

auto CreateDualShape(const DualShapeConfig& config) -> JPH::ShapeRefC {
    JPH::StaticCompoundShapeSettings compound;

    // 1. Lower Lifter Sphere (Bottom touches Y = 0.0m, Center at Y = R_L)
    JPH::ShapeRefC lifterShape = new JPH::SphereShape(config.lifterRadius);
    compound.AddShape(JPH::Vec3(0.0f, config.GetLifterOffsetY(), 0.0f), JPH::Quat::sIdentity(), lifterShape);

    // 2. Upper Bumper Oval / Spheroid Capsule (Centered at Y_B with exact equator cut)
    const float    cylinderHalfHeight = std::max(0.001f, config.bumperRadiusY - config.bumperRadiusXZ);
    JPH::ShapeRefC bumperShape        = (config.bumperRadiusY > config.bumperRadiusXZ) ?
                                            static_cast<JPH::ShapeRefC>(new JPH::CapsuleShape(cylinderHalfHeight, config.bumperRadiusXZ)) :
                                            static_cast<JPH::ShapeRefC>(new JPH::SphereShape(config.bumperRadiusXZ));

    compound.AddShape(JPH::Vec3(0.0f, config.GetBumperOffsetY(), 0.0f), JPH::Quat::sIdentity(), bumperShape);

    auto res = compound.Create();
    return res.HasError() ? nullptr : res.Get();
}

} // namespace Physics

auto PhysicsContext::CreateMeshBody(
    const VertexPosition* vertices,
    uint32_t              vertexCount,
    const uint32_t*       indices,
    uint32_t              indexCount,
    JPH::RVec3Arg         pos,
    JPH::QuatArg          rot,
    uint32_t              category,
    uint32_t              mask
) -> ZHLN::Entity {
    JPH::ShapeRefC shape = Physics::CreateMeshShape(vertices, vertexCount, indices, indexCount);
    if (shape == nullptr) {
        return ZHLN::NullEntity;
    }
    return CreateRigidBody(shape, pos, rot, JPH::EMotionType::Static, Layers::NON_MOVING, 0, category, mask);
}

auto PhysicsContext::CreateCharacter(JPH::RVec3Arg position, const Physics::DualShapeConfig& config, uint32_t category, uint32_t mask) -> ZHLN::Entity {
    auto* impl  = _impl.get();
    auto& world = impl->world;

    JPH::ShapeRefC charShape = Physics::CreateDualShape(config);
    if (charShape == nullptr) {
        charShape = GetOrCreateShape(Physics::ShapeType::Capsule, 0.5f, 0.3f);
    }

    ZHLN::Entity handle = world.AllocateHandle();
    ZHLN::Lock(world.sync.shadowLock, [&] -> void {
        JPH::CharacterVirtualSettings settings;
        settings.mShape                       = charShape;
        settings.mMaxSlopeAngle               = JPH::DegreesToRadians(45.0f);
        settings.mMaxStrength                 = 100.0f;
        settings.mBackFaceMode                = JPH::EBackFaceMode::CollideWithBackFaces;
        settings.mCharacterPadding            = 0.02f;
        settings.mPenetrationRecoverySpeed    = 1.0f;
        settings.mEnhancedInternalEdgeRemoval = true;
        // Accept ground contacts across the entire lower lifter sphere (y <= lifterRadius)
        settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -config.lifterRadius);

        auto* character = new JPH::CharacterVirtual(&settings, position, JPH::Quat::sIdentity(), &impl->physicsSystem);
        character->SetListener(&impl->characterListener);
        character->SetUserData(handle.Pack());

        if (handle.index >= impl->characterMap.size()) {
            impl->characterMap.resize(handle.index + 1);
        }
        impl->characterMap[handle.index] = character;
        impl->activeCharacters.push_back(character);

        auto dense                      = static_cast<uint32_t>(world.count.fetch_add(1, std::memory_order::relaxed));
        world.bodyIDs[dense]            = JPH::BodyID();
        world.slotToDense[handle.index] = dense;
        world.denseToSlot[dense]        = handle.index;
        world.slotStates[handle.index].store(SLOT_CHARACTER, std::memory_order::release);
        world.categories[dense] = category;
        world.masks[dense]      = mask;

        world.positions[dense * 4 + 0] = position.GetX();
        world.positions[dense * 4 + 1] = position.GetY();
        world.positions[dense * 4 + 2] = position.GetZ();
        world.positions[dense * 4 + 3] = 0.0;

        world.prevPositions[dense * 4 + 0] = position.GetX();
        world.prevPositions[dense * 4 + 1] = position.GetY();
        world.prevPositions[dense * 4 + 2] = position.GetZ();
        world.prevPositions[dense * 4 + 3] = 0.0;

        world.rotations[dense * 4 + 0] = 0.0f;
        world.rotations[dense * 4 + 1] = 0.0f;
        world.rotations[dense * 4 + 2] = 0.0f;
        world.rotations[dense * 4 + 3] = 1.0f;

        world.prevRotations[dense * 4 + 0] = 0.0f;
        world.prevRotations[dense * 4 + 1] = 0.0f;
        world.prevRotations[dense * 4 + 2] = 0.0f;
        world.prevRotations[dense * 4 + 3] = 1.0f;
    });
    return handle;
}

void PhysicsContext::SetCollisionFilter(ZHLN::Entity handle, uint32_t category, uint32_t mask) {
    auto& world = _impl->world;
    ZHLN::Lock(world.sync.shadowLock, [&] -> void {
        Physics::Command cmd {};
        cmd.type                                 = Physics::CommandType::SetCollisionFilter;
        cmd.setFilter.handle                     = handle;
        cmd.setFilter.category                   = category;
        cmd.setFilter.mask                       = mask;
        world.commandQueue[world.commandCount++] = cmd;
    });
}

auto PhysicsContext::GetDebugDrawData(bool drawShapes, bool drawConstraints, bool wireframe) const -> Physics::DebugDrawData {
    auto* impl = _impl.get();

    impl->debugRenderer->Clear();

    JPH::BodyManager::DrawSettings settings;
    settings.mDrawShape                 = drawShapes;
    settings.mDrawShapeWireframe        = wireframe;
    settings.mDrawBoundingBox           = false;
    settings.mDrawCenterOfMassTransform = false;

    if (drawShapes) {
        impl->physicsSystem.DrawBodies(settings, impl->debugRenderer.get());
    }
    if (drawConstraints) {
        impl->physicsSystem.DrawConstraints(impl->debugRenderer.get());
    }

    return {
        .lines         = reinterpret_cast<const Physics::DebugVertex*>(impl->debugRenderer->lines.data()),
        .lineCount     = impl->debugRenderer->lines.size(),
        .triangles     = reinterpret_cast<const Physics::DebugVertex*>(impl->debugRenderer->triangles.data()),
        .triangleCount = impl->debugRenderer->triangles.size()
    };
}

void PhysicsContext::SetCharacterVelocity(ZHLN::Entity handle, JPH::Vec3Arg velocity) {
    if (handle.index < _impl->characterMap.size()) {
        auto& character = _impl->characterMap[handle.index];
        if ((character != nullptr) && ZHLN::Entity::Unpack(character->GetUserData()).generation == handle.generation) {
            character->SetLinearVelocity(velocity);
        }
    }
}

void PhysicsContext::SetCharacterPosition(ZHLN::Entity handle, JPH::RVec3Arg position) {
    if (handle.index < _impl->characterMap.size()) {
        auto& character = _impl->characterMap[handle.index];
        if ((character != nullptr) && ZHLN::Entity::Unpack(character->GetUserData()).generation == handle.generation) {
            character->SetPosition(position);
        }
    }
}

void PhysicsContext::SetLinearVelocity(ZHLN::Entity handle, JPH::Vec3Arg velocity) {
    // FIXED: Passed _impl->world as first arg
    JPH::BodyID id = Physics::GetBodyID(_impl->world, handle);
    if (!id.IsInvalid()) {
        _impl->world.bodyInterface->SetLinearVelocity(id, velocity);
    }
}

auto PhysicsContext::GetCharacterVelocity(ZHLN::Entity handle) const -> JPH::Vec3 {
    if (handle.index < _impl->characterMap.size()) {
        auto& character = _impl->characterMap[handle.index];
        if (character != nullptr) {
            return character->GetLinearVelocity();
        }
    }
    return JPH::Vec3::sZero();
}

auto PhysicsContext::IsCharacterOnGround(ZHLN::Entity handle) const -> bool {
    if (handle.index < _impl->characterMap.size()) {
        auto& character = _impl->characterMap[handle.index];
        if (character != nullptr) {
            return character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
        }
    }
    return false;
}

auto PhysicsContext::GetPositionBuffer() const -> BufferView {
    const auto& world = GetWorld();
    BufferView  view {};
    view.buf        = world.positions;
    view.itemsize   = sizeof(JPH::Real);
    view.strides[0] = sizeof(JPH::Real) * 4;
    view.shape[0]   = world.count.load();
    view.ndim       = 1;
    view.format[0]  = (sizeof(JPH::Real) == 8) ? 'd' : 'f';
    return view;
}

auto PhysicsContext::GetRotation(JPH::BodyID bodyID) const -> JPH::Quat {
    return _impl->world.bodyInterface->GetRotation(bodyID);
}

auto PhysicsContext::GetEntityHandle(JPH::BodyID bodyID) const -> ZHLN::Entity {
    uint64_t rawData = _impl->world.bodyInterface->GetUserData(bodyID);
    return ZHLN::Entity::Unpack(rawData);
}

void PhysicsContext::DestroyBody(ZHLN::Entity handle) {
    auto&          world = _impl->world;
    const uint32_t slot  = handle.index;

    if (slot >= world.slotCapacity) {
        return;
    }
    if (world.generations[slot].load(std::memory_order::acquire) != handle.generation) {
        return;
    }

    const uint8_t                state = world.slotStates[slot].load(std::memory_order::acquire);
    const Physics::SlotPredicate pred  = Physics::GetSlotPredicate(state);

    if (!pred.isDestructible) {
        return;
    }

    world.slotStates[slot].store(Physics::SLOT_PENDING_DESTROY, std::memory_order::release);

    ZHLN::Lock(world.sync.shadowLock, [&] -> void {
        if (world.commandCount >= world.commandQueue.size()) {
            size_t newCap = world.commandQueue.size() == 0 ? 64 : world.commandQueue.size() * 2;
            world.commandQueue.resize(newCap);
            world.commandQueueSpare.resize(newCap);
        }
        world.commandQueue[world.commandCount++] = {.type = Physics::CommandType::DestroyBody, .handle = handle};
    });
}

void PhysicsContext::RegisterMaterial(uint32_t id, float friction, float restitution) {
    auto& world = _impl->world;
    ZHLN::Lock(world.sync.shadowLock, [&] -> void {
        for (size_t i = 0; i < world.materialCount; ++i) {
            if (world.materials[i].id == id) {
                world.materials[i].friction    = friction;
                world.materials[i].restitution = restitution;
                return;
            }
        }
        if (world.materialCount >= world.materials.size()) {
            size_t newCap = world.materials.size() == 0 ? 16 : world.materials.size() * 2;
            world.materials.resize(newCap);
        }
        world.materials[world.materialCount++] = {.id = id, .friction = friction, .restitution = restitution};
    });
}

void PhysicsContext::AddImpulse(ZHLN::Entity handle, JPH::Vec3Arg impulse) {
    // FIXED: Passed _impl->world to free function
    JPH::BodyID id = Physics::GetBodyID(_impl->world, handle);
    if (!id.IsInvalid()) {
        _impl->world.bodyInterface->AddImpulse(id, impulse);
        _impl->world.bodyInterface->ActivateBody(id);
    }
}

void PhysicsContext::AddImpulse(ZHLN::Entity handle, JPH::Vec3Arg impulse, JPH::RVec3Arg position) {
    // FIXED: Passed _impl->world to free function
    JPH::BodyID id = Physics::GetBodyID(_impl->world, handle);
    if (!id.IsInvalid()) {
        _impl->world.bodyInterface->AddImpulse(id, impulse, position);
        _impl->world.bodyInterface->ActivateBody(id);
    }
}

auto PhysicsContext::GetContactEvents() const -> std::pair<const Physics::ContactEvent*, size_t> {
    const auto& world = _impl->world;

    // Clamp the count to capacity in case the buffer overflowed
    size_t count = world.contactCount.load(std::memory_order::acquire);
    count        = std::min(count, world.contactCapacity);

    return {world.contactBuffer.data(), count};
}

auto PhysicsContext::GetInternalSystem() noexcept -> JPH::PhysicsSystem& {
    return _impl->physicsSystem;
}

auto PhysicsContext::GetInternalSystem() const noexcept -> const JPH::PhysicsSystem& {
    return _impl->physicsSystem;
}

auto PhysicsContext::GetInternalWorld() noexcept -> Physics::PhysicsWorld& {
    return _impl->world;
}

auto PhysicsContext::GetInternalWorld() const noexcept -> const Physics::PhysicsWorld& {
    return _impl->world;
}

} // namespace ZHLN
