// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/profile/PerfBaseline.hpp
//
// Performance baseline caching for TestPerformance / TestRenderPerformance,
// built on the JSON extra (ReflectJSON::TryParse to read,
// ReflectJSON::SerializeJSON to write), ZHLN::Mutex and ZHLN::Println.
//
// Every benchmark metric is recorded to perf-baseline.json at the PROJECT
// ROOT (ZHLN_PROJECT_ROOT, defined once in the root CMakeLists), so
// baselines survive deleting or reconfiguring the build tree. Each run
// compares against the LAST recorded value; a regression beyond the limit
// fails the test via ZHLN::Test::VerifyBaseline (TestsFramework.hpp).
//
// Storage: one reflected struct (PerfBaselineFile below) serialised through
// ReflectJSON::SerializeJSON and read back with ReflectJSON::TryParse --
// the file format IS the struct declaration:
// The machine key is hostname | Config.hpp Compiler | BuildType (+ " ASan"),
// so different machines/toolchains/profiles keep independent baselines in
// the shared file; an unknown key simply starts a fresh baseline. The file
// is machine-local and gitignored.
//
// Policy per metric:
//   * No previous value  -> record it, pass ("first run").
//   * Regression beyond the limit -> FAIL, and do NOT overwrite the stored
//     value (a failing run must not launder itself into the new baseline).
//   * Otherwise -> pass and record (stored value == last passing run).
//
// Environment knobs:
//   ZHLN_PERF_REGRESSION_LIMIT  global limit override in percent (e.g. "40")
//   ZHLN_PERF_REBASELINE=1      record everything without failing
//   ZHLN_PERF_BASELINE=<path>   use an explicit baseline file instead

#pragma once

#include "TestsFramework.hpp"
#include <Zahlen/Config.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <json/JSONSchema.hpp>
#include <map>
#include <numeric>
#include <span>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(_WIN32)
    #include <unistd.h> // gethostname
#endif

