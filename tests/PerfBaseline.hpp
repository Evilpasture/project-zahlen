// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/PerfBaseline.hpp
//
// Performance baseline caching for TestPerformance / TestRenderPerformance,
// built on the engine's own JSON module (ReflectJSON::Document to read,
// ReflectJSON::Value to write), ZHLN::Mutex and ZHLN::Println.
//
// Every benchmark metric is recorded to perf-baseline.json at the PROJECT
// ROOT (ZHLN_PROJECT_ROOT, defined once in the root CMakeLists), so
// baselines survive deleting or reconfiguring the build tree. Each run
// compares against the LAST recorded value; a regression beyond the limit
// fails the test via ZHLN::Test::VerifyBaseline (TestsFramework.hpp).
//
// Storage layout (one file, many machines):
//   { "_version": 1,
//     "<machine key>": { "<metric name>": <number>, ... }, ... }
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

#include <Zahlen/Config.hpp>
#include <Zahlen/JSON.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/Mutex.hpp>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#if !defined(_WIN32)
    #include <unistd.h> // gethostname
#endif

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
// Baseline file I/O (via the engine JSON module)
// ============================================================================

using MetricMap = std::map<std::string, double>;
using MachineMap = std::map<std::string, MetricMap>;

// Reads one machine's metrics out of the baseline file. A missing file or
// missing machine section is not an error (fresh baseline); an unparsable
// file warns on stderr and also yields a fresh baseline.
[[nodiscard]] inline auto LoadMachineMetrics(const std::filesystem::path& path, const std::string& machine) -> MetricMap {
    std::ifstream file {path, std::ios::binary};
    if (!file) {
        return {}; // no baseline yet
    }
    const std::string text {(std::istreambuf_iterator<char> {file}), std::istreambuf_iterator<char> {}};

    auto document = ReflectJSON::Document::Parse(text);
    if (!document) {
        ZHLN::Println(stderr, "[perf-baseline] {} is malformed; starting a fresh baseline", path.string());
        return {};
    }

    // Missing machine key (JSONError::MissingField) = fresh baseline.
    auto section = document->GetRoot().GetKey(machine);
    if (!section) {
        return {};
    }

    auto keys = section->GetObjectKeys();
    if (!keys) {
        ZHLN::Println(stderr, "[perf-baseline] machine section is not an object; starting a fresh baseline");
        return {};
    }

    MetricMap metrics;
    for (const std::string_view key: *keys) {
        auto field = section->GetKey(key);
        if (!field) {
            continue;
        }
        auto number = field->GetDouble();
        if (number && *number > 0.0) {
            metrics.emplace(std::string {key}, *number);
        }
    }
    return metrics;
}

// Serialises every machine's metrics through ReflectJSON::Value.
[[nodiscard]] inline auto SerializeMachines(const MachineMap& machines) -> std::string {
    auto root = ReflectJSON::Value::Object();
    (void) root.Set("_version", ReflectJSON::Value::Number(1));

    for (const auto& [machine, metrics]: machines) {
        auto section = ReflectJSON::Value::Object();
        for (const auto& [metric, value]: metrics) {
            (void) section.Set(metric, ReflectJSON::Value::Number(value));
        }
        (void) root.Set(machine, std::move(section));
    }
    return root.Stringify();
}

// Writes `text` to `path` atomically (tmp file + rename).
inline void WriteBaselineAtomically(const std::filesystem::path& path, const std::string& text) {
    try {
        const std::filesystem::path temporary {path.string() + ".tmp"};
        {
            std::ofstream file {temporary, std::ios::binary | std::ios::trunc};
            if (!file) {
                ZHLN::Println(stderr, "[perf-baseline] cannot open {} for writing", temporary.string());
                return;
            }
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
        }
        std::filesystem::rename(temporary, path); // throws on failure
    } catch (const std::filesystem::filesystem_error& error) {
        ZHLN::Println(stderr, "[perf-baseline] writing {} failed: {}", path.string(), error.what());
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
        ZHLN::Mutex  mutex;
        MetricMap    baseline; // this machine's last-known values
        MetricMap    pending;  // this process's accepted updates
        bool         loaded = false;

        ~BaselineStore() {
            SaveNow();
        }

        void SaveNow() {
            const std::lock_guard<ZHLN::Mutex> lock {mutex};

            // Re-read the whole file and replace only OUR machine's section.
            MachineMap machines;
            {
                std::ifstream file {BaselineFilePath(), std::ios::binary};
                if (file) {
                    const std::string text {(std::istreambuf_iterator<char> {file}), std::istreambuf_iterator<char> {}};
                    if (auto document = ReflectJSON::Document::Parse(text); document) {
                        if (auto machinesOnDisk = document->GetRoot().GetObjectKeys(); machinesOnDisk) {
                            for (const std::string_view machine: *machinesOnDisk) {
                                if (machine == "_version") {
                                    continue;
                                }
                                auto section = document->GetRoot().GetKey(machine);
                                if (!section) {
                                    continue;
                                }
                                MetricMap metrics;
                                if (auto metricKeys = section->GetObjectKeys(); metricKeys) {
                                    for (const std::string_view metric: *metricKeys) {
                                        auto field = section->GetKey(metric);
                                        if (!field) {
                                            continue;
                                        }
                                        auto number = field->GetDouble();
                                        if (number && *number > 0.0) {
                                            metrics.emplace(std::string {metric}, *number);
                                        }
                                    }
                                }
                                machines.emplace(std::string {machine}, std::move(metrics));
                            }
                        }
                    }
                }
            }
            machines[MachineKey()] = pending; // our accepted values win

            WriteBaselineAtomically(BaselineFilePath(), SerializeMachines(machines));
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

    auto&                             store = detail::Store();
    const std::lock_guard<ZHLN::Mutex> lock {store.mutex};

    if (!store.loaded) {
        store.baseline = LoadMachineMetrics(BaselineFilePath(), MachineKey());
        store.loaded   = true;
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
