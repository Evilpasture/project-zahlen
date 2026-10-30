// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/Core/Description.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class FrameworkTestError : uint8_t {
    FixtureLifecycleBroken ZHLN_ANNOTATION(ZHLN::Description<"Reflected fixture scope or teardown order violated."> {}) = 1,
    ParametrizeBroken      ZHLN_ANNOTATION(ZHLN::Description<"Parameterized case expansion or argument binding failed."> {}),
    ExpectDecomposeBroken  ZHLN_ANNOTATION(ZHLN::Description<"Decomposing Expect() failed to evaluate or record diagnostics."> {}),
};

enum class SamplePixelLayout : uint8_t {
    Rgba8  = 0,
    Rgba16 = 1,
    Bc7    = 2,
};

struct LifecycleLedger {
    uint32_t              sessionConstructs  = 0;
    uint32_t              sessionDestructs   = 0;
    uint32_t              suiteConstructs    = 0;
    uint32_t              suiteDestructs     = 0;
    uint32_t              functionConstructs = 0;
    uint32_t              functionDestructs  = 0;
    std::vector<char>     events {};
    std::filesystem::path previousTempPath {};
    uint32_t              cartesianHits      = 0;
    bool                  ambientActive      = false;
    uint32_t              ambientConstructs  = 0;
    bool                  suiteAmbientActive = false;
};

auto Ledger() noexcept -> LifecycleLedger& {
    static LifecycleLedger ledger;
    return ledger;
}

struct ZHLN_ANNOTATION(ZHLN::Test::SuiteFixture {}) TrackedSuiteAmbientFixture {
    TrackedSuiteAmbientFixture() {
        Ledger().suiteAmbientActive = true;
    }

    ~TrackedSuiteAmbientFixture() {
        Ledger().suiteAmbientActive = false;
    }

    TrackedSuiteAmbientFixture(const TrackedSuiteAmbientFixture&)            = delete;
    TrackedSuiteAmbientFixture& operator=(const TrackedSuiteAmbientFixture&) = delete;
};

struct ZHLN_ANNOTATION(ZHLN::Test::FunctionFixture {}) TrackedAmbientFixture {
    TrackedAmbientFixture() {
        auto& l         = Ledger();
        l.ambientActive = true;
        ++l.ambientConstructs;
    }

    ~TrackedAmbientFixture() {
        Ledger().ambientActive = false;
    }

    TrackedAmbientFixture(const TrackedAmbientFixture&)            = delete;
    TrackedAmbientFixture& operator=(const TrackedAmbientFixture&) = delete;
};

struct ZHLN_ANNOTATION(ZHLN::Test::SessionFixture {}) TrackedSessionFixture {
    uint32_t token = 100;

    TrackedSessionFixture() {
        auto& l = Ledger();
        ++l.sessionConstructs;
        l.events.push_back('S');
    }

    ~TrackedSessionFixture() {
        auto& l = Ledger();
        ++l.sessionDestructs;
        l.events.push_back('s');
    }

    TrackedSessionFixture(const TrackedSessionFixture&)            = delete;
    TrackedSessionFixture& operator=(const TrackedSessionFixture&) = delete;
};

struct ZHLN_ANNOTATION(ZHLN::Test::SuiteFixture {}) TrackedSuiteFixture {
    uint32_t inheritedToken = 0;
    uint32_t callsInSuite   = 0;

    explicit TrackedSuiteFixture(TrackedSessionFixture& session) : inheritedToken(session.token + 20) {
        auto& l = Ledger();
        ++l.suiteConstructs;
        l.events.push_back('U');
    }

    ~TrackedSuiteFixture() {
        auto& l = Ledger();
        ++l.suiteDestructs;
        l.events.push_back('u');
    }

    TrackedSuiteFixture(const TrackedSuiteFixture&)            = delete;
    TrackedSuiteFixture& operator=(const TrackedSuiteFixture&) = delete;
};

struct ZHLN_ANNOTATION(ZHLN::Test::FunctionFixture {}) TrackedFunctionFixture {
    uint32_t combined = 0;

    TrackedFunctionFixture(TrackedSuiteFixture& suite, TrackedSessionFixture& session)
        : combined(suite.inheritedToken + session.token + 3) {
        ++suite.callsInSuite;
        auto& l = Ledger();
        ++l.functionConstructs;
        l.events.push_back('F');
    }

    ~TrackedFunctionFixture() {
        auto& l = Ledger();
        ++l.functionDestructs;
        l.events.push_back('f');
    }

    TrackedFunctionFixture(const TrackedFunctionFixture&)            = delete;
    TrackedFunctionFixture& operator=(const TrackedFunctionFixture&) = delete;
};

