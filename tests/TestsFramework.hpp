// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <CommandLine/CommandLine.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Core/Reflection/Class.hpp>
#include <Zahlen/Core/Reflection/Utilities.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

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

namespace ZHLN::ECS {
class Registry;
}

namespace ZHLN::Test {

// ============================================================================
// Declarative C++26 Marks (attach via ZHLN_ANNOTATION(...) / [[= ... ]])
// ============================================================================
//
// Marks may be attached to either a suite struct (applying to every test case
// in that suite) or an individual test method (overriding or extending the
// suite marks).
//
// Quarantine is reserved for tests that fail on a KNOWN, diagnosed, still-open
// bug: their failures print [ QUARANTINE ] with the reason and neither fail
// the suite nor fail the process exit code unless ZHLN_TEST_QUARANTINE=off or
// --strict-quarantine is passed. When a quarantined test passes, the runner
// prints a reminder to remove the annotation.
template <ZHLN::StringLiteral Reason, ZHLN::StringLiteral Evidence = "">
struct Quarantine {
    static constexpr bool             isTestQuarantine = true;
    static constexpr std::string_view reason           = Reason;
    static constexpr std::string_view evidence         = Evidence;
};

template <ZHLN::StringLiteral Reason = "">
struct Skip {
    static constexpr bool             isTestSkip = true;
    static constexpr std::string_view reason     = Reason;
};

template <unsigned int Seconds>
struct Timeout {
    static constexpr bool         isTestTimeout = true;
    static constexpr unsigned int seconds       = Seconds;
};

template <ZHLN::StringLiteral... Names>
struct Tags {
    static constexpr bool                                           isTestTags = true;
    static constexpr std::array<std::string_view, sizeof...(Names)> names {Names...};
};

template <ZHLN::StringLiteral Name>
using Tag = Tags<Name>;

struct AllowValidation {
    static constexpr bool isAllowValidation = true;
};

struct AllowDeviceLostMark {
    static constexpr bool isAllowDeviceLost = true;
};

// Controls suite execution order within a group binary (default 0; higher runs
// later). Useful when an exclusive-engine suite should run after pooled ones.
template <int Order>
struct Priority {
    static constexpr bool isSuitePriority = true;
    static constexpr int  order           = Order;
};

// Excludes a TestResult-returning member function on a suite struct from being
// discovered as a test case.
struct Helper {
    static constexpr bool isTestHelper = true;
};

// ============================================================================
// Reflected Fixture Lifetimes & Built-In Fixtures
// ============================================================================
//
// Any struct/class marked with ZHLN_ANNOTATION(ZHLN::Test::FixtureLifetime<Scope>{})
// (or FunctionFixture / SuiteFixture / SessionFixture, or registered via
// FixtureTraits<T>) can be declared directly as a parameter on a test method.
// Fixtures may themselves depend on same- or longer-lived fixtures in their
// constructors; each scope tears its instances down in reverse creation order.
enum class Scope : uint8_t {
    Function = 0,
    Suite    = 1,
    Session  = 2,
};

template <Scope Lifetime = Scope::Function>
struct FixtureLifetime {
    static constexpr bool  isTestFixture = true;
    static constexpr Scope scope         = Lifetime;
};

using FunctionFixture = FixtureLifetime<Scope::Function>;
using SuiteFixture    = FixtureLifetime<Scope::Suite>;
using SessionFixture  = FixtureLifetime<Scope::Session>;

// Pytest @pytest.mark.usefixtures equivalent: activates ambient RAII fixtures
// (e.g. TaskSystemScope, TaskSystemAndJoltScope, Headless::SessionScope) on a
// suite or method without adding unused parameters to the method signature.
template <typename... Fixtures>
struct UseFixtures {
    static constexpr bool isUseFixturesMark = true;

    template <typename Resolver>
    static void Activate(Resolver&& resolver) {
        (resolver.template operator()<std::remove_cvref_t<Fixtures>>(), ...);
    }
};

template <typename T>
struct FixtureTraits {
    static constexpr bool  enabled = false;
    static constexpr Scope scope   = Scope::Function;
};

template <>
struct FixtureTraits<ZHLN::ECS::Registry> {
    static constexpr bool  enabled = true;
    static constexpr Scope scope   = Scope::Function;
};

struct ZHLN_ANNOTATION(FunctionFixture {}) CaseInfo {
    std::string_view suiteName {};
    std::string_view caseName {};
    std::string_view methodName {};
    uint32_t         paramIndex      = 0;
    bool             isParameterized = false;
    std::string_view filePath {};
};

class ZHLN_ANNOTATION(FunctionFixture {}) TempDir {
  public:
    TempDir() : TempDir(CaseInfo {.suiteName = "Suite", .caseName = "Case"}) {}

    explicit TempDir(const CaseInfo& info) {
        static std::atomic<uint64_t> counter {0};
        const uint64_t               seq   = counter.fetch_add(1, std::memory_order_relaxed);
        const auto                   ticks = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(__unix__) || defined(__APPLE__) || defined(__linux__)
        const auto pid = static_cast<long>(::getpid());
#else
        const long pid = 0;
#endif
        const std::string tag = SanitizeName(info.caseName.empty() ? "case" : info.caseName);
        m_root                = std::filesystem::temp_directory_path() / std::format("ztest_{}_{}_{}_{}", tag, pid, seq, ticks);
        std::error_code ec;
        std::filesystem::create_directories(m_root, ec);
    }

    ~TempDir() {
        if (!m_root.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(m_root, ec);
        }
    }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&)                 = delete;
    TempDir& operator=(TempDir&&)      = delete;

    [[nodiscard]] auto Path() const noexcept -> const std::filesystem::path& { return m_root; }
    operator const std::filesystem::path&() const noexcept { return m_root; }

    [[nodiscard]] auto SubPath(std::string_view relative) const -> std::filesystem::path {
        std::filesystem::path p = m_root / relative;
        std::error_code       ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        return p;
    }

    auto WriteText(std::string_view relative, std::string_view contents) const -> std::filesystem::path {
        std::filesystem::path p = SubPath(relative);
        std::ofstream         out {p, std::ios::binary | std::ios::trunc};
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        return p;
    }

    auto WriteBytes(std::string_view relative, std::span<const std::byte> bytes) const -> std::filesystem::path {
        std::filesystem::path p = SubPath(relative);
        std::ofstream         out {p, std::ios::binary | std::ios::trunc};
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return p;
    }

  private:
    std::filesystem::path m_root;

    static auto SanitizeName(std::string_view raw) -> std::string {
        std::string out;
        out.reserve(std::min<std::size_t>(raw.size(), 40));
        for (char c: raw) {
            if (out.size() >= 40) {
                break;
            }
            const auto uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc) || c == '_' || c == '-') {
                out.push_back(c);
            } else {
                out.push_back('_');
            }
        }
        return out.empty() ? std::string {"case"} : out;
    }
};

// ============================================================================
// Declarative @parametrize Marks (Case, NamedCase, CaseValues, Param)
// ============================================================================
//
// Attach repeatable Case{...} / NamedCase{"id", ...} rows or stacked Cartesian
// Param{...} axes via ZHLN_ANNOTATION(...) on any test method. Fixture
// parameters may be freely mixed alongside parameterized arguments.
namespace TemplatedDetail {

template <std::size_t N>
struct StructuralStringLiteral {
    static constexpr std::size_t kStorageSize = (((N > 0 ? N : 1) + 7u) / 8u) * 8u;

    char data[kStorageSize] {};

    constexpr StructuralStringLiteral() = default;
    constexpr StructuralStringLiteral(const char (&str)[N]) {
        std::copy_n(str, N, data);
    }

    [[nodiscard]] constexpr auto View() const noexcept -> std::string_view {
        return N > 0 ? std::string_view {data, N - 1} : std::string_view {};
    }
    [[nodiscard]] constexpr auto CStr() const noexcept -> const char* { return data; }
    constexpr operator std::string_view() const noexcept { return View(); }
    constexpr operator const char*() const noexcept { return data; }
};

template <typename T>
struct StructuralParamDecay {
    using Type = std::remove_cvref_t<T>;
};

template <std::size_t N>
struct StructuralParamDecay<const char (&)[N]> {
    using Type = StructuralStringLiteral<N>;
};

template <std::size_t N>
struct StructuralParamDecay<char (&)[N]> {
    using Type = StructuralStringLiteral<N>;
};

template <std::size_t Index, typename T>
struct StructuralTupleLeaf {
    T value;
};

template <typename Seq, typename... Ts>
struct StructuralTupleImpl;

template <std::size_t... Is, typename... Ts>
struct StructuralTupleImpl<std::index_sequence<Is...>, Ts...> : StructuralTupleLeaf<Is, Ts>... {
    constexpr StructuralTupleImpl() = default;

    template <typename... Us>
        requires (sizeof...(Us) == sizeof...(Ts))
    constexpr explicit StructuralTupleImpl(Us&&... args)
        : StructuralTupleLeaf<Is, Ts> {Ts(std::forward<Us>(args))}... {}
};

template <typename... Ts>
using StructuralTuple = StructuralTupleImpl<std::index_sequence_for<Ts...>, Ts...>;

template <std::size_t Index, typename... Ts>
constexpr auto GetStructuralElement(const StructuralTupleImpl<std::index_sequence_for<Ts...>, Ts...>& tup) noexcept
    -> const auto& {
    using Elem = std::tuple_element_t<Index, std::tuple<Ts...>>;
    return static_cast<const StructuralTupleLeaf<Index, Elem>&>(tup).value;
}

} // namespace TemplatedDetail

template <typename... Ts>
struct Case {
    static constexpr bool        isTestCaseRow = true;
    static constexpr bool        hasCustomName = false;
    static constexpr std::size_t arity         = sizeof...(Ts);

    TemplatedDetail::StructuralTuple<Ts...> values;

    constexpr Case() = default;

    template <typename... Us>
        requires (sizeof...(Us) == sizeof...(Ts))
    constexpr explicit Case(Us&&... args) : values(std::forward<Us>(args)...) {}
};

template <typename... Us>
Case(Us&&...) -> Case<typename TemplatedDetail::StructuralParamDecay<Us>::Type...>;

template <std::size_t NameN, typename... Ts>
struct NamedCase {
    static constexpr bool        isTestCaseRow = true;
    static constexpr bool        hasCustomName = true;
    static constexpr std::size_t arity         = sizeof...(Ts);

    TemplatedDetail::StructuralTuple<Ts...>         values;
    TemplatedDetail::StructuralStringLiteral<NameN> caseId;

    template <typename... Us>
        requires (sizeof...(Us) == sizeof...(Ts))
    constexpr explicit NamedCase(const char (&id)[NameN], Us&&... args)
        : values(std::forward<Us>(args)...), caseId(id) {}
};

template <std::size_t NameN, typename... Us>
NamedCase(const char (&)[NameN], Us&&...) -> NamedCase<NameN, typename TemplatedDetail::StructuralParamDecay<Us>::Type...>;

template <auto... Vs>
struct CaseValues {
    static constexpr bool        isTestCaseValues = true;
    static constexpr std::size_t arity            = sizeof...(Vs);

    static constexpr auto AsCase() { return Case {Vs...}; }
};

template <typename... Rows>
struct Cases {
    static constexpr bool        isTestCaseTable = true;
    static constexpr std::size_t rowCount        = sizeof...(Rows);

    TemplatedDetail::StructuralTuple<Rows...> rows;

    template <typename... URows>
        requires (sizeof...(URows) == sizeof...(Rows) && sizeof...(Rows) >= 1)
    constexpr explicit Cases(URows&&... r) : rows(std::forward<URows>(r)...) {}
};

template <typename... URows>
Cases(URows&&...) -> Cases<std::remove_cvref_t<URows>...>;

template <typename... Ts>
struct Param {
    static constexpr bool        isTestParamAxis = true;
    static constexpr std::size_t size            = sizeof...(Ts);

    TemplatedDetail::StructuralTuple<Ts...> values;

    template <typename... Us>
        requires (sizeof...(Us) == sizeof...(Ts) && sizeof...(Ts) >= 1)
    constexpr explicit Param(Us&&... args) : values(std::forward<Us>(args)...) {}
};

template <typename... Us>
Param(Us&&...) -> Param<typename TemplatedDetail::StructuralParamDecay<Us>::Type...>;

// Telemetry counters populated via ZHLN::SetLogObserver without plumbing test
// sinks through RenderConfig, RenderContext, or RHI instance internals.
inline std::atomic<uint32_t> g_validationErrors {0};
inline std::atomic<uint32_t> g_deviceLost {0};

inline void OnTestLog(ZHLN::LogSeverity severity, std::string_view message) noexcept {
    if (severity != ZHLN::LogSeverity::Error) {
        return;
    }
    if (message.starts_with("[Vulkan]")) {
        g_validationErrors.fetch_add(1, std::memory_order::relaxed);
    } else if (message.starts_with("[DeviceLost]")) {
        g_deviceLost.fetch_add(1, std::memory_order::relaxed);
    }
}

inline const bool g_testLogObserverInstalled = []() noexcept {
    ZHLN::SetLogObserver(&OnTestLog);
    return true;
}();

[[nodiscard]] inline uint32_t ValidationErrorCount() noexcept {
    return g_validationErrors.load(std::memory_order::relaxed);
}

[[nodiscard]] inline uint32_t DeviceLostCount() noexcept {
    return g_deviceLost.load(std::memory_order::relaxed);
}

// Internal to the framework: this is what the runner seeds a result with
// before invoking a case. Timeouts instead exit the process with status 124.
// It is deliberately NOT what a test returns — a test that reports this
// declined to say what went wrong. Callers define their own error enum and
// return that; see the Expectations comment.
enum class TestFrameworkError : uint8_t {
    AssertionFailed ZHLN_ANNOTATION(ZHLN::Description<"One or more assertions failed in this test. "> {}) = 1,
};