namespace ZHLN::Test {

// ============================================================================
// Optimization Barriers
// ============================================================================

template <typename T>
inline void DoNotOptimize(const T& value) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r,m"(value) : "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

template <typename T>
inline void DoNotOptimize(T& value) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : "+r,m"(value) : : "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

inline void ClobberMemory() {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : : "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// ============================================================================
// High-Resolution Benchmark Timer
// ============================================================================

struct BenchmarkTimer {
    using Clock = std::chrono::high_resolution_clock;
    Clock::time_point startTime;

    BenchmarkTimer() noexcept: startTime(Clock::now()) {
    }

    void Reset() noexcept {
        startTime = Clock::now();
    }

    [[nodiscard]] double ElapsedNanoseconds() const noexcept {
        const auto now = Clock::now();
        return std::chrono::duration<double, std::nano>(now - startTime).count();
    }

    [[nodiscard]] double ElapsedMicroseconds() const noexcept {
        const auto now = Clock::now();
        return std::chrono::duration<double, std::micro>(now - startTime).count();
    }

    [[nodiscard]] double ElapsedMilliseconds() const noexcept {
        const auto now = Clock::now();
        return std::chrono::duration<double, std::milli>(now - startTime).count();
    }

    [[nodiscard]] double ElapsedSeconds() const noexcept {
        const auto now = Clock::now();
        return std::chrono::duration<double>(now - startTime).count();
    }
};

// Runs `measure` n times and returns the FASTEST sample. Wall-clock
// benchmarks on a desktop are one-sided noisy (frequency scaling, scheduler
// migrations, cache state): interruptions only ever ADD time, so the minimum
// of a handful of samples is the stable estimate of a workload's true cost,
// and the first sample doubles as warm-up. Feed this into VerifyBaseline:
//
//   const double ms = ZHLN::Test::BestOf(5, [&] {
//       BenchmarkTimer timer;
//       DoWork();
//       return timer.ElapsedMilliseconds();
//   });
template <typename F>
[[nodiscard]] inline double BestOf(unsigned n, F&& measure) {
    double best = measure();
    for (unsigned i = 1; i < n; ++i) {
        const double sample = measure();
        if (sample < best) {
            best = sample;
        }
    }
    return best;
}

/// The same sampling, keeping the shape of the distribution.
///
/// `best` is what a baseline check should use -- see BestOf for why -- but the
/// best alone cannot tell a real regression from a noisy machine. A workload
/// that genuinely got slower moves its whole distribution; a machine that is
/// merely busy or thermally throttled grows a tail and leaves the median near
/// where it was. Print all three next to a disputed metric and the next run
/// answers the question instead of raising it.
struct SampleStats {
    double   best    = 0.0;
    double   median  = 0.0;
    double   worst   = 0.0;
    unsigned samples = 0;
};

template <typename F>
[[nodiscard]] inline auto SampleBestOf(unsigned n, F&& measure) -> SampleStats {
    std::vector<double> samples;
    samples.reserve(n > 0 ? n : 1);
    for (unsigned i = 0; i < (n > 0 ? n : 1); ++i) {
        samples.push_back(measure());
    }
    std::sort(samples.begin(), samples.end());
    return SampleStats {
        .best    = samples.front(),
        .median  = samples[samples.size() / 2],
        .worst   = samples.back(),
        .samples = static_cast<unsigned>(samples.size())
    };
}

/// Frame time distribution and derived FPS percentiles.
///
/// Standard gaming benchmark metrics:
///   * maxFps: fastest single frame (1000.0 / minFrameMs)
///   * minFps: slowest single frame (1000.0 / maxFrameMs)
///   * low1PctFps: average of the worst 1% slowest frames (1000.0 / avg_slowest_1pct_ms)
///   * low01PctFps: average of the worst 0.1% slowest frames (1000.0 / avg_slowest_01pct_ms)
///   * p50 / p90 / p95 / p99 / p99.9: frame time percentiles in milliseconds
struct FrameStats {
    double avgFrameMs   = 0.0;
    double minFrameMs   = 0.0;
    double maxFrameMs   = 0.0;
    double p50FrameMs   = 0.0;
    double p90FrameMs   = 0.0;
    double p95FrameMs   = 0.0;
    double p99FrameMs   = 0.0;
    double p99_9FrameMs = 0.0;
    double avgFps       = 0.0;
    double maxFps       = 0.0;
    double minFps       = 0.0;
    double low1PctFps   = 0.0;
    double low01PctFps  = 0.0;
};

[[nodiscard]] inline auto CalculateFrameStats(std::span<const double> frameTimesMs) -> FrameStats {
    if (frameTimesMs.empty()) {
        return {};
    }

    std::vector<double> sorted(frameTimesMs.begin(), frameTimesMs.end());
    std::ranges::sort(sorted);

    const size_t n     = sorted.size();
    const double sum   = std::accumulate(sorted.begin(), sorted.end(), 0.0);
    const double avgMs = sum / static_cast<double>(n);
    const double minMs = sorted.front();
    const double maxMs = sorted.back();

    const size_t p50Idx   = std::min(static_cast<size_t>(std::ceil(static_cast<double>(n) * 0.50)) - 1, n - 1);
    const size_t p90Idx   = std::min(static_cast<size_t>(std::ceil(static_cast<double>(n) * 0.90)) - 1, n - 1);
    const size_t p95Idx   = std::min(static_cast<size_t>(std::ceil(static_cast<double>(n) * 0.95)) - 1, n - 1);
    const size_t p99Idx   = std::min(static_cast<size_t>(std::ceil(static_cast<double>(n) * 0.99)) - 1, n - 1);
    const size_t p99_9Idx = std::min(static_cast<size_t>(std::ceil(static_cast<double>(n) * 0.999)) - 1, n - 1);

    const double p50Ms   = sorted[p50Idx];
    const double p90Ms   = sorted[p90Idx];
    const double p95Ms   = sorted[p95Idx];
    const double p99Ms   = sorted[p99Idx];
    const double p99_9Ms = sorted[p99_9Idx];

    const size_t count1Pct  = std::max(1UL, static_cast<size_t>(std::ceil(static_cast<double>(n) * 0.01)));
    const size_t count01Pct = std::max(1UL, static_cast<size_t>(std::ceil(static_cast<double>(n) * 0.001)));

    const double avgSlowest1PctMs  = std::accumulate(sorted.end() - count1Pct, sorted.end(), 0.0) / static_cast<double>(count1Pct);
    const double avgSlowest01PctMs = std::accumulate(sorted.end() - count01Pct, sorted.end(), 0.0) / static_cast<double>(count01Pct);

    return FrameStats {
        .avgFrameMs   = avgMs,
        .minFrameMs   = minMs,
        .maxFrameMs   = maxMs,
        .p50FrameMs   = p50Ms,
        .p90FrameMs   = p90Ms,
        .p95FrameMs   = p95Ms,
        .p99FrameMs   = p99Ms,
        .p99_9FrameMs = p99_9Ms,
        .avgFps       = (avgMs > 0.0) ? (1000.0 / avgMs) : 0.0,
        .maxFps       = (minMs > 0.0) ? (1000.0 / minMs) : 0.0,
        .minFps       = (maxMs > 0.0) ? (1000.0 / maxMs) : 0.0,
        .low1PctFps   = (avgSlowest1PctMs > 0.0) ? (1000.0 / avgSlowest1PctMs) : 0.0,
        .low01PctFps  = (avgSlowest01PctMs > 0.0) ? (1000.0 / avgSlowest01PctMs) : 0.0,
    };
}

} // namespace ZHLN::Test

namespace ZHLN::Test::Perf {

// ============================================================================
// Configuration
// ============================================================================

// Limit used when a check does not pass an explicit one. Percent, in the
// "worse" direction (a lower-is-better metric degrading by 40% is +40%).
[[nodiscard]] inline auto DefaultLimitPercent() noexcept -> double {
    if (const char* env = std::getenv("ZHLN_PERF_REGRESSION_LIMIT")) {
        const double parsed = std::atof(env);
        if (parsed > 0.0 && parsed <= 1000.0) {
            return parsed;
        }
    }
    return 30.0;
}

// ZHLN_PERF_REBASELINE set to anything non-empty/non-zero: record every
// metric without failing (re-baseline pass after a hardware/toolchain move).
[[nodiscard]] inline auto RebaselineRequested() noexcept -> bool {
    const char* env = std::getenv("ZHLN_PERF_REBASELINE");
    return env != nullptr && *env != '\0' && std::string_view {env} != "0";
}

[[nodiscard]] inline auto BaselineFilePath() -> std::filesystem::path {
    if (const char* env = std::getenv("ZHLN_PERF_BASELINE"); env != nullptr && *env != '\0') {
        return std::filesystem::path {env};
    }
#if defined(ZHLN_PROJECT_ROOT)
    return std::filesystem::path {ZHLN_PROJECT_ROOT} / "perf-baseline.json";
#else
    return std::filesystem::path {"perf-baseline.json"};
#endif
}

// Identifies the configuration a baseline belongs to: timing from different
// machines, compilers, build profiles or sanitizer builds must never be
// compared (Compiler/BuildType/Sanitizers come from Zahlen/Config.hpp).
[[nodiscard]] inline auto MachineKey() -> std::string {
    std::string host {"unknown-host"};
#if defined(_WIN32)
    if (const char* computer = std::getenv("COMPUTERNAME"); computer != nullptr && *computer != '\0') {
        host = computer;
    }
#else
    char buffer[64] {}; // zero-init: gethostname may not terminate on truncation
    ::gethostname(buffer, sizeof buffer - 1);
    host = buffer;
#endif

    std::string key = host + " | " + std::string {Compiler} + " | " + std::string {BuildType};
    if (Sanitizers == "enabled") {
        key += " | ASan";
    }
    return key;
}

// ============================================================================
// Baseline file schema (reflected: field names are the JSON keys)
// ============================================================================

struct FrameMetrics {
    double avg_frame_ms   = 0.0;
    double p50_frame_ms   = 0.0;
    double p95_frame_ms   = 0.0;
    double p99_frame_ms   = 0.0;
    double p99_9_frame_ms = 0.0;
    double avg_fps        = 0.0;
    double max_fps        = 0.0;
    double min_fps        = 0.0;
    double low_1pct_fps   = 0.0;
    double low_0_1pct_fps = 0.0;
};

struct MachineBaselines {
    std::string                         commit;
    std::map<std::string, double>       metrics;
    std::map<std::string, FrameMetrics> frame_benchmarks;
};

struct PerfBaselineFile {
    uint32_t                                version = 1;
    std::map<std::string, MachineBaselines> machines;
};

// Reads the whole baseline file. A missing or unparsable file is not an
// error: it warns and yields the default (fresh baseline), because the
// natural first-run flow is "no file yet".
[[nodiscard]] inline auto LoadBaselineFile(const std::filesystem::path& path) -> PerfBaselineFile {
    std::ifstream file {path, std::ios::binary};
    if (!file) {
        return {};
    }
    const std::string text {(std::istreambuf_iterator<char> {file}), std::istreambuf_iterator<char> {}};

    auto parsed = ReflectJSON::TryParse<PerfBaselineFile>(text, ReflectJSON::Options {.omitEmpty = true});
    if (!parsed) {
        ZHLN::Println(stderr, "[perf-baseline] {} is malformed; starting a fresh baseline", path.string());
        return {};
    }
    return std::move(*parsed);
}

// Writes `text` to `path` atomically (tmp file + rename). Uses only the
// non-throwing filesystem overloads: the engine builds with -fno-exceptions,
// so the throwing rename/remove are as banned as try/catch itself. Failures
// are reported as stderr warnings — a stale cache file must never fail a test.
inline void WriteBaselineAtomically(const std::filesystem::path& path, const std::string& text) {
    const std::filesystem::path temporary {path.string() + ".tmp"};
    {
        std::ofstream file {temporary, std::ios::binary | std::ios::trunc};
        if (!file) {
            ZHLN::Println(stderr, "[perf-baseline] cannot open {} for writing", temporary.string());
            return;
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) {
            ZHLN::Println(stderr, "[perf-baseline] short write to {}", temporary.string());
            return;
        }
    }
    std::error_code ec;
#if defined(_WIN32)
    std::filesystem::remove(path, ec); // rename() refuses existing targets on Windows
    ec.clear();
#endif
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        ZHLN::Println(stderr, "[perf-baseline] rename failed: {}", ec.message());
    }
}

// ============================================================================
// Store + Check
// ============================================================================

enum class Direction : uint8_t {
    LowerIsBetter,  // durations (ms): smaller is better
    HigherIsBetter, // throughput (ops/s): larger is better
};

struct Result {
    bool        known        = false; // a previous value existed for this metric
    double      previous     = 0.0;   // that previous value (0.0 when !known)
    std::string commit;               // commit/tag of the previous baseline run
    double      changePct    = 0.0;   // signed change in the WORSE direction (+ = worse)
    double      limitPct     = 0.0;   // limit that was applied
    bool        regressed    = false; // exceeded the limit; the test must fail
    bool        recorded     = false; // this run's value became the new baseline
};

namespace detail {

