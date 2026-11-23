// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Benchmark/Benchmark.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Vec3.hpp>
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
[[nodiscard]] auto LatticePosition(size_t index) noexcept -> FVec3 {
    const auto  width  = static_cast<float>(kLatticeWidth);
    const float column = static_cast<float>(index % kLatticeWidth) - (width * 0.5f);
    const float row    = static_cast<float>((index / kLatticeWidth) % kLatticeWidth) - (width * 0.5f);
    const float layer  = static_cast<float>(index / (kLatticeWidth * kLatticeWidth));

    // The first layer sits one spacing up, i.e. clear of the floor slab whose
    // top face is the origin plane.
    return FVec3 {column * kLatticeSpacing, (layer + 1.0f) * kLatticeSpacing, row * kLatticeSpacing};
}

// The config a workload of this size needs, never smaller than what the caller
// asked for. See the header for why this exists at all.
[[nodiscard]] auto RaisedFor(const PhysicsConfig& physCfg, size_t entityCount) noexcept -> PhysicsConfig {
    const auto needed = std::min<uint64_t>(static_cast<uint64_t>(entityCount) + LogicBenchmark::kBodyReserve + 1ULL, UINT32_MAX);

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

LogicBenchmark::~LogicBenchmark() { Reset(); }

auto LogicBenchmark::Initialize(size_t initialEntityCount, const PhysicsConfig& physCfg) -> bool {
    Reset();

    const PhysicsConfig config = RaisedFor(physCfg, initialEntityCount);

    auto chain = COM::InChain(config);
    auto world = COM::CreateLogicWorld(chain.Head());
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

    auto*                  physics = m_world->Simulation();
    Physics::BoxShapeDesc  slab {.halfExtents = {kFloorHalfExtent, kFloorHalfHeight, kFloorHalfExtent}};
    Physics::RigidBodyPose pose {.position = {0.0, -static_cast<double>(kFloorHalfHeight), 0.0}, .motionType = Physics::MotionType::Static};
    auto                   chain = COM::InChain(slab, pose);
    auto                   body  = physics->CreateRigidBody(chain.Head());
    if (!body.HasValue() || body.value == Physics::BodyID::Null())
        return;
    auto*        entities = m_world->Entities();
    const Entity e        = entities->CreateEntity();
    entities->Attach(e, Components::PhysicsComponent {.physicsID = body.value, .isStatic = true});
    ++m_liveBodies;
}

void LogicBenchmark::SpawnWorkload(size_t count) {
    if (!m_world) {
        return;
    }

    auto*                 registry = m_world->Entities();
    auto*                 physics  = m_world->Simulation();
    Physics::BoxShapeDesc cube {.halfExtents = {kCubeHalfExtent, kCubeHalfExtent, kCubeHalfExtent}};
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

        const FVec3 position = LatticePosition(m_spawnedCubes);
        Physics::RigidBodyPose pose {.position = position.Cast<double>(), .motionType = Physics::MotionType::Dynamic};
        auto                   chain = COM::InChain(cube, pose);
        auto                   body  = physics->CreateRigidBody(chain.Head());
        if (!body.HasValue() || body.value == Physics::BodyID::Null())
            break;
        const Entity entity = registry->CreateEntity();
        registry->Attach(entity, Components::TransformComponent {.position = position});
        registry->Attach(entity, Components::PhysicsComponent {.physicsID = body.value, .isStatic = false});
        m_entities.push_back(entity);
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

    auto* physics  = m_world->Simulation();
    auto* registry = m_world->Entities();

    const TickTimer::time_point begin = TickTimer::now();

    if (!m_world->Advance(fixedDt).HasValue())
        return;

    // The per-entity half of a game loop: scalar COM lookups and physics
    // readback into engine-owned typed ECS storage, in spawn order. This
    // intentionally measures the scalar boundary. No bulk map or registry
    // escape is introduced without profiling.
    size_t bodies = 0;
    for (const Entity entity: m_entities) {
        const auto physicsComponent = registry->Get<Components::PhysicsComponent>(entity);
        if (!physicsComponent || physicsComponent->isStatic || physicsComponent->physicsID == Physics::BodyID::Null()) {
            continue;
        }
        ++bodies;

        const auto position = physics->BodyPosition(physicsComponent->physicsID);
        if (!position.HasValue()) {
            continue;
        }
        if (auto transform = registry->Get<Components::TransformComponent>(entity)) {
            transform->position = position.value.Cast<float>();
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
    COM::LogicWorldInfo info {};
    auto                output = COM::OutChain(info);
    if (m_world->GetInfo(output.Head()).HasValue())
        m_metrics.activePhysicsBodies = info.activePhysicsBodies;

    if (m_history.size() == kHistoryCapacity) {
        m_history.erase(m_history.begin());
    }
    m_history.push_back(tickMilliseconds);
}

void LogicBenchmark::Reset() {
    m_entities.clear();
    m_world = {};

    m_metrics         = Metrics {};
    m_runStartSeconds = 0.0;
    m_liveBodies      = 0;
    m_spawnedCubes    = 0;
    m_bodyCeiling     = 0;
    m_ceilingReported = false;
    m_history.clear();
}

auto LogicBenchmark::GetMetrics() const noexcept -> const Metrics& {
    return m_metrics;
}

auto LogicBenchmark::GetTickHistory() const noexcept -> const std::vector<float>& {
    return m_history;
}

auto LogicBenchmark::HasWorld() const noexcept -> bool {
    return static_cast<bool>(m_world);
}

auto LogicBenchmark::GetWorld() noexcept -> COM::ILogicWorld* {
    return m_world.Get();
}

} // namespace ZHLN::Bench
