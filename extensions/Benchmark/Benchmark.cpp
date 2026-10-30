// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Benchmark/Benchmark.cpp
#include <Benchmark/Benchmark.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/Physics/Physics.hpp>

// clang-format off
#include <Zahlen/Physics/Jolt.hpp>
// clang-format on
#include <Jolt/Math/Vec3.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ZHLN::Bench {

namespace {

// The tick measurement uses steady_clock rather than ZHLN::Clock's
// high_resolution_clock, which is libstdc++'s system_clock: a benchmark whose
// min/max can be moved by an NTP step is not measuring the engine. The Clock
// member is still the run's clock (totalTimeSec), where wall time is the
// point.
using TickTimer = std::chrono::steady_clock;

// Workload shape. Cubes half a metre across on a lattice with a wider spacing,
// so a batch starts out of contact and falls onto itself: pairs to test,
// contacts to generate, islands to build and bodies to wake, which is the
// expensive shape rather than a settled stack the solver can put to sleep.
inline constexpr float  kCubeHalfExtent  = 0.25f;
inline constexpr float  kLatticeSpacing  = 0.75f;
inline constexpr size_t kLatticeWidth    = 16;
inline constexpr float  kFloorHalfExtent = 32.0f;
inline constexpr float  kFloorHalfHeight = 0.5f;

// Where body `index` of the workload starts, in metres. The lattice is
// continuous across SpawnWorkload() calls -- each batch continues where the
// last stopped -- so growing a run does not restack it.
[[nodiscard]] auto LatticePosition(size_t index) noexcept -> JPH::Vec3 {
    const auto  width  = static_cast<float>(kLatticeWidth);
    const float column = static_cast<float>(index % kLatticeWidth) - (width * 0.5f);
    const float row    = static_cast<float>((index / kLatticeWidth) % kLatticeWidth) - (width * 0.5f);
    const float layer  = static_cast<float>(index / (kLatticeWidth * kLatticeWidth));

    // The first layer sits one spacing up, i.e. clear of the floor slab whose
    // top face is the origin plane.
    return JPH::Vec3 {column * kLatticeSpacing, (layer + 1.0f) * kLatticeSpacing, row * kLatticeSpacing};
}

// The config a workload of this size needs, never smaller than what the caller
// asked for. See the header for why this exists at all.
[[nodiscard]] auto RaisedFor(const PhysicsConfig& physCfg, size_t entityCount) noexcept -> PhysicsConfig {
    const auto needed = std::min<uint64_t>(static_cast<uint64_t>(entityCount) + LogicBenchmark::kBodyReserve, UINT32_MAX);

    PhysicsConfig raised = physCfg;
    raised.maxBodies     = static_cast<uint32_t>(std::max<uint64_t>(raised.maxBodies, needed));

    // Four pairs and four constraints per body is Jolt's own sample ratio for a
    // dense stack; a host that passed more keeps its numbers.
    const uint64_t pairs         = std::min<uint64_t>(needed * 4ULL, UINT32_MAX);
    raised.maxBodyPairs          = static_cast<uint32_t>(std::max<uint64_t>(raised.maxBodyPairs, pairs));
    raised.maxContactConstraints = static_cast<uint32_t>(std::max<uint64_t>(raised.maxContactConstraints, pairs));

    // Jolt's temp allocator is sized for the body count too: the default 32 MiB
    // covers a workload this small many times over, but a batch large enough to
    // matter would otherwise assert inside the solver -- and an assert is a
    // worse answer than a number.
    const uint64_t tempBytes = std::min<uint64_t>(needed * 1024ULL, 512ULL * 1024ULL * 1024ULL);
    raised.tempAllocatorSize = static_cast<uint32_t>(std::max<uint64_t>(raised.tempAllocatorSize, tempBytes));

    return raised;
}

} // namespace

LogicBenchmark::LogicBenchmark() {
    m_history.reserve(kHistoryCapacity);
}

LogicBenchmark::~LogicBenchmark() = default;

auto LogicBenchmark::Initialize(size_t initialEntityCount, const PhysicsConfig& physCfg) -> bool {
    Reset();

    const PhysicsConfig config = RaisedFor(physCfg, initialEntityCount);

    auto world = World::Create(config);
    if (!world) {
        ZHLN::LogError("[Bench] world creation failed; no benchmark is running: {}", world.error());
        return false;
    }

    m_world = std::move(world.value());
    // The ceiling is what the PhysicsSystem was built for, less the reserve the
    // header documents. Spawning is bounded by this rather than by Jolt's own
    // refusal, which comes back as a silently invalid body.
    m_bodyCeiling = config.maxBodies > kBodyReserve ? static_cast<size_t>(config.maxBodies - kBodyReserve) : 0;

    SpawnFloor();
    SpawnWorkload(initialEntityCount);

    m_runStartSeconds = m_clock.WallTimeSinceStart();
    ZHLN::Log(
        "[Bench] initialized: {} entities, {} bodies live, {} body budget (maxBodies {}, {} body pairs, {} contact constraints)", m_metrics.entityCount,
        m_liveBodies, m_bodyCeiling, config.maxBodies, config.maxBodyPairs, config.maxContactConstraints
    );
    return true;
}

