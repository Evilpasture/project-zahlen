// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/TestsAutoCollect.hpp
//
// Zero-boilerplate translation-unit epilogue collector used by cmake/ZTest.cmake.
// Consumes the domain-agnostic C++26 namespace reflection primitives in
// <Zahlen/Core/Reflection/Namespace.hpp> using `ZHLN::Test::Scope` as the O(1)
// namespace anchor (`std::meta::parent_of(^^Scope)`) to discover `test_*` free
// functions and primary-TU test suite structs without macros, manual
// `RegisterSuites<...>` footers, or CMake globbing.

#pragma once

#include "TestsFramework.hpp"
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Reflection/Namespace.hpp>

#include <algorithm>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ZHLN::Test {

namespace TemplatedDetail {

inline constexpr auto IsFreeTestFunctionIdentifier = [](std::string_view id) constexpr noexcept -> bool {
    return id.starts_with("test_");
};

inline constexpr auto IsCandidateSuiteClassIdentifier = [](std::string_view id) constexpr noexcept -> bool {
    return id.find("Suite") != std::string_view::npos || id.starts_with("Test") || id.ends_with("Tests");
};

template <typename Cls>
consteval auto HasAtLeastOneSuiteTestCase() -> bool {
    using Target   = SuiteMethodTarget<Cls>;
    bool hasMethod = false;
    ForEachSuiteTestCase<Cls, Target>([&]<typename>() { hasMethod = true; });
    return hasMethod;
}

template <typename Cls>
consteval auto IsNestedTargetConstructible() -> bool {
    if constexpr (HasNestedTests<Cls>) {
        return std::is_default_constructible_v<typename Cls::Tests>;
    } else {
        return true;
    }
}

template <typename Cls>
consteval auto IsAutoDiscoverableSuite() -> bool {
    if constexpr (!std::is_class_v<Cls> || std::is_abstract_v<Cls> || IsTestFixture<Cls>) {
        return false;
    } else if constexpr (!std::is_default_constructible_v<Cls> || !IsNestedTargetConstructible<Cls>()) {
        return false;
    } else {
        return HasAtLeastOneSuiteTestCase<Cls>();
    }
}

template <ZHLN::StringLiteral AbsFile>
consteval auto HasFreeTestFunctionsInFile() -> bool {
    bool found = false;
    ZHLN::Reflect::ForEachNamespaceFunction<Scope, AbsFile, IsFreeTestFunctionIdentifier>([&]<typename FreeFn>() {
        if constexpr (IsFreeTestCaseFunction<FreeFn>()) {
            found = true;
        }
    });
    return found;
}

template <ZHLN::StringLiteral AbsFile>
consteval auto ExtractFreeModulePriority() -> int {
    int prio = 0;
    ZHLN::Reflect::ForEachNamespaceFunction<Scope, AbsFile, IsFreeTestFunctionIdentifier>([&]<typename FreeFn>() {
        FreeFn::template ForEachEnclosingNamespaceAnnotationType<Scope>([&]<typename A>() {
            if constexpr (requires { A::isSuitePriority; }) {
                prio = A::order;
            }
        });
    });
    return prio;
}

template <ZHLN::StringLiteral AbsFile>
auto GetFreeModuleCaseDescriptors() -> std::span<const CaseDescriptor> {
    static const std::vector<CaseDescriptor> cases = [] {
        std::vector<CaseDescriptor> list;
        ZHLN::Reflect::ForEachNamespaceFunction<Scope, AbsFile, IsFreeTestFunctionIdentifier>([&]<typename FreeFn>() {
            if constexpr (IsFreeTestCaseFunction<FreeFn>()) {
                AppendFreeFunctionCaseDescriptors<FreeFn>(list);
            }
        });
        DeduplicateCaseDescriptorNames(list);
        return list;
    }();
    return cases;
}

template <ZHLN::StringLiteral AbsFile>
void EmitFreeModuleCases(std::string_view relFile, const RunnerOptions& opts, uint32_t& collectedCount) {
    EmitSuiteCasesRuntime(relFile, {}, GetFreeModuleCaseDescriptors<AbsFile>(), opts, collectedCount);
}

template <ZHLN::StringLiteral AbsFile>
auto FreeModuleHasRunnableMatch(std::string_view relFile, const RunnerOptions& opts, SkipUntilState& skipState) -> bool {
    return SuiteHasRunnableMatchRuntime(relFile, {}, GetFreeModuleCaseDescriptors<AbsFile>(), opts, skipState);
}

template <ZHLN::StringLiteral AbsFile>
auto ExecuteFreeModule(
    std::string_view     relFile,
    const RunnerOptions& opts,
    SkipUntilState&      skipState,
    uint32_t&            cumulativeFailed,
    FixtureScopeStore&   sessionFixtures
) -> TestStats {
    return ExecuteSuiteRuntime(
        relFile,
        {},
        GetFreeModuleCaseDescriptors<AbsFile>(),
        MakeNoopSuiteLifecycleOps(),
        opts,
        skipState,
        cumulativeFailed,
        sessionFixtures
    );
}

template <ZHLN::StringLiteral AbsFile>
void RegisterFreeModuleIfNonEmpty(std::string_view relFile) {
    if constexpr (HasFreeTestFunctionsInFile<AbsFile>()) {
        auto& suites = RegisteredSuitesStorage();
        for (const auto& existing: suites) {
            if (existing.name.empty() && existing.relFile == relFile) {
                return;
            }
        }
        suites.push_back(RegisteredSuiteEntry {
            .name             = {},
            .relFile          = relFile,
            .priority         = ExtractFreeModulePriority<AbsFile>(),
            .emitCases        = &EmitFreeModuleCases<AbsFile>,
            .hasRunnableMatch = &FreeModuleHasRunnableMatch<AbsFile>,
            .execute          = &ExecuteFreeModule<AbsFile>,
        });
        std::stable_sort(suites.begin(), suites.end(), [](const RegisteredSuiteEntry& a, const RegisteredSuiteEntry& b) {
            return a.priority < b.priority;
        });
    }
}

} // namespace TemplatedDetail

template <ZHLN::StringLiteral AbsFile, ZHLN::StringLiteral RelFile, typename UnnamedScopeAnchor = void>
struct AutoCollectFile {
    AutoCollectFile() {
        const std::string_view relView = RelFile;
        TemplatedDetail::RegisterFreeModuleIfNonEmpty<AbsFile>(relView);
        ZHLN::Reflect::ForEachNamespaceClass<
            Scope,
            AbsFile,
            TemplatedDetail::IsCandidateSuiteClassIdentifier,
            true,
            true>([&]<typename Cls>() {
            if constexpr (TemplatedDetail::IsAutoDiscoverableSuite<Cls>()) {
                TemplatedDetail::RegisterSingleSuite<Cls>(relView);
            }
        });
        if constexpr (!std::is_void_v<UnnamedScopeAnchor>) {
            ZHLN::Reflect::ForEachNamespaceClass<
                UnnamedScopeAnchor,
                AbsFile,
                TemplatedDetail::IsCandidateSuiteClassIdentifier,
                false,
                true>([&]<typename Cls>() {
                if constexpr (TemplatedDetail::IsAutoDiscoverableSuite<Cls>()) {
                    TemplatedDetail::RegisterSingleSuite<Cls>(relView);
                }
            });
        }
    }
};

} // namespace ZHLN::Test
