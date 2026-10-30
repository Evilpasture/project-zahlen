// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CharacterManager.hpp"
#include "PhysicsContactEvents.hpp"
#include "PhysicsDebug.hpp"
#include "PhysicsWorld.hpp"
#include "RagdollManager.hpp"
#include "ShapeCache.hpp"
#include "Zahlen/Core/Profiler.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Core/Color.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Skeleton/SkeletonPose.h>
#include <Zahlen/Core/Buffer.h>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/Trace.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <utility>

namespace ZHLN {

using JPH::uint;

class BPLayerInterfaceImpl final: public JPH::BroadPhaseLayerInterface {
    static constexpr size_t                             kObjectLayerCount = ZHLN::Reflect::EnumCount<Layers::ID>();
    std::array<JPH::BroadPhaseLayer, kObjectLayerCount> mObjectToBroadPhase {};

  public:
    BPLayerInterfaceImpl() {
        mObjectToBroadPhase[static_cast<size_t>(Layers::ID::NON_MOVING)] = JPH::BroadPhaseLayer(static_cast<uint8_t>(BroadPhaseLayers::ID::NON_MOVING));
        mObjectToBroadPhase[static_cast<size_t>(Layers::ID::MOVING)]     = JPH::BroadPhaseLayer(static_cast<uint8_t>(BroadPhaseLayers::ID::MOVING));
    }
    ~BPLayerInterfaceImpl() override;
    [[nodiscard]] auto GetNumBroadPhaseLayers() const -> uint32_t override {
        return static_cast<uint32_t>(ZHLN::Reflect::EnumCount<BroadPhaseLayers::ID>());
    }

    [[nodiscard]] auto GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const -> JPH::BroadPhaseLayer override {
        const auto index = static_cast<size_t>(inLayer);
        return index < mObjectToBroadPhase.size() ? mObjectToBroadPhase[index] : JPH::BroadPhaseLayer {};
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    [[nodiscard]] auto GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const -> const char* override {
        const auto name = ZHLN::Reflect::EnumToString(static_cast<BroadPhaseLayers::ID>(static_cast<uint8_t>(inLayer)));
        return name.data();
    }
#endif
};

class ObjectVsBroadPhaseLayerFilterImpl final: public JPH::ObjectVsBroadPhaseLayerFilter {
  public:
    ~ObjectVsBroadPhaseLayerFilterImpl() override;
    [[nodiscard]] auto ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const -> bool override {
        switch (static_cast<Layers::ID>(inLayer1)) {
            case Layers::ID::NON_MOVING:
                return inLayer2 == JPH::BroadPhaseLayer(static_cast<uint8_t>(BroadPhaseLayers::ID::MOVING));
            case Layers::ID::MOVING:
                return true;
            default:
                return false;
        }
    }
};

class ObjectLayerPairFilterImpl final: public JPH::ObjectLayerPairFilter {
  public:
    ~ObjectLayerPairFilterImpl() override;
    [[nodiscard]] auto ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const -> bool override {
        switch (static_cast<Layers::ID>(inObject1)) {
            case Layers::ID::NON_MOVING:
                return inObject2 == static_cast<JPH::ObjectLayer>(Layers::ID::MOVING);
            case Layers::ID::MOVING:
                return true;
            default:
                return false;
        }
    }
};

BPLayerInterfaceImpl::~BPLayerInterfaceImpl()                           = default;
ObjectVsBroadPhaseLayerFilterImpl::~ObjectVsBroadPhaseLayerFilterImpl() = default;
ObjectLayerPairFilterImpl::~ObjectLayerPairFilterImpl()                 = default;
ZHLN::Physics::ContactListener::~ContactListener()                      = default;

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

    void WaitIdle() noexcept {
        ZHLN::TaskSystem::Wait(&mInFlight);
    }

    [[nodiscard]] auto GetMaxConcurrency() const -> int override {
        // TaskSystem::Dispatch runs jobs inline when the scheduler is absent.
        // That is still one executor: Jolt sizes job-handle arrays from this
        // value and unconditionally accesses their first element later.
        return std::max(1, static_cast<int>(ZHLN::TaskSystem::GetWorkerCount()));
    }

    auto CreateJob(const char* inName, JPH::ColorArg inColor, const JPH::JobSystem::JobFunction& inJobFunction, JPH::uint32 inNumDependencies = 0)
        -> JPH::JobSystem::JobHandle override {
        const JPH::uint32 index = mJobs.ConstructObject(inName, inColor, this, inJobFunction, inNumDependencies);
        if (index == JPH::FixedSizeFreeList<JPH::JobSystem::Job>::cInvalidObjectIndex) {
            ZHLN::Panic("Jolt: JobSystemFiber exceeded the maximum number of jobs!");
        }
        auto* job = &mJobs.Get(index);
        // Keep the handle alive before dispatch: a zero-dependency job can
        // finish on another worker before CreateJob returns. Jolt starts such
        // jobs immediately; merely constructing their handles leaves the
        // initial graph idle until a barrier happens to execute it.
        JPH::JobSystem::JobHandle handle(job);
        if (inNumDependencies == 0) {
            QueueJob(job);
        }
        return handle;
    }

