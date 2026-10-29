// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// CLI early exits and callable shape require no GPU/device or Engine::Create.
#include "TestsFramework.hpp"
#include <Zahlen/App.hpp>
#include <concepts>
#include <cstdlib>
#include <memory>
#include <type_traits>
#include <utility>

namespace ZHLN::Test {

namespace {
struct MoveOnlyFrameLogic {
    std::unique_ptr<int> counter;
    void operator()(Engine&, float) { ++*counter; }
};
static_assert(!std::copy_constructible<MoveOnlyFrameLogic>);
static_assert(requires { App().Run(std::declval<MoveOnlyFrameLogic>()); });
static_assert(requires { App().Run(std::declval<MoveOnlyFrameLogic&>()); });
template <typename T>
concept LaunchOnly = !requires(T value) { value.onFrameEvents; } &&
                     !requires(T value) { value.onConfigure; } &&
                     !requires(T value) { value.onStart; } &&
                     !requires(T value) { value.onUpdate; };
static_assert(std::is_trivially_copyable_v<EngineRunConfig>);
static_assert(LaunchOnly<EngineRunConfig>);
} // namespace

void test_app_help_exits_without_running_gameplay() {
    char program[] = "test-application";
    char option[]  = "--help";
    char* args[]   = {program, option};
    bool  ran      = false;
    Expect(App().Run(2, args, [&](Engine&) { ran = true; })) == EXIT_SUCCESS;
    Expect(!ran);
}

void test_app_rejects_unsupported_host_modes_without_starting_engine() {
    char program[] = "test-application";
    char editor[]  = "--editor";
    char bench[]   = "--benchmark";
    char* args[]   = {program, editor};
    bool  ran      = false;
    Expect(App().Run(2, args, [&](Engine&) { ran = true; })) == EXIT_FAILURE;
    args[1] = bench;
    Expect(App().Run(2, args, [&](Engine&) { ran = true; })) == EXIT_FAILURE;
    Expect(!ran);
}

void test_app_rejects_invalid_args_before_engine_creation() {
    char program[] = "test-application";
    char unknown[] = "--not-an-engine-option";
    char* args[]   = {program, unknown};
    Expect(App().Run(2, args)) == EXIT_FAILURE;
    Expect(App().Run(-1, args)) == EXIT_FAILURE;
}

} // namespace ZHLN::Test
