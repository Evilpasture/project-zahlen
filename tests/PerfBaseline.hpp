// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/PerfBaseline.hpp
//
// Performance baseline caching for TestPerformance / TestRenderPerformance.
//
// Every benchmark metric is recorded to perf-baseline.json at the PROJECT
// ROOT (not the build directory), so baselines survive deleting or
// reconfiguring the build tree. On each subsequent run the metric is
// compared against the LAST recorded value; a regression beyond the limit
// fails the test through the normal ZHLN_PERF_CHECK macro (TestsFramework).
//
// Storage layout (one file, many machines):
//   {
//     "_version": 1,
//     "<machine key>": { "<metric name>": <double>, ... },
//     ...
//   }
// The machine key is `hostname | compiler | build profile`, so baselines
// from different machines, toolchains, Debug/Release builds (and ASan
// builds) never cross-contaminate — an unknown key simply starts a fresh
// baseline. The file is machine-local and gitignored.
//
// Policy (per metric, individually):
//   * No previous value  -> record it, pass ("first run").
//   * Regression beyond the limit -> FAIL, and do NOT overwrite the stored
//     value (a failing run must not launder itself into the new baseline).
//   * Otherwise -> pass and record (the stored value is always "the last
//     passing run", which is what the next run compares against).
//
// Environment knobs:
//   ZHLN_PERF_REGRESSION_LIMIT  global limit override in percent (e.g. "40")
//   ZHLN_PERF_REBASELINE=1      record everything without failing (after a
//                               hardware/toolchain upgrade)
//   ZHLN_PERF_BASELINE=<path>   use an explicit baseline file instead of the
//                               project root
//
// This header is deliberately self-contained (standard library only, no
// Zahlen headers) so it can be unit-tested in isolation and included from
// anywhere, including TestsFramework.hpp.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#if !defined(_WIN32)
    #include <unistd.h> // gethostname
#endif

namespace ZHLN::Test::Perf {

// ============================================================================
// Configuration
// ============================================================================

// Limit used when a check does not pass an explicit one. Percent, in the
// "worse" direction (a lower-is-better metric degrading by 40% is +40%).
[[nodiscard]] inline double DefaultLimitPercent() noexcept {
    if (const char* env = std::getenv("ZHLN_PERF_REGRESSION_LIMIT")) {
        const double parsed = std::atof(env);
        if (parsed > 0.0 && parsed <= 1000.0) {
            return parsed;
        }
    }
    return 30.0;
}

// ZHLN_PERF_REBASELINE set to anything non-empty/non-zero: record every
// metric without failing (re-baseline pass).
[[nodiscard]] inline bool RebaselineRequested() noexcept {
    const char* env = std::getenv("ZHLN_PERF_REBASELINE");
    return env != nullptr && *env != '\0' && std::strcmp(env, "0") != 0;
}

// Where the baseline lives. ZHLN_PROJECT_ROOT is injected by CMake
// (tests/CMakeLists.txt, tests/render/CMakeLists.txt) as the project source
// root, so the file lands next to CMakeLists.txt regardless of where the
// build directory (and thus the test's working directory) is.
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

// Identifies the configuration a baseline belongs to. Timing from different
// compilers, build profiles or sanitizer builds must never be compared.
[[nodiscard]] inline auto MachineKey() -> std::string {
    char host[64] {"unknown-host"};
#if defined(_WIN32)
    if (const char* computer = std::getenv("COMPUTERNAME"); computer != nullptr && *computer != '\0') {
        std::snprintf(host, sizeof host, "%s", computer);
    }
#else
    ::gethostname(host, sizeof host - 1); // POSIX; -1 reserves room for the NUL
#endif

    const char* compiler = "unknown-compiler";
#if defined(__clang__)
    compiler = "Clang " __clang_version__; // NOLINT: MSVC needs the literal, not a ternary
#elif defined(__GNUC__)
    compiler = "GCC " __VERSION__;
#elif defined(_MSC_VER)
    compiler = "MSVC " _CRT_STRINGIZE(_MSC_VER);
#endif

    std::string key = std::string {host} + " | " + compiler;
#if defined(ZHLN_BUILD_PROFILE)
    key += " | " + std::string {ZHLN_BUILD_PROFILE};
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__ASAN_ENABLED__)
    key += " | ASan";
#endif
    return key;
}