  protected:
    void QueueJob(JPH::JobSystem::Job* inJob) override {
        inJob->AddRef();

        const ZHLN::TaskSystem::Task task = {.func = JoltJobThunk, .arg = inJob};
        ZHLN::TaskSystem::Dispatch(std::span<const ZHLN::TaskSystem::Task>(&task, 1), &mInFlight);
    }

    void QueueJobs(JPH::JobSystem::Job** inJobs, uint inNumJobs) override {
        if (inNumJobs == 0) {
            return;
        }

        auto* tasks = static_cast<ZHLN::TaskSystem::Task*>(JPH_STACK_ALLOC(inNumJobs * sizeof(ZHLN::TaskSystem::Task)));

        for (uint i = 0; i < inNumJobs; ++i) {
            inJobs[i]->AddRef();
            tasks[i] = {.func = JoltJobThunk, .arg = inJobs[i]};
        }

        ZHLN::TaskSystem::Dispatch(std::span<const ZHLN::TaskSystem::Task>(tasks, inNumJobs), &mInFlight);
    }

    void FreeJob(JPH::JobSystem::Job* inJob) override {
        mJobs.DestructObject(inJob);
    }

  private:
    using AvailableJobs = JPH::FixedSizeFreeList<JPH::JobSystem::Job>;
    ZHLN::TaskSystem::Counter mInFlight;
    AvailableJobs             mJobs;
};

[[nodiscard]] auto ReadBodySnapshot(
    const Physics::BodyTable&      bt,
    const Physics::TransformFrame& snap,
    Physics::BodyHandle            handle
) noexcept -> Physics::BodyStateSnapshot {
    Physics::BodyStateSnapshot outState {};
    const uint32_t             slot = handle.index;
    if (slot >= bt.slotCapacity || bt.generations[slot].load(std::memory_order::acquire) != handle.generation) {
        return outState;
    }

    const Physics::SlotState liveState = bt.LoadSlotState(slot);
    if (liveState == Physics::SlotState::Empty || liveState == Physics::SlotState::PendingDestroy) {
        return outState;
    }

    if (snap.slotGenerations[slot] == handle.generation) {
        const auto snapState = static_cast<Physics::SlotState>(snap.slotStates[slot]);
        if (Physics::GetSlotPredicate(snapState).isActive) {
            const uint32_t dense = snap.slotToDense[slot];
            if (dense < snap.count) {
                const size_t base         = static_cast<size_t>(dense) * 4;
                outState.previousPosition = JPH::Vec3(
                    static_cast<float>(snap.prevPositions[base]),
                    static_cast<float>(snap.prevPositions[base + 1]),
                    static_cast<float>(snap.prevPositions[base + 2])
                );
                outState.currentPosition = JPH::Vec3(
                    static_cast<float>(snap.positions[base]),
                    static_cast<float>(snap.positions[base + 1]),
                    static_cast<float>(snap.positions[base + 2])
                );
                outState.previousRotation =
                    JPH::Quat(snap.prevRotations[base], snap.prevRotations[base + 1], snap.prevRotations[base + 2], snap.prevRotations[base + 3]);
                outState.currentRotation =
                    JPH::Quat(snap.rotations[base], snap.rotations[base + 1], snap.rotations[base + 2], snap.rotations[base + 3]);
                outState.isCharacter = (snapState == Physics::SlotState::Character);
                outState.valid       = true;
                return outState;
            }
        }
    }

    if (liveState == Physics::SlotState::PendingCreateRigid || liveState == Physics::SlotState::PendingCreateChar) {
        const JPH::Vec3 spawnPos(bt.spawnPositions[slot]);
        const JPH::Quat spawnRot  = bt.spawnRotations[slot];
        outState.previousPosition = spawnPos;
        outState.currentPosition  = spawnPos;
        outState.previousRotation = spawnRot;
        outState.currentRotation  = spawnRot;
        outState.isCharacter      = (liveState == Physics::SlotState::PendingCreateChar);
        outState.valid            = true;
    }

    return outState;
}

} // namespace

struct PhysicsContext::Impl {
    JPH::PhysicsSystem                      physicsSystem;
    std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
    JobSystemFiber                          jobSystem {JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers};

    BPLayerInterfaceImpl              bpLayerInterface;
    ObjectVsBroadPhaseLayerFilterImpl objVsBpFilter;
    ObjectLayerPairFilterImpl         objVsObjFilter;