    // One per process. Accepted updates are merged over a FRESH copy of the
    // file at save time, so two test executables sharing one baseline file
    // (TestPerformance + TestRenderPerformance under `ctest -j`) cannot
    // clobber each other's metrics.
    struct BaselineStore {
        ZHLN::Mutex                         mutex;
        std::map<std::string, double>       baseline;
        std::map<std::string, FrameMetrics> baselineFrames;
        std::string                         baselineCommit;
        std::map<std::string, double>       pending;
        std::map<std::string, FrameMetrics> pendingFrames;
        bool                                loaded = false;

        ~BaselineStore() {
            SaveNow();
        }

        void SaveNow() {
            const MutexGuard lock(mutex);

            // Read-modify-write at METRIC granularity: only the keys this
            // process accepted are overwritten. Rejected metrics keep their
            // stored value (that is the "failing run cannot launder itself"
            // guarantee), metrics this process never measured survive
            // untouched, and sibling test executables sharing this machine
            // key (cpu.*/render.* namespaces) cannot clobber each other.
            PerfBaselineFile file = LoadBaselineFile(BaselineFilePath());
            auto&            ours = file.machines[MachineKey()];
            if (!pending.empty() || !pendingFrames.empty() || ours.commit.empty()) {
                ours.commit = std::string {GetGitCommitHash()};
            }
            for (const auto& [metric, value]: pending) {
                ours.metrics[metric] = value;
            }
            for (const auto& [name, frameMetrics]: pendingFrames) {
                ours.frame_benchmarks[name] = frameMetrics;
            }

            WriteBaselineAtomically(BaselineFilePath(), ReflectJSON::SerializeJSON(file, 2));
        }
    };