struct AssertionFailure {
    std::string_view file {};
    uint32_t         line = 0;
    std::string      actualValue {};
    std::string      expectedValue {};
    std::string_view op {};         // "==", "!=", "<", "<=", ">", ">=", "true", "false", "ValidationError", "DeviceLost", ...
    std::string      expression {}; // trimmed source line at `line`, when readable
    bool             decomposed = false;
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
         .expectedValue = std::format("< {}", FormatValue(bound)),
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
         .expectedValue = std::format("> {}", FormatValue(bound)),
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
         .expectedValue = std::format("<= {}", FormatValue(bound)),
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
         .expectedValue = std::format(">= {}", FormatValue(bound)),
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
         .expectedValue = std::format("[{}, {}]", FormatValue(lo), FormatValue(hi)),
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

// ============================================================================
// Macro-Free Expression-Decomposing Expect(lhs) == rhs
// ============================================================================
//
// Usage:
//   ZHLN::Test::Expect(actual) == expected;
//   ZHLN::Test::Expect(count) >= 1;
//   ZHLN::Test::Expect(value) == ZHLN::Test::Approx(1.0f, 1e-4f);
//   ZHLN::Test::Expect(reg.IsAlive(entity));
//   if (!(ZHLN::Test::Expect(ptr != nullptr))) return std::unexpected(...);

#if defined(__clang__)
// `Expect(lhs) == rhs;` intentionally invokes overloaded comparison operators
// as standalone assertion statements. Clang's `Expr::isUnusedResultAWarning` /
// `DiagnoseUnusedComparison` unconditionally flags discarded overloaded
// `==`, `!=`, `<`, `<=`, `>`, and `>=` calls regardless of side effects or
// return type unless `-Wunused-comparison` is suppressed across test TUs.
#pragma clang diagnostic ignored "-Wunused-comparison"
#endif

template <std::floating_point T = double>
struct ApproxValue {
    static constexpr bool isApproxMatcher = true;
    T                     value;
    T                     epsilon;
};

template <std::floating_point T>
[[nodiscard]] constexpr auto Approx(T value, T epsilon = static_cast<T>(1e-5)) noexcept -> ApproxValue<T> {
    return ApproxValue<T> {.value = value, .epsilon = epsilon};
}

namespace TemplatedDetail {

template <typename T>
concept SafeComparableInteger = std::integral<std::remove_cvref_t<T>>
    && !std::same_as<std::remove_cvref_t<T>, bool>
    && !std::same_as<std::remove_cvref_t<T>, char>
    && !std::same_as<std::remove_cvref_t<T>, char8_t>
    && !std::same_as<std::remove_cvref_t<T>, char16_t>
    && !std::same_as<std::remove_cvref_t<T>, char32_t>
    && !std::same_as<std::remove_cvref_t<T>, wchar_t>;

template <typename T>
concept NarrowCStringLike =
    std::same_as<std::decay_t<T>, const char*> || std::same_as<std::decay_t<T>, char*>;

template <typename T>
concept ApproxMatcherType = requires { std::remove_cvref_t<T>::isApproxMatcher; };

template <typename T>
auto FormatOperand(const T& operand) -> std::string {
    using CleanT = std::remove_cvref_t<T>;
    if constexpr (ApproxMatcherType<CleanT>) {
        return std::format("{} (+/- {})", FormatValue(operand.value), FormatValue(operand.epsilon));
    } else if constexpr (NarrowCStringLike<T>) {
        const char* ptr = operand;
        return ptr == nullptr ? std::string {"nullptr"} : std::format("\"{}\"", ptr);
    } else if constexpr (std::same_as<CleanT, std::string> || std::same_as<CleanT, std::string_view>) {
        return std::format("\"{}\"", operand);
    } else if constexpr (std::same_as<CleanT, std::nullptr_t>) {
        return "nullptr";
    } else {
        return FormatValue(operand);
    }
}

inline void RecordDecomposedFailure(
    std::source_location loc,
    std::string_view     op,
    std::string          actualValue,
    std::string          expectedValue
) {
    auto& ctx = GetThreadLocalContext();
    ctx.failures.push_back(AssertionFailure {
        .file          = loc.file_name(),
        .line          = loc.line(),
        .actualValue   = std::move(actualValue),
        .expectedValue = std::move(expectedValue),
        .op            = op,
        .expression    = ReadSourceLine(loc.file_name(), loc.line()),
        .decomposed    = true,
    });
}

template <typename Expr>
class ExpectLhs {
    using Stored = std::conditional_t<
        std::is_lvalue_reference_v<Expr>,
        const std::remove_reference_t<Expr>&,
        std::remove_cvref_t<Expr>>;

    Stored               m_lhs;
    std::source_location m_loc;
    bool                 m_consumed = false;

  public:
    constexpr explicit ExpectLhs(Expr&& lhs, std::source_location loc)
        : m_lhs(std::forward<Expr>(lhs)), m_loc(loc) {}

    ExpectLhs(const ExpectLhs&)            = delete;
    ExpectLhs& operator=(const ExpectLhs&) = delete;
    ExpectLhs(ExpectLhs&&)                 = delete;
    ExpectLhs& operator=(ExpectLhs&&)      = delete;

    ~ExpectLhs() {
        if (m_consumed) {
            return;
        }
        if constexpr (std::convertible_to<const Stored&, bool>) {
            if (!static_cast<bool>(m_lhs)) {
                auto& ctx = GetThreadLocalContext();
                ctx.failures.push_back(AssertionFailure {
                    .file          = m_loc.file_name(),
                    .line          = m_loc.line(),
                    .actualValue   = "false",
                    .expectedValue = "true",
                    .op            = "true",
                    .expression    = ReadSourceLine(m_loc.file_name(), m_loc.line()),
                    .decomposed    = false,
                });
            }
        } else {
            RecordDecomposedFailure(m_loc, "incomplete", FormatOperand(m_lhs), "<missing comparison operator>");
        }
    }

    auto IsTrue() && -> bool {
        m_consumed       = true;
        const bool ok    = static_cast<bool>(m_lhs);
        if (!ok) {
            RecordDecomposedFailure(m_loc, "==", FormatOperand(m_lhs), "true");
        }
        return ok;
    }

    auto IsFalse() && -> bool {
        m_consumed       = true;
        const bool ok    = !static_cast<bool>(m_lhs);
        if (!ok) {
            RecordDecomposedFailure(m_loc, "==", FormatOperand(m_lhs), "false");
        }
        return ok;
    }

    template <typename Rhs>
    auto operator==(const Rhs& rhs) && -> bool {
        m_consumed = true;
        bool ok    = false;
        if constexpr (ApproxMatcherType<Rhs>) {
            using Common = std::common_type_t<std::remove_cvref_t<Stored>, decltype(rhs.value)>;
            const Common diff = std::abs(static_cast<Common>(m_lhs) - static_cast<Common>(rhs.value));
            ok                = diff <= static_cast<Common>(rhs.epsilon);
        } else if constexpr (SafeComparableInteger<Stored> && SafeComparableInteger<Rhs>) {
            ok = std::cmp_equal(m_lhs, rhs);
        } else if constexpr (NarrowCStringLike<Stored> && NarrowCStringLike<Rhs>) {
            const char* a = m_lhs;
            const char* b = rhs;
            ok            = (a == nullptr && b == nullptr) || (a != nullptr && b != nullptr && std::string_view {a} == std::string_view {b});
        } else {
            ok = static_cast<bool>(m_lhs == rhs);
        }
        if (!ok) {
            RecordDecomposedFailure(m_loc, "==", FormatOperand(m_lhs), FormatOperand(rhs));
        }
        return ok;
    }

    template <typename Rhs>
    auto operator!=(const Rhs& rhs) && -> bool {
        m_consumed = true;
        bool ok    = false;
        if constexpr (ApproxMatcherType<Rhs>) {
            using Common = std::common_type_t<std::remove_cvref_t<Stored>, decltype(rhs.value)>;
            const Common diff = std::abs(static_cast<Common>(m_lhs) - static_cast<Common>(rhs.value));
            ok                = diff > static_cast<Common>(rhs.epsilon);
        } else if constexpr (SafeComparableInteger<Stored> && SafeComparableInteger<Rhs>) {
            ok = std::cmp_not_equal(m_lhs, rhs);
        } else if constexpr (NarrowCStringLike<Stored> && NarrowCStringLike<Rhs>) {
            const char* a = m_lhs;
            const char* b = rhs;
            ok            = !((a == nullptr && b == nullptr) || (a != nullptr && b != nullptr && std::string_view {a} == std::string_view {b}));
        } else {
            ok = static_cast<bool>(m_lhs != rhs);
        }
        if (!ok) {
            RecordDecomposedFailure(m_loc, "!=", FormatOperand(m_lhs), FormatOperand(rhs));
        }
        return ok;
    }

    template <typename Rhs>
    auto operator<(const Rhs& rhs) && -> bool {
        m_consumed = true;
        bool ok    = false;
        if constexpr (SafeComparableInteger<Stored> && SafeComparableInteger<Rhs>) {
            ok = std::cmp_less(m_lhs, rhs);
        } else if constexpr (NarrowCStringLike<Stored> && NarrowCStringLike<Rhs>) {
            ok = std::string_view {m_lhs ? m_lhs : ""} < std::string_view {rhs ? rhs : ""};
        } else {
            ok = static_cast<bool>(m_lhs < rhs);
        }
        if (!ok) {
            RecordDecomposedFailure(m_loc, "<", FormatOperand(m_lhs), FormatOperand(rhs));
        }
        return ok;
    }

    template <typename Rhs>
    auto operator<=(const Rhs& rhs) && -> bool {
        m_consumed = true;
        bool ok    = false;
        if constexpr (SafeComparableInteger<Stored> && SafeComparableInteger<Rhs>) {
            ok = std::cmp_less_equal(m_lhs, rhs);
        } else if constexpr (NarrowCStringLike<Stored> && NarrowCStringLike<Rhs>) {
            ok = std::string_view {m_lhs ? m_lhs : ""} <= std::string_view {rhs ? rhs : ""};
        } else {
            ok = static_cast<bool>(m_lhs <= rhs);
        }
        if (!ok) {
            RecordDecomposedFailure(m_loc, "<=", FormatOperand(m_lhs), FormatOperand(rhs));
        }
        return ok;
    }

    template <typename Rhs>
    auto operator>(const Rhs& rhs) && -> bool {
        m_consumed = true;
        bool ok    = false;
        if constexpr (SafeComparableInteger<Stored> && SafeComparableInteger<Rhs>) {
            ok = std::cmp_greater(m_lhs, rhs);
        } else if constexpr (NarrowCStringLike<Stored> && NarrowCStringLike<Rhs>) {
            ok = std::string_view {m_lhs ? m_lhs : ""} > std::string_view {rhs ? rhs : ""};
        } else {
            ok = static_cast<bool>(m_lhs > rhs);
        }
        if (!ok) {
            RecordDecomposedFailure(m_loc, ">", FormatOperand(m_lhs), FormatOperand(rhs));
        }
        return ok;
    }

    template <typename Rhs>
    auto operator>=(const Rhs& rhs) && -> bool {
        m_consumed = true;
        bool ok    = false;
        if constexpr (SafeComparableInteger<Stored> && SafeComparableInteger<Rhs>) {
            ok = std::cmp_greater_equal(m_lhs, rhs);
        } else if constexpr (NarrowCStringLike<Stored> && NarrowCStringLike<Rhs>) {
            ok = std::string_view {m_lhs ? m_lhs : ""} >= std::string_view {rhs ? rhs : ""};
        } else {
            ok = static_cast<bool>(m_lhs >= rhs);
        }
        if (!ok) {
            RecordDecomposedFailure(m_loc, ">=", FormatOperand(m_lhs), FormatOperand(rhs));
        }
        return ok;
    }
};

} // namespace TemplatedDetail

template <typename Expr>
constexpr auto Expect(Expr&& lhs, std::source_location loc = std::source_location::current()) -> TemplatedDetail::ExpectLhs<Expr> {
    return TemplatedDetail::ExpectLhs<Expr>(std::forward<Expr>(lhs), loc);
}

struct TestStats {
    uint32_t passed      = 0;
    uint32_t failed      = 0;
    uint32_t quarantined = 0;
    uint32_t skipped     = 0;
};

struct FailedTestSummary {
    std::string suite;
    std::string test;
    std::string detail;
};

struct FailedTestSummaryStorage {
    std::vector<FailedTestSummary> values;
};

inline auto FailedTestSummariesStorage() noexcept -> FailedTestSummaryStorage& {
    static FailedTestSummaryStorage storage;
    return storage;
}

[[nodiscard]] inline auto GetFailedTestSummaries() noexcept -> std::span<const FailedTestSummary> {
    return FailedTestSummariesStorage().values;
}

inline void RecordFailedTestSummary(FailedTestSummary summary) {
    FailedTestSummariesStorage().values.push_back(std::move(summary));
}

inline void ClearFailedTestSummaries() noexcept {
    FailedTestSummariesStorage().values.clear();
}

template <typename T>
concept ErrorCodeTestResult = requires(T t) {
    { t.has_value() } -> std::convertible_to<bool>;
    { t.error() } -> std::convertible_to<ZHLN::Error>;
};

template <typename T>
concept StringTestResult = requires(T t) {
    { t.has_value() } -> std::convertible_to<bool>;
    { t.error() } -> std::convertible_to<std::string_view>;
};

template <typename T>
concept TestResult = ErrorCodeTestResult<T> || StringTestResult<T>;

template <typename T>
concept FreeTestReturnType = std::is_void_v<T> || TestResult<T>;

template <typename T>
concept HasNestedTests = requires { typename T::Tests; };

// ============================================================================
// Worker CLI Options, Filter Expressions (-k / -m), & Mark Extraction
// ============================================================================

struct RunnerOptions {
    bool                     ztestList        = false; // --ztest-list (machine-readable discovery)
    bool                     collectOnly      = false; // --collect-only / --list (human discovery)
    bool                     workerMode       = false; // --ztest-worker (machine event stream)
    bool                     verbose          = false; // -v / --verbose
    bool                     strictQuarantine = false; // --strict-quarantine
    bool                     showHelp         = false; // -h / --help / --version
    bool                     parseFailed      = false; // CLI parse error
    uint32_t                 maxFail          = 0;     // -x (1) or --maxfail=N (0 = unlimited)
    std::string              keywordExpr;              // -k <expr>
    std::string              markExpr;                 // -m <expr>
    std::string              legacyFilter;             // ZHLN_TEST_FILTER compatibility
    std::string              targetName;               // --target-name=<name>
    std::vector<std::string> targetTags;               // --target-tags=<csv>
    std::vector<std::string> sourceFiles;              // --file=<relFile>
    std::vector<std::string> exactCases;               // --exact=<nodeId>
    std::string              skipUntilCase;            // --skip-until=<nodeId>
};

struct SkipUntilState {
    bool active = false;
};

namespace TemplatedDetail {

struct CaseMarks {
    bool                             isQuarantined         = false;
    std::string_view                 quarantineReason      = {};
    std::string_view                 quarantineEvidence    = {};
    bool                             isSkipped             = false;
    std::string_view                 skipReason            = {};
    bool                             hasTimeout            = false;
    unsigned int                     timeoutSeconds        = 0;
    bool                             allowValidationErrors = false;
    bool                             allowDeviceLost       = false;
    bool                             isHelper              = false;
    std::array<std::string_view, 16> tags                  = {};
    std::size_t                      tagCount              = 0;