    Physics::PhysicsWorld    world {};
    Physics::ContactListener contactListener {&world};

    Physics::ShapeCache       shapeCache {};
    Physics::CharacterManager characters {};
    Physics::RagdollManager   ragdolls {};

    std::unique_ptr<Physics::PhysicsDebugRenderer> debugRenderer;

    void FlushPendingSetup() {
        if (world.isStepping.load(std::memory_order::acquire)) {
            return;
        }
        const auto commands = world.mailbox.Drain();
        if (commands.empty()) {
            return;
        }
        world.transforms.BeginWriteFromFront();
        auto& backFrame = world.transforms.GetWriteBuffer();
        world.ProcessCommands(commands, characters, backFrame);
        world.mailbox.ResetDrained();
        world.transforms.PublishTableMetadataToBack(world.bodyTable);
        world.transforms.Swap();
    }
};

PhysicsContext::PhysicsContext(const PhysicsConfig& cfg): _impl(std::make_unique<Impl>()) {
    _impl->tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(cfg.tempAllocatorSize);

    _impl->physicsSystem.SetContactListener(&_impl->contactListener);
    _impl->physicsSystem.Init(
        cfg.maxBodies, 0, cfg.maxBodyPairs, cfg.maxContactConstraints, _impl->bpLayerInterface, _impl->objVsBpFilter, _impl->objVsObjFilter
    );

    _impl->debugRenderer = std::make_unique<Physics::PhysicsDebugRenderer>();
    _impl->world.Init(cfg.maxBodies, &_impl->physicsSystem, &_impl->jobSystem, _impl->tempAllocator.get());
    _impl->characters.Init(cfg.maxBodies, &_impl->world);
}

PhysicsContext::~PhysicsContext() {
    _impl->jobSystem.WaitIdle();
    _impl->ragdolls.Shutdown();
    _impl->FlushPendingSetup();
    _impl->characters.Shutdown();
    _impl->shapeCache.Clear();
    _impl->physicsSystem.SetContactListener(nullptr);
    _impl->world.Shutdown();
}