// ============================================================================
// Minimal JSON (flat two-level: string -> (string -> number)).
// Written for, and only guaranteed for, this file's own output format.
// ============================================================================

namespace detail {

    [[nodiscard]] inline auto EscapeJson(std::string_view text) -> std::string {
        std::string out;
        out.reserve(text.size() + 2);
        for (const char c: text) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\t': out += "\\t";  break;
                case '\r': out += "\\r";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(c));
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        return out;
    }

    inline void AppendDouble(std::string& out, double value) {
        char buf[64];
        const int n = std::snprintf(buf, sizeof buf, "%.17g", value); // exact double round-trip
        if (n > 0) {
            out.append(buf, static_cast<size_t>(n));
        }
    }

    // Recursive-descent parser for the shape produced by SerializeBaseline.
    // Any deviation (trailing junk, broken escapes, non-numeric metric
    // values) is reported as a parse failure so the caller can fall back to
    // an empty baseline instead of trusting half-read numbers.
    [[nodiscard]] inline auto ParseBaseline(std::string_view text, std::map<std::string, std::map<std::string, double>>& out) -> bool {
        const size_t      size = text.size();
        size_t            i    = 0;

        const auto skipWs = [&] {
            while (i < size && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) {
                ++i;
            }
        };
        const auto parseString = [&](std::string& into) -> bool {
            skipWs();
            if (i >= size || text[i] != '"') {
                return false;
            }
            ++i;
            std::string parsed;
            while (i < size) {
                const char c = text[i++];
                if (c == '"') {
                    into = std::move(parsed);
                    return true;
                }
                if (c == '\\') {
                    if (i >= size) {
                        return false;
                    }
                    switch (const char e = text[i++]) {
                        case '"':  parsed += '"';  break;
                        case '\\': parsed += '\\'; break;
                        case '/':  parsed += '/';  break;
                        case 'n':  parsed += '\n'; break;
                        case 't':  parsed += '\t'; break;
                        case 'r':  parsed += '\r'; break;
                        default:   (void) e;      return false; // \uXXXX etc: not produced by our writer
                    }
                } else {
                    parsed += c;
                }
            }
            return false;
        };
        const auto parseNumber = [&](double& into) -> bool {
            skipWs();
            const size_t start = i;
            if (i < size && (text[i] == '-' || text[i] == '+')) {
                ++i;
            }
            while (i < size && ((text[i] >= '0' && text[i] <= '9') || text[i] == '.' || text[i] == 'e' || text[i] == 'E' || text[i] == '-' || text[i] == '+')) {
                ++i;
            }
            if (i == start) {
                return false;
            }
            const std::string numberText {text.substr(start, i - start)};
            char*             end = nullptr;
            const double      v    = std::strtod(numberText.c_str(), &end);
            if (end == nullptr || *end != '\0') {
                return false;
            }
            into = v;
            return true;
        };

        skipWs();
        if (i >= size || text[i++] != '{') {
            return false;
        }
        skipWs();
        if (i < size && text[i] == '}') {
            return true; // empty object
        }
        while (true) {
            std::string key;
            if (!parseString(key)) {
                return false;
            }
            skipWs();
            if (i >= size || text[i++] != ':') {
                return false;
            }
            skipWs();
            if (i < size && text[i] == '{') {
                ++i;
                skipWs();
                std::map<std::string, double> metrics;
                if (i < size && text[i] == '}') {
                    ++i;
                } else {
                    while (true) {
                        std::string metric;
                        double      value = 0.0;
                        if (!parseString(metric)) {
                            return false;
                        }
                        skipWs();
                        if (i >= size || text[i++] != ':') {
                            return false;
                        }
                        if (!parseNumber(value)) {
                            return false;
                        }
                        metrics[std::move(metric)] = value;
                        skipWs();
                        if (i < size && text[i] == ',') {
                            ++i;
                            continue;
                        }
                        if (i < size && text[i] == '}') {
                            ++i;
                            break;
                        }
                        return false;
                    }
                }
                out[std::move(key)] = std::move(metrics);
            } else {
                // Top-level scalar (the "_version" marker): accepted, unused.
                double ignored = 0.0;
                if (!parseNumber(ignored)) {
                    return false;
                }
            }
            skipWs();
            if (i < size && text[i] == ',') {
                ++i;
                continue;
            }
            if (i < size && text[i] == '}') {
                ++i;
                break;
            }
            return false;
        }
        skipWs();
        return i == size;
    }

    [[nodiscard]] inline auto
        SerializeBaseline(const std::map<std::string, std::map<std::string, double>>& machines) -> std::string {
        std::string out = "{\n  \"_version\": 1";
        for (const auto& [machine, metrics]: machines) {
            out += ",\n  \"" + EscapeJson(machine) + "\": {";
            bool first = true;
            for (const auto& [metric, value]: metrics) {
                if (!first) {
                    out += ",";
                }
                first = false;
                out += "\n    \"" + EscapeJson(metric) + "\": ";
                AppendDouble(out, value);
            }
            out += "\n  }";
        }
        out += "\n}\n";
        return out;
    }

    [[nodiscard]] inline auto LoadBaselineFile(const std::filesystem::path& path, std::map<std::string, std::map<std::string, double>>& out) -> bool {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) {
            return false;
        }
        std::ifstream file {path, std::ios::binary};
        if (!file) {
            return false;
        }
        std::string text {(std::istreambuf_iterator<char> {file}), std::istreambuf_iterator<char> {}};
        if (!ParseBaseline(text, out)) {
            std::fprintf(stderr, "[perf-baseline] %s is malformed; starting a fresh baseline\n", path.string().c_str());
            out.clear();
            return false;
        }
        return true;
    }

} // namespace detail

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

    // One per process. Pending updates are merged over a FRESH copy of the
    // file at save time, so two test executables sharing one baseline file
    // (TestPerformance + TestRenderPerformance under `ctest -j`) cannot
    // clobber each other's metrics.
    struct BaselineStore {
        std::mutex                    mu;
        std::map<std::string, double> baseline; // this machine's last-known values
        std::map<std::string, double> pending;  // this process's accepted updates
        bool                          loaded = false;

        ~BaselineStore() {
            SaveNow();
        }

        void SaveNow() {
            const std::lock_guard<std::mutex> lock {mu};
            const auto                        path = BaselineFilePath();
            std::map<std::string, std::map<std::string, double>> all;
            (void) detail::LoadBaselineFile(path, all); // missing/malformed -> start fresh
            all[MachineKey()] = pending;                // our accepted values win
            const std::string text = detail::SerializeBaseline(all);

            const auto tmp = path;
            const auto tmpPath = tmp.string() + ".tmp";
            {
                std::ofstream file {tmpPath, std::ios::binary | std::ios::trunc};
                if (!file) {
                    std::fprintf(stderr, "[perf-baseline] cannot open %s for writing\n", tmpPath.c_str());
                    return;
                }
                file.write(text.data(), static_cast<std::streamsize>(text.size()));
                if (!file) {
                    std::fprintf(stderr, "[perf-baseline] short write to %s\n", tmpPath.c_str());
                    return;
                }
            }
            std::error_code ec;
#if defined(_WIN32)
            std::filesystem::remove(path, ec); // rename() refuses existing targets on Windows
            ec.clear();
#endif
            std::filesystem::rename(tmpPath, path, ec);
            if (ec) {
                std::fprintf(stderr, "[perf-baseline] rename failed: %s\n", ec.message().c_str());
            }
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

    auto& store = detail::Store();
    const std::lock_guard<std::mutex> lock {store.mu};

    if (!store.loaded) {
        std::map<std::string, std::map<std::string, double>> all;
        (void) detail::LoadBaselineFile(BaselineFilePath(), all);
        const auto it = all.find(MachineKey());
        if (it != all.end()) {
            store.baseline = it->second;
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
