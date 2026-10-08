// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Reflection/Class.hpp>
#include <Zahlen/Core/Reflection/Utilities.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <atomic>
#include <cstddef>
#include <concepts>
#include <cstdlib>
#include <expected>
#include <format>
#include <fstream>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Render diagnostics: the framework owns persistent counters and passes them
// explicitly through RenderConfig::diagnostics. The sink outlives every test
// engine, including teardown where Vulkan validation callbacks can still fire.
#include <Zahlen/Render/Render.hpp>

// Performance baselines live in tests/extras/profile/PerfBaseline.hpp, because
// storing them is a JSON document and JSON is an extra. This header stays
// extras-free on purpose: every test suite includes it, and a suite that only
// exercises core must build in a build without extras.

#if defined(__unix__) || defined(__APPLE__) || defined(__linux__)
#define ZHLN_TEST_TIMEOUT_SUPPORTED 1
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

// SIGALRM is process-wide. A longjmp out of Engine::Create/Destroy skips
// destructors and can leave a live Vulkan device or locked mutex behind;
// continuing the group then fabricates unrelated engine-acquisition failures.
// Write only with async-signal-safe POSIX calls and stop this test process.
// The handler may interrupt another thread; only lock-free atomics may carry
// a case name from the runner into an asynchronous C++ signal handler.
inline std::atomic<const char*> g_testTimeoutName {nullptr};
inline std::atomic<size_t>      g_testTimeoutNameLength {0};
static_assert(std::atomic<const char*>::is_always_lock_free && std::atomic<size_t>::is_always_lock_free);

inline void WriteTimeoutText(const char* text, size_t length) noexcept {
    while (length != 0) {
        const ssize_t written = ::write(STDERR_FILENO, text, length);
        if (written <= 0) {
            return; // Best effort: never attempt a non-signal-safe fallback.
        }
        text += written;
        length -= static_cast<size_t>(written);
    }
}

inline void TestTimeoutSignalHandler(int sig) {
    if (sig == SIGALRM) {
        static constexpr char prefix[] = "  [ TIMEOUT ] ";
        static constexpr char suffix[] = " (deadline exceeded; terminating this test process)\n";
        WriteTimeoutText(prefix, sizeof(prefix) - 1);
        const char* const name = g_testTimeoutName.load(std::memory_order_acquire);
        if (name != nullptr) {
            WriteTimeoutText(name, g_testTimeoutNameLength.load(std::memory_order_relaxed));
        }
        WriteTimeoutText(suffix, sizeof(suffix) - 1);
        ::_exit(124);
    }
}
#endif

namespace ZHLN::Test {

// Diagnostics totals owned by this framework. Engines receive this sink
// explicitly through their RenderConfig; it remains alive through engine and
// Vulkan teardown so post-mortem callbacks are included in test deltas.
inline std::atomic<uint32_t> g_validationErrors {0};
inline std::atomic<uint32_t> g_deviceLost {0};
inline DiagnosticsSink       g_renderDiagnostics {&g_validationErrors, &g_deviceLost};

// Internal to the framework: this is what the runner seeds a result with
// before invoking a case. Timeouts instead exit the process with status 124.
// It is deliberately NOT what a test returns — a test that reports this
// declined to say what went wrong. Callers define their own error enum and
// return that; see the Expectations comment.
enum class TestFrameworkError : uint8_t {
    AssertionFailed ZHLN_ANNOTATION(ZHLN::Description<"One or more assertions failed in this test. ">{}) = 1,
};

struct AssertionFailure {
    std::string_view file;
    uint32_t         line;
    std::string      actualValue;
    std::string      expectedValue;
    std::string_view op; // "==" or "!=" or "true" or "false" or "ValidationError" or "DeviceLost" or "PerfRegression"
    std::string      expression; // trimmed source line at `line`, when readable
};

// Best-effort: source_location has no expression text, so we read the file.
[[nodiscard]] inline auto ReadSourceLine(std::string_view path, uint32_t line) -> std::string {
    if (path.empty() || line == 0) {
        return {};
    }
    std::ifstream in {std::string {path}};
    if (!in) {
        return {};
    }
    std::string text;
    uint32_t    n = 0;
    while (std::getline(in, text)) {
        ++n;
        if (n != line) {
            continue;
        }
        const auto start = text.find_first_not_of(" \t");
        if (start != std::string::npos) {
            text.erase(0, start);
        }
        while (!text.empty() && (text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) {
            text.pop_back();
        }
        return text;
    }
    return {};
}


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
// Pick the most specific check available: ExpectEq/ExpectNe for equality,
// ExpectLt/Gt/Le/Ge for one-sided numeric thresholds, ExpectInRange for
// two-sided ones. Reach for ExpectTrue only when no value is involved --
// the value-capturing forms print the measured number against the bound on
// failure, while ExpectTrue can only print "false".
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
        {.file          = loc.file_name(),
         .line          = loc.line(),
         .actualValue   = FormatValue(actual),
         .expectedValue = FormatValue(expected),
         .op            = "==",
         .expression    = ReadSourceLine(loc.file_name(), loc.line())}
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
        {.file          = loc.file_name(),
         .line          = loc.line(),
         .actualValue   = FormatValue(actual),
         .expectedValue = FormatValue(expected),
         .op            = "!=",
         .expression    = ReadSourceLine(loc.file_name(), loc.line())}
    );
    return false;
}

