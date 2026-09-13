// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/diagnostics/DiagnosticsInternal.hpp
//
// The seams between the diagnostics translation units. Everything here is
// engine-private: the public surface of this whole directory is still
// include/Zahlen/Log.hpp, and nothing outside src/engine/ includes this file.
//
// What lives where:
//
//   WriteToChannel      Log.cpp             the lowest-level writer, defined
//                                           beside the log sink it writes to
//   SafeRead            MemoryInspector.cpp probing possibly-unmapped memory
//   DumpFaultRegion     MemoryInspector.cpp the hex/ASCII window around a fault
//   CaptureStackTrace   StackTrace.cpp      frames into a caller's buffer
//   InitializeSymbolResolver  StackTrace.cpp  one-time DbgHelp setup (Windows)
//
// The observer bus has its own header, CrashObservers.hpp, because Engine.cpp
// includes that one and has no business seeing the rest of this.

#pragma once

#include <Zahlen/Log.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ZHLN::Diagnostics {

/// Writes straight to a descriptor, or to the log file, with no formatting and
/// no allocation. This is the only writer the crash path is allowed to use:
/// std::println and std::format both allocate, and an allocator that is holding
/// its own lock when the fault arrived will deadlock the handler.
void WriteToChannel(uint8_t channel, std::string_view msg) noexcept;

/// The crash path only ever talks to stderr. Kept as a named helper so the call
/// sites read as intent rather than as a channel constant.
inline void WriteErr(std::string_view msg) noexcept {
    WriteToChannel(static_cast<uint8_t>(LogChannel::StdErr), msg);
}

/// Copies `size` bytes out of `src` without faulting, returning false if any of
/// it was unmapped.
///
/// The whole point is that `src` is usually a wild pointer: the crash handler is
/// handed the faulting address and has to look at the memory around it without
/// taking a second fault. On Linux this is process_vm_readv, which asks the
/// kernel and allocates no descriptor; the older pipe()/write()/read() dance it
/// replaced opened two file descriptors per call, which is how a dump that
/// probes a few hundred lines exhausts the descriptor table and starts failing
/// on the lines that would have been readable.
auto SafeRead(const void* src, void* dest, size_t size) noexcept -> bool;

/// Dumps a window of memory centred on `faultAddress`, highlighting the exact
/// faulting byte. Writes "unreadable" rows for the parts that are not mapped.
void DumpFaultRegion(const void* faultAddress) noexcept;

/// Formats a backtrace into `buf`, returning the number of bytes written.
///
/// Takes the caller's buffer rather than returning a std::string so the crash
/// path can keep the frames on its own stack: a std::string that grows while the
/// heap may be corrupt is how a diagnostic turns one crash into two.
/// `maxFrames` bounds both the work and the output.
auto CaptureStackTrace(char* buf, size_t capacity, int maxFrames) noexcept -> size_t;

/// Prepares whatever the platform symbolizer needs, while the process is still
/// healthy.
///
/// On Windows this is the single SymInitialize; DbgHelp wants it once per
/// process, and calling it per stack trace -- as the code this replaced did --
/// reinitializes the symbol table underneath the walk and never releases the
/// previous one. On every other platform it does nothing.
void InitializeSymbolResolver() noexcept;

} // namespace ZHLN::Diagnostics
