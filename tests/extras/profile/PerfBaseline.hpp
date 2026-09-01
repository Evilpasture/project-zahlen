// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/profile/PerfBaseline.hpp
//
// Performance baseline caching for TestPerformance / TestRenderPerformance,
// built on the JSON extra (ReflectJSON::TryParse to read,
// ZHLN::Reflect::SerializeJSON to write), ZHLN::Mutex and ZHLN::Println.
//
// Every benchmark metric is recorded to perf-baseline.json at the PROJECT
// ROOT (ZHLN_PROJECT_ROOT, defined once in the root CMakeLists), so
// baselines survive deleting or reconfiguring the build tree. Each run
// compares against the LAST recorded value; a regression beyond the limit
// fails the test via ZHLN::Test::VerifyBaseline (TestsFramework.hpp).
//
// Storage: one reflected struct (PerfBaselineFile below) serialised through
// ZHLN::Reflect::SerializeJSON and read back with ReflectJSON::TryParse --
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <json/JSON.hpp>
#include <map>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(_WIN32)
    #include <unistd.h> // gethostname
#endif

namespace ZHLN::Test {

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

struct MachineBaselines {
    std::map<std::string, double> metrics;
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

    auto parsed = ReflectJSON::TryParse<PerfBaselineFile>(text);
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
    bool   known     = false; // a previous value existed for this metric
    double previous  = 0.0;   // that previous value (0.0 when !known)
    double changePct = 0.0;   // signed change in the WORSE direction (+ = worse)
    double limitPct  = 0.0;   // limit that was applied
    bool   regressed = false; // exceeded the limit; the test must fail
    bool   recorded  = false; // this run's value became the new baseline
};

namespace detail {

    // One per process. Accepted updates are merged over a FRESH copy of the
    // file at save time, so two test executables sharing one baseline file
    // (TestPerformance + TestRenderPerformance under `ctest -j`) cannot
    // clobber each other's metrics.
    struct BaselineStore {
        ZHLN::Mutex                       mutex;
        std::map<std::string, double>     baseline; // this machine's last-known values
        std::map<std::string, double>     pending;  // this process's accepted updates
        bool                              loaded = false;

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
            auto&            ours = file.machines[MachineKey()].metrics;
            for (const auto& [metric, value]: pending) {
                ours[metric] = value;
            }

            WriteBaselineAtomically(BaselineFilePath(), Reflect::SerializeJSON(file, 2));
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
[[nodiscard]] inline auto Check(std::string_view metric, double value, double limitPct = -1.0, Direction direction = Direction::LowerIsBetter) -> Result {
    Result result;
    result.limitPct = limitPct > 0.0 ? limitPct : DefaultLimitPercent();

    auto&                store = detail::Store();
    const MutexGuard     lock(store.mutex);

    if (!store.loaded) {
        const PerfBaselineFile file = LoadBaselineFile(BaselineFilePath());
        const auto             ours = file.machines.find(MachineKey());
        if (ours != file.machines.end()) {
            store.baseline = ours->second.metrics;
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
        return result;
    }

    result.known     = true;
    result.previous  = previous->second;
    const double delta = (direction == Direction::LowerIsBetter) ? (value - result.previous) : (result.previous - value);
    result.changePct = 100.0 * delta / result.previous;
    result.regressed = !RebaselineRequested() && result.changePct > result.limitPct;

    if (!result.regressed) {
        store.baseline[key] = value;
        store.pending[key]  = value;
        result.recorded     = true;
    }
    // Regressed: keep the previous value as the baseline so a failing run
    // cannot launder itself into the next comparison.
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
    std::string_view                    metric,
    double                              value,
    double                              limitPercent      = -1.0,
    Perf::Direction                     direction         = Perf::Direction::LowerIsBetter,
    std::source_location                location          = std::source_location::current()
) {
    const Perf::Result result = Perf::Check(metric, value, limitPercent, direction);

    if (result.known) {
        ZHLN::Println(
            "    {}[Baseline]{} {} = {:.3f} (last run: {:.3f}, {:+.1f}% vs limit {:+.1f}%){}", result.regressed ? Color::Red : Color::Green,
            Color::Reset, metric, value, result.previous, result.changePct, result.limitPct, result.regressed ? "  << REGRESSION" : ""
        );
    } else {
        ZHLN::Println("    {}[Baseline]{} {} = {:.3f} (first run, baseline recorded)", Color::Green, Color::Reset, metric, value);
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

} // namespace ZHLN::Test