    constexpr void AddTag(std::string_view tag) {
        if (tag.empty() || tagCount >= tags.size()) {
            return;
        }
        for (std::size_t i = 0; i < tagCount; ++i) {
            if (tags[i] == tag) {
                return;
            }
        }
        tags[tagCount++] = tag;
    }
};

template <typename Annotation>
consteval void ApplyAnnotation(CaseMarks& marks) {
    if constexpr (requires { Annotation::isTestQuarantine; }) {
        marks.isQuarantined      = true;
        marks.quarantineReason   = Annotation::reason;
        marks.quarantineEvidence = Annotation::evidence;
    }
    if constexpr (requires { Annotation::isTestSkip; }) {
        marks.isSkipped  = true;
        marks.skipReason = Annotation::reason;
    }
    if constexpr (requires { Annotation::isTestTimeout; }) {
        marks.hasTimeout     = true;
        marks.timeoutSeconds = Annotation::seconds;
    }
    if constexpr (requires { Annotation::isTestTags; }) {
        for (std::string_view tag: Annotation::names) {
            marks.AddTag(tag);
        }
    }
    if constexpr (requires { Annotation::isAllowValidation; }) {
        marks.allowValidationErrors = true;
    }
    if constexpr (requires { Annotation::isAllowDeviceLost; }) {
        marks.allowDeviceLost = true;
    }
    if constexpr (requires { Annotation::isTestHelper; }) {
        marks.isHelper = true;
    }
}

template <typename Suite, typename Method>
consteval auto ExtractCaseMarks() -> CaseMarks {
    CaseMarks marks {};
    ZHLN::Reflect::ForEachAnnotationType<Suite>([&]<typename A>() { ApplyAnnotation<A>(marks); });
    if constexpr (HasNestedTests<Suite>) {
        ZHLN::Reflect::ForEachAnnotationType<typename Suite::Tests>([&]<typename A>() { ApplyAnnotation<A>(marks); });
    }
    Method::ForEachAnnotationType([&]<typename A>() { ApplyAnnotation<A>(marks); });
    return marks;
}

template <typename FreeFn>
consteval auto ExtractFreeFunctionCaseMarks() -> CaseMarks {
    CaseMarks marks {};
    FreeFn::template ForEachEnclosingNamespaceAnnotationType<Scope>([&]<typename A>() { ApplyAnnotation<A>(marks); });
    FreeFn::ForEachAnnotationType([&]<typename A>() { ApplyAnnotation<A>(marks); });
    return marks;
}

// ============================================================================
// Fixture Metadata, Scope Store, & Dependency Injection
// ============================================================================

struct FixtureMeta {
    bool  enabled = false;
    Scope scope   = Scope::Function;
};

template <typename T>
consteval auto ResolveFixtureMeta() -> FixtureMeta {
    using CleanT = std::remove_cvref_t<T>;
    if constexpr (FixtureTraits<CleanT>::enabled) {
        return FixtureMeta {.enabled = true, .scope = FixtureTraits<CleanT>::scope};
    } else if constexpr (!std::is_class_v<CleanT> || !requires { sizeof(CleanT); }) {
        return FixtureMeta {};
    } else {
        FixtureMeta meta {};
        ZHLN::Reflect::ForEachAnnotationType<CleanT>([&]<typename A>() {
            if constexpr (requires { A::isTestFixture; }) {
                meta.enabled = true;
                meta.scope   = A::scope;
            }
        });
        return meta;
    }
}

template <typename T>
concept IsTestFixture = ResolveFixtureMeta<T>().enabled;

template <typename CleanT>
inline constexpr char kFixtureTypeAnchor = 0;

class FixtureScopeStore {
  public:
    FixtureScopeStore() = default;
    ~FixtureScopeStore() { Clear(); }

    FixtureScopeStore(const FixtureScopeStore&)            = delete;
    FixtureScopeStore& operator=(const FixtureScopeStore&) = delete;
    FixtureScopeStore(FixtureScopeStore&&)                 = delete;
    FixtureScopeStore& operator=(FixtureScopeStore&&)      = delete;

    [[nodiscard]] auto Find(const void* key) const noexcept -> void* {
        for (const auto& entry: m_entries) {
            if (entry.key == key) {
                return entry.instance;
            }
        }
        return nullptr;
    }

    void Insert(const void* key, void* instance, void (*destroy)(void*)) {
        m_entries.push_back(Entry {.key = key, .instance = instance, .destroy = destroy});
    }

    void Clear() noexcept {
        while (!m_entries.empty()) {
            const Entry entry = m_entries.back();
            m_entries.pop_back();
            entry.destroy(entry.instance);
        }
    }

  private:
    struct Entry {
        const void* key      = nullptr;
        void*       instance = nullptr;
        void (*destroy)(void*) = nullptr;
    };

    std::vector<Entry> m_entries;
};

struct FixtureRuntimeContext {
    FixtureScopeStore& session;
    FixtureScopeStore& suite;
    FixtureScopeStore& function;
    CaseInfo           caseInfo {};
};

template <typename Ctor>
consteval auto IsInjectableFixtureConstructor() -> bool {
    if constexpr (Ctor::arity == 0) {
        return false;
    } else {
        return []<std::size_t... Is>(std::index_sequence<Is...>) {
            return (IsTestFixture<std::remove_cvref_t<typename Ctor::template ParameterType<Is>>> && ...);
        }(std::make_index_sequence<Ctor::arity> {});
    }
}

template <typename CleanT>
consteval auto HasInjectableFixtureConstructor() -> bool {
    bool found = false;
    ZHLN::Reflect::ForEachReflectedConstructor<CleanT>([&]<typename Ctor>() {
        if constexpr (IsInjectableFixtureConstructor<Ctor>()) {
            found = true;
        }
    });
    return found;
}

template <typename CleanT>
auto ResolveFixture(FixtureRuntimeContext& rt) -> CleanT&;

template <typename CleanT>
auto ConstructFixtureOnHeap([[maybe_unused]] FixtureRuntimeContext& rt) -> CleanT* {
    constexpr Scope ownScope = ResolveFixtureMeta<CleanT>().scope;
    if constexpr (requires { FixtureTraits<CleanT>::Create(rt); }) {
        return FixtureTraits<CleanT>::Create(rt);
    } else if constexpr (std::is_constructible_v<CleanT, const CaseInfo&>) {
        static_assert(
            ownScope == Scope::Function,
            "Fixtures constructed from per-case CaseInfo must have Scope::Function lifetime."
        );
        return new CleanT(rt.caseInfo);
    } else if constexpr (HasInjectableFixtureConstructor<CleanT>()) {
        CleanT* created = nullptr;
        ZHLN::Reflect::ForEachReflectedConstructor<CleanT>([&]<typename Ctor>() {
            if constexpr (IsInjectableFixtureConstructor<Ctor>()) {
                if (created == nullptr) {
                    created = [&]<std::size_t... Is>(std::index_sequence<Is...>) -> CleanT* {
                        static_assert(
                            ((static_cast<uint8_t>(
                                  ResolveFixtureMeta<std::remove_cvref_t<typename Ctor::template ParameterType<Is>>>().scope
                              )
                              >= static_cast<uint8_t>(ownScope))
                             && ...),
                            "A Suite- or Session-scoped fixture cannot depend on a shorter-lived fixture."
                        );
                        return new CleanT(
                            ResolveFixture<std::remove_cvref_t<typename Ctor::template ParameterType<Is>>>(rt)...
                        );
                    }(std::make_index_sequence<Ctor::arity> {});
                }
            }
        });
        return created;
    } else {
        static_assert(
            std::is_default_constructible_v<CleanT>,
            "Test fixture must be default-constructible, constructible from const CaseInfo&, or have a constructor accepting other fixtures."
        );
        return new CleanT {};
    }
}

template <typename CleanT>
auto ResolveFixture(FixtureRuntimeContext& rt) -> CleanT& {
    if constexpr (std::same_as<CleanT, CaseInfo>) {
        return rt.caseInfo;
    } else {
        constexpr Scope     scope = ResolveFixtureMeta<CleanT>().scope;
        FixtureScopeStore&  store = scope == Scope::Session ? rt.session : (scope == Scope::Suite ? rt.suite : rt.function);
        const void* const   key   = &kFixtureTypeAnchor<CleanT>;
        if (void* existing = store.Find(key); existing != nullptr) {
            return *static_cast<CleanT*>(existing);
        }
        CleanT* created = ConstructFixtureOnHeap<CleanT>(rt);
        store.Insert(key, created, +[](void* ptr) { delete static_cast<CleanT*>(ptr); });
        return *created;
    }
}

// ============================================================================
// @parametrize Introspection & Method Eligibility
// ============================================================================

template <typename T>
concept CaseRowAnnotation = requires { std::remove_cvref_t<T>::isTestCaseRow; }
    || requires { std::remove_cvref_t<T>::isTestCaseValues; };

template <typename T>
concept CaseTableAnnotation = requires { std::remove_cvref_t<T>::isTestCaseTable; };

template <typename T>
concept ParamAxisAnnotation = requires { std::remove_cvref_t<T>::isTestParamAxis; };

template <typename Method>
consteval auto ExplicitCaseAnnotationCount() -> std::size_t {
    std::size_t count = 0;
    Method::ForEachAnnotationType([&]<typename A>() {
        if constexpr (CaseRowAnnotation<A>) {
            ++count;
        } else if constexpr (CaseTableAnnotation<A>) {
            count += std::remove_cvref_t<A>::rowCount;
        }
    });
    return count;
}

template <typename Method>
consteval auto ParamAxisAnnotationCount() -> std::size_t {
    std::size_t count = 0;
    Method::ForEachAnnotationType([&]<typename A>() {
        if constexpr (ParamAxisAnnotation<A>) {
            ++count;
        }
    });
    return count;
}

template <typename Method>
consteval auto NonFixtureParamCount() -> std::size_t {
    if constexpr (Method::arity == 0) {
        return 0;
    } else {
        return []<std::size_t... Is>(std::index_sequence<Is...>) {
            std::size_t count = 0;
            ((IsTestFixture<std::remove_cvref_t<typename Method::template ParameterType<Is>>> ? void() : void(++count)), ...);
            return count;
        }(std::make_index_sequence<Method::arity> {});
    }
}

template <typename Method, std::size_t ParamIndex>
consteval auto NonFixtureOrdinal() -> std::size_t {
    return []<std::size_t... Is>(std::index_sequence<Is...>) {
        std::size_t ordinal = 0;
        ((Is < ParamIndex && !IsTestFixture<std::remove_cvref_t<typename Method::template ParameterType<Is>>>
              ? void(++ordinal)
              : void()),
         ...);
        return ordinal;
    }(std::make_index_sequence<Method::arity> {});
}

template <typename Suite, typename Method>
consteval auto IsTestCaseMethod() -> bool {
    constexpr CaseMarks marks       = ExtractCaseMarks<Suite, Method>();
    constexpr bool      isNamedTest = Method::name.starts_with("test_");
    if constexpr (marks.isHelper) {
        return false;
    } else if constexpr (ExplicitCaseAnnotationCount<Method>() > 0 || ParamAxisAnnotationCount<Method>() > 0) {
        static_assert(
            FreeTestReturnType<typename Method::ReturnType>,
            "Methods annotated with Case/NamedCase/CaseValues/Param must return void or std::expected<void, Error>."
        );
        return true;
    } else if constexpr (isNamedTest && std::is_void_v<typename Method::ReturnType>) {
        return NonFixtureParamCount<Method>() == 0;
    } else if constexpr (!TestResult<typename Method::ReturnType>) {
        return false;
    } else {
        return NonFixtureParamCount<Method>() == 0;
    }
}

template <typename FreeFn>
consteval auto IsFreeTestCaseFunction() -> bool {
    constexpr CaseMarks marks = ExtractFreeFunctionCaseMarks<FreeFn>();
    if constexpr (marks.isHelper) {
        return false;
    } else if constexpr (ExplicitCaseAnnotationCount<FreeFn>() > 0 || ParamAxisAnnotationCount<FreeFn>() > 0) {
        static_assert(
            FreeTestReturnType<typename FreeFn::ReturnType>,
            "Free test_* functions annotated with Case/NamedCase/CaseValues/Param must return void or std::expected<void, Error>."
        );
        return true;
    } else {
        static_assert(
            FreeTestReturnType<typename FreeFn::ReturnType>,
            "Free test_* functions in namespace ZHLN::Test must return void or std::expected<void, Error> (or mark helpers with TestHelper{})."
        );
        static_assert(
            NonFixtureParamCount<FreeFn>() == 0,
            "Free test_* function has non-fixture parameter(s) without Case/NamedCase/CaseValues/Param annotations; "
            "annotate fixture types with FunctionFixture/SuiteFixture/SessionFixture, add Case{...}, or mark helpers with TestHelper{}."
        );
        return true;
    }
}

inline auto AsciiLower(char c) noexcept -> char {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

inline auto EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept -> bool {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (AsciiLower(a[i]) != AsciiLower(b[i])) {
            return false;
        }
    }
    return true;
}

inline auto ContainsIgnoreCase(std::string_view haystack, std::string_view needle) noexcept -> bool {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > haystack.size()) {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        if (EqualsIgnoreCase(haystack.substr(i, needle.size()), needle)) {
            return true;
        }
    }
    return false;
}

template <typename Predicate>
class BooleanExprEvaluator {
  public:
    BooleanExprEvaluator(std::string_view expr, Predicate&& pred)
        : m_expr(expr), m_pred(std::forward<Predicate>(pred)) {}

