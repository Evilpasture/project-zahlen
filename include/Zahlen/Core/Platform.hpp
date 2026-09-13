// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// INCLUDE THIS HEADER INSTEAD OF <windows.h>!!!

#pragma once

#include <cstddef>
#include <cstdint>
#ifdef _WIN32
#undef WINVER
#undef _WIN32_WINNT
#define WINVER       0x0A00
#define _WIN32_WINNT 0x0A00

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#pragma comment(lib, "User32.lib")

// -------------------------------------------------------------------------
// 1. Near / Far / other spatial keywords
//    Clash with camera nearZ/farZ, Jolt's math, and GLSL-style naming
// -------------------------------------------------------------------------
#undef near
#undef far
#undef Near
#undef Far
#undef NEAR
#undef FAR

// -------------------------------------------------------------------------
// 2. Threading / Synchronization / Memory
//    These stomp std::, Jolt atomics, and your custom Mutex/Fiber
// -------------------------------------------------------------------------
#undef MemoryBarrier
#undef Yield
#undef CreateThread // conflicts if you wrap thread creation
#undef GetCurrentThread
#undef Sleep // std::this_thread::sleep_for is safer anyway

// -------------------------------------------------------------------------
// 3. Math / Geometry types
//    Jolt, GLM, and most renderers define their own
// -------------------------------------------------------------------------
#undef Rect
#undef Point
#undef BOOL // int typedef that silently corrupts bool return types
#undef TRUE
#undef FALSE
#undef min // redundant if NOMINMAX, but be explicit
#undef max

// -------------------------------------------------------------------------
// 4. Graphics / UI / COM
//    LLGL and Vulkan headers especially hate these
// -------------------------------------------------------------------------
#undef interface   // COM keyword, breaks C++ class/concept design
#undef OPAQUE      // Vulkan and LLGL use this as an identifier
#undef TRANSPARENT // same
#undef DrawText    // GDI macro, A/W suffixed — nukes any DrawText method
#undef DrawState   // GDI
#undef CreateFont  // GDI — A/W macro that breaks font manager classes
#undef LoadImage   // GDI — A/W macro, nukes asset loaders named LoadImage
#undef LoadBitmap  // GDI
#undef GetObject   // GDI — extremely common name, nukes asset/ECS code
#undef SetPort     // nukes any networking or port abstractions

// -------------------------------------------------------------------------
// 5. Error / Status codes redefined as macros
//    These corrupt enum values or constexpr error code definitions
// -------------------------------------------------------------------------
#undef ERROR
#undef NO_ERROR
#undef DELETE
#undef IN
#undef OUT
#undef IGNORE
#undef STRICT

// -------------------------------------------------------------------------
// 6. String / Encoding macros
//    Force redefinition as A/W variants that silently corrupt your own APIs
// -------------------------------------------------------------------------
#undef GetMessage   // A/W macro — conflicts with message queue classes
#undef SendMessage  // same
#undef PostMessage  // same
#undef PeekMessage  // same — LLGL pumps its own event loop
#undef CreateWindow // A/W macro — stomps Window factory functions
#undef CreateWindowEx
#undef FindWindow
#undef RegisterClass
#undef UnregisterClass
#undef GetClassName

// -------------------------------------------------------------------------
// 7. Process / Module
//    Clash with engine module/plugin systems
// -------------------------------------------------------------------------
#undef GetCurrentProcess
#undef OpenProcess
#undef TerminateProcess
#undef LoadModule // old Win16 relic, still defined in some SDK versions
#undef FreeModule
#undef GetModuleHandle   // A/W macro
#undef GetModuleFileName // A/W macro

// -------------------------------------------------------------------------
// 8. Misc identifiers that appear in engine/physics/renderer namespaces
// -------------------------------------------------------------------------
#undef DIFFERENCE // set-math name occasionally defined
#undef DOMAIN     // math.h / <cmath> conflict on MSVC
#undef VOID       // typedef void — corrupts template void specializations
#undef pascal     // old calling convention keyword still lurking
#undef cdecl
#undef CDECL
#undef small
#endif

#if defined(__unix__) || defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if __cplusplus < 202400L
#error "Project-Zahlen requires C++26"
#endif

#ifdef _MSC_VER
#ifndef ZHLN_RESTRICT
#define ZHLN_RESTRICT __restrict
#endif
#else
#ifndef ZHLN_RESTRICT
#define ZHLN_RESTRICT __restrict__
#endif
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

namespace ZHLN {

inline auto GetPID() noexcept {
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

// Check if the compiler supports a standardized debug break hook
inline void DebugBreak() noexcept {
#if defined(_WIN32) || defined(_WIN64)
// We are strictly on Windows
#if defined(_MSC_VER) || defined(__clang__)
    __debugbreak();
#endif
#elif defined(__linux__)
// We are strictly on Linux
#if defined(__GNUC__) || defined(__clang__)
    __builtin_trap();
#endif
#elif defined(__APPLE__)
// We are strictly on macOS
#if defined(__GNUC__) || defined(__clang__)
    __builtin_trap();
#endif
#endif
}

inline void CPURelax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

inline void HaltThread() noexcept {
#if defined(_WIN32)
    ::Sleep(INFINITE);
#else
    pause();
#endif
}

// ============================================================================
// Cached Stack Bounds
// ============================================================================

/**
 * @brief The bounds of the stack that is currently running.
 *
 * `base` is the highest address (where a downwards-growing stack starts),
 * `limit` the lowest one it may grow to.
 */
struct StackBounds {
    void* base  = nullptr;
    void* limit = nullptr;
};

/**
 * @brief Reads the stack bounds the OS recorded for the calling thread.
 *
 * Some platforms keep a copy of the active stack bounds in per-thread OS state:
 * Windows stores them in the TEB, where the kernel, stack probes, SEH and
 * GetCurrentThreadStackLimits() all read them. Anything that swaps stacks by
 * hand (fibers, coroutines, user-space schedulers) has to keep that copy in
 * sync with the stack it switches to.
 *
 * Platforms with no such bookkeeping return a zeroed struct.
 */
[[nodiscard]] inline auto GetCurrentStackBounds() noexcept -> StackBounds {
#if defined(_WIN32)
    auto* const tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    return {.base = tib->StackBase, .limit = tib->StackLimit};
#else
    return {};
#endif
}

/**
 * @brief Overwrites the OS's copy of the calling thread's stack bounds.
 * No-op on platforms that don't keep one.
 */
inline void SetCurrentStackBounds([[maybe_unused]] StackBounds bounds) noexcept {
#if defined(_WIN32)
    auto* const tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    tib->StackBase  = bounds.base;
    tib->StackLimit = bounds.limit;
#else
    // Nothing to keep in sync.
#endif
}

} // namespace ZHLN