struct NonCopyableValue {
    int value = 0;

    explicit NonCopyableValue(int v) : value(v) {}
    NonCopyableValue(const NonCopyableValue&)            = delete;
    NonCopyableValue& operator=(const NonCopyableValue&) = delete;

    auto operator==(int rhs) const noexcept -> bool { return value == rhs; }
};

struct ZHLN_ANNOTATION(ZHLN::Test::Priority<10> {}) FrameworkFixtureSuiteA {
    using Result = std::expected<void, ZHLN::ErrorCode>;

    // Public helper taking non-fixture parameters: must NOT be collected as a test case.
    auto check_sum_helper(int a, int b) -> Result {
        ZHLN::Test::Expect(a + b) >= 0;
        return {};
    }

    auto first_case_creates_hierarchy_and_temp_dir(
        TrackedFunctionFixture& fn,
        TrackedSuiteFixture&    suite,
        TrackedSessionFixture&  session,
        ZHLN::Test::TempDir&    tmp,
        ZHLN::Test::CaseInfo    info
    ) -> Result {
        auto& l = Ledger();
        ZHLN::Test::Expect(session.token) == 100;
        ZHLN::Test::Expect(suite.inheritedToken) == 120;
        ZHLN::Test::Expect(fn.combined) == 223;
        ZHLN::Test::Expect(suite.callsInSuite) == 1;
        ZHLN::Test::Expect(l.sessionConstructs) == 1;
        ZHLN::Test::Expect(l.suiteConstructs) == 1;
        ZHLN::Test::Expect(l.functionConstructs) == 1;
        ZHLN::Test::Expect(l.functionDestructs) == 0;

        ZHLN::Test::Expect(info.suiteName) == "FrameworkFixtureSuiteA";
        ZHLN::Test::Expect(info.caseName) == "first_case_creates_hierarchy_and_temp_dir";
        ZHLN::Test::Expect(!info.isParameterized);

        const auto written = tmp.WriteText("nested/hello.txt", "phase2");
        ZHLN::Test::Expect(std::filesystem::exists(written));
        l.previousTempPath = tmp.Path();
        return {};
    }

    auto second_case_reuses_suite_and_cleans_prior_temp_dir(
        TrackedFunctionFixture& fn,
        TrackedSuiteFixture&    suite,
        ZHLN::Test::TempDir&    tmp
    ) -> Result {
        auto& l = Ledger();
        ZHLN::Test::Expect(fn.combined) == 223;
        ZHLN::Test::Expect(suite.callsInSuite) == 2;
        ZHLN::Test::Expect(l.sessionConstructs) == 1;
        ZHLN::Test::Expect(l.suiteConstructs) == 1;
        ZHLN::Test::Expect(l.suiteDestructs) == 0;
        ZHLN::Test::Expect(l.functionConstructs) == 2;
        ZHLN::Test::Expect(l.functionDestructs) == 1;

        if (!l.previousTempPath.empty()) {
            ZHLN::Test::Expect(!std::filesystem::exists(l.previousTempPath));
            ZHLN::Test::Expect(tmp.Path()) != l.previousTempPath;
        }
        return {};
    }
};

struct ZHLN_ANNOTATION(ZHLN::Test::Priority<11> {}) ZHLN_ANNOTATION(ZHLN::Test::UseFixtures<TrackedSuiteAmbientFixture> {}) FrameworkFixtureSuiteB {
    using Result = std::expected<void, ZHLN::ErrorCode>;

    auto suite_b_keeps_session_and_rotates_suite_fixture(
        TrackedFunctionFixture& fn,
        TrackedSuiteFixture&    suite,
        TrackedSessionFixture&  session
    ) -> Result {
        const auto& l = Ledger();
        ZHLN::Test::Expect(l.suiteAmbientActive);
        ZHLN::Test::Expect(session.token) == 100;
        ZHLN::Test::Expect(fn.combined) == 223;
        ZHLN::Test::Expect(suite.callsInSuite) == 1;
        ZHLN::Test::Expect(l.sessionConstructs) == 1;
        ZHLN::Test::Expect(l.sessionDestructs) == 0;
        ZHLN::Test::Expect(l.suiteConstructs) == 2;
        ZHLN::Test::Expect(l.suiteDestructs) == 1;

        // Verify Suite A tore its Function fixtures down before tearing its Suite fixture down:
        // S -> U -> F -> f -> F -> f -> u -> U -> F
        const std::vector<char> expectedPrefix {'S', 'U', 'F', 'f', 'F', 'f', 'u', 'U', 'F'};
        if (l.events.size() >= expectedPrefix.size()) {
            for (std::size_t i = 0; i < expectedPrefix.size(); ++i) {
                ZHLN::Test::Expect(l.events[i]) == expectedPrefix[i];
            }
        }
        return {};
    }
};