    auto Evaluate() -> bool {
        if (Trim(m_expr).empty()) {
            return true;
        }
        const bool value = ParseOr();
        SkipSpaces();
        return m_pos == m_expr.size() ? value : false;
    }

  private:
    std::string_view m_expr;
    std::size_t      m_pos = 0;
    Predicate        m_pred;

    static auto Trim(std::string_view sv) noexcept -> std::string_view {
        while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
            sv.remove_prefix(1);
        }
        while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
            sv.remove_suffix(1);
        }
        return sv;
    }

    static auto IsTokenBoundary(char c) noexcept -> bool {
        return std::isspace(static_cast<unsigned char>(c)) || c == '(' || c == ')' || c == '!' || c == '&' || c == '|';
    }

    void SkipSpaces() noexcept {
        while (m_pos < m_expr.size() && std::isspace(static_cast<unsigned char>(m_expr[m_pos]))) {
            ++m_pos;
        }
    }

    auto ConsumeKeyword(std::string_view kw) noexcept -> bool {
        SkipSpaces();
        if (m_pos + kw.size() > m_expr.size()) {
            return false;
        }
        if (!EqualsIgnoreCase(m_expr.substr(m_pos, kw.size()), kw)) {
            return false;
        }
        if (m_pos + kw.size() < m_expr.size() && !IsTokenBoundary(m_expr[m_pos + kw.size()])) {
            return false;
        }
        m_pos += kw.size();
        return true;
    }

    auto ConsumeOp(std::string_view op) noexcept -> bool {
        SkipSpaces();
        if (m_expr.substr(m_pos).starts_with(op)) {
            m_pos += op.size();
            return true;
        }
        return false;
    }

    auto ParseOr() -> bool {
        bool lhs = ParseAnd();
        while (ConsumeKeyword("or") || ConsumeOp("||")) {
            const bool rhs = ParseAnd();
            lhs            = lhs || rhs;
        }
        return lhs;
    }

    auto ParseAnd() -> bool {
        bool lhs = ParseUnary();
        while (ConsumeKeyword("and") || ConsumeOp("&&")) {
            const bool rhs = ParseUnary();
            lhs            = lhs && rhs;
        }
        return lhs;
    }

    auto ParseUnary() -> bool {
        if (ConsumeKeyword("not") || ConsumeOp("!")) {
            return !ParseUnary();
        }
        SkipSpaces();
        if (m_pos < m_expr.size() && m_expr[m_pos] == '(') {
            ++m_pos;
            const bool val = ParseOr();
            SkipSpaces();
            if (m_pos < m_expr.size() && m_expr[m_pos] == ')') {
                ++m_pos;
            }
            return val;
        }
        return ParseAtom();
    }

    auto ParseAtom() -> bool {
        SkipSpaces();
        if (m_pos >= m_expr.size()) {
            return false;
        }
        if (m_expr[m_pos] == '"' || m_expr[m_pos] == '\'') {
            const char        quote = m_expr[m_pos++];
            const std::size_t start = m_pos;
            while (m_pos < m_expr.size() && m_expr[m_pos] != quote) {
                ++m_pos;
            }
            const std::string_view token = m_expr.substr(start, m_pos - start);
            if (m_pos < m_expr.size()) {
                ++m_pos;
            }
            return m_pred(token);
        }
        const std::size_t start = m_pos;
        while (m_pos < m_expr.size() && !IsTokenBoundary(m_expr[m_pos])) {
            ++m_pos;
        }
        const std::string_view token = m_expr.substr(start, m_pos - start);
        return !token.empty() && m_pred(token);
    }
};

inline auto SanitizeProtocolField(std::string_view sv) -> std::string {
    std::string out;
    out.reserve(sv.size());
    for (char c: sv) {
        if (c == '\t' || c == '\n' || c == '\r') {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

inline void SplitCsvTags(std::string_view csv, std::vector<std::string>& out) {
    std::size_t pos = 0;
    while (pos <= csv.size()) {
        const std::size_t comma = csv.find_first_of(",;", pos);
        std::string_view  tok   = comma == std::string_view::npos ? csv.substr(pos) : csv.substr(pos, comma - pos);
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front()))) {
            tok.remove_prefix(1);
        }
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back()))) {
            tok.remove_suffix(1);
        }
        if (!tok.empty()) {
            out.emplace_back(tok);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
}

inline auto ParseRunnerOptions(int argc, char** argv) -> RunnerOptions {
    RunnerOptions opts {};
    if (const char* legacy = std::getenv("ZHLN_TEST_FILTER"); legacy != nullptr && *legacy != '\0') {
        opts.legacyFilter = legacy;
    }
    if (const char* targetEnv = std::getenv("ZHLN_ZTEST_TARGET"); targetEnv != nullptr && *targetEnv != '\0') {
        opts.targetName = targetEnv;
    }
    if (const char* labelsEnv = std::getenv("ZHLN_ZTEST_LABELS"); labelsEnv != nullptr && *labelsEnv != '\0') {
        SplitCsvTags(labelsEnv, opts.targetTags);
    }
    if (const char* logEnv = std::getenv("ZHLN_TEST_LOG"); logEnv != nullptr && std::string_view {logEnv} == "verbose") {
        opts.verbose = true;
    }
    if (const char* qEnv = std::getenv("ZHLN_TEST_QUARANTINE"); qEnv != nullptr && std::string_view {qEnv} == "off") {
        opts.strictQuarantine = true;
    }

    if (argv == nullptr || argc <= 1) {
        return opts;
    }

    auto takesSeparateValue = [](std::string_view flag) noexcept -> bool {
        return flag == "-k" || flag == "--filter" || flag == "-m" || flag == "--mark" || flag == "--target-name"
            || flag == "--target-tags" || flag == "--file" || flag == "--exact" || flag == "--skip-until"
            || flag == "--maxfail";
    };

    std::vector<std::string> normalizedStorage;
    normalizedStorage.reserve(static_cast<std::size_t>(argc));
    normalizedStorage.emplace_back(argv[0] != nullptr ? argv[0] : "ztest_worker");

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg {argv[i] != nullptr ? argv[i] : ""};
        if (arg == "--list") {
            normalizedStorage.emplace_back("--collect-only");
        } else if (arg == "--no-quarantine") {
            normalizedStorage.emplace_back("--strict-quarantine");
        } else if (arg.starts_with("-k") && arg.size() > 2 && arg[2] != '=') {
            normalizedStorage.push_back(std::format("--filter={}", arg.substr(2)));
        } else if (arg.starts_with("-m") && arg.size() > 2 && arg[2] != '=') {
            normalizedStorage.push_back(std::format("--mark={}", arg.substr(2)));
        } else if (takesSeparateValue(arg) && i + 1 < argc && argv[i + 1] != nullptr) {
            const std::string_view next {argv[++i]};
            normalizedStorage.push_back(std::format("{}={}", arg, next));
        } else if (!arg.starts_with("-")) {
            normalizedStorage.push_back(std::format("--file={}", arg));
        } else {
            normalizedStorage.emplace_back(arg);
        }
    }

    std::vector<char*> normalizedPtrs;
    normalizedPtrs.reserve(normalizedStorage.size());
    for (auto& s: normalizedStorage) {
        normalizedPtrs.push_back(s.data());
    }

    const std::array workerFlags = {
        ZHLN::CliFlag {
            .key         = "--ztest-list",
            .description = "Emit machine-readable test discovery records for ztest",
            .action      = [&](std::string_view) -> std::expected<void, ZHLN::CommandLineError> {
                opts.ztestList = true;
                return {};
            },
        },
        ZHLN::CliFlag {
            .key         = "--collect-only",
            .description = "List matching file::test entries without executing",
            .action      = [&](std::string_view) -> std::expected<void, ZHLN::CommandLineError> {
                opts.collectOnly = true;
                return {};
            },
        },
        ZHLN::CliFlag {
            .key         = "--ztest-worker",
            .description = "Emit structured @@ZTEST execution events for ztest",
            .action      = [&](std::string_view) -> std::expected<void, ZHLN::CommandLineError> {
                opts.workerMode = true;
                return {};
            },
        },
        ZHLN::CliFlag {
            .key         = "--stream-logs",
            .shortKey    = "-v",
            .description = "Stream engine logs live during test execution",
            .action      = [&](std::string_view) -> std::expected<void, ZHLN::CommandLineError> {
                opts.verbose = true;
                return {};
            },
        },
        ZHLN::CliFlag {
            .key         = "--verbose",
            .description = "Alias for -v / --stream-logs",
            .action      = [&](std::string_view) -> std::expected<void, ZHLN::CommandLineError> {
                opts.verbose = true;
                return {};
            },
        },
        ZHLN::CliFlag {
            .key         = "--quiet",
            .shortKey    = "-q",
            .description = "Silence engine console logs during worker execution",
            .action      = [&](std::string_view) -> std::expected<void, ZHLN::CommandLineError> {
                ZHLN::SetLogLevel(ZHLN::LogLevel::Quiet);
                return {};
            },
        },
        ZHLN::CliFlag {
            .key         = "--strict-quarantine",
            .description = "Treat quarantined failures as hard failures",
            .action      = [&](std::string_view) -> std::expected<void, ZHLN::CommandLineError> {
                opts.strictQuarantine = true;
                return {};
            },
        },
        ZHLN::CliFlag {
            .key         = "--exitfirst",
            .shortKey    = "-x",
            .description = "Stop immediately after the first failure",
            .action      = [&](std::string_view) -> std::expected<void, ZHLN::CommandLineError> {
                opts.maxFail = 1;
                return {};
            },
        },
        ZHLN::CliFlag {
            .key         = "--filter",
            .shortKey    = "-k",
            .placeholder = "<expr>",
            .description = "Boolean substring filter expression (and / or / not)",
            .action      = [&](std::string_view v) -> std::expected<void, ZHLN::CommandLineError> {
                opts.keywordExpr = std::string {v};
                return {};
            },
        },
        ZHLN::CliFlag {
            .key         = "--mark",
            .shortKey    = "-m",
            .placeholder = "<expr>",
            .description = "Boolean mark/tag filter expression (and / or / not)",
            .action      = [&](std::string_view v) -> std::expected<void, ZHLN::CommandLineError> {
                opts.markExpr = std::string {v};
                return {};
            },
        },
        ZHLN::CliFlag {
            .key         = "--target-name",
            .placeholder = "<name>",
            .description = "Logical target name passed by ztest",
            .action      = [&](std::string_view v) -> std::expected<void, ZHLN::CommandLineError> {
                opts.targetName = std::string {v};
                return {};
            },
        },
        ZHLN::CliFlag {
            .key         = "--target-tags",
            .placeholder = "<csv>",
            .description = "Comma-separated target tags passed by ztest",
            .action      = [&](std::string_view v) -> std::expected<void, ZHLN::CommandLineError> {
                SplitCsvTags(v, opts.targetTags);
                return {};
            },
        },
        ZHLN::CliFlag {
            .key         = "--file",
            .placeholder = "<relFile>",
            .description = "Restrict execution/collection to source file(s)",
            .action      = [&](std::string_view v) -> std::expected<void, ZHLN::CommandLineError> {
                if (!v.empty()) {
                    opts.sourceFiles.emplace_back(v);
                }
                return {};
            },
        },
        ZHLN::CliFlag {
            .key         = "--exact",
            .placeholder = "<nodeId>",
            .description = "Run exact test case node ID(s) (repeatable)",
            .action      = [&](std::string_view v) -> std::expected<void, ZHLN::CommandLineError> {
                if (!v.empty()) {
                    opts.exactCases.emplace_back(v);
                }
                return {};
            },
        },
        ZHLN::CliFlag {
            .key         = "--skip-until",
            .placeholder = "<nodeId>",
            .description = "Skip cases through <nodeId> and resume after",
            .action      = [&](std::string_view v) -> std::expected<void, ZHLN::CommandLineError> {
                opts.skipUntilCase = std::string {v};
                return {};
            },
        },
        ZHLN::CliFlag {
            .key         = "--maxfail",
            .placeholder = "<N>",
            .description = "Stop after N hard failures",
            .action      = [&](std::string_view v) -> std::expected<void, ZHLN::CommandLineError> {
                uint32_t             parsed = 0;
                const auto [ptr, ec]        = std::from_chars(v.data(), v.data() + v.size(), parsed);
                if (ec != std::errc {} || ptr != v.data() + v.size() || parsed == 0) {
                    return std::unexpected(ZHLN::CommandLineError::InvalidValue);
                }
                opts.maxFail = parsed;
                return {};
            },
        },
    };

    const auto parsed = ZHLN::ParseCli(
        std::span<char* const>(normalizedPtrs.data(), normalizedPtrs.size()),
        workerFlags,
        ZHLN::CliParseConfig {
            .programTitle   = "Zahlen Test Worker",
            .usageSuffix    = "[options] [source_file ...]",
            .primaryHeading = "Test Worker Options",
        }
    );
    if (!parsed) {
        opts.parseFailed = true;
        return opts;
    }
    const ZHLN::CliParseResult& cmdRes = *parsed;
    if (cmdRes.helpRequested || cmdRes.versionRequested) {
        opts.showHelp = true;
    }
    return opts;
}