void PhysicsContext::Step(float deltaTime) {
    ZHLN::ScopedTimer profTimer("Physics (JPH)");
    auto&             world = _impl->world;

    // Phase 1: Command Ingestion (O(1) mailbox swap, lock-free command application onto back frame)
    world.transforms.BeginWriteFromFront();
    auto& backFrame = world.transforms.GetWriteBuffer();
    if (const auto commands = world.mailbox.Drain(); !commands.empty()) {
        world.ProcessCommands(commands, _impl->characters, backFrame);
        world.mailbox.ResetDrained();
    }

    // Phase 2: Simulation (Jolt rigid-body solver + parallel CharacterVirtual integration)
    world.contactCount.store(0u, std::memory_order::relaxed);
    world.BeginSteppingGate();

    _impl->physicsSystem.Update(deltaTime, 2, _impl->tempAllocator.get(), &_impl->jobSystem);
    _impl->characters.UpdateCharacters(deltaTime, _impl->physicsSystem, *_impl->tempAllocator);
    _impl->jobSystem.WaitIdle();

    world.EndSteppingGate();
    world.time += static_cast<double>(deltaTime);

    // Phase 3: Synchronization (SIMD scatter into inactive back TransformBuffer)
    world.Synchronize(&_impl->physicsSystem, _impl->characters.ActiveSpan(), backFrame);

    // Phase 4: Publish Snapshot (atomic release flip of frontIndex)
    world.transforms.Swap();
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

void PhysicsContext::TraceDiagnostics() const {
    ZHLN::Trace(_impl->world);
}

void PhysicsContext::OptimizeBroadphase() {
    _impl->FlushPendingSetup();
    _impl->physicsSystem.OptimizeBroadPhase();
}

auto PhysicsContext::GetOrCreateShape(Physics::ShapeType type, float p1, float p2, float p3, float p4) -> JPH::ShapeRefC {
    return _impl->shapeCache.GetOrCreate(type, p1, p2, p3, p4);
}

auto PhysicsContext::CreateRigidBody(
    const JPH::ShapeRefC& shape,
    JPH::RVec3Arg         pos,
    JPH::QuatArg          rot,
    JPH::EMotionType      motion,
    Layers::ID            layer,
    uint32_t              materialID,
    uint32_t              category,
    uint32_t              mask,
    JPH::EActivation      activation
) -> Physics::BodyHandle {
    if (shape == nullptr) {
        return Physics::BodyHandle::Null();
    }

    auto&                     world  = _impl->world;
    auto&                     bt     = world.bodyTable;
    const Physics::BodyHandle handle = bt.ReserveSlot(Physics::SlotState::PendingCreateRigid, motion, pos, rot);

    uint32_t precreatedBodyID = JPH::BodyID::cInvalidBodyID;
    if (world.TryEnterIdleJoltMutation()) {
        const Physics::MaterialData mat = world.ResolveMaterial(materialID);
        JPH::BodyCreationSettings   settings(shape, pos, rot, motion, static_cast<JPH::ObjectLayer>(layer));
        settings.mUserData    = handle.Pack();
        settings.mFriction    = mat.friction;
        settings.mRestitution = mat.restitution;

        if (motion == JPH::EMotionType::Dynamic) {
            settings.mAllowSleeping = true;
            settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
        }

        const JPH::BodyID id =
            world.bodyInterface->CreateAndAddBody(settings, (motion == JPH::EMotionType::Static) ? JPH::EActivation::DontActivate : activation);
        precreatedBodyID = id.GetIndexAndSequenceNumber();

        bt.slotBodyIDs[handle.index].store(precreatedBodyID, std::memory_order::release);
        bt.slotMaterialIDs[handle.index].store(materialID, std::memory_order::relaxed);

        const uint32_t jIdx = precreatedBodyID & JPH::BodyID::cMaxBodyIndex;
        ZHLN::Assert(jIdx < bt.idToHandleMap.size(), "PhysicsContext::CreateRigidBody: jIdx ({}) exceeds idToHandleMap!", jIdx);
        bt.idToHandleMap[jIdx].store(handle.Pack(), std::memory_order::release);

        world.LeaveIdleJoltMutation();
    }

    Physics::Command cmd {};
    cmd.type                = Physics::CommandType::CreateRigidBody;
    cmd.shape               = shape;
    cmd.payload.createRigid = Physics::CreateRigidPayload {
        .handle           = handle,
        .px               = pos.GetX(),
        .py               = pos.GetY(),
        .pz               = pos.GetZ(),
        .rx               = rot.GetX(),
        .ry               = rot.GetY(),
        .rz               = rot.GetZ(),
        .rw               = rot.GetW(),
        .precreatedBodyID = precreatedBodyID,
        .materialID       = materialID,
        .category         = category,
        .mask             = mask,
        .motion           = motion,
        .layer            = layer,
        .activation       = activation,
    };
    world.mailbox.Enqueue(std::move(cmd));
    return handle;
}

namespace Physics {

auto GetBodyID(const PhysicsWorld& world, BodyHandle handle) -> JPH::BodyID {
    const auto& bt = world.bodyTable;
    if (handle.index >= bt.slotCapacity) {
        return {};
    }
    if (bt.generations[handle.index].load(std::memory_order::acquire) != handle.generation) {
        return {};
    }
    const SlotState state = bt.LoadSlotState(handle.index);
    if (state != SlotState::Alive && state != SlotState::PendingCreateRigid) {
        return {};
    }
    return JPH::BodyID(bt.slotBodyIDs[handle.index].load(std::memory_order::acquire));
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
        const uint32_t i1 = indices[i + 0];
        const uint32_t i2 = indices[i + 1];
        const uint32_t i3 = indices[i + 2];

        if (i1 == i2 || i2 == i3 || i1 == i3) {
            continue;
        }

        joltTriangles.push_back(JPH::IndexedTriangle(i1, i2, i3, 0, 0));
    }

    if (joltTriangles.empty()) {
        return nullptr;
    }

    JPH::MeshShapeSettings        settings(std::move(joltVertices), std::move(joltTriangles));
    const JPH::Shape::ShapeResult result = settings.Create();
    return result.HasError() ? nullptr : result.Get();
}

auto CreateHeightFieldShape(const float* heights, int sampleCount, float worldSize) -> JPH::ShapeRefC {
    JPH::HeightFieldShapeSettings settings;
    settings.mSampleCount = static_cast<uint32_t>(sampleCount);
    settings.mHeightSamples.resize(static_cast<size_t>(sampleCount) * static_cast<size_t>(sampleCount));

    for (int i = 0; i < sampleCount * sampleCount; ++i) {
        settings.mHeightSamples[static_cast<size_t>(i)] = heights[i];
    }

    const float step = worldSize / static_cast<float>(sampleCount - 1);
    settings.mOffset = JPH::Vec3(-worldSize * 0.5f, 0.0f, -worldSize * 0.5f);
    settings.mScale  = JPH::Vec3(step, 1.0f, step);

    const JPH::Shape::ShapeResult result = settings.Create();
    return result.HasError() ? nullptr : result.Get();
}

auto CreateDualShape(const DualShapeConfig& config) -> JPH::ShapeRefC {
    JPH::StaticCompoundShapeSettings compound;

    const JPH::ShapeRefC lifterShape = new JPH::SphereShape(config.lifterRadius);
    compound.AddShape(JPH::Vec3(0.0f, config.GetLifterOffsetY(), 0.0f), JPH::Quat::sIdentity(), lifterShape);

    const float          cylinderHalfHeight = std::max(0.001f, config.bumperRadiusY - config.bumperRadiusXZ);
    const JPH::ShapeRefC bumperShape        = (config.bumperRadiusY > config.bumperRadiusXZ) ?
                                                  static_cast<JPH::ShapeRefC>(new JPH::CapsuleShape(cylinderHalfHeight, config.bumperRadiusXZ)) :
                                                  static_cast<JPH::ShapeRefC>(new JPH::SphereShape(config.bumperRadiusXZ));

    compound.AddShape(JPH::Vec3(0.0f, config.GetBumperOffsetY(), 0.0f), JPH::Quat::sIdentity(), bumperShape);

    const auto res = compound.Create();
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
) -> Physics::BodyHandle {
    const JPH::ShapeRefC shape = Physics::CreateMeshShape(vertices, vertexCount, indices, indexCount);
    if (shape == nullptr) {
        return Physics::BodyHandle::Null();
    }
    return CreateRigidBody(shape, pos, rot, JPH::EMotionType::Static, Layers::ID::NON_MOVING, 0, category, mask);
}

auto PhysicsContext::CreateCharacter(JPH::RVec3Arg position, const Physics::CharacterParams& params) -> Physics::BodyHandle {
    auto* impl  = _impl.get();
    auto& world = impl->world;

    JPH::ShapeRefC charShape = params.shape;
    if (charShape == nullptr) {
        charShape = GetOrCreateShape(Physics::ShapeType::Capsule, 0.5f, 0.3f);
    }
    if (charShape == nullptr) {
        return Physics::BodyHandle::Null();
    }

    // Cache centre-to-feet ground offset once at creation so gameplay never
    // queries Jolt shape bounds on hot steering ticks.
    const float groundOffset = charShape->GetLocalBounds().mMax.GetY();

    const Physics::BodyHandle handle =
        world.bodyTable.ReserveSlot(Physics::SlotState::PendingCreateChar, JPH::EMotionType::Kinematic, position, JPH::Quat::sIdentity());
    impl->characters.InitReservedSlot(handle, groundOffset);

    const JPH::Vec3  planeNormal   = params.supportingVolume.GetNormal();
    const float      planeConstant = params.supportingVolume.GetConstant();
    Physics::Command cmd {};
    cmd.type               = Physics::CommandType::CreateCharacter;
    cmd.shape              = std::move(charShape);
    cmd.payload.createChar = Physics::CreateCharacterPayload {
        .handle                   = handle,
        .px                       = position.GetX(),
        .py                       = position.GetY(),
        .pz                       = position.GetZ(),
        .maxSlopeAngle            = params.maxSlopeAngle,
        .maxStrength              = params.maxStrength,
        .characterPadding         = params.characterPadding,
        .penetrationRecoverySpeed = params.penetrationRecoverySpeed,
        .planeNx                  = planeNormal.GetX(),
        .planeNy                  = planeNormal.GetY(),
        .planeNz                  = planeNormal.GetZ(),
        .planeC                   = planeConstant,
        .category                 = params.category,
        .mask                     = params.mask,
    };
    world.mailbox.Enqueue(std::move(cmd));
    return handle;
}

void PhysicsContext::SetCollisionFilter(Physics::BodyHandle handle, uint32_t category, uint32_t mask) {
    Physics::Command cmd {};
    cmd.type              = Physics::CommandType::SetCollisionFilter;
    cmd.payload.setFilter = Physics::SetFilterPayload {
        .handle   = handle,
        .category = category,
        .mask     = mask,
    };
    _impl->world.mailbox.Enqueue(std::move(cmd));
}

auto PhysicsContext::GetDebugDrawData(bool drawShapes, bool drawConstraints, bool wireframe) const -> Physics::DebugDrawData {
    auto* impl = _impl.get();
    impl->debugRenderer->Clear();

    if (impl->world.isStepping.load(std::memory_order::acquire)) {
        return {};
    }

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
        .lines         = impl->debugRenderer->lines.data(),
        .lineCount     = impl->debugRenderer->lines.size(),
        .triangles     = impl->debugRenderer->triangles.data(),
        .triangleCount = impl->debugRenderer->triangles.size(),
    };
}