// Ordering expectations for numeric thresholds. Prefer these over
// ExpectTrue(a < b): ExpectTrue only records "false" against "true", while
// these record the measured value against the bound, so a red run says how
// far the check missed instead of just that it did.
template <typename T1, typename T2>
bool ExpectLt(const T1& actual, const T2& bound, std::source_location loc = std::source_location::current()) {
    if constexpr (requires { actual < bound; }) {
        if (actual < bound) {
            return true;
        }
    } else {
        static_assert(sizeof(T1) == 0, "Types are not orderable!");
        return false;
    }

    auto& ctx = GetThreadLocalContext();
    ctx.failures.push_back(
        {.file          = loc.file_name(),
         .line          = loc.line(),
         .actualValue   = FormatValue(actual),
         .expectedValue = "< " + FormatValue(bound),
         .op            = "<",
         .expression    = ReadSourceLine(loc.file_name(), loc.line())}
    );
    return false;
}

template <typename T1, typename T2>
bool ExpectGt(const T1& actual, const T2& bound, std::source_location loc = std::source_location::current()) {
    if constexpr (requires { actual > bound; }) {
        if (actual > bound) {
            return true;
        }
    } else {
        static_assert(sizeof(T1) == 0, "Types are not orderable!");
        return false;
    }

    auto& ctx = GetThreadLocalContext();
    ctx.failures.push_back(
        {.file          = loc.file_name(),
         .line          = loc.line(),
         .actualValue   = FormatValue(actual),
         .expectedValue = "> " + FormatValue(bound),
         .op            = ">",
         .expression    = ReadSourceLine(loc.file_name(), loc.line())}
    );
    return false;
}

template <typename T1, typename T2>
bool ExpectLe(const T1& actual, const T2& bound, std::source_location loc = std::source_location::current()) {
    if constexpr (requires { actual <= bound; }) {
        if (actual <= bound) {
            return true;
        }
    } else {
        static_assert(sizeof(T1) == 0, "Types are not orderable!");
        return false;
    }

    auto& ctx = GetThreadLocalContext();
    ctx.failures.push_back(
        {.file          = loc.file_name(),
         .line          = loc.line(),
         .actualValue   = FormatValue(actual),
         .expectedValue = "<= " + FormatValue(bound),
         .op            = "<=",
         .expression    = ReadSourceLine(loc.file_name(), loc.line())}
    );
    return false;
}

template <typename T1, typename T2>
bool ExpectGe(const T1& actual, const T2& bound, std::source_location loc = std::source_location::current()) {
    if constexpr (requires { actual >= bound; }) {
        if (actual >= bound) {
            return true;
        }
    } else {
        static_assert(sizeof(T1) == 0, "Types are not orderable!");
        return false;
    }

    auto& ctx = GetThreadLocalContext();
    ctx.failures.push_back(
        {.file          = loc.file_name(),
         .line          = loc.line(),
         .actualValue   = FormatValue(actual),
         .expectedValue = ">= " + FormatValue(bound),
         .op            = ">=",
         .expression    = ReadSourceLine(loc.file_name(), loc.line())}
    );
    return false;
}