struct ZHLN_ANNOTATION(ZHLN::Test::Priority<12> {}) FrameworkParametrizeAndExpectSuite {
    using Result = std::expected<void, ZHLN::ErrorCode>;

    ZHLN_ANNOTATION(ZHLN::Test::Cases {
        ZHLN::Test::Case {"alpha", 5, SamplePixelLayout::Rgba8},
        ZHLN::Test::Case {"rgba_hdr", 8, SamplePixelLayout::Rgba16},
        ZHLN::Test::Case {"compressed_bc7", 14, SamplePixelLayout::Bc7},
    })
    auto explicit_cases_bind_strings_ints_and_enums(
        std::string_view       label,
        int                    expectedLen,
        SamplePixelLayout      layout,
        ZHLN::Test::CaseInfo   info
    ) -> Result {
        ZHLN::Test::Expect(label.size()) == expectedLen;
        ZHLN::Test::Expect(info.isParameterized);
        ZHLN::Test::Expect(info.methodName) == "explicit_cases_bind_strings_ints_and_enums";
        if (layout == SamplePixelLayout::Bc7) {
            ZHLN::Test::Expect(info.caseName.find("Bc7")) != std::string_view::npos;
        }
        return {};
    }

    ZHLN_ANNOTATION(ZHLN::Test::NamedCase {"origin", 0.0f, 0.0f, 0.0f})
    ZHLN_ANNOTATION(ZHLN::Test::NamedCase {"pythagorean_3_4_5", 3.0f, 4.0f, 5.0f})
    auto named_cases_use_custom_ids(
        float                x,
        float                y,
        float                expectedHypot,
        ZHLN::Test::CaseInfo info
    ) -> Result {
        const float hypot = std::sqrt(x * x + y * y);
        ZHLN::Test::Expect(hypot) == ZHLN::Test::Approx(expectedHypot, 1e-5f);
        if (expectedHypot == 0.0f) {
            ZHLN::Test::Expect(info.caseName) == "named_cases_use_custom_ids[origin]";
        } else {
            ZHLN::Test::Expect(info.caseName) == "named_cases_use_custom_ids[pythagorean_3_4_5]";
        }
        return {};
    }

    ZHLN_ANNOTATION(ZHLN::Test::CaseValues<2, 4, 8> {})
    ZHLN_ANNOTATION(ZHLN::Test::CaseValues<3, 5, 15> {})
    auto nttp_case_values_expand_cleanly(int a, int b, int product) -> Result {
        ZHLN::Test::Expect(a * b) == product;
        return {};
    }

    ZHLN_ANNOTATION(ZHLN::Test::Param {64, 128, 256})
    ZHLN_ANNOTATION(ZHLN::Test::Param {"r8", "rgba16f"})
    auto cartesian_axes_combine_with_fixtures(
        ZHLN::Test::TempDir& tmp,
        int                  extent,
        std::string_view     fmt,
        ZHLN::Test::CaseInfo info
    ) -> Result {
        ZHLN::Test::Expect(extent) >= 64;
        ZHLN::Test::Expect(extent) <= 256;
        ZHLN::Test::Expect(!fmt.empty());
        ZHLN::Test::Expect(info.isParameterized);

        const auto marker = tmp.WriteText("extent.txt", std::format("{}:{}", extent, fmt));
        std::ifstream in {marker};
        std::string   readBack;
        std::getline(in, readBack);
        ZHLN::Test::Expect(readBack) == std::format("{}:{}", extent, fmt);

        ++Ledger().cartesianHits;
        return {};
    }

    auto verify_cartesian_expanded_all_six_combinations() -> Result {
        ZHLN::Test::Expect(Ledger().cartesianHits) == 6;
        return {};
    }

