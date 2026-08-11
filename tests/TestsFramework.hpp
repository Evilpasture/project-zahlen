// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Error.hpp>
#include <array>
#include <concepts>
#include <expected>
#include <string_view>
#include <type_traits>

namespace ZHLN::Test {

struct TestStats {
    uint32_t passed = 0;
    uint32_t failed = 0;
};

// C++20 Concept to verify that a test method returns a std::expected-like type
template <typename T>
concept TestResult = requires(T t) {
    { t.has_value() } -> std::convertible_to<bool>;
    { t.error() } -> std::convertible_to<ZHLN::Error>;
};

// C++20 Concept to detect nested Tests struct
template <typename T>
concept HasNestedTests = requires { typename T::Tests; };

template <typename Suite>
TestStats RunSuite() {
    // ALWAYS instantiate the parent Suite to guarantee RAII Setup/Teardown execution
    Suite     suite;
    TestStats stats;

    ZHLN::Println("==================================================");
    ZHLN::Println("Running Test Suite: {}", ZHLN::Reflect::TypeName<Suite>());
    ZHLN::Println("==================================================");

    if constexpr (HasNestedTests<Suite>) {
        // Nested Struct Path: No prefix filtering required
        using Target = typename Suite::Tests;
        Target target;

        ZHLN::Reflect::ForEachMethodPointer<Target>([&](std::string_view name, auto pmf) {
            using MethodType = decltype(pmf);
            using ReturnType = std::invoke_result_t<MethodType, Target>;

            if constexpr (TestResult<ReturnType>) {
                auto result = (target.*pmf)();
                if (result) {
                    ZHLN::Println("  [ PASS ] {}", name);
                    stats.passed++;
                } else {
                    ZHLN::Println("  [ FAIL ] {}: {}", name, result.error().Message());
                    stats.failed++;
                }
            }
        });
    } else {
        // Fallback Path: Filter using "test_" prefix on the suite itself
        ZHLN::Reflect::ForEachMethodPointer<Suite>([&](std::string_view name, auto pmf) {
            if (name.starts_with("test_")) {
                using MethodType = decltype(pmf);
                using ReturnType = std::invoke_result_t<MethodType, Suite>;

                if constexpr (TestResult<ReturnType>) {
                    auto result = (suite.*pmf)();
                    if (result) {
                        ZHLN::Println("  [ PASS ] {}", name);
                        stats.passed++;
                    } else {
                        ZHLN::Println("  [ FAIL ] {}: {}", name, result.error().Message());
                        stats.failed++;
                    }
                }
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
