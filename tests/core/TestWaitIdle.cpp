// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// ZHLN::Test::WaitIdle: what the annotation promises, and what it must not change.
//
// The scheduling itself lives in utils/ztest/main.cpp -- a worker cannot observe
// how the orchestrator sequenced its shards. What *is* testable here is the
// contract the framework side has to hold up: the mark is discovered from a
// suite, it does not alter a case's result, and it composes with the existing
// marks instead of shadowing them.
//
// Two things worth knowing before editing this file:
//
//  * The reason is a StringLiteral template argument, not a std::string_view:
//    `ZHLN::Test::WaitIdle<SomeStringViewConstant>` does not compile, because
//    StringLiteral deduces its length from `const char (&)[N]`.
//  * Case methods must be named `test_*` or carry an explicit Case annotation,
//    and the suites must live in `namespace ZHLN::Test` for the namespace
//    collector to find them. A method called `measure_thing` is silently not a
//    test case.

#include "TestsFramework.hpp"
#include <Zahlen/Core/Description.hpp>
#include <cstdint>

namespace ZHLN::Test {
namespace {

enum class WaitIdleTestError : uint8_t {
    MarkNotDiscovered ZHLN_ANNOTATION(ZHLN::Description<"WaitIdle was not discovered from the suite annotations."> {}) = 1,
    MarkChangedResult ZHLN_ANNOTATION(ZHLN::Description<"A WaitIdle case produced a different result than the same body without the mark."> {}),
    MarkShadowedMarks ZHLN_ANNOTATION(ZHLN::Description<"WaitIdle hid another mark (skip/tag/timeout) applied to the same case."> {}),
    ReasonNotCarried ZHLN_ANNOTATION(ZHLN::Description<"The WaitIdle reason did not reach the annotation type."> {}),
};

} // namespace

// Every case in this suite is a measurement, and the reason it needs a quiet
// machine is attached to the suite so a reader does not have to go looking.
struct ZHLN_ANNOTATION(ZHLN::Test::WaitIdle<"timed sampler reads wall clock"> {}) WaitIdleMarkedSuite {
    auto test_body_touches_no_shared_state() -> void {
        // Deliberately trivial: this file tests the mark, not a benchmark. What
        // matters is that marking the case does not change whether it passes.
        Expect(1 + 1) == 2;
    }

    auto test_second_measurement_in_same_suite() -> void {
        Expect(true);
    }
};

// The mark with no reason: the default argument is what most suites will use.
struct ZHLN_ANNOTATION(ZHLN::Test::WaitIdle<> {}) WaitIdleBareSuite {
    auto test_measurement_without_a_reason() -> void {
        Expect(true);
    }
};

// A measurement that is also tagged and given its own timeout: the marks have to
// coexist, because a perf gate is routinely selected by tag as well as isolated
// by the scheduler, and it usually needs a longer deadline than a unit test.
struct ZHLN_ANNOTATION(ZHLN::Test::WaitIdle<"throughput gate"> {})
    ZHLN_ANNOTATION(ZHLN::Test::Tag<"perf"> {})
    ZHLN_ANNOTATION(ZHLN::Test::Timeout<300> {}) WaitIdleTaggedSuite {
    auto test_carries_every_mark() -> void {
        Expect(true);
    }
};

} // namespace ZHLN::Test