inline auto NormalizePathSlashes(std::string_view raw) -> std::string {
    while (raw.starts_with("./") || raw.starts_with(".\\")) {
        raw.remove_prefix(2);
    }
    std::string out;
    out.reserve(raw.size());
    for (char c: raw) {
        out.push_back(c == '\\' ? '/' : c);
    }
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

inline auto SourceFileMatches(std::string_view relFile, std::string_view wanted) -> bool {
    const std::string normFile   = NormalizePathSlashes(relFile);
    const std::string normWanted = NormalizePathSlashes(wanted);
    if (normFile.empty() || normWanted.empty()) {
        return false;
    }
    if (EqualsIgnoreCase(normFile, normWanted)) {
        return true;
    }
    // Directory prefix match (e.g. "tests/assets" matches "tests/assets/TestCooker.cpp")
    if (normFile.size() > normWanted.size() && normFile[normWanted.size()] == '/'
        && EqualsIgnoreCase(std::string_view {normFile}.substr(0, normWanted.size()), normWanted)) {
        return true;
    }
    // Filename/tail match (e.g. "TestCooker.cpp" or "assets/TestCooker.cpp")
    if (normFile.size() > normWanted.size()) {
        const std::size_t off = normFile.size() - normWanted.size();
        if (normFile[off - 1] == '/' && EqualsIgnoreCase(std::string_view {normFile}.substr(off), normWanted)) {
            return true;
        }
    }
    return false;
}

inline auto MakeNodeId(std::string_view relFile, std::string_view suiteName, std::string_view testName) -> std::string {
    if (!relFile.empty() && !suiteName.empty() && suiteName != relFile) {
        return std::format("{}::{}::{}", relFile, suiteName, testName);
    }
    if (!relFile.empty()) {
        return std::format("{}::{}", relFile, testName);
    }
    if (!suiteName.empty()) {
        return std::format("{}::{}", suiteName, testName);
    }
    return std::string {testName};
}

inline auto SuiteDisplayLabel(std::string_view relFile, std::string_view suiteName) -> std::string {
    if (!relFile.empty() && !suiteName.empty() && suiteName != relFile) {
        return std::format("{} ({})", relFile, suiteName);
    }
    if (!relFile.empty()) {
        return std::string {relFile};
    }
    return std::string {suiteName};
}

inline auto CaseMatchesOptions(
    std::string_view     relFile,
    std::string_view     suiteName,
    std::string_view     testName,
    std::string_view     baseMethodName,
    const CaseMarks&     marks,
    const RunnerOptions& opts
) -> bool {
    if (!opts.sourceFiles.empty()) {
        bool fileMatched = false;
        for (const auto& f: opts.sourceFiles) {
            if (SourceFileMatches(relFile, f)) {
                fileMatched = true;
                break;
            }
        }
        if (!fileMatched) {
            return false;
        }
    }

    const std::string nodeId         = MakeNodeId(relFile, suiteName, testName);
    const std::string baseNodeId     = baseMethodName == testName ? nodeId : MakeNodeId(relFile, suiteName, baseMethodName);
    const std::string legacyId       = suiteName.empty() ? std::string {testName} : std::format("{}::{}", suiteName, testName);
    const std::string baseLegacyId   = suiteName.empty() ? std::string {baseMethodName} : std::format("{}::{}", suiteName, baseMethodName);
    const std::string fileCaseId     = relFile.empty() ? std::string {} : std::format("{}::{}", relFile, testName);
    const std::string baseFileCaseId = relFile.empty() ? std::string {} : std::format("{}::{}", relFile, baseMethodName);

    if (!opts.legacyFilter.empty()) {
        const std::string_view f            = opts.legacyFilter;
        const std::string      wildcard     = std::format("{}::*", suiteName);
        const std::string      fileWildcard = relFile.empty() ? std::string {} : std::format("{}::*", relFile);
        if (f != suiteName && f != relFile && f != testName && f != baseMethodName && f != wildcard && f != fileWildcard
            && f != legacyId && f != baseLegacyId && f != nodeId && f != baseNodeId && f != fileCaseId && f != baseFileCaseId) {
            return false;
        }
    }

    if (!opts.exactCases.empty()) {
        bool found = false;
        for (const auto& exact: opts.exactCases) {
            if (exact == nodeId || exact == baseNodeId || exact == legacyId || exact == baseLegacyId
                || (!fileCaseId.empty() && (exact == fileCaseId || exact == baseFileCaseId))
                || (!suiteName.empty() && exact == suiteName) || (!relFile.empty() && SourceFileMatches(relFile, exact))
                || exact == testName || exact == baseMethodName) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }

    if (!opts.keywordExpr.empty()) {
        const std::string qualified = opts.targetName.empty() ? nodeId : std::format("{}::{}", opts.targetName, nodeId);
        BooleanExprEvaluator eval {opts.keywordExpr, [&](std::string_view needle) { return ContainsIgnoreCase(qualified, needle); }};
        if (!eval.Evaluate()) {
            return false;
        }
    }

    if (!opts.markExpr.empty()) {
        BooleanExprEvaluator eval {opts.markExpr, [&](std::string_view wanted) {
            if ((EqualsIgnoreCase(wanted, "quarantine") || EqualsIgnoreCase(wanted, "quarantined")) && marks.isQuarantined) {
                return true;
            }
            if ((EqualsIgnoreCase(wanted, "skip") || EqualsIgnoreCase(wanted, "skipped")) && marks.isSkipped) {
                return true;
            }
            for (std::size_t i = 0; i < marks.tagCount; ++i) {
                if (EqualsIgnoreCase(marks.tags[i], wanted)) {
                    return true;
                }
            }
            for (const auto& targetTag: opts.targetTags) {
                if (EqualsIgnoreCase(targetTag, wanted)) {
                    return true;
                }
            }
            return false;
        }};
        if (!eval.Evaluate()) {
            return false;
        }
    }

    return true;
}

template <typename Suite, typename Target, typename F>
constexpr void ForEachSuiteTestCase(F&& f) {
    ZHLN::Reflect::ForEachReflectedMethod<Target>([&]<typename Method>() {
        if constexpr (IsTestCaseMethod<Suite, Method>()) {
            f.template operator()<Method>();
        }
    });
}

template <typename Suite>
struct SuiteMethodTargetImpl {
    using Type = Suite;
};

template <HasNestedTests Suite>
struct SuiteMethodTargetImpl<Suite> {
    using Type = typename Suite::Tests;
};

template <typename Suite>
using SuiteMethodTarget = typename SuiteMethodTargetImpl<Suite>::Type;

struct CaseInvokeOutcome {
    bool        ok = false;
    std::string fatalBanner {};
    std::string fatalSummary {};
};

struct CaseDescriptor {
    std::string      name {};
    std::string_view methodName {};
    CaseMarks        marks {};
    uint32_t         paramIndex      = 0;
    bool             isParameterized = false;
    auto (*invoke)(void* receiver, FixtureRuntimeContext& fixtures) -> CaseInvokeOutcome = nullptr;
};

template <typename ReturnType>
auto ConvertTestReturn(ReturnType&& res) -> CaseInvokeOutcome {
    if (res.has_value()) {
        return CaseInvokeOutcome {.ok = true};
    }

    CaseInvokeOutcome out {.ok = false};
    if constexpr (ErrorCodeTestResult<ReturnType>) {
        if (res.error() != TestFrameworkError::AssertionFailed) {
            const ZHLN::Error err {res.error()};
            out.fatalBanner  = std::format("{}::{}: {}", err.Category(), err.Name(), err.Message());
            out.fatalSummary = std::format("{}::{}", err.Category(), err.Name());
        }
    } else if constexpr (StringTestResult<ReturnType>) {
        out.fatalBanner  = std::string {std::string_view {res.error()}};
        out.fatalSummary = out.fatalBanner;
    }
    return out;
}

inline auto SanitizeBracketToken(std::string_view raw) -> std::string {
    std::string out;
    out.reserve(raw.size());
    for (char c: raw) {
        if (c == '\t' || c == '\n' || c == '\r' || c == ' ') {
            continue;
        }
        if (c == '[') {
            out.push_back('(');
        } else if (c == ']') {
            out.push_back(')');
        } else {
            out.push_back(c);
        }
        if (out.size() >= 48) {
            break;
        }
    }
    return out.empty() ? std::string {"case"} : out;
}

template <typename T>
auto FormatParamElementForCaseId(const T& elem) -> std::string {
    using CleanT = std::remove_cvref_t<T>;
    if constexpr (requires(const CleanT& lit) { lit.View(); }) {
        return SanitizeBracketToken(elem.View());
    } else if constexpr (std::same_as<CleanT, std::string_view> || std::same_as<CleanT, std::string>) {
        return SanitizeBracketToken(elem);
    } else if constexpr (NarrowCStringLike<CleanT>) {
        return elem == nullptr ? std::string {"null"} : SanitizeBracketToken(elem);
    } else if constexpr (std::same_as<CleanT, bool>) {
        return elem ? "true" : "false";
    } else {
        return SanitizeBracketToken(FormatValue(elem));
    }
}

template <typename... Ts>
auto FormatStructuralTupleForCaseId(const StructuralTupleImpl<std::index_sequence_for<Ts...>, Ts...>& tup) -> std::string {
    if constexpr (sizeof...(Ts) == 0) {
        return "0";
    } else {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            std::string joined;
            ((joined.empty()
                  ? joined.append(FormatParamElementForCaseId(GetStructuralElement<Is>(tup)))
                  : (joined.push_back(','), joined.append(FormatParamElementForCaseId(GetStructuralElement<Is>(tup))))),
             ...);
            return joined;
        }(std::make_index_sequence<sizeof...(Ts)> {});
    }
}

template <typename ParamType>
decltype(auto) ResolveFixtureParameter(FixtureRuntimeContext& fixtures) {
    using CleanT = std::remove_cvref_t<ParamType>;
    if constexpr (!std::is_reference_v<ParamType>) {
        static_assert(
            std::is_copy_constructible_v<CleanT>,
            "Non-copyable test fixtures must be accepted by reference (e.g. TempDir&)."
        );
    }
    return ResolveFixture<CleanT>(fixtures);
}

template <typename Entity>
void ActivateTypeUseFixtures(FixtureRuntimeContext& fixtures) {
    ZHLN::Reflect::ForEachTypeAnnotationConstant<Entity>([&]<auto AnnotationVal>() {
        using A = std::remove_cvref_t<decltype(AnnotationVal)>;
        if constexpr (requires { A::isUseFixturesMark; }) {
            A::Activate([&]<typename F>() {
                static_assert(IsTestFixture<F>, "Types listed in UseFixtures<...> must be test fixtures.");
                ResolveFixture<F>(fixtures);
            });
        }
    });
}

template <typename Suite, typename Method>
void ActivateAllUseFixtures(FixtureRuntimeContext& fixtures) {
    ActivateTypeUseFixtures<Suite>(fixtures);
    if constexpr (HasNestedTests<Suite>) {
        ActivateTypeUseFixtures<typename Suite::Tests>(fixtures);
    }
    Method::ForEachAnnotationConstant([&]<auto AnnotationVal>() {
        using A = std::remove_cvref_t<decltype(AnnotationVal)>;
        if constexpr (requires { A::isUseFixturesMark; }) {
            A::Activate([&]<typename F>() {
                static_assert(IsTestFixture<F>, "Types listed in UseFixtures<...> must be test fixtures.");
                ResolveFixture<F>(fixtures);
            });
        }
    });
}

template <typename Suite, typename Target, typename Method>
auto InvokeDirectCaseTrampoline(void* rawReceiver, [[maybe_unused]] FixtureRuntimeContext& fixtures) -> CaseInvokeOutcome {
    ActivateAllUseFixtures<Suite, Method>(fixtures);
    auto& receiver = *static_cast<Target*>(rawReceiver);
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> CaseInvokeOutcome {
        if constexpr (std::is_void_v<typename Method::ReturnType>) {
            Method::Invoke(receiver, ResolveFixtureParameter<typename Method::template ParameterType<Is>>(fixtures)...);
            return CaseInvokeOutcome {.ok = true};
        } else {
            return ConvertTestReturn(
                Method::Invoke(receiver, ResolveFixtureParameter<typename Method::template ParameterType<Is>>(fixtures)...)
            );
        }
    }(std::make_index_sequence<Method::arity> {});
}

template <typename FreeFn>
void ActivateFreeFunctionUseFixtures(FixtureRuntimeContext& fixtures) {
    FreeFn::template ForEachEnclosingNamespaceAnnotationConstant<Scope>([&]<auto AnnotationVal>() {
        using A = std::remove_cvref_t<decltype(AnnotationVal)>;
        if constexpr (requires { A::isUseFixturesMark; }) {
            A::Activate([&]<typename F>() {
                static_assert(IsTestFixture<F>, "Types listed in UseFixtures<...> must be test fixtures.");
                ResolveFixture<F>(fixtures);
            });
        }
    });
    FreeFn::ForEachAnnotationConstant([&]<auto AnnotationVal>() {
        using A = std::remove_cvref_t<decltype(AnnotationVal)>;
        if constexpr (requires { A::isUseFixturesMark; }) {
            A::Activate([&]<typename F>() {
                static_assert(IsTestFixture<F>, "Types listed in UseFixtures<...> must be test fixtures.");
                ResolveFixture<F>(fixtures);
            });
        }
    });
}

template <typename FreeFn>
auto InvokeFreeDirectCaseTrampoline(void*, [[maybe_unused]] FixtureRuntimeContext& fixtures) -> CaseInvokeOutcome {
    ActivateFreeFunctionUseFixtures<FreeFn>(fixtures);
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> CaseInvokeOutcome {
        if constexpr (std::is_void_v<typename FreeFn::ReturnType>) {
            FreeFn::Invoke(ResolveFixtureParameter<typename FreeFn::template ParameterType<Is>>(fixtures)...);
            return CaseInvokeOutcome {.ok = true};
        } else {
            return ConvertTestReturn(
                FreeFn::Invoke(ResolveFixtureParameter<typename FreeFn::template ParameterType<Is>>(fixtures)...)
            );
        }
    }(std::make_index_sequence<FreeFn::arity> {});
}

template <typename ParamType, typename StoredElem>
constexpr decltype(auto) ConvertCaseArgument(const StoredElem& elem) {
    using CleanParam = std::remove_cvref_t<ParamType>;
    if constexpr (requires { elem.View(); } && std::constructible_from<CleanParam, std::string_view>) {
        return CleanParam(elem.View());
    } else if constexpr (requires { elem.CStr(); } && std::constructible_from<CleanParam, const char*>) {
        return CleanParam(elem.CStr());
    } else {
        return static_cast<CleanParam>(elem);
    }
}

template <typename Method, std::size_t ParamIdx, auto RowConstant>
decltype(auto) ResolveMixedParameter([[maybe_unused]] FixtureRuntimeContext& fixtures) {
    using ParamType  = typename Method::template ParameterType<ParamIdx>;
    using CleanParam = std::remove_cvref_t<ParamType>;
    if constexpr (IsTestFixture<CleanParam>) {
        return ResolveFixtureParameter<ParamType>(fixtures);
    } else {
        constexpr std::size_t ord  = NonFixtureOrdinal<Method, ParamIdx>();
        static constexpr auto elem = GetStructuralElement<ord>(RowConstant.values);
        return ConvertCaseArgument<ParamType>(elem);
    }
}

template <typename Suite, typename Target, typename Method, auto RowConstant>
auto InvokeParameterizedCaseTrampoline(void* rawReceiver, FixtureRuntimeContext& fixtures) -> CaseInvokeOutcome {
    ActivateAllUseFixtures<Suite, Method>(fixtures);
    auto& receiver = *static_cast<Target*>(rawReceiver);
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> CaseInvokeOutcome {
        if constexpr (std::is_void_v<typename Method::ReturnType>) {
            Method::Invoke(receiver, ResolveMixedParameter<Method, Is, RowConstant>(fixtures)...);
            return CaseInvokeOutcome {.ok = true};
        } else {
            return ConvertTestReturn(Method::Invoke(receiver, ResolveMixedParameter<Method, Is, RowConstant>(fixtures)...));
        }
    }(std::make_index_sequence<Method::arity> {});
}

template <typename FreeFn, auto RowConstant>
auto InvokeFreeParameterizedCaseTrampoline(void*, FixtureRuntimeContext& fixtures) -> CaseInvokeOutcome {
    ActivateFreeFunctionUseFixtures<FreeFn>(fixtures);
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> CaseInvokeOutcome {
        if constexpr (std::is_void_v<typename FreeFn::ReturnType>) {
            FreeFn::Invoke(ResolveMixedParameter<FreeFn, Is, RowConstant>(fixtures)...);
            return CaseInvokeOutcome {.ok = true};
        } else {
            return ConvertTestReturn(FreeFn::Invoke(ResolveMixedParameter<FreeFn, Is, RowConstant>(fixtures)...));
        }
    }(std::make_index_sequence<FreeFn::arity> {});
}

template <auto Ann>
consteval auto NormalizeCaseRowAnnotation() {
    using AnnT = std::remove_cvref_t<decltype(Ann)>;
    if constexpr (requires { AnnT::isTestCaseValues; }) {
        return AnnT::AsCase();
    } else {
        return Ann;
    }
}

template <typename Method, std::size_t AxisCount>
consteval auto ExtractParamAxisExtents() -> std::array<std::size_t, AxisCount> {
    std::array<std::size_t, AxisCount> extents {};
    std::size_t                        idx = 0;
    Method::ForEachAnnotationType([&]<typename A>() {
        if constexpr (ParamAxisAnnotation<A>) {
            extents[idx++] = std::remove_cvref_t<A>::size;
        }
    });
    return extents;
}

template <std::size_t AxisCount>
consteval auto TotalCartesianCombinations(const std::array<std::size_t, AxisCount>& extents) -> std::size_t {
    std::size_t prod = 1;
    for (std::size_t e: extents) {
        prod *= e;
    }
    return prod;
}

template <std::size_t AxisCount>
consteval auto DecodeCartesianCoordinates(
    std::size_t                              comboIndex,
    const std::array<std::size_t, AxisCount>& extents
) -> std::array<std::size_t, AxisCount> {
    std::array<std::size_t, AxisCount> coords {};
    std::size_t                        rem = comboIndex;
    for (std::size_t i = AxisCount; i > 0; --i) {
        const std::size_t axis = i - 1;
        coords[axis]           = rem % extents[axis];
        rem /= extents[axis];
    }
    return coords;
}

template <typename Method, std::size_t TargetAxis>
consteval auto FindParamAxisAnnotationSlot() -> std::size_t {
    std::size_t axisIdx = 0;
    std::size_t annSlot = 0;
    std::size_t result  = 0;
    Method::ForEachAnnotationType([&]<typename A>() {
        if constexpr (ParamAxisAnnotation<A>) {
            if (axisIdx == TargetAxis) {
                result = annSlot;
            }
            ++axisIdx;
        }
        ++annSlot;
    });
    return result;
}

template <typename Method, std::size_t AxisCount, std::size_t ComboIndex>
consteval auto BuildCartesianRowConstant() {
    constexpr auto extents = ExtractParamAxisExtents<Method, AxisCount>();
    constexpr auto coords  = DecodeCartesianCoordinates<AxisCount>(ComboIndex, extents);
    return [&]<std::size_t... AxisIs>(std::index_sequence<AxisIs...>) {
        return Case {
            GetStructuralElement<coords[AxisIs]>(
                Method::template AnnotationConstantAt<FindParamAxisAnnotationSlot<Method, AxisIs>()>().values
            )...
        };
    }(std::make_index_sequence<AxisCount> {});
}

template <typename Suite, typename Target, typename Method, std::size_t AxisCount, std::size_t ComboIndex>
void AppendCartesianCaseDescriptor(std::vector<CaseDescriptor>& out, const CaseMarks& marks) {
    constexpr auto row       = BuildCartesianRowConstant<Method, AxisCount, ComboIndex>();
    std::string    bracketId = FormatStructuralTupleForCaseId(row.values);
    out.push_back(CaseDescriptor {
        .name            = std::format("{}[{}]", Method::name, bracketId),
        .methodName      = Method::name,
        .marks           = marks,
        .paramIndex      = static_cast<uint32_t>(ComboIndex),
        .isParameterized = true,
        .invoke          = &InvokeParameterizedCaseTrampoline<Suite, Target, Method, row>,
    });
}

template <typename Suite, typename Target, typename Method>
void AppendMethodCaseDescriptors(std::vector<CaseDescriptor>& out) {
    constexpr CaseMarks   marks            = ExtractCaseMarks<Suite, Method>();
    constexpr std::size_t nonFixtureCount  = NonFixtureParamCount<Method>();
    constexpr std::size_t explicitRowCount = ExplicitCaseAnnotationCount<Method>();
    constexpr std::size_t axisCount        = ParamAxisAnnotationCount<Method>();

    static_assert(
        !(explicitRowCount > 0 && axisCount > 0),
        "Do not mix Case/NamedCase/CaseValues and Param annotations on the same test method."
    );

    if constexpr (explicitRowCount == 0 && axisCount == 0) {
        out.push_back(CaseDescriptor {
            .name            = std::string {Method::name},
            .methodName      = Method::name,
            .marks           = marks,
            .paramIndex      = 0,
            .isParameterized = false,
            .invoke          = &InvokeDirectCaseTrampoline<Suite, Target, Method>,
        });
    } else if constexpr (explicitRowCount > 0) {
        uint32_t rowIdx     = 0;
        auto     appendRow  = [&]<auto RawRow>() {
            constexpr auto row = NormalizeCaseRowAnnotation<RawRow>();
            static_assert(
                decltype(row)::arity == nonFixtureCount,
                "Case/NamedCase argument count must match the number of non-fixture parameters on this test method."
            );
            std::string bracketId;
            if constexpr (decltype(row)::hasCustomName) {
                bracketId = SanitizeBracketToken(row.caseId.View());
            } else {
                bracketId = FormatStructuralTupleForCaseId(row.values);
            }
            out.push_back(CaseDescriptor {
                .name            = std::format("{}[{}]", Method::name, bracketId),
                .methodName      = Method::name,
                .marks           = marks,
                .paramIndex      = rowIdx++,
                .isParameterized = true,
                .invoke          = &InvokeParameterizedCaseTrampoline<Suite, Target, Method, row>,
            });
        };

        Method::ForEachAnnotationConstant([&]<auto Ann>() {
            using AnnT = std::remove_cvref_t<decltype(Ann)>;
            if constexpr (CaseRowAnnotation<AnnT>) {
                appendRow.template operator()<Ann>();
            } else if constexpr (CaseTableAnnotation<AnnT>) {
                [&]<std::size_t... RowIs>(std::index_sequence<RowIs...>) {
                    (appendRow.template operator()<GetStructuralElement<RowIs>(Ann.rows)>(), ...);
                }(std::make_index_sequence<AnnT::rowCount> {});
            }
        });
    } else {
        static_assert(
            axisCount == nonFixtureCount,
            "Number of Param{...} annotations must equal the number of non-fixture parameters on this test method."
        );
        constexpr auto        extents     = ExtractParamAxisExtents<Method, axisCount>();
        constexpr std::size_t totalCombos = TotalCartesianCombinations<axisCount>(extents);
        [&]<std::size_t... ComboIs>(std::index_sequence<ComboIs...>) {
            (AppendCartesianCaseDescriptor<Suite, Target, Method, axisCount, ComboIs>(out, marks), ...);
        }(std::make_index_sequence<totalCombos> {});
    }
}

template <typename FreeFn, std::size_t AxisCount, std::size_t ComboIndex>
void AppendFreeCartesianCaseDescriptor(std::vector<CaseDescriptor>& out, const CaseMarks& marks) {
    constexpr auto row       = BuildCartesianRowConstant<FreeFn, AxisCount, ComboIndex>();
    std::string    bracketId = FormatStructuralTupleForCaseId(row.values);
    out.push_back(CaseDescriptor {
        .name            = std::format("{}[{}]", FreeFn::name, bracketId),
        .methodName      = FreeFn::name,
        .marks           = marks,
        .paramIndex      = static_cast<uint32_t>(ComboIndex),
        .isParameterized = true,
        .invoke          = &InvokeFreeParameterizedCaseTrampoline<FreeFn, row>,
    });
}

template <typename FreeFn>
void AppendFreeFunctionCaseDescriptors(std::vector<CaseDescriptor>& out) {
    constexpr CaseMarks   marks            = ExtractFreeFunctionCaseMarks<FreeFn>();
    constexpr std::size_t nonFixtureCount  = NonFixtureParamCount<FreeFn>();
    constexpr std::size_t explicitRowCount = ExplicitCaseAnnotationCount<FreeFn>();
    constexpr std::size_t axisCount        = ParamAxisAnnotationCount<FreeFn>();

    static_assert(
        !(explicitRowCount > 0 && axisCount > 0),
        "Do not mix Case/NamedCase/CaseValues and Param annotations on the same test function."
    );

    if constexpr (explicitRowCount == 0 && axisCount == 0) {
        out.push_back(CaseDescriptor {
            .name            = std::string {FreeFn::name},
            .methodName      = FreeFn::name,
            .marks           = marks,
            .paramIndex      = 0,
            .isParameterized = false,
            .invoke          = &InvokeFreeDirectCaseTrampoline<FreeFn>,
        });
    } else if constexpr (explicitRowCount > 0) {
        uint32_t rowIdx    = 0;
        auto     appendRow = [&]<auto RawRow>() {
            constexpr auto row = NormalizeCaseRowAnnotation<RawRow>();
            static_assert(
                decltype(row)::arity == nonFixtureCount,
                "Case/NamedCase argument count must match the number of non-fixture parameters on this test function."
            );
            std::string bracketId;
            if constexpr (decltype(row)::hasCustomName) {
                bracketId = SanitizeBracketToken(row.caseId.View());
            } else {
                bracketId = FormatStructuralTupleForCaseId(row.values);
            }
            out.push_back(CaseDescriptor {
                .name            = std::format("{}[{}]", FreeFn::name, bracketId),
                .methodName      = FreeFn::name,
                .marks           = marks,
                .paramIndex      = rowIdx++,
                .isParameterized = true,
                .invoke          = &InvokeFreeParameterizedCaseTrampoline<FreeFn, row>,
            });
        };

        FreeFn::ForEachAnnotationConstant([&]<auto Ann>() {
            using AnnT = std::remove_cvref_t<decltype(Ann)>;
            if constexpr (CaseRowAnnotation<AnnT>) {
                appendRow.template operator()<Ann>();
            } else if constexpr (CaseTableAnnotation<AnnT>) {
                [&]<std::size_t... RowIs>(std::index_sequence<RowIs...>) {
                    (appendRow.template operator()<GetStructuralElement<RowIs>(Ann.rows)>(), ...);
                }(std::make_index_sequence<AnnT::rowCount> {});
            }
        });
    } else {
        static_assert(
            axisCount == nonFixtureCount,
            "Number of Param{...} annotations must equal the number of non-fixture parameters on this test function."
        );
        constexpr auto        extents     = ExtractParamAxisExtents<FreeFn, axisCount>();
        constexpr std::size_t totalCombos = TotalCartesianCombinations<axisCount>(extents);
        [&]<std::size_t... ComboIs>(std::index_sequence<ComboIs...>) {
            (AppendFreeCartesianCaseDescriptor<FreeFn, axisCount, ComboIs>(out, marks), ...);
        }(std::make_index_sequence<totalCombos> {});
    }
}

inline void DeduplicateCaseDescriptorNames(std::vector<CaseDescriptor>& list) {
    for (std::size_t i = 0; i < list.size(); ++i) {
        uint32_t collisionOrd = 1;
        for (std::size_t j = i + 1; j < list.size(); ++j) {
            if (list[j].name == list[i].name) {
                if (collisionOrd == 1) {
                    list[i].name += "#0";
                }
                list[j].name += std::format("#{}", collisionOrd++);
            }
        }
    }
}

template <typename Suite>
auto GetSuiteCaseDescriptors() -> std::span<const CaseDescriptor> {
    using Target = SuiteMethodTarget<Suite>;
    static const std::vector<CaseDescriptor> cases = [] {
        std::vector<CaseDescriptor> list;
        ForEachSuiteTestCase<Suite, Target>([&]<typename Method>() {
            AppendMethodCaseDescriptors<Suite, Target, Method>(list);
        });
        DeduplicateCaseDescriptorNames(list);
        return list;
    }();
    return cases;
}

inline void EmitSuiteCasesRuntime(
    std::string_view                relFile,
    std::string_view                suiteName,
    std::span<const CaseDescriptor> cases,
    const RunnerOptions&            opts,
    uint32_t&                       collectedCount
) {
    for (const auto& desc: cases) {
        if (!CaseMatchesOptions(relFile, suiteName, desc.name, desc.methodName, desc.marks, opts)) {
            continue;
        }
        ++collectedCount;

        std::string tagsCsv;
        for (std::size_t i = 0; i < desc.marks.tagCount; ++i) {
            if (!tagsCsv.empty()) {
                tagsCsv.push_back(',');
            }
            tagsCsv.append(desc.marks.tags[i]);
        }

        const std::string  nodeId     = MakeNodeId(relFile, suiteName, desc.name);
        const unsigned int timeoutSec = desc.marks.hasTimeout ? desc.marks.timeoutSeconds : GetDefaultTimeoutSeconds();
        if (opts.ztestList) {
            std::string_view state  = "normal";
            std::string      reason = {};
            if (desc.marks.isSkipped) {
                state  = "skip";
                reason = SanitizeProtocolField(desc.marks.skipReason);
            } else if (desc.marks.isQuarantined) {
                state  = "quarantine";
                reason = SanitizeProtocolField(desc.marks.quarantineReason);
            }
            ZHLN::Println("@@ZTEST CASE\t{}\t{}\t{}\t{}\t{}\t{}", relFile, nodeId, tagsCsv, timeoutSec, state, reason);
        } else {
            std::string suffix;
            if (!tagsCsv.empty()) {
                suffix += std::format(" [tags: {}]", tagsCsv);
            }
            if (desc.marks.isSkipped) {
                suffix += desc.marks.skipReason.empty() ? " [skip]" : std::format(" [skip: {}]", desc.marks.skipReason);
            } else if (desc.marks.isQuarantined) {
                suffix += " [quarantine]";
            }
            ZHLN::Println("  {}{}", nodeId, suffix);
        }
    }
}

inline auto SuiteHasRunnableMatchRuntime(
    std::string_view                relFile,
    std::string_view                suiteName,
    std::span<const CaseDescriptor> cases,
    const RunnerOptions&            opts,
    SkipUntilState&                 skipState
) -> bool {
    bool anyRunnable = false;
    for (const auto& desc: cases) {
        if (!CaseMatchesOptions(relFile, suiteName, desc.name, desc.methodName, desc.marks, opts)) {
            continue;
        }
        const std::string nodeId   = MakeNodeId(relFile, suiteName, desc.name);
        const std::string legacyId = suiteName.empty() ? std::string {desc.name} : std::format("{}::{}", suiteName, desc.name);
        if (skipState.active) {
            if (nodeId == opts.skipUntilCase || legacyId == opts.skipUntilCase) {
                skipState.active = false;
            }
            continue;
        }
        if (!desc.marks.isSkipped) {
            anyRunnable = true;
        }
    }
    return anyRunnable;
}

struct SuiteLifecycleOps {
    auto (*createSuite)() -> void*     = nullptr;
    void (*destroySuite)(void*)        = nullptr;
    auto (*createReceiver)(void*) -> void* = nullptr;
    void (*destroyReceiver)(void*)     = nullptr;
};

inline auto MakeNoopSuiteLifecycleOps() -> SuiteLifecycleOps {
    return SuiteLifecycleOps {
        .createSuite     = []() -> void* { return nullptr; },
        .destroySuite    = [](void*) {},
        .createReceiver  = [](void*) -> void* { return nullptr; },
        .destroyReceiver = [](void*) {},
    };
}

template <typename Suite>
auto MakeSuiteLifecycleOps() -> SuiteLifecycleOps {
    if constexpr (HasNestedTests<Suite>) {
        using Target = typename Suite::Tests;
        return SuiteLifecycleOps {
            .createSuite     = []() -> void* { return new Suite {}; },
            .destroySuite    = [](void* p) { delete static_cast<Suite*>(p); },
            .createReceiver  = [](void*) -> void* { return new Target {}; },
            .destroyReceiver = [](void* p) { delete static_cast<Target*>(p); },
        };
    } else {
        return SuiteLifecycleOps {
            .createSuite     = []() -> void* { return new Suite {}; },
            .destroySuite    = [](void* p) { delete static_cast<Suite*>(p); },
            .createReceiver  = [](void* suitePtr) -> void* { return suitePtr; },
            .destroyReceiver = [](void*) {},
        };
    }
}

inline auto ExecuteSuiteRuntime(
    std::string_view                relFile,
    std::string_view                suiteName,
    std::span<const CaseDescriptor> cases,
    const SuiteLifecycleOps&        lifecycle,
    const RunnerOptions&            opts,
    SkipUntilState&                 skipState,
    uint32_t&                       cumulativeFailed,
    FixtureScopeStore&              sessionFixtures
) -> TestStats {
    bool           hasAnyMatch      = false;
    bool           needsSuiteObject = false;
    SkipUntilState probeSkip        = skipState;

    if (ZHLN::GetLogObserver() == nullptr) {
        ZHLN::SetLogObserver(&OnTestLog);
    }

    for (const auto& desc: cases) {
        if (opts.maxFail > 0 && cumulativeFailed >= opts.maxFail) {
            break;
        }
        if (!CaseMatchesOptions(relFile, suiteName, desc.name, desc.methodName, desc.marks, opts)) {
            continue;
        }
        const std::string nodeId   = MakeNodeId(relFile, suiteName, desc.name);
        const std::string legacyId = suiteName.empty() ? std::string {desc.name} : std::format("{}::{}", suiteName, desc.name);
        if (probeSkip.active) {
            if (nodeId == opts.skipUntilCase || legacyId == opts.skipUntilCase) {
                probeSkip.active = false;
            }
            continue;
        }
        hasAnyMatch = true;
        if (!desc.marks.isSkipped) {
            needsSuiteObject = true;
        }
    }

    if (!hasAnyMatch) {
        skipState = probeSkip;
        return {};
    }

    const std::string displaySuite  = SuiteDisplayLabel(relFile, suiteName);
    const std::string infoSuiteName = !suiteName.empty() ? std::string {suiteName} : std::string {relFile};

    TestStats stats;
    ZHLN::Println("{}=================================================={}", Color::Cyan, Color::Reset);
    ZHLN::Println("{}Running Test Suite: {}{}", Color::Cyan, displaySuite, Color::Reset);
    ZHLN::Println("{}=================================================={}", Color::Cyan, Color::Reset);

    void*             suiteInstance    = nullptr;
    void*             receiverInstance = nullptr;
    FixtureScopeStore suiteFixtures;
    if (needsSuiteObject) {
        suiteInstance    = lifecycle.createSuite();
        receiverInstance = lifecycle.createReceiver(suiteInstance);
    }

    for (const auto& desc: cases) {
        if (opts.maxFail > 0 && cumulativeFailed >= opts.maxFail) {
            break;
        }
        const std::string_view name  = desc.name;
        const CaseMarks&       marks = desc.marks;
        if (!CaseMatchesOptions(relFile, suiteName, name, desc.methodName, marks, opts)) {
            continue;
        }

        const std::string nodeId     = MakeNodeId(relFile, suiteName, name);
        const std::string fullCaseId = suiteName.empty() ? std::string {name} : std::format("{}::{}", suiteName, name);
        if (skipState.active) {
            if (nodeId == opts.skipUntilCase || fullCaseId == opts.skipUntilCase) {
                skipState.active = false;
            }
            continue;
        }

        if (marks.isSkipped) {
            if (opts.workerMode) {
                ZHLN::Println("@@ZTEST BEGIN\t{}\t{}", relFile, nodeId);
            }
            if (marks.skipReason.empty()) {
                ZHLN::Println("  {}[ SKIP ] {}{}", Color::Yellow, name, Color::Reset);
            } else {
                ZHLN::Println("  {}[ SKIP ] {}{} ({})", Color::Yellow, name, Color::Reset, marks.skipReason);
            }
            if (opts.workerMode) {
                ZHLN::Println("@@ZTEST END\t{}\t{}\tSKIP\t0", relFile, nodeId);
            }
            stats.skipped++;
            continue;
        }

        auto& ctx = GetThreadLocalContext();
        ctx.Reset(name);
        if (marks.hasTimeout) {
            ctx.timeoutSeconds = marks.timeoutSeconds;
        }
        if (marks.allowValidationErrors) {
            ctx.allowValidationErrors = true;
        }
        if (marks.allowDeviceLost) {
            ctx.allowDeviceLost = true;
        }

        if (opts.workerMode) {
            ZHLN::Println("@@ZTEST BEGIN\t{}\t{}", relFile, nodeId);
        }
        ZHLN::Println("  [ RUN  ] {}", name);

        const uint32_t valErrorsBefore = g_validationErrors.load(std::memory_order::relaxed);
        const uint32_t devLostBefore   = g_deviceLost.load(std::memory_order::relaxed);

        const bool           captureTestLogs = !opts.verbose;
        const ZHLN::LogLevel outerLogLevel   = ZHLN::GetLogLevel();
        if (captureTestLogs) {
            ZHLN::SetLogLevel(ZHLN::LogLevel::Verbose);
            ZHLN::BeginLogCapture(true);
        }

        const auto startSteady = std::chrono::steady_clock::now();

        CaseInvokeOutcome invokeOut {};
        {
            FixtureScopeStore     functionFixtures;
            FixtureRuntimeContext fixtureCtx {
                .session  = sessionFixtures,
                .suite    = suiteFixtures,
                .function = functionFixtures,
                .caseInfo = CaseInfo {
                    .suiteName       = infoSuiteName,
                    .caseName        = name,
                    .methodName      = desc.methodName,
                    .paramIndex      = desc.paramIndex,
                    .isParameterized = desc.isParameterized,
                    .filePath        = relFile,
                },
            };

#if defined(ZHLN_TEST_TIMEOUT_SUPPORTED)
            struct sigaction sa {};
            sa.sa_handler = TestTimeoutSignalHandler;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = 0;
            struct sigaction old_sa {};
            g_testTimeoutNameLength.store(nodeId.size(), std::memory_order_relaxed);
            g_testTimeoutName.store(nodeId.data(), std::memory_order_release);
            sigaction(SIGALRM, &sa, &old_sa);

            if (IsDebuggerAttached() || ctx.timeoutSeconds == 0) {
                alarm(0);
            } else {
                alarm(ctx.timeoutSeconds);
            }

            invokeOut = desc.invoke(receiverInstance, fixtureCtx);
            functionFixtures.Clear();
            alarm(0);
            sigaction(SIGALRM, &old_sa, nullptr);
            g_testTimeoutName.store(nullptr, std::memory_order_release);
            g_testTimeoutNameLength.store(0, std::memory_order_relaxed);
#else
            invokeOut = desc.invoke(receiverInstance, fixtureCtx);
            functionFixtures.Clear();
#endif
        }

        const auto elapsedUs =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - startSteady).count();

        std::vector<std::string> capturedTestLogs;
        if (captureTestLogs) {
            capturedTestLogs = ZHLN::EndLogCapture();
            ZHLN::SetLogLevel(outerLogLevel);
        }

        const uint32_t valErrorsAfter = g_validationErrors.load(std::memory_order::relaxed);
        if (valErrorsAfter > valErrorsBefore && !ctx.allowValidationErrors) {
            const uint32_t count = valErrorsAfter - valErrorsBefore;
            ctx.failures.push_back(
                {.file          = "Vulkan Validation Layer",
                 .line          = 0,
                 .actualValue   = std::format("{} Vulkan validation layer error(s) logged", count),
                 .expectedValue = "0 validation errors",
                 .op            = "ValidationError"}
            );
        }

        const uint32_t devLostAfter = g_deviceLost.load(std::memory_order::relaxed);
        if (devLostAfter > devLostBefore && !ctx.allowDeviceLost) {
            const uint32_t count = devLostAfter - devLostBefore;
            ctx.failures.push_back(
                {.file          = "Vulkan Device",
                 .line          = 0,
                 .actualValue   = std::format("{} GPU device lost / hang event(s) detected", count),
                 .expectedValue = "0 device lost events",
                 .op            = "DeviceLost"}
            );
        }

        const bool testPassed        = invokeOut.ok && ctx.failures.empty();
        const bool quarantineEnabled = marks.isQuarantined && !opts.strictQuarantine;

        const std::string elapsedMs = std::format("{:.2f}", static_cast<double>(elapsedUs) / 1000.0);

        if (testPassed) {
            ZHLN::Println("  {}[ PASS ] {}{} ({} ms)", Color::Green, name, Color::Reset, elapsedMs);
            if (quarantineEnabled) {
                ZHLN::Println(
                    "    {}[QUARANTINE] Passed while quarantined -- remove its Quarantine annotation:{} {}",
                    Color::Yellow, Color::Reset, marks.quarantineReason
                );
            }
            if (opts.workerMode) {
                ZHLN::Println(
                    "@@ZTEST END\t{}\t{}\t{}\t{}",
                    relFile, nodeId, quarantineEnabled ? "QUARANTINE_PASS" : "PASS", elapsedUs
                );
            }
            stats.passed++;
        } else {
            if (quarantineEnabled) {
                ZHLN::Println("  {}[ QUARANTINE ] {}{} ({} ms)", Color::Yellow, name, Color::Reset, elapsedMs);
                ZHLN::Println("    {}Reason: {}{}", Color::Yellow, marks.quarantineReason, Color::Reset);
                if (!marks.quarantineEvidence.empty()) {
                    ZHLN::Println("    {}Evidence: {}{}", Color::Yellow, marks.quarantineEvidence, Color::Reset);
                }
            } else {
                ZHLN::Println("  {}[ FAIL ] {}{} ({} ms)", Color::Red, name, Color::Reset, elapsedMs);
            }

            if (!invokeOut.fatalBanner.empty()) {
                ZHLN::Println("    {}Fatal Suite Error: {}{}", Color::Red, invokeOut.fatalBanner, Color::Reset);
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
                    } else if (f.op == "incomplete") {
                        ZHLN::Println("      Incomplete Expect(...) statement without comparison operator (value: {})", f.actualValue);
                    } else if (f.decomposed) {
                        ZHLN::Println("      Expanded:  {} {} {}", f.actualValue, f.op, f.expectedValue);
                    } else {
                        ZHLN::Println("      Comparison mismatch on  : '{}'", f.op);
                        ZHLN::Println("        Actual value          : {}", f.actualValue);
                        ZHLN::Println("        Expected value        : {}", f.expectedValue);
                    }
                }
            }
            if (!capturedTestLogs.empty()) {
                ZHLN::Println(
                    "    {}[ENGINE LOG] {} line(s) captured during this test (-v / ZHLN_TEST_LOG=verbose to stream):{}",
                    Color::Gray, capturedTestLogs.size(), Color::Reset
                );
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

            if (opts.workerMode) {
                ZHLN::Println(
                    "@@ZTEST END\t{}\t{}\t{}\t{}",
                    relFile, nodeId, quarantineEnabled ? "QUARANTINE" : "FAIL", elapsedUs
                );
            }

            if (quarantineEnabled) {
                stats.quarantined++;
            } else {
                stats.failed++;
                cumulativeFailed++;

                std::string detail    = invokeOut.fatalSummary;
                const size_t recorded = ctx.failures.size();
                if (!detail.empty() && recorded > 0) {
                    detail += " + ";
                }
                if (recorded > 0) {
                    detail += std::format("{} {}", recorded, recorded == 1 ? "recorded failure" : "recorded failures");
                }
                if (detail.empty()) {
                    detail = "failed without recorded details";
                }
                RecordFailedTestSummary(FailedTestSummary {infoSuiteName, std::string {name}, std::move(detail)});
            }
        }
    }

    suiteFixtures.Clear();
    if (needsSuiteObject) {
        lifecycle.destroyReceiver(receiverInstance);
        lifecycle.destroySuite(suiteInstance);
    }

    ZHLN::Println("--------------------------------------------------");
    if (stats.quarantined == 0 && stats.skipped == 0) {
        ZHLN::Println("Summary for {}: {} Passed, {} Failed", displaySuite, stats.passed, stats.failed);
    } else {
        ZHLN::Println(
            "Summary for {}: {} Passed, {} Failed, {} Quarantined, {} Skipped",
            displaySuite, stats.passed, stats.failed, stats.quarantined, stats.skipped
        );
    }
    ZHLN::Println("==================================================\n");

    return stats;
}