void PhysicsContext::SetCharacterVelocity(Physics::BodyHandle handle, JPH::Vec3Arg velocity) {
    if (!_impl->characters.RecordDesiredVelocity(handle, velocity)) {
        return;
    }
    Physics::Command cmd {};
    cmd.type                = Physics::CommandType::SetCharacterVelocity;
    cmd.payload.setVelocity = Physics::SetVelocityPayload {
        .handle = handle,
        .vx     = velocity.GetX(),
        .vy     = velocity.GetY(),
        .vz     = velocity.GetZ(),
    };
    _impl->world.mailbox.Enqueue(std::move(cmd));
}

void PhysicsContext::SetCharacterPosition(Physics::BodyHandle handle, JPH::RVec3Arg position) {
    auto& bt = _impl->world.bodyTable;
    if (handle.index >= bt.slotCapacity || bt.generations[handle.index].load(std::memory_order::acquire) != handle.generation) {
        return;
    }
    Physics::Command cmd {};
    cmd.type                = Physics::CommandType::SetCharacterPosition;
    cmd.payload.setPosition = Physics::SetPositionPayload {
        .handle = handle,
        .px     = position.GetX(),
        .py     = position.GetY(),
        .pz     = position.GetZ(),
    };
    _impl->world.mailbox.Enqueue(std::move(cmd));
}

