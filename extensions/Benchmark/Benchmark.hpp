// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Benchmark/Benchmark.hpp
//
// The headless half of the logic benchmark (zahlen_benchmark).
//
// What it times is a standalone ZHLN::World -- the ECS registry and the Jolt
// PhysicsContext behind it -- stepped at a fixed timestep, plus the per-entity
// transform sync a game loop would run over the bodies physics just wrote.
// Nothing here names the Engine, a render header, a window or Clay: the target
// this header belongs to links zahlen_engine and nothing else, so a CI job, a
// startup self-check or a profiling harness can measure ECS + physics without
// a device, a scene or a swapchain.
//
// The interactive overlay over these numbers is a second target
// (<Benchmark/Viewer/BenchmarkViewer.hpp>, zahlen_benchmark_viewer), which is
// the whole reason for the split: an application that only wants the numbers
// does not link a GUI, and one that only wants the panel does not link -- or
// write -- a timing loop. extensions/Scripting (core bindings in the root,
// LuaJIT runtime under Lua/) and extensions/net are the same shape.
#pragma once

#include <Zahlen/Engine/Clock.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Engine/World.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ZHLN::Bench {

// One run's numbers.
//
// The per-tick times are wall-clock milliseconds of the Step() call itself --
// physics, the ECS pass over the bodies, and nothing else -- so a hitch is
// maxTickTimeMs and the cost of the entity set is entityCount. totalTimeSec is
// wall time since Initialize(), which makes totalTicks / totalTimeSec the
// end-to-end tick rate the host actually achieved, while avgTickTimeMs is what
// one Step() cost the engine: the two differ by however much the host spent
// between calls. Both are worth having -- a host that stars at 2000 ticks/s
// and reaches 900 is either looking at the engine or at its own frame.
struct BenchmarkMetrics {
    uint64_t totalTicks          = 0;
    double   totalTimeSec        = 0.0;
    float    lastTickTimeMs      = 0.0f;
    float    avgTickTimeMs       = 0.0f;
    float    minTickTimeMs       = 1e9f;
    float    maxTickTimeMs       = 0.0f;
    size_t   entityCount         = 0;
    size_t   activePhysicsBodies = 0;
};

class ZHLN_STATIC_API LogicBenchmark {
  public:
    // Samples GetTickHistory() keeps. Fixed rather than growing, so a run of
    // any length costs the same memory and a host can size its chart once.
    static constexpr size_t kHistoryCapacity = 256;

    // Bodies Initialize() leaves in the physics config on top of the workload,
    // for the floor slab and for whatever the host adds afterwards.
    static constexpr uint32_t kBodyReserve = 64;

    LogicBenchmark();
    ~LogicBenchmark();

    LogicBenchmark(const LogicBenchmark&)                    = delete;
    auto operator=(const LogicBenchmark&) -> LogicBenchmark& = delete;

    // Creates the world and spawns initialEntityCount dynamic bodies on a
    // lattice above a floor. False means the world could not be created -- the
    // reason is logged, no world is held, and the object stays usable (a later
    // Initialize() may succeed).
    //
    // physCfg is a floor, not the last word. A Jolt PhysicsSystem built for
    // fewer bodies than the workload needs refuses the extra ones rather than
    // failing loudly, so a run asking for 2000 bodies under the default
    // 1024-body config would time the config instead of the engine. maxBodies,
    // maxBodyPairs, maxContactConstraints and tempAllocatorSize are raised to
    // fit the workload, and never lowered -- a host that passed its own
    // numbers keeps them.
    auto Initialize(size_t initialEntityCount, const PhysicsConfig& physCfg = {}) -> bool;

    // One tick: physics at fixedDt, then the ECS pass that writes each body's
    // position back into its TransformComponent. The elapsed wall time lands
    // in the metrics and in the tick history. A no-op before Initialize().
    void Step(float fixedDt);

    // Adds count bodies to the running world on the same lattice, continuing
    // where the last batch stopped. A batch the physics config has no room for
    // is cut short -- the loop stops and warns once -- rather than handed to
    // Jolt to refuse.
    void SpawnWorkload(size_t count);

    // Drops the world and zeroes every counter. Initialize() re-arms; a host
    // that wants the same workload back calls it with the same count.
    void Reset();

    [[nodiscard]] const BenchmarkMetrics& GetMetrics() const noexcept;

    // Oldest first, at most kHistoryCapacity samples: one float per tick, the
    // same number that went into lastTickTimeMs.
    [[nodiscard]] const std::vector<float>& GetTickHistory() const noexcept;

    [[nodiscard]] auto HasWorld() const noexcept -> bool;

    // The world this benchmark owns, for a host that wants to look at what it
    // stepped. Only valid between a successful Initialize() and Reset(), which
    // is what HasWorld() answers without the precondition.
    [[nodiscard]] auto GetWorld() noexcept -> World&;

  private:
    // The static slab the workload lands on, and the per-run bookkeeping the
    // spawn ceiling needs. Defined in Benchmark.cpp.
    auto SpawnFloor() -> void;

    std::unique_ptr<World> m_world;
    // The run clock: wall time since the object was constructed, so Initialize
    // seeds m_runStartSeconds from it and totalTimeSec is the difference.
    Clock              m_clock;
    double             m_runStartSeconds = 0.0;
    BenchmarkMetrics   m_metrics {};
    std::vector<float> m_history;

    // Live bodies the benchmark itself created: the floor plus every dynamic
    // cube. The ceiling is what the PhysicsSystem was built for, so the spawn
    // loop stops before Jolt is handed a body it has no room for.
    size_t m_liveBodies = 0;
    // Cubes spawned so far, which is also the index the next one takes on the
    // lattice -- the floor is a body but not a workload slot.
    size_t m_spawnedCubes    = 0;
    size_t m_bodyCeiling     = 0;
    bool   m_ceilingReported = false;
};

} // namespace ZHLN::Bench