auto LogicBenchmark::SpawnFloor() -> void {
    if (!m_world) {
        return;
    }

    auto&                physics = m_world->GetPhysics();
    const JPH::ShapeRefC slab    = physics.GetOrCreateShape(Physics::ShapeType::Box, kFloorHalfExtent, kFloorHalfHeight, kFloorHalfExtent);
    if (slab == nullptr) {
        ZHLN::LogWarning("[Bench] the floor shape could not be created; the workload will fall through the world.");
        return;
    }

    // A slab rather than a Jolt plane: a plane is infinite, and the workload is
    // meant to land on a surface whose contacts cost what any other surface's
    // do.
    m_world->GetRegistry().Create(
        Components::PhysicsComponent {
            .physicsHandle = physics.CreateRigidBody(
                slab, JPH::RVec3 {0.0, -static_cast<double>(kFloorHalfHeight), 0.0}, JPH::Quat::sIdentity(), JPH::EMotionType::Static, Layers::ID::NON_MOVING
            ),
            .isStatic = true
        }
    );
    ++m_liveBodies;
}

void LogicBenchmark::SpawnWorkload(size_t count) {
    if (!m_world) {
        return;
    }

    auto&                registry = m_world->GetRegistry();
    auto&                physics  = m_world->GetPhysics();
    const JPH::ShapeRefC cube     = physics.GetOrCreateShape(Physics::ShapeType::Box, kCubeHalfExtent, kCubeHalfExtent, kCubeHalfExtent);
    if (cube == nullptr) {
        ZHLN::LogWarning("[Bench] the workload shape could not be created; no bodies were spawned.");
        return;
    }

    size_t spawned = 0;
    for (; spawned < count; ++spawned) {
        if (m_liveBodies >= m_bodyCeiling) {
            if (!m_ceilingReported) {
                m_ceilingReported = true;
                ZHLN::LogWarning(
                    "[Bench] workload stopped at {} bodies: the physics config has no room for more. Raise PhysicsConfig::maxBodies (reserve {}).",
                    m_liveBodies, kBodyReserve
                );
            }
            break;
        }

        const JPH::Vec3 position = LatticePosition(m_spawnedCubes);
        const auto      handle   = physics.CreateRigidBody(
            cube, JPH::RVec3 {position.GetX(), position.GetY(), position.GetZ()}, JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, Layers::ID::MOVING
        );

        const Entity entity = registry.Create();
        registry.Add(entity, Components::TransformComponent {.position = position});
        registry.Add(entity, Components::PhysicsComponent {.physicsHandle = handle, .isStatic = false});
        ++m_spawnedCubes;
        ++m_liveBodies;
    }

    // The workload, not the registry: the floor slab carries a
    // PhysicsComponent too, and it is not part of what is being timed.
    m_metrics.entityCount = m_spawnedCubes;
}

void LogicBenchmark::Step(float fixedDt) {
    if (!m_world) {
        return;
    }

    auto& physics  = m_world->GetPhysics();
    auto& registry = m_world->GetRegistry();

    const TickTimer::time_point begin = TickTimer::now();

    physics.Step(fixedDt);

    // The per-entity half of a game loop's tick: read every body's transform
    // back out of physics and write it into the ECS. It walks the component
    // set in dense order -- the same walk an archetype query does -- and each
    // iteration is a real SoA lookup plus a Jolt handle decode, so the timing
    // tracks the entity count instead of a loop a compiler could fold away.
    size_t bodies = 0;
    for (const Entity entity: registry.GetEntitiesWith<Components::PhysicsComponent>()) {
        const auto physicsComponent = registry.Get<Components::PhysicsComponent>(entity);
        if (!physicsComponent || physicsComponent->isStatic || physicsComponent->physicsHandle == Physics::BodyHandle::Null()) {
            continue;
        }
        ++bodies;

        const auto position = physics.BodyPosition(physicsComponent->physicsHandle);
        if (!position) {
            continue;
        }
        if (auto transform = registry.Get<Components::TransformComponent>(entity)) {
            transform->position = static_cast<JPH::Vec3>(*position);
        }
    }

    const double tickSeconds      = std::chrono::duration<double>(TickTimer::now() - begin).count();
    const auto   tickMilliseconds = static_cast<float>(tickSeconds * 1000.0);

    m_metrics.totalTicks += 1;
    m_metrics.lastTickTimeMs = tickMilliseconds;
    m_metrics.minTickTimeMs  = std::min(m_metrics.minTickTimeMs, tickMilliseconds);
    m_metrics.maxTickTimeMs  = std::max(m_metrics.maxTickTimeMs, tickMilliseconds);
    // Running mean, so the average survives the history window rolling over.
    m_metrics.avgTickTimeMs += (tickMilliseconds - m_metrics.avgTickTimeMs) / static_cast<float>(m_metrics.totalTicks);
    m_metrics.totalTimeSec        = m_clock.WallTimeSinceStart() - m_runStartSeconds;
    m_metrics.entityCount         = bodies;
    m_metrics.activePhysicsBodies = physics.GetActiveBodyCount();

    if (m_history.size() == kHistoryCapacity) {
        m_history.erase(m_history.begin());
    }
    m_history.push_back(tickMilliseconds);
}

void LogicBenchmark::Reset() {
    // The world owns every body it holds: dropping it destroys the Jolt bodies
    // through the PhysicsComponents and releases its Jolt registration, so
    // there is nothing left to unwind by hand here.
    m_world.reset();

    m_metrics         = BenchmarkMetrics {};
    m_runStartSeconds = 0.0;
    m_liveBodies      = 0;
    m_spawnedCubes    = 0;
    m_bodyCeiling     = 0;
    m_ceilingReported = false;
    m_history.clear();
}

auto LogicBenchmark::GetMetrics() const noexcept -> const BenchmarkMetrics& {
    return m_metrics;
}

auto LogicBenchmark::GetTickHistory() const noexcept -> const std::vector<float>& {
    return m_history;
}

auto LogicBenchmark::HasWorld() const noexcept -> bool {
    return m_world != nullptr;
}

auto LogicBenchmark::GetWorld() noexcept -> World& {
    return *m_world;
}

} // namespace ZHLN::Bench