    auto expression_decomposing_expect_operators_and_diagnostics() -> Result {
        const std::vector<int> nums {10, 20, 30};
        ZHLN::Test::Expect(nums.size()) == 3;
        ZHLN::Test::Expect(nums.size()) != 4;
        ZHLN::Test::Expect(nums.size()) < 10;
        ZHLN::Test::Expect(nums.size()) <= 3;
        ZHLN::Test::Expect(nums.size()) > 2;
        ZHLN::Test::Expect(nums.size()) >= 3;

        // Safe signed/unsigned integer comparison: -1 must never equal UINT32_MAX.
        const uint32_t maxU32 = 0xFFFFFFFFu;
        ZHLN::Test::Expect(maxU32) != -1;
        ZHLN::Test::Expect(-1) < maxU32;

        // Distinct buffer C-strings compare by content, not pointer address.
        std::string dynamicBuf = "zahlen_phase2";
        const char* cstrPtr    = dynamicBuf.c_str();
        ZHLN::Test::Expect(cstrPtr) == "zahlen_phase2";
        ZHLN::Test::Expect(cstrPtr) != "other";

        // Non-copyable lvalues & Approx float matcher.
        NonCopyableValue nonCopy {77};
        ZHLN::Test::Expect(nonCopy) == 77;
        ZHLN::Test::Expect(0.1 + 0.2) == ZHLN::Test::Approx(0.3, 1e-9);
        ZHLN::Test::Expect(0.5f) != ZHLN::Test::Approx(0.25f, 1e-3f);
        ZHLN::Test::Expect(!nums.empty());
        ZHLN::Test::Expect(nums.size() == 3).IsTrue();
        ZHLN::Test::Expect(nums.empty()).IsFalse();

        // Controlled mismatch probe: verify failure decomposition records LHS/op/RHS, then clear.
        auto&      ctx    = ZHLN::Test::GetThreadLocalContext();
        const auto before = ctx.failures.size();
        const int  lhsVal = 10;
        const bool ok     = (ZHLN::Test::Expect(lhsVal + 1) == 42);
        if (ok || ctx.failures.size() != before + 1) {
            return std::unexpected(FrameworkTestError::ExpectDecomposeBroken);
        }

        const auto captured = ctx.failures.back();
        ctx.failures.pop_back();

        ZHLN::Test::Expect(captured.decomposed);
        ZHLN::Test::Expect(captured.op) == "==";
        ZHLN::Test::Expect(captured.actualValue) == "11";
        ZHLN::Test::Expect(captured.expectedValue) == "42";
        ZHLN::Test::Expect(captured.line) > 0;
        return {};
    }

    ZHLN_ANNOTATION(ZHLN::Test::UseFixtures<TrackedAmbientFixture> {})
    auto use_fixtures_mark_activates_ambient_raii_without_unused_parameters() -> Result {
        ZHLN::Test::Expect(Ledger().ambientActive);
        ZHLN::Test::Expect(Ledger().ambientConstructs) == 1;
        return {};
    }

    auto verify_ambient_fixture_tore_down_after_marked_case() -> Result {
        ZHLN::Test::Expect(!Ledger().ambientActive);
        ZHLN::Test::Expect(!Ledger().suiteAmbientActive);
        ZHLN::Test::Expect(Ledger().ambientConstructs) == 1;
        return {};
    }
};
} // namespace

// ============================================================================
// Phase 3 Self-Tests: Zero-Boilerplate `namespace ZHLN::Test` `test_*` Functions
// ============================================================================

namespace ZHLN::Test {
namespace {

struct ZHLN_ANNOTATION(SuiteFixture {}) FreeModuleSuiteCounter {
    uint32_t touches = 0;
};

void test_free_function_void_return_and_fixture_di(
    FreeModuleSuiteCounter& counter,
    TempDir&                temp,
    ZHLN::ECS::Registry&    reg,
    const CaseInfo&         info
) {
    ++counter.touches;
    Expect(counter.touches) == 1u;
    Expect(std::filesystem::is_directory(temp.Path()));
    const ZHLN::Entity e = reg.Create();
    Expect(reg.IsAlive(e));
    Expect(info.methodName) == std::string_view {"test_free_function_void_return_and_fixture_di"};
    Expect(!info.filePath.empty());
}

ZHLN_ANNOTATION(Cases {
    NamedCase {"alpha", 3, 7, 10},
    NamedCase {"beta", 12, 30, 42},
})
void test_free_function_parametrized_rows(
    FreeModuleSuiteCounter& counter,
    int                     lhs,
    int                     rhs,
    int                     expectedSum,
    const CaseInfo&         info
) {
    ++counter.touches;
    Expect(counter.touches) >= 2u;
    Expect(lhs + rhs) == expectedSum;
    Expect(info.isParameterized);
}

} // namespace

namespace ZHLN_ANNOTATION(Tags<"phase3_ns"> {}) MarkedSubgroup {

auto test_free_function_enclosing_namespace_marks_and_expected_return(const CaseInfo& info) -> std::expected<void, ZHLN::ErrorCode> {
    Expect(info.caseName) == std::string_view {"test_free_function_enclosing_namespace_marks_and_expected_return"};
    Expect(info.filePath.ends_with("TestTestFramework.cpp"));
    return {};
}

} // namespace MarkedSubgroup
} // namespace ZHLN::Test