template <typename Suite>
void EmitSuiteCases(std::string_view relFile, const RunnerOptions& opts, uint32_t& collectedCount) {
    EmitSuiteCasesRuntime(relFile, ZHLN::Reflect::TypeName<Suite>(), GetSuiteCaseDescriptors<Suite>(), opts, collectedCount);
}

template <typename Suite>
auto SuiteHasRunnableMatch(std::string_view relFile, const RunnerOptions& opts, SkipUntilState& skipState) -> bool {
    return SuiteHasRunnableMatchRuntime(relFile, ZHLN::Reflect::TypeName<Suite>(), GetSuiteCaseDescriptors<Suite>(), opts, skipState);
}

template <typename Suite>
auto ExecuteSuite(
    std::string_view     relFile,
    const RunnerOptions& opts,
    SkipUntilState&      skipState,
    uint32_t&            cumulativeFailed,
    FixtureScopeStore&   sessionFixtures
) -> TestStats {
    return ExecuteSuiteRuntime(
        relFile,
        ZHLN::Reflect::TypeName<Suite>(),
        GetSuiteCaseDescriptors<Suite>(),
        MakeSuiteLifecycleOps<Suite>(),
        opts,
        skipState,
        cumulativeFailed,
        sessionFixtures
    );
}

struct RegisteredSuiteEntry {
    std::string_view name;
    std::string_view relFile {};
    int              priority = 0;
    void (*emitCases)(std::string_view, const RunnerOptions&, uint32_t&);
    auto (*hasRunnableMatch)(std::string_view, const RunnerOptions&, SkipUntilState&) -> bool;
    auto (*execute)(std::string_view, const RunnerOptions&, SkipUntilState&, uint32_t&, FixtureScopeStore&) -> TestStats;
};

