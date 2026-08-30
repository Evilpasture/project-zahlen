// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <atomic>
#include <concepts>
#include <cstdlib>
#include <expected>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Render diagnostics: the framework owns the persistent counters. They are
// registered (RunSuite) as the process diagnostics sink, so every engine --
// including its teardown, where the persistent messenger fires destroy-time
// validation events -- increments them directly. That is what lets the
// per-test before/after snapshots below bracket a WHOLE engine lifecycle and
// stay exact, with no post-mortem state in the library. The public
// RenderContext::ValidationErrorCount()/DeviceLostCount() are live views
// (zero while no engine exists) and are meant for workload-scoped deltas.
#include <Zahlen/Render.hpp>

// Performance baseline caching (self-contained; see PerfBaseline.hpp).
#include "PerfBaseline.hpp"

#if defined(__unix__) || defined(__APPLE__) || defined(__linux__)
#define ZHLN_TEST_TIMEOUT_SUPPORTED 1
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
inline bool IsDebuggerAttached() noexcept {
    int               mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc info {};
    size_t            size = sizeof(info);
    if (sysctl(mib, 4, &info, &size, nullptr, 0) == 0) {
        return (info.kp_proc.p_flag & P_TRACED) != 0;
    }
    return false;
}
#elif defined(__linux__)
#include <fstream>
#include <string>
inline bool IsDebuggerAttached() noexcept {
    std::ifstream file("/proc/self/status");
    std::string   line;
    while (std::getline(file, line)) {
        if (line.starts_with("TracerPid:")) {
            size_t first = line.find_first_not_of(" \t", 10);
            if (first != std::string::npos) {
                std::string val = line.substr(first);
                return !val.empty() && val != "0" && val != "0\n";
            }
        }
    }
    return false;
}
#else
inline bool IsDebuggerAttached() noexcept {
    return false;
}
#endif

inline sigjmp_buf g_testTimeoutJmpBuf;

inline void TestTimeoutSignalHandler(int sig) {
    if (sig == SIGALRM) {
        siglongjmp(g_testTimeoutJmpBuf, 1);
    }
}
#endif

namespace ZHLN::Test {

// Diagnostics totals owned by this framework. They outlast every engine in
// the process, which is exactly why the engine increments them directly
// (registered in RunSuite via RenderContext::UseDiagnostics): teardown-time
// validation events land here too, and the per-test snapshots in RunSuite
// observe a whole engine lifecycle without any post-mortem state in the
// library.
inline std::atomic<uint32_t> g_validationErrors {0};
inline std::atomic<uint32_t> g_deviceLost {0};

enum class TestFrameworkError : uint8_t {
    AssertionFailed[[= ZHLN::Description<"One or more assertions failed in this test. ">{}]] = 1,
};

struct AssertionFailure {
    std::string_view file;
    uint32_t         line;
    std::string      actualValue;
    std::string      expectedValue;
    std::string_view op; // "==" or "!=" or "true" or "false" or "ValidationError" or "DeviceLost" or "PerfRegression"
};

// ============================================================================
// Performance baseline check.
//
// Records the metric in perf-baseline.json (project root, per machine) and
// fails the current test when it regressed beyond the limit versus the LAST
// recorded run. First run of a metric records the baseline and passes.
//
//   ZHLN_PERF_CHECK("cpu.ecs_dense_iterate", iterDurationMs);
//   ZHLN_PERF_CHECK("render.hw_ray_tracing", durationMs, 30.0); // limit %
//
// `value` is evaluated multiple times; pass a pure expression. Limits can
// be overridden globally with ZHLN_PERF_REGRESSION_LIMIT (percent); a fresh
// baseline can be forced with ZHLN_PERF_REBASELINE=1. The stored value is
// only updated by PASSING runs, so a regression stays visible until it is
// fixed (or explicitly re-baselined).
// ============================================================================
#define ZHLN_PERF_CHECK(metric, value, ...)                                                                                            \
    do {                                                                                                                               \
        const ::ZHLN::Test::Perf::Result _zhlnPerf = ::ZHLN::Test::Perf::Check((metric), static_cast<double>(value) __VA_OPT__(, __VA_ARGS__)); \
        if (_zhlnPerf.known) {                                                                                                         \
            ZHLN::Println(                                                                                                             \
                "    {}[Baseline]{} {} -> {:.3f} (last run: {:.3f}, {:+.1f}% vs limit {:+.1f}%){}",                                     \
                _zhlnPerf.regressed ? ZHLN::Color::Red : ZHLN::Color::Green, ZHLN::Color::Reset, (metric), (value), _zhlnPerf.previous, \
                _zhlnPerf.changePct, _zhlnPerf.limitPct, _zhlnPerf.regressed ? "  << REGRESSION" : ""                                   \
            );                                                                                                                         \
        } else {                                                                                                                       \
            ZHLN::Println("    {}[Baseline]{} {} = {:.3f} (first run, baseline recorded)", ZHLN::Color::Green, ZHLN::Color::Reset, (metric), (value)); \
        }                                                                                                                              \
        if (_zhlnPerf.regressed) {                                                                                                     \
            ::ZHLN::Test::GetThreadLocalContext().failures.push_back(                                                                  \
                {.file          = __FILE__,                                                                                            \
                 .line          = static_cast<uint32_t>(__LINE__),                                                                     \
                 .actualValue   = std::format("{} = {} ({:+.1f}% vs last run {})", (metric), (value), _zhlnPerf.changePct, _zhlnPerf.previous), \
                 .expectedValue = std::format("within {:+.1f}% of last run", _zhlnPerf.limitPct),                                      \
                 .op            = "PerfRegression"}                                                                                   \
            );                                                                                                                         \
        }                                                                                                                              \
    } while (false)

inline unsigned int GetDefaultTimeoutSeconds() noexcept {
    static unsigned int defaultSec = []() -> unsigned int {
        if (const char* env = std::getenv("ZHLN_TEST_TIMEOUT")) {
            char* end = nullptr;
            long  val = std::strtol(env, &end, 10);
            if (end != env && val >= 0) {
                return static_cast<unsigned int>(val);
            }
        }
        return 15;
    }();
    return defaultSec;
}

struct TestContext {
    std::string_view              currentTestName;
    std::vector<AssertionFailure> failures;
    bool                          allowValidationErrors = false;
    bool                          allowDeviceLost       = false;
    unsigned int                  timeoutSeconds        = 15;

