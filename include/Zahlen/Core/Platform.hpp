// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
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

// MinGW's winnt.h reaches GCC intrinsics with C linkage; this must precede
// standalone immintrin.h (including Jolt's use in module global fragments).
#include <windows.h>

// MSVC auto-links libraries named here. Every other toolchain links them from
// CMake instead -- see the dbghelp note in CMakeLists.txt -- and warns about the
// pragma under -Wunknown-pragmas, so it is MSVC-only on purpose.
#if defined(_MSC_VER)
#pragma comment(lib, "User32.lib")
#endif

#undef near
#undef far
#undef Near
#undef Far
#undef NEAR
#undef FAR

#undef MemoryBarrier
#undef Yield
#undef CreateThread
#undef GetCurrentThread
#undef Sleep

#undef Rect
#undef Point
#undef min
#undef max

#undef BOOL

using BOOL = int;

#undef interface
#undef OPAQUE
#undef TRANSPARENT
#undef DrawText
#undef DrawState
#undef CreateFont
#undef LoadImage
#undef LoadBitmap
#undef GetObject
#undef SetPort

#undef ERROR
#undef NO_ERROR
#undef DELETE
#undef IN
#undef OUT
#undef IGNORE
#undef STRICT

#undef GetMessage
#undef SendMessage
#undef PostMessage
#undef PeekMessage
#undef CreateWindow
#undef CreateWindowEx
#undef FindWindow
#undef RegisterClass
#undef UnregisterClass
#undef GetClassName

#undef GetCurrentProcess
#undef OpenProcess
#undef TerminateProcess
#undef LoadModule
#undef FreeModule
#undef GetModuleHandle
#undef GetModuleFileName

#undef DIFFERENCE
#undef DOMAIN
#undef pascal

#undef cdecl
#undef CDECL
#undef small

#undef VOID
using VOID = void;

#endif

#if __cplusplus < 202400L
#error "Project-Zahlen requires C++26"
#endif

#ifdef _MSC_VER
#include <intrin.h>
#ifndef ZHLN_RESTRICT
#define ZHLN_RESTRICT __restrict
#endif
#else
#ifndef ZHLN_RESTRICT
#define ZHLN_RESTRICT __restrict__
#endif
#endif

namespace ZHLN {

ZHLN_API auto GetPID() noexcept -> int;

inline void DebugBreak() noexcept {
#if defined(_WIN32) || defined(_WIN64)
#if defined(_MSC_VER) || defined(__clang__)
    __debugbreak();
#elif defined(__GNUC__)
    __builtin_trap();
#endif
#elif defined(__linux__) || defined(__APPLE__)
#if defined(__GNUC__) || defined(__clang__)
    __builtin_trap();
#endif
#endif
}

inline void CPURelax() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(__clang__) || defined(__GNUC__)
    __builtin_ia32_pause();
#elif defined(_MSC_VER)
    _mm_pause();
#endif
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

ZHLN_API void HaltThread() noexcept;

struct StackBounds {
    void* base  = nullptr;
    void* limit = nullptr;
};

[[nodiscard]] inline auto GetCurrentStackBounds() noexcept -> StackBounds {
#if defined(_WIN32)
    auto* const tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    return {.base = tib->StackBase, .limit = tib->StackLimit};
#else
    return {};
#endif
}

inline void SetCurrentStackBounds([[maybe_unused]] StackBounds bounds) noexcept {
#if defined(_WIN32)
    auto* const tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    tib->StackBase  = bounds.base;
    tib->StackLimit = bounds.limit;
#endif
}

} // namespace ZHLN
