// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone: c++ -std=c++20 -Itools/ztest tools/ztest/tests/debugger_replay_smoke.cpp tools/ztest/DebuggerReplay.cpp -o /tmp/ztest-debugger-smoke

#include "DebuggerReplay.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace Debug = Ztest::Debugger;

static auto HasSequence(const std::vector<std::string>& args, const std::vector<std::string>& sequence) -> bool {
    return std::search(args.begin(), args.end(), sequence.begin(), sequence.end()) != args.end();
}

static auto CheckCommandConstruction() -> int {
    Debug::Replay replay {
        .argv = {"/tmp/worker with spaces", "--target-name=mock", "--file=tests/case with spaces.cpp",
                 "--exact=tests/case with spaces.cpp::test_segfault"},
        .workingDir = "/tmp/work with spaces",
        .environment = {{"DYLD_LIBRARY_PATH", "/tmp/lib with spaces"}},
    };
    const auto gdb = Debug::BuildDebuggerArgv(Debug::Tool {Debug::Kind::Gdb, "/usr/bin/gdb"}, replay);
    const auto lldb = Debug::BuildDebuggerArgv(Debug::Tool {Debug::Kind::Lldb, "/usr/bin/lldb"}, replay);
    if (!HasSequence(gdb, {"-ex", "run", "-ex", "bt", "--args", replay.argv[0]})
        || !HasSequence(lldb, {"-o", "run", "-o", "bt", "--", replay.argv[0]})
        || !HasSequence(gdb, replay.argv) || !HasSequence(lldb, replay.argv)
        || !Debug::IsYes("Y") || !Debug::IsYes("yes") || !Debug::IsNo("N") || !Debug::IsNo("")) {
        return 1;
    }
    replay.environment["PATH"] = "/definitely/not/a/debugger";
    if (Debug::FindTool(replay).has_value() || !Debug::Launch(replay).has_value()) {
        return 3;
    }
#if defined(__APPLE__)
    if (!HasSequence(lldb, {"-o", "settings set target.env-vars \"DYLD_LIBRARY_PATH=/tmp/lib with spaces\""})) {
        return 2;
    }
#endif
    return 0;
}

static auto InteractiveReplay(int argc, char** argv) -> int {
    if (argc != 4 || !Debug::HasInteractiveTerminal()) {
        return 2;
    }
    if (!Debug::Prompt()) {
        return 0; // N / EOF: the debugger must not start
    }
    Debug::Replay replay {
        .argv = {"/tmp/fake worker with spaces", "--target-name=mock", "--file=tests/case with spaces.cpp",
                 "--exact=tests/case with spaces.cpp::test_segfault"},
        .workingDir = argv[2],
        .environment = {
            {"DYLD_LIBRARY_PATH", "/tmp/library path"},
            {"PATH", std::string {argv[1]} + ":/usr/bin:/bin"},
            {"ZTEST_CAPTURE", argv[3]},
            {"ZTEST_MARKER", "from-child-env"},
        },
    };
    if (const auto failure = Debug::Launch(replay)) {
        std::cerr << *failure << '\n';
        return 3;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 1) {
        return CheckCommandConstruction();
    }
    if (argc == 2 && std::string_view {argv[1]} == "--no-terminal") {
        return Debug::Prompt() ? 1 : 0;
    }
    return InteractiveReplay(argc, argv);
}