    void Reset(std::string_view testName) {
        currentTestName = testName;
        failures.clear();
        allowValidationErrors = false;
        allowDeviceLost       = false;
        timeoutSeconds        = GetDefaultTimeoutSeconds();
    }
};

inline TestContext& GetThreadLocalContext() noexcept {
    thread_local TestContext ctx;
    return ctx;
}

// Escape-hatches for tests deliberately provoking errors/hangs (e.g. testing recovery)
inline void AllowValidationErrors(bool allow = true) noexcept {
    GetThreadLocalContext().allowValidationErrors = allow;
}

inline void AllowDeviceLost(bool allow = true) noexcept {
    GetThreadLocalContext().allowDeviceLost = allow;
}

// Dynamic test timeout adjustment
inline void SetTimeout(unsigned int seconds) noexcept {
    GetThreadLocalContext().timeoutSeconds = seconds;
#if defined(ZHLN_TEST_TIMEOUT_SUPPORTED)
    if (!IsDebuggerAttached()) {
        alarm(seconds);
    }
#endif
}

inline void DisableTimeout() noexcept {
    SetTimeout(0);
}

template <typename T>
std::string FormatValue(const T& val) {
    return ZHLN::Reflect::ToDebugString(val);
}

// Non-aborting Expectations
template <typename T1, typename T2>
bool ExpectEq(const T1& actual, const T2& expected, std::source_location loc = std::source_location::current()) {
    if constexpr (requires { actual == expected; }) {
        if (actual == expected) {
            return true;
        }
    } else {
        static_assert(sizeof(T1) == 0, "Types are not comparable for equality!");
        return false;
    }

    auto& ctx = GetThreadLocalContext();
    ctx.failures.push_back(
        {.file = loc.file_name(), .line = loc.line(), .actualValue = FormatValue(actual), .expectedValue = FormatValue(expected), .op = "=="}
    );
    return false;
}

template <typename T1, typename T2>
bool ExpectNe(const T1& actual, const T2& expected, std::source_location loc = std::source_location::current()) {
    if constexpr (requires { actual != expected; }) {
        if (actual != expected) {
            return true;
        }
    } else {
        static_assert(sizeof(T1) == 0, "Types are not comparable for inequality!");
        return false;
    }

    auto& ctx = GetThreadLocalContext();
    ctx.failures.push_back(
        {.file = loc.file_name(), .line = loc.line(), .actualValue = FormatValue(actual), .expectedValue = FormatValue(expected), .op = "!="}
    );
    return false;
}

inline bool ExpectTrue(bool condition, std::source_location loc = std::source_location::current()) {
    if (condition) {
        return true;
    }
    auto& ctx = GetThreadLocalContext();
    ctx.failures.push_back({.file = loc.file_name(), .line = loc.line(), .actualValue = "false", .expectedValue = "true", .op = "true"});
    return false;
}

inline bool ExpectFalse(bool condition, std::source_location loc = std::source_location::current()) {
    if (!condition) {
        return true;
    }
    auto& ctx = GetThreadLocalContext();
    ctx.failures.push_back({.file = loc.file_name(), .line = loc.line(), .actualValue = "true", .expectedValue = "false", .op = "false"});
    return false;
}

// Aborting Assertions
template <typename T1, typename T2>
[[nodiscard]] std::expected<void, ZHLN::Error> AssertEq(const T1& actual, const T2& expected, std::source_location loc = std::source_location::current()) {
    if (ExpectEq(actual, expected, loc)) {
        return {};
    }
    return std::unexpected(ZHLN::Error(TestFrameworkError::AssertionFailed));
}

template <typename T1, typename T2>
[[nodiscard]] std::expected<void, ZHLN::Error> AssertNe(const T1& actual, const T2& expected, std::source_location loc = std::source_location::current()) {
    if (ExpectNe(actual, expected, loc)) {
        return {};
    }
    return std::unexpected(ZHLN::Error(TestFrameworkError::AssertionFailed));
}

[[nodiscard]] inline std::expected<void, ZHLN::Error> AssertTrue(bool condition, std::source_location loc = std::source_location::current()) {
    if (ExpectTrue(condition, loc)) {
        return {};
    }
    return std::unexpected(ZHLN::Error(TestFrameworkError::AssertionFailed));
}

[[nodiscard]] inline std::expected<void, ZHLN::Error> AssertFalse(bool condition, std::source_location loc = std::source_location::current()) {
    if (ExpectFalse(condition, loc)) {
        return {};
    }
    return std::unexpected(ZHLN::Error(TestFrameworkError::AssertionFailed));
}

struct TestStats {
    uint32_t passed = 0;
    uint32_t failed = 0;
};

template <typename T>
concept TestResult = requires(T t) {
    { t.has_value() } -> std::convertible_to<bool>;
    { t.error() } -> std::convertible_to<ZHLN::Error>;
};

template <typename T>
concept HasNestedTests = requires { typename T::Tests; };

template <typename Suite>
TestStats RunSuite() {
    // Take ownership of the process diagnostics: framework storage outlasts
    // every engine, so engines increment it directly (teardown included) and
    // per-test deltas below bracket whole engine lifecycles exactly.
    // Idempotent: nested suites re-register the same storage.
    RenderContext::UseDiagnostics(&g_validationErrors, &g_deviceLost);

    Suite     suite;
    TestStats stats;

    ZHLN::Println("{}=================================================={}", Color::Cyan, Color::Reset);
    ZHLN::Println("{}Running Test Suite: {}{}", Color::Cyan, ZHLN::Reflect::TypeName<Suite>(), Color::Reset);
    ZHLN::Println("{}=================================================={}", Color::Cyan, Color::Reset);

    auto run_test_method = [&](auto target, auto pmf, std::string_view name) {
        using MethodType = decltype(pmf);
        using ReturnType = std::invoke_result_t<MethodType, decltype(target)>;

        if constexpr (TestResult<ReturnType>) {
            auto& ctx = GetThreadLocalContext();
            ctx.Reset(name);

            // 1. Snapshot telemetry before test begins (framework-owned
            // totals: they persist across engine lifetimes)
            const uint32_t valErrorsBefore = g_validationErrors.load(std::memory_order_relaxed);
            const uint32_t devLostBefore   = g_deviceLost.load(std::memory_order_relaxed);

            ReturnType result = std::unexpected(ZHLN::Error(TestFrameworkError::AssertionFailed));

#if defined(ZHLN_TEST_TIMEOUT_SUPPORTED)
            struct sigaction sa {};
            sa.sa_handler = TestTimeoutSignalHandler;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = 0;
            struct sigaction old_sa;
            sigaction(SIGALRM, &sa, &old_sa);

            if (IsDebuggerAttached() || ctx.timeoutSeconds == 0) {
                alarm(0);
            } else {
                alarm(ctx.timeoutSeconds);
            }

            if (sigsetjmp(g_testTimeoutJmpBuf, 1) == 0) {
                result = (target.*pmf)();
                alarm(0);
                sigaction(SIGALRM, &old_sa, nullptr);
            } else {
                alarm(0);
                sigaction(SIGALRM, &old_sa, nullptr);

                ctx.failures.push_back(
                    {.file          = "Unknown",
                     .line          = 0,
                     .actualValue   = "Test execution timed out (deadlock or infinite loop)",
                     .expectedValue = "Test execution completes under " + std::to_string(ctx.timeoutSeconds) + " seconds",
                     .op            = "Timeout"}
                );
                result = std::unexpected(ZHLN::Error(TestFrameworkError::AssertionFailed));
            }
#else
            result = (target.*pmf)();
#endif

            // 2. Fail if new Vulkan Validation Errors occurred
            const uint32_t valErrorsAfter = g_validationErrors.load(std::memory_order_relaxed);
            if (valErrorsAfter > valErrorsBefore && !ctx.allowValidationErrors) {
                const uint32_t count = valErrorsAfter - valErrorsBefore;
                ctx.failures.push_back(
                    {.file          = "Vulkan Validation Layer",
                     .line          = 0,
                     .actualValue   = std::to_string(count) + " Vulkan validation layer error(s) logged",
                     .expectedValue = "0 validation errors",
                     .op            = "ValidationError"}
                );
            }

            // 3. Fail if GPU Device Lost / Hang occurred
            const uint32_t devLostAfter = g_deviceLost.load(std::memory_order_relaxed);
            if (devLostAfter > devLostBefore && !ctx.allowDeviceLost) {
                const uint32_t count = devLostAfter - devLostBefore;
                ctx.failures.push_back(
                    {.file          = "Vulkan Device",
                     .line          = 0,
                     .actualValue   = std::to_string(count) + " GPU device lost / hang event(s) detected",
                     .expectedValue = "0 device lost events",
                     .op            = "DeviceLost"}
                );
            }

            // 4. Evaluate overall test pass/fail
            bool testPassed = result.has_value() && ctx.failures.empty();

            if (testPassed) {
                ZHLN::Println("  {}[ PASS ] {}{}", Color::Green, name, Color::Reset);
                stats.passed++;
            } else {
                ZHLN::Println("  {}[ FAIL ] {}{}", Color::Red, name, Color::Reset);
                if (!result.has_value() && result.error() != TestFrameworkError::AssertionFailed) {
                    ZHLN::Println("    {}Fatal Suite Error: {}{}", Color::Red, result.error().Message(), Color::Reset);
                }
                for (const auto& f: ctx.failures) {
                    if (f.op == "Timeout") {
                        ZHLN::Println("    {}Timeout Error: {}{}", Color::Red, f.actualValue, Color::Reset);
                    } else if (f.op == "ValidationError" || f.op == "DeviceLost") {
                        ZHLN::Println("    {}GPU Failure: {}{}", Color::Red, f.actualValue, Color::Reset);
                    } else {
                        ZHLN::Println("    {}Location: {}:{}{}", Color::Gray, f.file, f.line, Color::Reset);
                        if (f.op == "true" || f.op == "false") {
                            ZHLN::Println("      Expected condition to be: {}", f.expectedValue);
                            ZHLN::Println("      Actual condition was    : {}", f.actualValue);
                        } else {
                            ZHLN::Println("      Comparison mismatch on  : '{}'", f.op);
                            ZHLN::Println("        Actual value          : {}", f.actualValue);
                            ZHLN::Println("        Expected value        : {}", f.expectedValue);
                        }
                    }
                }
                stats.failed++;
            }
        }
    };

    if constexpr (HasNestedTests<Suite>) {
        using Target = typename Suite::Tests;
        Target target;

        ZHLN::Reflect::ForEachMethodPointer<Target>([&](std::string_view name, auto pmf) { run_test_method(target, pmf, name); });
    } else {
        ZHLN::Reflect::ForEachMethodPointer<Suite>([&](std::string_view name, auto pmf) {
            if (name.starts_with("test_")) {
                run_test_method(suite, pmf, name);
            }
        });
    }

    ZHLN::Println("--------------------------------------------------");
    ZHLN::Println("Summary for {}: {} Passed, {} Failed", ZHLN::Reflect::TypeName<Suite>(), stats.passed, stats.failed);
    ZHLN::Println("==================================================\n");

    return stats;
}

class Runner {
  public:
    template <typename... Suites>
    static int Run() {
        TestStats totalStats;

        auto run_one = [&]<typename Suite>() {
            TestStats s = RunSuite<Suite>();
            totalStats.passed += s.passed;
            totalStats.failed += s.failed;
        };

        (run_one.template operator()<Suites>(), ...);

        ZHLN::Println("==================================================");
        ZHLN::Println("GLOBAL TEST RESULTS");
        ZHLN::Println("Total Passed: {}", totalStats.passed);
        ZHLN::Println("Total Failed: {}", totalStats.failed);
        ZHLN::Println("==================================================");

        return totalStats.failed > 0 ? 1 : 0;
    }
};

} // namespace ZHLN::Test
