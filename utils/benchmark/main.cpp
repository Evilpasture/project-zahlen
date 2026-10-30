// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone, windowless logic benchmark. Neither the renderer nor App's game
// loop is started here; metrics are comparable on a headless build machine.
#include <Benchmark/Benchmark.hpp>
#include <CommandLine/CommandLine.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

namespace {

// `zahlen_benchmark`: the headless half of extensions/Benchmark, run once
// and reported.
//
// It builds no window, no device and no scene -- the benchmark owns a
// standalone World -- which is what makes the flag usable on a machine with no
// display and the number comparable across machines. Workload sizes come from
// the environment so a CI job can pin them without a rebuild:
//
//   ZHLN_BENCH_ENTITIES=4000 ZHLN_BENCH_TICKS=20000 ./build/current/bin/zahlen_benchmark
//
// The sizes below are the defaults: big enough that a tick is dominated by the
// engine rather than by the timer's resolution, small enough that a workstation
// finishes in a few seconds.
inline constexpr size_t   kBenchmarkEntities = 2000;
inline constexpr uint64_t kBenchmarkTicks    = 5000;

[[nodiscard]] auto BenchmarkEnvU64(const char* name, uint64_t fallback) -> uint64_t {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    char*                    end    = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    return (end != value) ? static_cast<uint64_t>(parsed) : fallback;
}

// Runs the workload and prints one report. False only when the benchmark's
// world could not be created; the reason is logged by the extension itself.
[[nodiscard]] auto RunLogicBenchmark() -> bool {
    const auto entities = static_cast<size_t>(BenchmarkEnvU64("ZHLN_BENCH_ENTITIES", kBenchmarkEntities));
    const auto ticks    = BenchmarkEnvU64("ZHLN_BENCH_TICKS", kBenchmarkTicks);

    ZHLN::Bench::LogicBenchmark benchmark;
    if (!benchmark.Initialize(entities)) {
        return false;
    }

    // The fixed step a game loop would use; the reported cost is per call, so
    // the value only decides how far the world moves between measurements.
    constexpr float kFixedDt = 1.0f / 60.0f;
    for (uint64_t tick = 0; tick < ticks; ++tick) {
        benchmark.Step(kFixedDt);
    }

    const auto& metrics = benchmark.GetMetrics();
    // Engine-only rate: measured tick time inverted. The wall rate below also
    // carries whatever the host spent between Step() calls, and here that is
    // the loop above and nothing else.
    const double engineRate = metrics.avgTickTimeMs > 0.0f ? 1000.0 / static_cast<double>(metrics.avgTickTimeMs) : 0.0;
    const double wallRate   = metrics.totalTimeSec > 0.0 ? static_cast<double>(metrics.totalTicks) / metrics.totalTimeSec : 0.0;

    ZHLN::Log<ZHLN::LogChannel::StdOut>("[Bench] logic benchmark: {} entities, {} ticks", metrics.entityCount, metrics.totalTicks);
    ZHLN::Log<ZHLN::LogChannel::StdOut>(
        "[Bench] tick ms: avg {:.3f}  min {:.3f}  max {:.3f}  last {:.3f}", metrics.avgTickTimeMs, metrics.minTickTimeMs, metrics.maxTickTimeMs,
        metrics.lastTickTimeMs
    );
    ZHLN::Log<ZHLN::LogChannel::StdOut>(
        "[Bench] throughput: {:.0f} ticks/s engine-only, {:.0f} ticks/s wall ({:.2f} s total)", engineRate, wallRate, metrics.totalTimeSec
    );
    return true;
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    constexpr std::array docs = {
        ZHLN::CliEnvDoc {"ZHLN_BENCH_ENTITIES", "2000", "Number of entities in the benchmark world"},
        ZHLN::CliEnvDoc {"ZHLN_BENCH_TICKS", "5000", "Number of fixed-step updates to measure"},
    };
    const auto parsed = ZHLN::ParseCli(
        std::span(argv, static_cast<size_t>(argc)), {},
        ZHLN::CliParseConfig {.programTitle = "Zahlen Logic Benchmark", .environmentDocs = docs}
    );
    if (!parsed) return EXIT_FAILURE;
    if (parsed->helpRequested || parsed->versionRequested) return EXIT_SUCCESS;
    return RunLogicBenchmark() ? EXIT_SUCCESS : EXIT_FAILURE;
}