inline bool ExpectTrue(bool condition, std::source_location loc = std::source_location::current()) {
    if (condition) {
        return true;
    }
    auto& ctx = GetThreadLocalContext();
    ctx.failures.push_back(
        {.file          = loc.file_name(),
         .line          = loc.line(),
         .actualValue   = "false",
         .expectedValue = "true",
         .op            = "true",
         .expression    = ReadSourceLine(loc.file_name(), loc.line())}
    );
    return false;
}

template <typename T>
bool ExpectInRange(const T& actual, const T& lo, const T& hi, std::source_location loc = std::source_location::current()) {
    if (actual >= lo && actual <= hi) {
        return true;
    }
    auto& ctx = GetThreadLocalContext();
    ctx.failures.push_back(
        {.file          = loc.file_name(),
         .line          = loc.line(),
         .actualValue   = FormatValue(actual),
         .expectedValue = "[" + FormatValue(lo) + ", " + FormatValue(hi) + "]",
         .op            = "in range",
         .expression    = ReadSourceLine(loc.file_name(), loc.line())}
    );
    return false;
}

inline bool ExpectFalse(bool condition, std::source_location loc = std::source_location::current()) {
    if (!condition) {
        return true;
    }
    auto& ctx = GetThreadLocalContext();
    ctx.failures.push_back(
        {.file          = loc.file_name(),
         .line          = loc.line(),
         .actualValue   = "true",
         .expectedValue = "false",
         .op            = "false",
         .expression    = ReadSourceLine(loc.file_name(), loc.line())}
    );
    return false;
}


struct TestStats {
    uint32_t passed      = 0;
    uint32_t failed      = 0;
    uint32_t quarantined = 0;
};

// Quarantine: tests that fail on a KNOWN, still-open renderer bug -- never a
// stale gate -- keep running, but their failures print [ QUARANTINE ] with
// the reason and neither fail the suite nor the process exit code. A
// quarantined test that passes prints a remove-the-entry nudge: the entry
// must die when the bug does. ZHLN_TEST_QUARANTINE=off disables every entry
// (strict mode for tracking a bug to zero).
//
// An entry needs the exact suite + test names (as [ RUN ] prints them), a
// one-line mechanism, and a pointer to the evidence. No entry without all
// three -- quarantine is for diagnosed bugs, not for red tests.
struct QuarantinedTest {
    std::string_view suite;
    std::string_view test;
    std::string_view reason;
};

inline const std::vector<QuarantinedTest>& QuarantineList() {
    static const std::vector<QuarantinedTest> list {
        {"RayTracedReflectionNoiseTestSuite", "rtr_is_live_and_rough_surfaces_stay_still",
         "RTR switch shifts 3.4% of the roughness-cutoff probe rows through a non-dithering path (on/on-top control reads exactly 0 with "
         "top meanAbs 0.12 vs 0.00: systematic sub-luma post coupling, bloom/exposure suspected); "
         "see tests/INVARIANT_TESTING.md verdict 14. Remove when topChanged < 0.002 runs green unquarantined."},
    };
    return list;
}

inline std::string_view QuarantineReason(std::string_view suite, std::string_view test) {
    const char* const env = std::getenv("ZHLN_TEST_QUARANTINE");
    if (env != nullptr && std::string_view {env} == "off") {
        return {};
    }
    for (const auto& entry: QuarantineList()) {
        if (entry.suite == suite && entry.test == test) {
            return entry.reason;
        }
    }
    return {};
}

// One line per failed test, collected across every suite in the process
// (RunDeferred's suites live in other translation units, so the registry is
// an inline function static: all instantiations share the one object). The
// global results section lists these so a red run names every failed test
// and its error enum at the end of the log, without scrolling back.
struct FailedTestSummary {
    std::string suite;
    std::string test;
    std::string detail; // "LightingRTTestError::EngineInitFailed", "3 recorded failures", ...
};

