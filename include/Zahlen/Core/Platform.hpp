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

// ============================================================================
// Virtual Memory & OS Page Primitives
// ============================================================================

enum class PageProtection : std::uint8_t {
    NoAccess,  // PROT_NONE / PAGE_NOACCESS
    ReadWrite, // PROT_READ | PROT_WRITE / PAGE_READWRITE
    Guard,     // PROT_NONE on POSIX / PAGE_READWRITE | PAGE_GUARD on Windows
};

[[nodiscard]] inline auto GetPageSize() noexcept -> size_t {
    static const size_t pageSize = []() noexcept -> size_t {
#if defined(_WIN32)
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return static_cast<size_t>(si.dwPageSize);
#else
        return static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
    }();
    return pageSize;
}

[[nodiscard]] inline auto AlignUpToPage(size_t bytes) noexcept -> size_t {
    const size_t page = GetPageSize();
    return (bytes + page - 1) & ~(page - 1);
}

[[nodiscard]] inline auto AllocatePages(size_t bytes, PageProtection prot = PageProtection::ReadWrite) noexcept -> void* {
    if (bytes == 0) {
        return nullptr;
    }
    const size_t alignedBytes = AlignUpToPage(bytes);

#if defined(_WIN32)
    DWORD winProt = PAGE_READWRITE;
    if (prot == PageProtection::NoAccess) {
        winProt = PAGE_NOACCESS;
    } else if (prot == PageProtection::Guard) {
        winProt = PAGE_READWRITE | PAGE_GUARD;
    }
    return VirtualAlloc(nullptr, alignedBytes, MEM_COMMIT | MEM_RESERVE, winProt);
#else
    int posixProt = PROT_READ | PROT_WRITE;
    if (prot == PageProtection::NoAccess || prot == PageProtection::Guard) {
        posixProt = PROT_NONE;
    }
    void* ptr = mmap(nullptr, alignedBytes, posixProt, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (ptr == MAP_FAILED) ? nullptr : ptr;
#endif
}

inline auto ProtectPages(void* address, size_t bytes, PageProtection prot) noexcept -> bool {
    if (address == nullptr || bytes == 0) {
        return false;
    }
    const size_t alignedBytes = AlignUpToPage(bytes);

#if defined(_WIN32)
    DWORD winProt = PAGE_READWRITE;
    if (prot == PageProtection::NoAccess) {
        winProt = PAGE_NOACCESS;
    } else if (prot == PageProtection::Guard) {
        winProt = PAGE_READWRITE | PAGE_GUARD;
    }
    DWORD oldProtect = 0;
    return VirtualProtect(address, alignedBytes, winProt, &oldProtect) != 0;
#else
    int posixProt = PROT_READ | PROT_WRITE;
    if (prot == PageProtection::NoAccess || prot == PageProtection::Guard) {
        posixProt = PROT_NONE;
    }
    return mprotect(address, alignedBytes, posixProt) == 0;
#endif
}

inline void FreePages(void* address, [[maybe_unused]] size_t bytes) noexcept {
    if (address == nullptr) {
        return;
    }

#if defined(_WIN32)
    // Note: 'bytes' must be 0 when using MEM_RELEASE on Windows.
    // Address must be the base address from VirtualAlloc.
    VirtualFree(address, 0, MEM_RELEASE);
#else
    munmap(address, AlignUpToPage(bytes));
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

// ============================================================================
// Guarded Regions
// ============================================================================

/**
 * @brief A page-aligned allocation walled in by one inaccessible page on each
 * side, so that stepping off either end faults instead of silently corrupting
 * whatever is mapped next to it.
 */
struct GuardedRegion {
    void*  base  = nullptr; // Base of the whole mapping; hand this to FreeGuardedRegion().
    size_t size  = 0;       // Size of the whole mapping, guard pages included.
    void*  begin = nullptr; // First usable byte, just above the low guard page.
    void*  end   = nullptr; // One past the last usable byte; stacks grow down from here.

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return base != nullptr;
    }
};

/**
 * @brief Reserves `bytes` of read/write memory with a guard page on both ends.
 *
 * The usable payload is rounded up to a whole page, so `end - begin` may be
 * larger than requested. Returns an invalid region (`valid() == false`) if the
 * mapping or either guard page could not be set up.
 */
[[nodiscard]] inline auto AllocateGuardedRegion(size_t bytes) noexcept -> GuardedRegion {
    if (bytes == 0) {
        return {};
    }

    const size_t page   = GetPageSize();
    const size_t usable = AlignUpToPage(bytes);
    const size_t total  = usable + (page * 2); // Payload + 2 guard pages

    void* const base = AllocatePages(total, PageProtection::ReadWrite);
    if (base == nullptr) {
        return {};
    }

    auto* const low  = static_cast<std::byte*>(base);
    auto* const high = low + page + usable;

    if (!ProtectPages(low, page, PageProtection::Guard) || !ProtectPages(high, page, PageProtection::Guard)) {
        FreePages(base, total);
        return {};
    }

    return {.base = base, .size = total, .begin = low + page, .end = high};
}

/**
 * @brief Releases a region previously handed out by AllocateGuardedRegion().
 */
inline void FreeGuardedRegion(GuardedRegion region) noexcept {
    FreePages(region.base, region.size);
}

} // namespace ZHLN