    [[nodiscard]] inline auto Store() -> BaselineStore& {
        static BaselineStore store; // function-local static: one store, saved at exit
        return store;
    }

} // namespace detail

// Records `value` for `metric` and compares it against the last recorded
// run. See the header comment for the full policy. Thread-safe.
//
//   limitPct <= 0  -> use DefaultLimitPercent() (env-overridable).
//   RebaselineRequested() -> never regressed, always records.
[[nodiscard]] inline auto Check(
    std::string_view metric,
    double           value,
    double           limitPct   = -1.0,
    Direction        direction  = Direction::LowerIsBetter,
    double           noiseFloor = 0.05
) -> Result {
    Result result;
    result.limitPct = limitPct > 0.0 ? limitPct : DefaultLimitPercent();

    auto&            store = detail::Store();
    const MutexGuard lock(store.mutex);

    if (!store.loaded) {
        const PerfBaselineFile file = LoadBaselineFile(BaselineFilePath());
        const auto             ours = file.machines.find(MachineKey());
        if (ours != file.machines.end()) {
            store.baseline       = ours->second.metrics;
            store.baselineFrames = ours->second.frame_benchmarks;
            store.baselineCommit = ours->second.commit;
        }
        store.loaded = true;
    }

    const std::string key {metric};
    const auto        previous = store.baseline.find(key);
    if (previous == store.baseline.end() || !(previous->second > 0.0)) {
        // First run (or unusable previous value): record, don't judge.
        store.baseline[key] = value;
        store.pending[key]  = value;
        result.recorded     = true;
        result.commit       = std::string {GetGitCommitHash()};
        return result;
    }

    result.known       = true;
    result.previous    = previous->second;
    result.commit      = store.baselineCommit;
    const double delta = (direction == Direction::LowerIsBetter) ? (value - result.previous) : (result.previous - value);
    result.changePct   = 100.0 * delta / result.previous;
    result.regressed   = !RebaselineRequested() && (result.changePct > result.limitPct) && (delta > noiseFloor);

    if (!result.regressed) {
        store.baseline[key] = value;
        store.pending[key]  = value;
        result.recorded     = true;
    }
    // Regressed: keep the previous value as the baseline so a failing run
    // cannot launder itself into the next comparison.
    return result;
}

struct FrameCheckResult {
    bool         known        = false;
    FrameMetrics previous     = {};
    std::string  commit;
    double       avgChangePct = 0.0;
    double       p99ChangePct = 0.0;
    double       avgLimitPct  = 0.0;
    double       p99LimitPct  = 0.0;
    bool         avgRegressed = false;
    bool         p99Regressed = false;
    bool         regressed    = false;
    bool         recorded     = false;
};

[[nodiscard]] inline auto CheckFrame(
    std::string_view  benchmarkName,
    const FrameStats& stats,
    double            avgLimitPct     = -1.0,
    double            p99LimitPct     = 75.0,
    double            avgNoiseFloorMs = 0.1,
    double            p99NoiseFloorMs = 1.0
) -> FrameCheckResult {
    FrameCheckResult result;
    result.avgLimitPct = avgLimitPct > 0.0 ? avgLimitPct : DefaultLimitPercent();
    result.p99LimitPct = p99LimitPct > 0.0 ? p99LimitPct : 75.0;

    auto&            store = detail::Store();
    const MutexGuard lock(store.mutex);

    if (!store.loaded) {
        const PerfBaselineFile file = LoadBaselineFile(BaselineFilePath());
        const auto             ours = file.machines.find(MachineKey());
        if (ours != file.machines.end()) {
            store.baseline       = ours->second.metrics;
            store.baselineFrames = ours->second.frame_benchmarks;
            store.baselineCommit = ours->second.commit;
        }
        store.loaded = true;
    }

    FrameMetrics current {
        .avg_frame_ms   = stats.avgFrameMs,
        .p50_frame_ms   = stats.p50FrameMs,
        .p95_frame_ms   = stats.p95FrameMs,
        .p99_frame_ms   = stats.p99FrameMs,
        .p99_9_frame_ms = stats.p99_9FrameMs,
        .avg_fps        = stats.avgFps,
        .max_fps        = stats.maxFps,
        .min_fps        = stats.minFps,
        .low_1pct_fps   = stats.low1PctFps,
        .low_0_1pct_fps = stats.low01PctFps,
    };

    const std::string key {benchmarkName};
    const auto        prev = store.baselineFrames.find(key);
    if (prev == store.baselineFrames.end() || !(prev->second.avg_frame_ms > 0.0)) {
        store.baselineFrames[key] = current;
        store.pendingFrames[key]  = current;
        result.recorded           = true;
        result.commit             = std::string {GetGitCommitHash()};
        return result;
    }

    result.known        = true;
    result.previous     = prev->second;
    result.commit       = store.baselineCommit;

    const double avgDeltaMs = current.avg_frame_ms - result.previous.avg_frame_ms;
    const double p99DeltaMs = current.p99_frame_ms - result.previous.p99_frame_ms;

    result.avgChangePct = 100.0 * avgDeltaMs / result.previous.avg_frame_ms;
    result.p99ChangePct = (result.previous.p99_frame_ms > 0.0)
                              ? (100.0 * p99DeltaMs / result.previous.p99_frame_ms)
                              : 0.0;

    // Both percentage threshold and absolute noise floor must be exceeded to fail.
    // This prevents microsecond / sub-millisecond desktop jitter from tripping false regressions.
    result.avgRegressed = !RebaselineRequested() && (result.avgChangePct > result.avgLimitPct) && (avgDeltaMs > avgNoiseFloorMs);
    result.p99Regressed = !RebaselineRequested() && (result.p99ChangePct > result.p99LimitPct) && (p99DeltaMs > p99NoiseFloorMs);
    result.regressed    = result.avgRegressed || result.p99Regressed;

    if (!result.regressed) {
        store.baselineFrames[key] = current;
        store.pendingFrames[key]  = current;
        result.recorded           = true;
    }
    return result;
}

// Flush accepted updates to disk immediately (they are also saved
// automatically when the process exits normally).
inline void SaveBaseline() {
    detail::Store().SaveNow();
}

} // namespace ZHLN::Test::Perf

