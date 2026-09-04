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

// Performance baselines live in extras/profile/PerfBaseline.hpp, because
// storing them is a JSON document and JSON is an extra. This header stays
// extras-free on purpose: every test suite includes it, and a suite that only
// exercises core must build in a build without extras.

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

// Internal to the framework: this is what the runner seeds a result with and
// what a timeout produces. It is deliberately NOT what a test returns — a test
// that reports this is a test that declined to say what went wrong. Callers
// define their own error enum and return that; see the Expectations comment.
enum class TestFrameworkError : uint8_t {
    AssertionFailed ZHLN_ANNOTATION(ZHLN::Description<"One or more assertions failed in this test. ">{}) = 1,
};

struct AssertionFailure {
    std::string_view file;
    uint32_t         line;
    std::string      actualValue;
    std::string      expectedValue;
    std::string_view op; // "==" or "!=" or "true" or "false" or "ValidationError" or "DeviceLost" or "PerfRegression"
};


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

// Expectations
//
// The only assertion family. Each returns bool: true = the condition held,
// false = it did not, and the failure has been recorded against loc (the
// caller's position, captured by the defaulted source_location, so no macro is
// needed to report where).
//
// Returning bool rather than std::expected is the deliberate part. The check
// decides *whether* something is wrong; only the caller knows *what it means*.
// An expected-returning Assert* collapsed every failure into one
// TestFrameworkError::AssertionFailed, so a red run said "an assertion failed"
// and nothing about what the test was doing when it did — and because the
// error was already consumed, callers stopped there instead of naming it.
//
// Propagate with an error of your own:
//
//   if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
//       return std::unexpected(LightingRTTestError::EngineInitFailed);
//   }
//
// A bare discarded call still fails the test (failures are recorded), it just
// does not stop it — so use it for "and also check this", and guard with an
// error for anything the rest of the test depends on.
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
            const uint32_t valErrorsBefore = g_validationErrors.load(std::memory_order::relaxed);
            const uint32_t devLostBefore   = g_deviceLost.load(std::memory_order::relaxed);

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
            const uint32_t valErrorsAfter = g_validationErrors.load(std::memory_order::relaxed);
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
            const uint32_t devLostAfter = g_deviceLost.load(std::memory_order::relaxed);
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

        return Summarize(totalStats);
    }

    /// Runs suites that live in other translation units of the same binary.
    ///
    /// A group binary cannot name its members' suite types: the definitions
    /// stay inside their own .cpp, which is exactly what keeps each file's
    /// anonymous-namespace helpers from colliding once several files share a
    /// link. Each file therefore exports a stats-returning function instead,
    /// and the group main hands those here to get one aggregated summary.
    ///
    ///   // tests/core/TestContainers.cpp
    ///   auto RunContainersSuite() -> ZHLN::Test::TestStats { return ZHLN::Test::RunSuite<ContainersTestSuite>(); }
    ///
    ///   // tests/core/RunCoreTests.cpp
    ///   int main() { return ZHLN::Test::Runner::RunDeferred(RunContainersSuite, RunReflectionSuite, RunErrorSuite); }
    ///
    /// The exported name is `Run<Base>Suite`, not `<Base>Suite`. Naming it
    /// after the suite it runs compiles at the declaration -- the function
    /// merely hides the class -- and then fails inside its own body with
    /// "no matching function for call to RunSuite<ContainersTestSuite>()",
    /// because the suite type is no longer visible. TestGraphicsSettings.cpp
    /// has a struct literally named GraphicsSettingsSuite, so this is not
    /// hypothetical.
    template <typename... SuiteRunners>
    static int RunDeferred(SuiteRunners... runners) {
        TestStats totalStats;

        const auto add = [&totalStats](TestStats s) {
            totalStats.passed += s.passed;
            totalStats.failed += s.failed;
        };

        (add(runners()), ...);

        return Summarize(totalStats);
    }

  private:
    static int Summarize(const TestStats& totalStats) {
        ZHLN::Println("==================================================");
        ZHLN::Println("GLOBAL TEST RESULTS");
        ZHLN::Println("Total Passed: {}", totalStats.passed);
        ZHLN::Println("Total Failed: {}", totalStats.failed);
        ZHLN::Println("==================================================");

        return totalStats.failed > 0 ? 1 : 0;
    }
};

} // namespace ZHLN::Test