template <typename Suite>
consteval auto ExtractSuitePriority() -> int {
    int prio = 0;
    ZHLN::Reflect::ForEachAnnotationType<Suite>([&]<typename A>() {
        if constexpr (requires { A::isSuitePriority; }) {
            prio = A::order;
        }
    });
    return prio;
}

inline auto RegisteredSuitesStorage() -> std::vector<RegisteredSuiteEntry>& {
    static std::vector<RegisteredSuiteEntry> suites;
    return suites;
}

template <typename Suite>
void RegisterSingleSuite(std::string_view relFile = {}) {
    auto&                  suites    = RegisteredSuitesStorage();
    const std::string_view suiteName = ZHLN::Reflect::TypeName<Suite>();
    for (auto& existing: suites) {
        if (existing.name == suiteName) {
            if (existing.relFile.empty() && !relFile.empty()) {
                existing.relFile = relFile;
            }
            return;
        }
    }
    suites.push_back(RegisteredSuiteEntry {
        .name             = suiteName,
        .relFile          = relFile,
        .priority         = ExtractSuitePriority<Suite>(),
        .emitCases        = &EmitSuiteCases<Suite>,
        .hasRunnableMatch = &SuiteHasRunnableMatch<Suite>,
        .execute          = &ExecuteSuite<Suite>,
    });
    std::stable_sort(suites.begin(), suites.end(), [](const RegisteredSuiteEntry& a, const RegisteredSuiteEntry& b) {
        return a.priority < b.priority;
    });
}

} // namespace TemplatedDetail