namespace ZHLN::Test {

// ============================================================================
// Performance baseline verification (see tests/extras/profile/PerfBaseline.hpp).
//
// Records the metric in perf-baseline.json (project root, per machine) and
// fails the current test when it regressed beyond the limit versus the LAST
// recorded run. First run of a metric records the baseline and passes.
//
//   VerifyBaseline("cpu.ecs_dense_iterate", iterDurationMs);
//   VerifyBaseline("render.hw_ray_tracing", durationMs, 30.0); // limit %
//
// Limits can be overridden globally with ZHLN_PERF_REGRESSION_LIMIT
// (percent); a fresh baseline can be forced with ZHLN_PERF_REBASELINE=1.
// The stored value is only updated by PASSING runs, so a regression stays
// visible until it is fixed (or explicitly re-baselined).
// ============================================================================
inline void VerifyBaseline(
    std::string_view     metric,
    double               value,
    double               limitPercent = -1.0,
    Perf::Direction      direction    = Perf::Direction::LowerIsBetter,
    double               noiseFloor   = 0.05,
    std::source_location location     = std::source_location::current()
) {
    const Perf::Result result = Perf::Check(metric, value, limitPercent, direction, noiseFloor);

    if (result.known) {
        const std::string commitInfo = result.commit.empty() ? "" : std::format(" [{}]", result.commit);
        ZHLN::Println(
            "    {}[Baseline]{} {} = {:.3f} (last run{}: {:.3f}, {:+.1f}% vs limit {:+.1f}%){}", result.regressed ? Color::Red : Color::Green,
            Color::Reset, metric, value, commitInfo, result.previous, result.changePct, result.limitPct, result.regressed ? "  << REGRESSION" : ""
        );
    } else {
        const auto        gitHash    = GetGitCommitHash();
        const std::string commitInfo = gitHash.empty() ? "" : std::format(" [{}]", gitHash);
        ZHLN::Println("    {}[Baseline]{} {} = {:.3f} (first run{}, baseline recorded)", Color::Green, Color::Reset, metric, value, commitInfo);
    }

    if (result.regressed) {
        GetThreadLocalContext().failures.push_back(
            {.file          = location.file_name(),
             .line          = static_cast<uint32_t>(location.line()),
             .actualValue   = std::format("{} = {:.3f} ({:+.1f}% vs last run {:.3f})", metric, value, result.changePct, result.previous),
             .expectedValue = std::format("within {:+.1f}% of last run", result.limitPct),
             .op            = "PerfRegression"}
        );
    }
}

// ============================================================================
// Multi-frame benchmark verification (nested percentiles & framerates).
//
// Records the complete FrameMetrics structure under frame_benchmarks[name] in
// perf-baseline.json, providing clean grouping for multi-frame benchmarks.
// ============================================================================
inline void VerifyFrameBaseline(
    std::string_view     benchmarkName,
    const FrameStats&    stats,
    double               avgLimitPercent   = -1.0,
    double               p99LimitPercent   = 75.0,
    double               avgNoiseFloorMs   = 0.1,
    double               p99NoiseFloorMs   = 1.0,
    std::source_location location          = std::source_location::current()
) {
    const Perf::FrameCheckResult result =
        Perf::CheckFrame(benchmarkName, stats, avgLimitPercent, p99LimitPercent, avgNoiseFloorMs, p99NoiseFloorMs);

    const std::string commitInfo = result.commit.empty() ? "" : std::format(" [{}]", result.commit);
    if (result.known) {
        ZHLN::Println(
            "    {}[Baseline]{} {} = {:.3f} ms (last run{}: {:.3f} ms, {:+.1f}% vs limit {:+.1f}%){}",
            result.regressed ? Color::Red : Color::Green, Color::Reset, benchmarkName, stats.avgFrameMs, commitInfo,
            result.previous.avg_frame_ms, result.avgChangePct, result.avgLimitPct,
            result.avgRegressed ? "  << AVG REGRESSION" : (result.p99Regressed ? "  << P99 REGRESSION" : "")
        );
        ZHLN::Println(
            "      {}- Frame Times{} : Avg {:.3f} ms, P50 {:.3f} ms, P95 {:.3f} ms, P99 {:.3f} ms (last: {:.3f} ms, {:+.1f}% vs limit {:+.1f}%){}, P99.9 {:.3f} ms",
            Color::Cyan, Color::Reset, stats.avgFrameMs, stats.p50FrameMs, stats.p95FrameMs, stats.p99FrameMs,
            result.previous.p99_frame_ms, result.p99ChangePct, result.p99LimitPct,
            result.p99Regressed ? " << REGRESSION" : "", stats.p99_9FrameMs
        );
        ZHLN::Println(
            "      {}- Framerates {} : Avg {:.1f} FPS, Max {:.1f} FPS, Min {:.1f} FPS, 1% Low {:.1f} FPS, 0.1% Low {:.1f} FPS",
            Color::Cyan, Color::Reset, stats.avgFps, stats.maxFps, stats.minFps, stats.low1PctFps, stats.low01PctFps
        );
    } else {
        ZHLN::Println(
            "    {}[Baseline]{} {} = {:.3f} ms (first run{}, baseline recorded)",
            Color::Green, Color::Reset, benchmarkName, stats.avgFrameMs, commitInfo
        );
        ZHLN::Println(
            "      {}- Frame Times{} : Avg {:.3f} ms, P50 {:.3f} ms, P95 {:.3f} ms, P99 {:.3f} ms, P99.9 {:.3f} ms",
            Color::Cyan, Color::Reset, stats.avgFrameMs, stats.p50FrameMs, stats.p95FrameMs, stats.p99FrameMs, stats.p99_9FrameMs
        );
        ZHLN::Println(
            "      {}- Framerates {} : Avg {:.1f} FPS, Max {:.1f} FPS, Min {:.1f} FPS, 1% Low {:.1f} FPS, 0.1% Low {:.1f} FPS",
            Color::Cyan, Color::Reset, stats.avgFps, stats.maxFps, stats.minFps, stats.low1PctFps, stats.low01PctFps
        );
    }

    if (result.regressed) {
        std::string failureActual;
        std::string failureExpected;
        if (result.avgRegressed && result.p99Regressed) {
            failureActual = std::format(
                "{} avg={:.3f} ms ({:+.1f}%), p99={:.3f} ms ({:+.1f}%)",
                benchmarkName, stats.avgFrameMs, result.avgChangePct, stats.p99FrameMs, result.p99ChangePct
            );
            failureExpected = std::format("avg within {:+.1f}%, p99 within {:+.1f}%", result.avgLimitPct, result.p99LimitPct);
        } else if (result.avgRegressed) {
            failureActual = std::format(
                "{} avg={:.3f} ms ({:+.1f}% vs last run {:.3f} ms)",
                benchmarkName, stats.avgFrameMs, result.avgChangePct, result.previous.avg_frame_ms
            );
            failureExpected = std::format("avg within {:+.1f}% of last run", result.avgLimitPct);
        } else {
            failureActual = std::format(
                "{} p99={:.3f} ms ({:+.1f}% vs last run {:.3f} ms)",
                benchmarkName, stats.p99FrameMs, result.p99ChangePct, result.previous.p99_frame_ms
            );
            failureExpected = std::format("p99 within {:+.1f}% of last run (noise floor > {:.1f} ms)", result.p99LimitPct, p99NoiseFloorMs);
        }

        GetThreadLocalContext().failures.push_back(
            {.file          = location.file_name(),
             .line          = static_cast<uint32_t>(location.line()),
             .actualValue   = std::move(failureActual),
             .expectedValue = std::move(failureExpected),
             .op            = "PerfRegression"}
        );
    }
}

// ============================================================================
// Modern Benchmarking Framework Execution Harness
// ============================================================================

/// Execution context provided to stateful benchmark bodies.
class BenchmarkState {
public:
    explicit BenchmarkState(size_t maxIterations = 1) noexcept: m_maxIterations(maxIterations) {
    }