void PhysicsContext::SetKinematicPose(Physics::BodyHandle handle, JPH::RVec3Arg position, JPH::QuatArg rotation) {
    auto& bt = _impl->world.bodyTable;
    if (handle.index >= bt.slotCapacity || bt.generations[handle.index].load(std::memory_order::acquire) != handle.generation) {
        return;
    }
    Physics::Command cmd {};
    cmd.type                     = Physics::CommandType::SetKinematicPose;
    cmd.payload.setKinematicPose = Physics::SetKinematicPosePayload {
        .handle = handle,
        .px     = position.GetX(),
        .py     = position.GetY(),
        .pz     = position.GetZ(),
        .rx     = rotation.GetX(),
        .ry     = rotation.GetY(),
        .rz     = rotation.GetZ(),
        .rw     = rotation.GetW(),
    };
    _impl->world.mailbox.Enqueue(std::move(cmd));
}

void PhysicsContext::SetLinearVelocity(Physics::BodyHandle handle, JPH::Vec3Arg velocity) {
    auto& bt = _impl->world.bodyTable;
    if (handle.index >= bt.slotCapacity || bt.generations[handle.index].load(std::memory_order::acquire) != handle.generation) {
        return;
    }
    Physics::Command cmd {};
    cmd.type                = Physics::CommandType::SetLinearVelocity;
    cmd.payload.setVelocity = Physics::SetVelocityPayload {
        .handle = handle,
        .vx     = velocity.GetX(),
        .vy     = velocity.GetY(),
        .vz     = velocity.GetZ(),
    };
    _impl->world.mailbox.Enqueue(std::move(cmd));
}

auto PhysicsContext::GetCharacterVelocity(Physics::BodyHandle handle) const -> JPH::Vec3 {
    return _impl->characters.GetVelocity(handle);
}

auto PhysicsContext::GetCharacterGroundOffset(Physics::BodyHandle handle) const -> float {
    return _impl->characters.GetGroundOffset(handle);
}

auto PhysicsContext::IsCharacterOnGround(Physics::BodyHandle handle) const -> bool {
    const auto&    world = _impl->world;
    const auto&    bt    = world.bodyTable;
    const uint32_t slot  = handle.index;
    if (slot >= bt.slotCapacity || bt.generations[slot].load(std::memory_order::acquire) != handle.generation) {
        return false;
    }
    if (bt.LoadSlotState(slot) != Physics::SlotState::Character) {
        return false;
    }

    const Physics::TransformFrame& snap = world.transforms.GetReadSnapshot();
    if (snap.slotGenerations[slot] != handle.generation || static_cast<Physics::SlotState>(snap.slotStates[slot]) != Physics::SlotState::Character) {
        return false;
    }
    const uint32_t dense = snap.slotToDense[slot];
    return dense < snap.count && (snap.characterFlags[dense] & Physics::kCharacterFlagOnGround) != 0u;
}

auto PhysicsContext::IsBodyDynamic(Physics::BodyHandle handle) const -> bool {
    const auto&    bt   = _impl->world.bodyTable;
    const uint32_t slot = handle.index;
    if (slot >= bt.slotCapacity || bt.generations[slot].load(std::memory_order::acquire) != handle.generation) {
        return false;
    }

    const Physics::SlotState state = bt.LoadSlotState(slot);
    if (state != Physics::SlotState::Alive && state != Physics::SlotState::PendingCreateRigid) {
        return false;
    }

    return static_cast<JPH::EMotionType>(bt.slotMotionTypes[slot].load(std::memory_order::acquire)) == JPH::EMotionType::Dynamic;
}

auto PhysicsContext::GetPositionBuffer() const -> BufferView {
    const Physics::TransformFrame& snap = _impl->world.transforms.GetReadSnapshot();
    BufferView                     view {};
    view.buf        = snap.positions;
    view.itemsize   = sizeof(JPH::Real);
    view.strides[0] = sizeof(JPH::Real) * 4;
    view.shape[0]   = snap.count;
    view.ndim       = 1;
    view.format[0]  = (sizeof(JPH::Real) == 8) ? 'd' : 'f';
    return view;
}

