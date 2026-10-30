// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"

namespace ZHLN::Test {

inline int gNamespaceFixtureInstances = 0;

struct ZHLN_ANNOTATION(SuiteFixture {}) NamespaceFixtureProbe {
    NamespaceFixtureProbe() { ++gNamespaceFixtureInstances; }
    ~NamespaceFixtureProbe() { --gNamespaceFixtureInstances; }
    NamespaceFixtureProbe(const NamespaceFixtureProbe&) = delete;
    NamespaceFixtureProbe& operator=(const NamespaceFixtureProbe&) = delete;
};

// This is intentionally the first unnamed namespace in this TU's ZHLN::Test
// scope. Unlike an explicit fixture parameter, it exercises the namespace mark
// used by physics suites, without assuming their compiler reflects it.
namespace ZHLN_ANNOTATION(UseFixtures<NamespaceFixtureProbe> {}) {

void test_unnamed_namespace_activates_suite_fixture() {
    Expect(gNamespaceFixtureInstances) == 1;
}

} // namespace
} // namespace ZHLN::Test