    [[nodiscard]] bool KeepRunning() noexcept {
        if (m_currentIteration == 0) {
            m_timer.Reset();
        }
        if (m_currentIteration < m_maxIterations) {
            ++m_currentIteration;
            return true;
        }
        return false;
    }

    void PauseTiming() noexcept {
        m_pauseStart = std::chrono::high_resolution_clock::now();
        m_isPaused   = true;
    }

    void ResumeTiming() noexcept {
        if (m_isPaused) {
            const auto now = std::chrono::high_resolution_clock::now();
            m_pausedOffsetNs += std::chrono::duration<double, std::nano>(now - m_pauseStart).count();
            m_isPaused = false;
        }
    }

    void SetItemsProcessed(uint64_t items) noexcept {
        m_itemsProcessed = items;
    }

    void SetBytesProcessed(uint64_t bytes) noexcept {
        m_bytesProcessed = bytes;
    }

    [[nodiscard]] uint64_t ItemsProcessed() const noexcept {
        return m_itemsProcessed;
    }

    [[nodiscard]] uint64_t BytesProcessed() const noexcept {
        return m_bytesProcessed;
    }

    [[nodiscard]] size_t Iterations() const noexcept {
        return m_maxIterations;
    }

    [[nodiscard]] double ActiveMilliseconds() const noexcept {
        const double elapsedNs = m_timer.ElapsedNanoseconds() - m_pausedOffsetNs;
        return (elapsedNs > 0.0 ? elapsedNs : 0.0) / 1'000'000.0;
    }

private:
    size_t                                         m_maxIterations = 1;
    size_t                                         m_currentIteration = 0;
    BenchmarkTimer                                 m_timer;
    std::chrono::high_resolution_clock::time_point m_pauseStart;
    double                                         m_pausedOffsetNs = 0.0;
    bool                                           m_isPaused       = false;
    uint64_t                                       m_itemsProcessed = 0;
    uint64_t                                       m_bytesProcessed = 0;
};

/// Aggregated statistical outcomes of a benchmark run.
struct BenchmarkStats {
    double   minMs          = 0.0;
    double   medianMs       = 0.0;
    double   avgMs          = 0.0;
    double   maxMs          = 0.0;
    double   stddevMs       = 0.0;
    size_t   samples        = 0;
    size_t   iterations     = 0;
    uint64_t itemsProcessed = 0;
    uint64_t bytesProcessed = 0;