auto PhysicsContext::GetRotation(JPH::BodyID bodyID) const -> JPH::Quat {
    return _impl->world.bodyInterface->GetRotation(bodyID);
}

auto PhysicsContext::GetBodyHandle(JPH::BodyID bodyID) const -> Physics::BodyHandle {
    if (bodyID.IsInvalid()) {
        return Physics::BodyHandle::Null();
    }
    const auto&    bt   = _impl->world.bodyTable;
    const uint32_t jIdx = bodyID.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;
    if (jIdx >= bt.idToHandleMap.size()) {
        return Physics::BodyHandle::Null();
    }
    const uint64_t rawData = bt.idToHandleMap[jIdx].load(std::memory_order::acquire);
    return rawData != 0u ? Physics::BodyHandle::Unpack(rawData) : Physics::BodyHandle::Null();
}

auto PhysicsContext::RegisterRagdoll(JPH::Ref<JPH::Ragdoll>) -> Physics::RagdollHandle {
    return Physics::RagdollHandle::Invalid;
}

auto PhysicsContext::CreateSkeletalRagdoll(JPH::Ref<JPH::Skeleton> skeleton, std::span<const Physics::RagdollPartParams> parts) -> Physics::RagdollHandle {
    return _impl->ragdolls.CreateSkeletalRagdoll(_impl->physicsSystem, std::move(skeleton), parts);
}

void PhysicsContext::DestroyRagdoll(Physics::RagdollHandle handle) noexcept {
    _impl->ragdolls.DestroyRagdolls(std::span {&handle, 1});
}

void PhysicsContext::DestroyRagdolls(std::span<const Physics::RagdollHandle> handles) noexcept {
    _impl->ragdolls.DestroyRagdolls(handles);
}

auto PhysicsContext::GetRagdoll(Physics::RagdollHandle handle) const noexcept -> ZHLN::Optional<JPH::Ragdoll&> {
    return _impl->ragdolls.GetRagdoll(handle);
}

void PhysicsContext::ActivateRagdoll(Physics::RagdollHandle handle, const JPH::SkeletonPose& pose, JPH::Vec3Arg initialVelocity) noexcept {
    _impl->ragdolls.ActivateRagdoll(handle, pose, initialVelocity);
}

void PhysicsContext::RemoveRagdoll(Physics::RagdollHandle handle) noexcept {
    _impl->ragdolls.RemoveRagdoll(handle);
}

void PhysicsContext::DriveRagdollPose(Physics::RagdollHandle handle, const JPH::SkeletonPose& pose) noexcept {
    _impl->ragdolls.DriveRagdollPose(handle, pose);
}

void PhysicsContext::AddRagdollImpulse(Physics::RagdollHandle handle, uint32_t jointIndex, JPH::Vec3Arg impulse) noexcept {
    _impl->ragdolls.AddRagdollImpulse(handle, jointIndex, impulse, *_impl->world.bodyInterface);
}

auto PhysicsContext::GetRagdollPose(Physics::RagdollHandle handle, JPH::RVec3& outRootOffset, JPH::Mat44* outWorldJoints) const noexcept -> bool {
    return _impl->ragdolls.GetRagdollPose(handle, outRootOffset, outWorldJoints);
}

auto PhysicsContext::BodyPosition(Physics::BodyHandle handle) const noexcept -> std::optional<JPH::RVec3> {
    const auto&    world = _impl->world;
    const auto&    bt    = world.bodyTable;
    const uint32_t slot  = handle.index;
    if (slot >= bt.slotCapacity || bt.generations[slot].load(std::memory_order::acquire) != handle.generation) {
        return std::nullopt;
    }

    const Physics::SlotState liveState = bt.LoadSlotState(slot);
    if (liveState == Physics::SlotState::Empty || liveState == Physics::SlotState::PendingDestroy) {
        return std::nullopt;
    }

    const Physics::TransformFrame& snap = world.transforms.GetReadSnapshot();
    if (snap.slotGenerations[slot] == handle.generation) {
        const auto snapState = static_cast<Physics::SlotState>(snap.slotStates[slot]);
        if (Physics::GetSlotPredicate(snapState).isActive) {
            const uint32_t dense = snap.slotToDense[slot];
            if (dense < snap.count) {
                const size_t base = static_cast<size_t>(dense) * 4;
                return JPH::RVec3(snap.positions[base], snap.positions[base + 1], snap.positions[base + 2]);
            }
        }
    }

    if (liveState == Physics::SlotState::PendingCreateRigid || liveState == Physics::SlotState::PendingCreateChar) {
        return bt.spawnPositions[slot];
    }

    return std::nullopt;
}

