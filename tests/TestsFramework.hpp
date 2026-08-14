// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <concepts>
#include <expected>
#include <source_location>
#include <string_view>
#include <type_traits>
#include <vector>

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

// Inline global jump buffer for timeout recovery
inline sigjmp_buf g_testTimeoutJmpBuf;

inline void TestTimeoutSignalHandler(int sig) {
    if (sig == SIGALRM) {
        siglongjmp(g_testTimeoutJmpBuf, 1);
    }
}
#endif

namespace ZHLN::Test {

enum class TestFrameworkError : uint32_t {
    Success = 0,
    AssertionFailed[[= ZHLN::Reflect::Description("One or more assertions failed in this test. ")]],
};

struct AssertionFailure {
    std::string_view file;
    uint32_t         line;
    std::string      actualValue;
    std::string      expectedValue;
    std::string_view op; // "==" or "!=" or "true" or "false"
};

struct TestContext {
    std::string_view              currentTestName;
    std::vector<AssertionFailure> failures;

    void Reset(std::string_view testName) {
        currentTestName = testName;
        failures.clear();
    }
};

inline TestContext& GetThreadLocalContext() noexcept {
    thread_local TestContext ctx;
    return ctx;
}

// Convert types to debug strings using Reflection formatter
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

            ReturnType result = std::unexpected(ZHLN::Error(TestFrameworkError::AssertionFailed));

#if defined(ZHLN_TEST_TIMEOUT_SUPPORTED)
            // Register SIGALRM handler to recover from deadlocks
            struct sigaction sa {};
            sa.sa_handler = TestTimeoutSignalHandler;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = 0;
            struct sigaction old_sa;
            sigaction(SIGALRM, &sa, &old_sa);

            // Bypass alarm completely if running under a debugger (stepping/inspecting locks takes time)
            if (IsDebuggerAttached()) {
                alarm(0);
            } else {
                alarm(15); // Standard 15-second timeout for typical CLI/CI environment runs
            }

            if (sigsetjmp(g_testTimeoutJmpBuf, 1) == 0) {
                result = (target.*pmf)();

                // Clear alarm and restore old signal handler on success
                alarm(0);
                sigaction(SIGALRM, &old_sa, nullptr);
            } else {
                // Timeout fired! Clear alarm and record diagnostics
                alarm(0);
                sigaction(SIGALRM, &old_sa, nullptr);

                ctx.failures.push_back(
                    {.file          = "Unknown",
                     .line          = 0,
                     .actualValue   = "Test execution timed out (deadlock or infinite loop)",
                     .expectedValue = "Test execution completes under 15 seconds",
                     .op            = "Timeout"}
                );
                result = std::unexpected(ZHLN::Error(TestFrameworkError::AssertionFailed));
            }
#else
            // Fallback for non-POSIX platforms
            result = (target.*pmf)();
#endif

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
