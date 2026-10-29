// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Signal/WorkerCrashReports.hpp
//
// Crash reporting for ztest worker processes.
//
// `ztest` watches a worker only through the child's stdout/stderr pipe, so a
// crash report has to satisfy three constraints at once:
//
//   1. It has to be produced from inside a fatal-signal handler, with nothing
//      but async-signal-safe POSIX calls: no allocation, no stdio, no locks,
//      no logger. (WorkerCrashReports.cpp is the only place that touches the
//      preallocated buffers and lock-free atomics this needs.)
//   2. It has to be framed, because everything a worker prints while a case is
//      in flight is otherwise captured as that case's engine log and folded
//      into `[ FAIL ]` diagnostics -- a trace in there is unreadable noise.
//   3. It has to be attributed to the case that was running. The orchestrator
//      learns a case's identity from its BEGIN record, so the report closes the
//      in-flight case itself (`@@ZTEST END ... CRASHED`) before the trace, and
//      the trace repeats the node ID as an out-of-band field.
//
// The stream grammar added here is one record per line, tab separated:
//
//   @@ZTEST BEGIN-NEW  <relFile> <nodeId>
//       Printed instead of `@@ZTEST BEGIN`. The `-NEW` suffix is a protocol
//       version: it distinguishes a worker built from this header from one that
//       cannot report crashes, and lets `ztest` keep reading both. It is also
//       why this marker is not a prefix match of the old one -- the tab that
//       separates the columns differs, so the two can never collide.
//
//   @@ZTEST STACKTRACE-BEGIN  <nodeId> <threadId> <reason> <faultAddress>
//   @@ZTEST NOTE              <text>
//   @@ZTEST FRAME             <index> <address> <module> <moduleOffset>
//   @@ZTEST STACKTRACE-END    <nodeId> <frameCount>
//
// Frame addresses are absolute. `<module>` + `<moduleOffset>` is the same frame
// expressed as (module base, file offset), which is the form a symbolizer in the
// parent process can use: the absolute address means nothing there, because the
// worker had its own address space and its own ASLR slide.
//
// Deliberately not built on <Signal/SignalManager.hpp>: installing that would
// also take over Interrupt/Terminate/Quit/User1/User2 in the worker, and a test
// binary has to keep the default "a signal kills me" behaviour for those -- the
// orchestrator's own shutdown path depends on it. Only fatal signals are claimed
// here, and only for the duration of a case.

#include <cstdint>
#include <string_view>

namespace ZHLN {

// Markers `tools/ztest/main.cpp` matches on; kept here so the producer and the
// consumer cannot drift apart.
inline constexpr std::string_view kWorkerCaseBeginMarker  = "@@ZTEST BEGIN-NEW\t";
inline constexpr std::string_view kWorkerCaseEndMarker    = "@@ZTEST END\t";
inline constexpr std::string_view kWorkerTraceBeginMarker = "@@ZTEST STACKTRACE-BEGIN\t";
inline constexpr std::string_view kWorkerTraceEndMarker   = "@@ZTEST STACKTRACE-END\t";
inline constexpr std::string_view kWorkerTraceFrameMarker = "@@ZTEST FRAME\t";
inline constexpr std::string_view kWorkerTraceNoteMarker  = "@@ZTEST NOTE\t";

// Exit status a worker reports after a fatal signal: what a shell reports for a
// signal-terminated child, so `ztest` can keep treating a self-report the same
// way it treats a worker the kernel killed.
inline constexpr int kSignalExitCodeBase = 128;

// Arms the reporter for this process. Call once, from the worker's main thread,
// after the command line is known to be a worker run.
//
// `forkedFilter` should stay true when this process's stdout/stderr are a pipe
// that another process may stop reading: the report is then handed to a forked
// relay so a report cannot be truncated by an unrelated `close(1)` in the test
// process. Pass false only when nothing is draining a pipe (a terminal, say).
void InstallWorkerCrashReports(bool forkedFilter = true);

// Restores the previous handlers. Fatal signals during teardown are a crash, not
// a report opportunity, so a worker that reaches the end of its run disarms.
void UninstallWorkerCrashReports();

[[nodiscard]] auto WorkerCrashReportsInstalled() noexcept -> bool;

// Emits one crash record for the calling thread. Async-signal-safe: only
// preallocated buffers, lock-free atomics and raw file descriptors.
//
// `skipFrames` drops that many innermost frames before capturing, so the
// reporter's own call chain does not show up as the top of every trace.
void EmitWorkerCrashReport(std::string_view reason, std::uintptr_t faultAddress, int skipFrames = 2) noexcept;

// Prints the BEGIN record and remembers the case, so a fatal signal raised
// anywhere before the matching EndWorkerCase can still name it. `relFile` and
// `nodeId` only have to stay valid until EndWorkerCase returns.
void BeginWorkerCase(std::string_view relFile, std::string_view nodeId) noexcept;

// Closes the case first and prints the END record second: a fatal signal in
// between then finds the case already closed (harmless), while one before it
// still gets the synthetic terminator from EmitWorkerCrashReport.
void EndWorkerCase(std::string_view relFile, std::string_view nodeId, std::string_view status, long long durationUs) noexcept;

} // namespace ZHLN