    [[nodiscard]] double ItemsPerSecond() const noexcept {
        if (minMs > 0.0 && itemsProcessed > 0) {
            return (static_cast<double>(itemsProcessed) * 1000.0) / minMs;
        }
        return 0.0;
    }

    [[nodiscard]] double MegaBytesPerSecond() const noexcept {
        if (minMs > 0.0 && bytesProcessed > 0) {
            return (static_cast<double>(bytesProcessed) / (1024.0 * 1024.0)) / (minMs / 1000.0);
        }
        return 0.0;
    }

    [[nodiscard]] double OperationsPerSecond() const noexcept {
        if (minMs > 0.0 && iterations > 0) {
            return (static_cast<double>(iterations) * 1000.0) / minMs;
        }
        return 0.0;
    }
};

/// Fluent benchmark harness for microbenchmarks and subsystem throughput measurements.
class Benchmark {
public:
    explicit Benchmark(std::string_view name) noexcept: m_name(name) {
    }

    Benchmark& Warmup(size_t count) noexcept {
        m_warmupCount = count;
        return *this;
    }

    Benchmark& Samples(size_t count) noexcept {
        m_sampleCount = count > 0 ? count : 1;
        return *this;
    }

    Benchmark& Iterations(size_t count) noexcept {
        m_iterations = count > 0 ? count : 1;
        return *this;
    }

    Benchmark& Limit(double limitPct) noexcept {
        m_limitPercent = limitPct;
        return *this;
    }

    Benchmark& Direction(Perf::Direction dir) noexcept {
        m_direction = dir;
        return *this;
    }

    Benchmark& Items(uint64_t items) noexcept {
        m_itemsProcessed = items;
        return *this;
    }

    Benchmark& Bytes(uint64_t bytes) noexcept {
        m_bytesProcessed = bytes;
        return *this;
    }

    Benchmark& Metric(std::string_view metricKey) noexcept {
        m_metricKey = metricKey;
        return *this;
    }

    // Run with parameterless or non-stateful callable () -> void or () -> T
    template <typename F>
        requires(!std::is_invocable_v<F, BenchmarkState&>)
    auto Run(F&& func, std::source_location location = std::source_location::current()) -> BenchmarkStats {
        // Warmup runs
        for (size_t i = 0; i < m_warmupCount; ++i) {
            for (size_t j = 0; j < m_iterations; ++j) {
                if constexpr (std::is_void_v<std::invoke_result_t<F>>) {
                    func();
                } else {
                    auto res = func();
                    DoNotOptimize(res);
                }
            }
        }

        // Timed sample runs
        std::vector<double> sampleTimesMs;
        sampleTimesMs.reserve(m_sampleCount);

        for (size_t s = 0; s < m_sampleCount; ++s) {
            BenchmarkTimer timer;
            for (size_t j = 0; j < m_iterations; ++j) {
                if constexpr (std::is_void_v<std::invoke_result_t<F>>) {
                    func();
                } else {
                    auto res = func();
                    DoNotOptimize(res);
                }
            }
            sampleTimesMs.push_back(timer.ElapsedMilliseconds() / static_cast<double>(m_iterations));
        }

        return FinalizeAndVerify(sampleTimesMs, location);
    }

