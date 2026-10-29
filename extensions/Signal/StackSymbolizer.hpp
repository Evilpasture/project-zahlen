// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Signal/StackSymbolizer.hpp
//
// Offline symbolication of stack frames captured in *another* process. The
// worker only ships addresses (module + module offset); turning them back into
// `function at file:line` happens in `ztest`, where the binaries and their
// debug information still live.
//
// POSIX shells out to `addr2line` (`llvm-addr2line` and `eu-addr2line` are
// accepted too, and `ADDR2LINE=<path>` overrides the lookup). Windows keeps the
// `module + 0xoffset` form -- dbghelp needs the matching .pdb and the win32
// worker path is synchronous, so it reports the exit code rather than frames.
//
// Everything here is best-effort: a frame that cannot be resolved keeps its
// `module + 0xoffset` form, which is still enough to map back by hand.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN {

struct WorkerStackFrame {
    int            index   = 0;
    std::uintptr_t address = 0; // absolute, as seen inside the worker
    std::string    module;      // image path reported by the worker (may be empty)
    std::uintptr_t moduleOffset = 0;
};

struct SymbolizedFrame {
    std::string              primary; // "Foo::bar() at src/foo.cpp:12" or "<module>+0x1f2e"
    std::vector<std::string> inlined; // innermost first; populated when addr2line -i reports inlining
};

// `mainExecutable` is the worker binary that produced the frames: dladdr and
// /proc maps report the main image with an empty name on Linux, so that is what
// a frame without a module is resolved against.
auto SymbolizeWorkerFrames(std::span<const WorkerStackFrame> frames, std::string_view mainExecutable) -> std::vector<SymbolizedFrame>;

// Symbolizes a single address (used for the fault address).
auto DescribeWorkerAddress(std::uintptr_t address, std::string_view module, std::uintptr_t moduleOffset) -> std::string;

} // namespace ZHLN