template <typename Suite>
TestStats RunSuite() {
    const RunnerOptions                opts = TemplatedDetail::ParseRunnerOptions(0, nullptr);
    SkipUntilState                     skipState {.active = false};
    uint32_t                           cumulativeFailed = 0;
    TemplatedDetail::FixtureScopeStore sessionFixtures;
    return TemplatedDetail::ExecuteSuite<Suite>({}, opts, skipState, cumulativeFailed, sessionFixtures);
}

// Macro-free static auto-registration for test suites private to a .cpp file:
//
//   namespace {
//   const ZHLN::Test::RegisterSuites<MeshletTestSuite> kRegisterMeshletsSuite {};
//   }
template <typename... Suites>
struct RegisterSuites {
    RegisterSuites() {
        (TemplatedDetail::RegisterSingleSuite<Suites>(), ...);
    }
};

class Runner {
  public:
    template <typename... Suites>
    static int Run(int argc = 0, char** argv = nullptr) {
        std::array<TemplatedDetail::RegisteredSuiteEntry, sizeof...(Suites)> entries {
            TemplatedDetail::RegisteredSuiteEntry {
                .name             = ZHLN::Reflect::TypeName<Suites>(),
                .relFile          = {},
                .priority         = TemplatedDetail::ExtractSuitePriority<Suites>(),
                .emitCases        = &TemplatedDetail::EmitSuiteCases<Suites>,
                .hasRunnableMatch = &TemplatedDetail::SuiteHasRunnableMatch<Suites>,
                .execute          = &TemplatedDetail::ExecuteSuite<Suites>,
            }...
        };
        return ExecuteEntries(entries, argc, argv, [](auto&& runMatched) { return runMatched(); });
    }

    static int Main(int argc, char** argv) {
        return ExecuteEntries(TemplatedDetail::RegisteredSuitesStorage(), argc, argv, [](auto&& runMatched) { return runMatched(); });
    }

    template <typename SessionWrapper>
    static int Main(int argc, char** argv, SessionWrapper&& sessionWrapper) {
        return ExecuteEntries(TemplatedDetail::RegisteredSuitesStorage(), argc, argv, std::forward<SessionWrapper>(sessionWrapper));
    }

    template <typename... SuiteRunners>
    static int RunDeferred(SuiteRunners... runners) {
        TestStats totalStats;

        const auto add = [&totalStats](TestStats s) {
            totalStats.passed += s.passed;
            totalStats.failed += s.failed;
            totalStats.quarantined += s.quarantined;
            totalStats.skipped += s.skipped;
        };

        (add(runners()), ...);

        const RunnerOptions opts = TemplatedDetail::ParseRunnerOptions(0, nullptr);
        return Summarize(totalStats, opts);
    }

  private:
    template <typename SessionWrapper>
    static int ExecuteEntries(
        std::span<const TemplatedDetail::RegisteredSuiteEntry> entries,
        int                                                    argc,
        char**                                                 argv,
        SessionWrapper&&                                       sessionWrapper
    ) {
        const RunnerOptions opts = TemplatedDetail::ParseRunnerOptions(argc, argv);

        if (opts.parseFailed) {
            return 2;
        }
        if (opts.showHelp) {
            return 0;
        }

        if (opts.ztestList || opts.collectOnly) {
            uint32_t collected = 0;
            for (const auto& entry: entries) {
                entry.emitCases(entry.relFile, opts, collected);
            }
            if (opts.collectOnly) {
                ZHLN::Println("\n{} test(s) collected across {} module/suite entry(ies).", collected, entries.size());
            }
            return 0;
        }

        // Probe whether any non-skipped case will actually execute before
        // invoking sessionWrapper (which may initialize fiber workers or a
        // pooled Vulkan engine).
        SkipUntilState probeSkip {.active = !opts.skipUntilCase.empty()};
        bool           anyRunnable = false;
        for (const auto& entry: entries) {
            if (entry.hasRunnableMatch(entry.relFile, opts, probeSkip)) {
                anyRunnable = true;
                break;
            }
        }

        auto runMatched = [&]() -> int {
            TestStats                          totalStats {};
            SkipUntilState                     skipState {.active = !opts.skipUntilCase.empty()};
            uint32_t                           cumulativeFailed = 0;
            TemplatedDetail::FixtureScopeStore sessionFixtures;

            for (const auto& entry: entries) {
                if (opts.maxFail > 0 && cumulativeFailed >= opts.maxFail) {
                    break;
                }
                const TestStats s = entry.execute(entry.relFile, opts, skipState, cumulativeFailed, sessionFixtures);
                totalStats.passed += s.passed;
                totalStats.failed += s.failed;
                totalStats.quarantined += s.quarantined;
                totalStats.skipped += s.skipped;
            }
            sessionFixtures.Clear();
            return Summarize(totalStats, opts);
        };

        if (!anyRunnable) {
            return runMatched();
        }
        return std::forward<SessionWrapper>(sessionWrapper)(runMatched);
    }

    static int Summarize(const TestStats& totalStats, const RunnerOptions& opts) {
        ZHLN::Println("==================================================");
        ZHLN::Println("GLOBAL TEST RESULTS");
        ZHLN::Println("Total Passed: {}", totalStats.passed);
        ZHLN::Println("Total Failed: {}", totalStats.failed);
        if (totalStats.quarantined > 0) {
            ZHLN::Println("Total Quarantined: {} (known open bugs; see [ QUARANTINE ] reasons above)", totalStats.quarantined);
        }
        if (totalStats.skipped > 0) {
            ZHLN::Println("Total Skipped: {}", totalStats.skipped);
        }

        const bool hasLegacyFilter = !opts.legacyFilter.empty();
        const bool nothingRan =
            totalStats.passed == 0 && totalStats.failed == 0 && totalStats.quarantined == 0 && totalStats.skipped == 0;
        const bool unmatchedLegacyFilter = hasLegacyFilter && nothingRan && !opts.workerMode;
        if (unmatchedLegacyFilter) {
            ZHLN::Println("No test matched ZHLN_TEST_FILTER='{}'.", opts.legacyFilter);
        }

        const auto summaries = GetFailedTestSummaries();
        if (!summaries.empty()) {
            ZHLN::Println("Failed tests:");
            for (const auto& f: summaries) {
                ZHLN::Println("  {}{}::{}{}: {}", Color::Red, f.suite, f.test, Color::Reset, f.detail);
            }
        }
        ZHLN::Println("==================================================");

        ClearFailedTestSummaries();

        return totalStats.failed > 0 || unmatchedLegacyFilter ? 1 : 0;
    }
};

} // namespace ZHLN::Test