inline std::vector<FailedTestSummary>& GetFailedTestSummaries() noexcept {
    static std::vector<FailedTestSummary> summaries;
    return summaries;
}

template <typename T>
concept TestResult = requires(T t) {
    { t.has_value() } -> std::convertible_to<bool>;
    { t.error() } -> std::convertible_to<ZHLN::Error>;
};

template <typename T>
concept HasNestedTests = requires { typename T::Tests; };

template <typename Suite>
TestStats RunSuite() {
    // Run one cold-engine case in a fresh process without the preceding tests
    // (or their leaked state after a SIGALRM). For example:
    // ZHLN_TEST_FILTER=RenderPipelinesTestSuite::engines_are_serial_and_the_slot_is_released
    const char* const filterEnv = std::getenv("ZHLN_TEST_FILTER");
    const std::string_view filter = filterEnv != nullptr ? std::string_view {filterEnv} : std::string_view {};
    const std::string_view suiteName = ZHLN::Reflect::TypeName<Suite>();
    if (const auto separator = filter.find("::"); separator != std::string_view::npos && filter.substr(0, separator) != suiteName) {
        return {}; // Do not construct an unrelated suite (it may own an Engine).
    }

    Suite     suite;
    TestStats stats;

    ZHLN::Println("{}=================================================={}", Color::Cyan, Color::Reset);
    ZHLN::Println("{}Running Test Suite: {}{}", Color::Cyan, suiteName, Color::Reset);
    ZHLN::Println("{}=================================================={}", Color::Cyan, Color::Reset);

    auto run_test_method = [&](auto target, auto pmf, std::string_view name) {
        if (!filter.empty() && filter != suiteName && filter != name && filter != std::format("{}::{}", suiteName, name)) {
            return;
        }
        using MethodType = decltype(pmf);
        using ReturnType = std::invoke_result_t<MethodType, decltype(target)>;

        if constexpr (TestResult<ReturnType>) {
            auto& ctx = GetThreadLocalContext();
            ctx.Reset(name);
            ZHLN::Println("  [ RUN  ] {}", name);

            // 1. Snapshot telemetry before test begins (framework-owned
            // totals: they persist across engine lifetimes)
            const uint32_t valErrorsBefore = g_validationErrors.load(std::memory_order::relaxed);
            const uint32_t devLostBefore   = g_deviceLost.load(std::memory_order::relaxed);

            // Quiet engine chatter: capture every engine log line and print
            // the buffer only when the test fails, so a green run shows
            // RUN/PASS/FAIL instead of IBL bakes and per-frame capture
            // statistics. The level is raised to Verbose inside the capture
            // so the failure dump keeps even debug-level lines (including
            // the demoted [Test Capture] statistics). ZHLN_TEST_LOG=verbose
            // disables the capture and streams engine logs for debugging.
            const char* const testLogEnv     = std::getenv("ZHLN_TEST_LOG");
            const bool        captureTestLogs = testLogEnv == nullptr || std::string_view {testLogEnv} != "verbose";
            const ZHLN::LogLevel outerLogLevel = ZHLN::GetLogLevel();
            if (captureTestLogs) {
                ZHLN::SetLogLevel(ZHLN::LogLevel::Verbose);
                ZHLN::BeginLogCapture(true);
            }

            ReturnType result = std::unexpected(ZHLN::ErrorCode(TestFrameworkError::AssertionFailed));

#if defined(ZHLN_TEST_TIMEOUT_SUPPORTED)
            struct sigaction sa {};
            sa.sa_handler = TestTimeoutSignalHandler;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = 0;
            struct sigaction old_sa;
            g_testTimeoutNameLength.store(name.size(), std::memory_order_relaxed);
            g_testTimeoutName.store(name.data(), std::memory_order_release);
            sigaction(SIGALRM, &sa, &old_sa);

            if (IsDebuggerAttached() || ctx.timeoutSeconds == 0) {
                alarm(0);
            } else {
                alarm(ctx.timeoutSeconds);
            }

            result = (target.*pmf)();
            alarm(0);
            sigaction(SIGALRM, &old_sa, nullptr);
            g_testTimeoutName.store(nullptr, std::memory_order_release);
            g_testTimeoutNameLength.store(0, std::memory_order_relaxed);
#else
            result = (target.*pmf)();
#endif

            std::vector<std::string> capturedTestLogs;
            if (captureTestLogs) {
                capturedTestLogs = ZHLN::EndLogCapture();
                ZHLN::SetLogLevel(outerLogLevel);
            }

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
                if (const std::string_view reason = QuarantineReason(suiteName, name); !reason.empty()) {
                    ZHLN::Println("    {}[QUARANTINE] Passed while quarantined -- remove its entry:{} {}", Color::Yellow, Color::Reset, reason);
                }
                stats.passed++;
            } else {
                const bool quarantined = !QuarantineReason(suiteName, name).empty();
                if (quarantined) {
                    ZHLN::Println("  {}[ QUARANTINE ] {}{}", Color::Yellow, name, Color::Reset);
                    ZHLN::Println("    {}Reason: {}{}", Color::Yellow, QuarantineReason(suiteName, name), Color::Reset);
                } else {
                    ZHLN::Println("  {}[ FAIL ] {}{}", Color::Red, name, Color::Reset);
                }
                if (!result.has_value() && result.error() != TestFrameworkError::AssertionFailed) {
                    ZHLN::Println(
                        "    {}Fatal Suite Error: {}::{}: {}{}", Color::Red, ZHLN::Error(result.error()).Category(), ZHLN::Error(result.error()).Name(), ZHLN::Error(result.error()).Message(),
                        Color::Reset
                    );
                }
                for (const auto& f: ctx.failures) {
                    if (f.op == "ValidationError" || f.op == "DeviceLost") {
                        ZHLN::Println("    {}GPU Failure: {}{}", Color::Red, f.actualValue, Color::Reset);
                    } else {
                        ZHLN::Println("    {}Location: {}:{}{}", Color::Gray, f.file, f.line, Color::Reset);
                        if (!f.expression.empty()) {
                            ZHLN::Println("      Condition: {}", f.expression);
                        }
                        if (f.op == "true" || f.op == "false") {
                            ZHLN::Println("      Evaluated to: {}", f.actualValue);
                            ZHLN::Println("      Expected:     {}", f.expectedValue);
                        } else {
                            ZHLN::Println("      Comparison mismatch on  : '{}'", f.op);
                            ZHLN::Println("        Actual value          : {}", f.actualValue);
                            ZHLN::Println("        Expected value        : {}", f.expectedValue);
                        }
                    }
                }
                if (!capturedTestLogs.empty()) {
                    ZHLN::Println(
                        "    {}[ENGINE LOG] {} line(s) captured during this test (ZHLN_TEST_LOG=verbose to stream):{}",
                        Color::Gray, capturedTestLogs.size(), Color::Reset
                    );
                    // Head for context (device, configuration), tail for the
                    // failure itself; the middle is steady-state spam.
                    constexpr size_t kHeadLines = 15;
                    constexpr size_t kTailLines = 50;
                    if (capturedTestLogs.size() <= kHeadLines + kTailLines) {
                        for (const auto& line: capturedTestLogs) {
                            ZHLN::Println("      | {}", line);
                        }
                    } else {
                        for (size_t i = 0; i < kHeadLines; ++i) {
                            ZHLN::Println("      | {}", capturedTestLogs[i]);
                        }
                        ZHLN::Println("      | ... ({} lines omitted) ...", capturedTestLogs.size() - kHeadLines - kTailLines);
                        for (size_t i = capturedTestLogs.size() - kTailLines; i < capturedTestLogs.size(); ++i) {
                            ZHLN::Println("      | {}", capturedTestLogs[i]);
                        }
                    }
                }
                // Quarantined failures keep their full diagnosis (fatal, recorded
                // expectations, engine log above) but count aside and stay out
                // of the global failed list: the bug is known and tracked in
                // QuarantineList, so the suite must not go red over it twice.
                if (quarantined) {
                    stats.quarantined++;
                } else {
                    stats.failed++;

                    // Feed the global results section: name the error enum the test
                    // propagated, and count what the expectations recorded.
                    std::string detail;
                    if (!result.has_value() && result.error() != TestFrameworkError::AssertionFailed) {
                        detail = std::format("{}::{}", ZHLN::Error(result.error()).Category(), ZHLN::Error(result.error()).Name());
                    }
                    const size_t recorded = ctx.failures.size();
                    if (!detail.empty() && recorded > 0) {
                        detail += " + ";
                    }
                    if (recorded > 0) {
                        detail += std::to_string(recorded) + (recorded == 1 ? " recorded failure" : " recorded failures");
                    }
                    if (detail.empty()) {
                        detail = "failed without recorded details";
                    }
                    GetFailedTestSummaries().push_back(FailedTestSummary {std::string {suiteName}, std::string {name}, std::move(detail)});
                }
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
    if (stats.quarantined == 0) {
        ZHLN::Println("Summary for {}: {} Passed, {} Failed", ZHLN::Reflect::TypeName<Suite>(), stats.passed, stats.failed);
    } else {
        ZHLN::Println(
            "Summary for {}: {} Passed, {} Failed, {} Quarantined", ZHLN::Reflect::TypeName<Suite>(), stats.passed, stats.failed, stats.quarantined
        );
    }
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
            totalStats.quarantined += s.quarantined;
        };

        (run_one.template operator()<Suites>(), ...);

        return Summarize(totalStats);
    }

    // Runs suites that live in other translation units of the same binary.
    //
    // A group binary cannot name its members' suite types: the definitions
    // stay inside their own .cpp, which is exactly what keeps each file's
    // anonymous-namespace helpers from colliding once several files share a
    // link. Each file therefore exports a stats-returning function instead,
    // and the group main hands those here to get one aggregated summary.
    //
    //   // tests/core/TestContainers.cpp
    //   auto RunContainersSuite() -> ZHLN::Test::TestStats { return ZHLN::Test::RunSuite<ContainersTestSuite>(); }
    //
    //   // tests/core/RunCoreTests.cpp
    //   int main() { return ZHLN::Test::Runner::RunDeferred(RunContainersSuite, RunReflectionSuite, RunErrorSuite); }
    //
    // The exported name is `Run<Base>Suite`, not `<Base>Suite`. Naming it
    // after the suite it runs compiles at the declaration -- the function
    // merely hides the class -- and then fails inside its own body with
    // "no matching function for call to RunSuite<ContainersTestSuite>()",
    // because the suite type is no longer visible. TestGraphicsSettings.cpp
    // has a struct literally named GraphicsSettingsSuite, so this is not
    // hypothetical.
    template <typename... SuiteRunners>
    static int RunDeferred(SuiteRunners... runners) {
        TestStats totalStats;

        const auto add = [&totalStats](TestStats s) {
            totalStats.passed += s.passed;
            totalStats.failed += s.failed;
            totalStats.quarantined += s.quarantined;
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
        if (totalStats.quarantined > 0) {
            ZHLN::Println("Total Quarantined: {} (known open bugs; see [ QUARANTINE ] reasons above)", totalStats.quarantined);
        }
        const char* const selected = std::getenv("ZHLN_TEST_FILTER");
        const bool unmatchedFilter =
            selected != nullptr && *selected != '\0' && totalStats.passed == 0 && totalStats.failed == 0 && totalStats.quarantined == 0;
        if (unmatchedFilter) {
            ZHLN::Println("No test matched ZHLN_TEST_FILTER='{}'.", selected);
        }

        auto& summaries = GetFailedTestSummaries();
        if (!summaries.empty()) {
            ZHLN::Println("Failed tests:");
            for (const auto& f: summaries) {
                ZHLN::Println("  {}{}::{}{}: {}", Color::Red, f.suite, f.test, Color::Reset, f.detail);
            }
        }
        ZHLN::Println("==================================================");

        // One summary per Runner invocation: if a process ever runs a second
        // Runner, its results section must not re-list the first run's failures.
        summaries.clear();

        // Quarantined tests intentionally do not affect the exit code: the bug
        // is known and tracked in QuarantineList, so the suite must not go red
        // over it twice. ZHLN_TEST_QUARANTINE=off restores strictness.
        return totalStats.failed > 0 || unmatchedFilter ? 1 : 0;
    }
};

} // namespace ZHLN::Test