auto PhysicsContext::BodyState(Physics::BodyHandle handle) const noexcept -> std::optional<Physics::BodyStateSnapshot> {
    const auto&                      world    = _impl->world;
    const Physics::TransformFrame&   snap     = world.transforms.GetReadSnapshot();
    const Physics::BodyStateSnapshot outState = ReadBodySnapshot(world.bodyTable, snap, handle);
    if (outState.valid) {
        return outState;
    }
    return std::nullopt;
}

void PhysicsContext::FillBodyStates(std::span<const Physics::BodyHandle> handles, std::span<Physics::BodyStateSnapshot> outStates) const noexcept {
    if (outStates.size() != handles.size()) {
        for (auto& state: outStates) {
            state = {};
        }
        return;
    }

    const auto&                    world = _impl->world;
    const Physics::BodyTable&      bt    = world.bodyTable;
    const Physics::TransformFrame& snap  = world.transforms.GetReadSnapshot();

    for (size_t i = 0; i < handles.size(); ++i) {
        outStates[i] = ReadBodySnapshot(bt, snap, handles[i]);
    }
}

void PhysicsContext::DestroyBody(Physics::BodyHandle handle) {
    DestroyBodies(std::span {&handle, 1});
}

void PhysicsContext::DestroyBodies(std::span<const Physics::BodyHandle> handles) {
    if (handles.empty()) {
        return;
    }
    auto& world = _impl->world;
    auto& bt    = world.bodyTable;

    JPH::Array<Physics::Command> batch;
    batch.reserve(handles.size());
    for (const auto handle: handles) {
        if (bt.TryMarkPendingDestroy(handle)) {
            Physics::Command cmd {};
            cmd.type           = Physics::CommandType::DestroyBody;
            cmd.payload.handle = handle;
            batch.push_back(std::move(cmd));
        }
    }
    if (!batch.empty()) {
        world.mailbox.EnqueueBatch(std::span<Physics::Command>(batch.data(), batch.size()));
    }
}

void PhysicsContext::RegisterMaterial(uint32_t id, float friction, float restitution) {
    _impl->world.RegisterMaterial(id, friction, restitution);
}

void PhysicsContext::AddImpulse(Physics::BodyHandle handle, JPH::Vec3Arg impulse) {
    auto& bt = _impl->world.bodyTable;
    if (handle.index >= bt.slotCapacity || bt.generations[handle.index].load(std::memory_order::acquire) != handle.generation) {
        return;
    }
    Physics::Command cmd {};
    cmd.type                = Physics::CommandType::AddImpulse;
    cmd.payload.setVelocity = Physics::SetVelocityPayload {
        .handle = handle,
        .vx     = impulse.GetX(),
        .vy     = impulse.GetY(),
        .vz     = impulse.GetZ(),
    };
    _impl->world.mailbox.Enqueue(std::move(cmd));
}

void PhysicsContext::AddImpulse(Physics::BodyHandle handle, JPH::Vec3Arg impulse, JPH::RVec3Arg position) {
    auto& bt = _impl->world.bodyTable;
    if (handle.index >= bt.slotCapacity || bt.generations[handle.index].load(std::memory_order::acquire) != handle.generation) {
        return;
    }
    Physics::Command cmd {};
    cmd.type                    = Physics::CommandType::AddImpulseAtPosition;
    cmd.payload.addImpulseAtPos = Physics::AddImpulseAtPosPayload {
        .handle = handle,
        .px     = position.GetX(),
        .py     = position.GetY(),
        .pz     = position.GetZ(),
        .ix     = impulse.GetX(),
        .iy     = impulse.GetY(),
        .iz     = impulse.GetZ(),
    };
    _impl->world.mailbox.Enqueue(std::move(cmd));
}

void PhysicsContext::AddRadialImpulse(JPH::RVec3Arg center, float radius, float maxImpulse) {
    JPH::Array<Physics::BodyHandle> overlapped;
    OverlapSphere(center, radius, overlapped);

    for (const Physics::BodyHandle physHandle: overlapped) {
        const auto bodyPos = BodyPosition(physHandle);
        if (!bodyPos) {
            continue;
        }

        const JPH::RVec3 diff    = *bodyPos - center;
        const auto       dist    = static_cast<float>(diff.Length());
        const float      falloff = 1.0f - std::clamp(dist / radius, 0.0f, 1.0f);

        if (dist > 1e-4f && falloff > 0.01f) {
            const JPH::Vec3 impulseDir = JPH::Vec3(diff / static_cast<double>(dist)) + JPH::Vec3(0.0f, 0.35f, 0.0f);
            const JPH::Vec3 impulse    = impulseDir.Normalized() * (maxImpulse * falloff * falloff);
            AddImpulse(physHandle, impulse);
        }
    }
}

auto PhysicsContext::GetContactEvents() const -> std::pair<const Physics::ContactEvent*, size_t> {
    const auto&  world = _impl->world;
    const size_t count = std::min(world.contactCount.load(std::memory_order::acquire), world.contactCapacity);
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