    // Run with stateful callable (BenchmarkState&) -> void
    template <typename F>
        requires std::is_invocable_v<F, BenchmarkState&>
    auto Run(F&& func, std::source_location location = std::source_location::current()) -> BenchmarkStats {
        // Warmup runs
        for (size_t i = 0; i < m_warmupCount; ++i) {
            BenchmarkState state(m_iterations);
            func(state);
        }

        // Timed sample runs
        std::vector<double> sampleTimesMs;
        sampleTimesMs.reserve(m_sampleCount);

        for (size_t s = 0; s < m_sampleCount; ++s) {
            BenchmarkState state(m_iterations);
            func(state);
            const double activeMs = state.ActiveMilliseconds();
            sampleTimesMs.push_back(activeMs / static_cast<double>(m_iterations));
            if (state.ItemsProcessed() > 0) {
                m_itemsProcessed = state.ItemsProcessed();
            }
            if (state.BytesProcessed() > 0) {
                m_bytesProcessed = state.BytesProcessed();
            }
        }

        return FinalizeAndVerify(sampleTimesMs, location);
    }

private:
    auto FinalizeAndVerify(std::span<const double> sampleTimesMs, std::source_location location) -> BenchmarkStats {
        std::vector<double> sorted(sampleTimesMs.begin(), sampleTimesMs.end());
        std::ranges::sort(sorted);

        const size_t n        = sorted.size();
        const double minMs    = sorted.front();
        const double medianMs = sorted[n / 2];
        const double maxMs    = sorted.back();
        const double sum      = std::accumulate(sorted.begin(), sorted.end(), 0.0);
        const double avgMs    = sum / static_cast<double>(n);

        double varSum = 0.0;
        for (double val: sorted) {
            varSum += (val - avgMs) * (val - avgMs);
        }
        const double stddevMs = (n > 1) ? std::sqrt(varSum / static_cast<double>(n - 1)) : 0.0;

        BenchmarkStats stats {
            .minMs          = minMs,
            .medianMs       = medianMs,
            .avgMs          = avgMs,
            .maxMs          = maxMs,
            .stddevMs       = stddevMs,
            .samples        = n,
            .iterations     = m_iterations,
            .itemsProcessed = m_itemsProcessed,
            .bytesProcessed = m_bytesProcessed,
        };

        const std::string metricKey = m_metricKey.empty() ? m_name : m_metricKey;
        VerifyBaseline(metricKey, stats.minMs, m_limitPercent, m_direction, m_noiseFloor, location);

        return stats;
    }

    std::string     m_name;
    std::string     m_metricKey;
    size_t          m_warmupCount    = 2;
    size_t          m_sampleCount    = 5;
    size_t          m_iterations     = 1;
    double          m_limitPercent   = -1.0;
    Perf::Direction m_direction      = Perf::Direction::LowerIsBetter;
    double          m_noiseFloor     = 0.05;
    uint64_t        m_itemsProcessed = 0;
    uint64_t        m_bytesProcessed = 0;
};

/// Fluent benchmark harness for multi-frame real-time loops (CPU integration & GPU rendering).
class BenchmarkFrames {
public:
    explicit BenchmarkFrames(std::string_view name) noexcept: m_name(name) {
    }

    BenchmarkFrames& Warmup(size_t frameCount) noexcept {
        m_warmupFrames = frameCount;
        return *this;
    }

    BenchmarkFrames& Frames(size_t frameCount) noexcept {
        m_frameCount = frameCount > 0 ? frameCount : 1;
        return *this;
    }

    BenchmarkFrames& AvgLimit(double limitPct) noexcept {
        m_avgLimitPercent = limitPct;
        return *this;
    }

    BenchmarkFrames& P99Limit(double limitPct) noexcept {
        m_p99LimitPercent = limitPct;
        return *this;
    }

    BenchmarkFrames& AvgNoiseFloor(double ms) noexcept {
        m_avgNoiseFloorMs = ms;
        return *this;
    }

    BenchmarkFrames& P99NoiseFloor(double ms) noexcept {
        m_p99NoiseFloorMs = ms;
        return *this;
    }

    template <typename F>
    auto Run(F&& renderFrameFunc, std::source_location location = std::source_location::current()) -> FrameStats {
        // Warmup frames (untimed)
        for (size_t f = 0; f < m_warmupFrames; ++f) {
            if constexpr (std::is_invocable_v<F, uint32_t>) {
                renderFrameFunc(static_cast<uint32_t>(f));
            } else {
                renderFrameFunc();
            }
        }

        // Measured frames
        std::vector<double> frameTimesMs;
        frameTimesMs.reserve(m_frameCount);

        for (size_t f = 0; f < m_frameCount; ++f) {
            BenchmarkTimer frameTimer;
            if constexpr (std::is_invocable_v<F, uint32_t>) {
                renderFrameFunc(static_cast<uint32_t>(f));
            } else {
                renderFrameFunc();
            }
            frameTimesMs.push_back(frameTimer.ElapsedMilliseconds());
        }

        FrameStats stats = CalculateFrameStats(frameTimesMs);
        VerifyFrameBaseline(m_name, stats, m_avgLimitPercent, m_p99LimitPercent, m_avgNoiseFloorMs, m_p99NoiseFloorMs, location);
        return stats;
    }

private:
    std::string m_name;
    size_t      m_warmupFrames    = 0;
    size_t      m_frameCount      = 120;
    double      m_avgLimitPercent = -1.0;
    double      m_p99LimitPercent = 75.0;
    double      m_avgNoiseFloorMs = 0.1;
    double      m_p99NoiseFloorMs = 1.0;
};

} // namespace ZHLN::Test
