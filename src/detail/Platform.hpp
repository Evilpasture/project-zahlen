// INCLUDE THIS HEADER INSTEAD OF <windows.h>!!!

#pragma once

#ifdef _WIN32
    #undef WINVER
    #undef _WIN32_WINNT
    #define WINVER 0x0A00
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
    #undef CreateThread      // conflicts if you wrap thread creation
    #undef GetCurrentThread
    #undef Sleep             // std::this_thread::sleep_for is safer anyway

    // -------------------------------------------------------------------------
    // 3. Math / Geometry types
    //    Jolt, GLM, and most renderers define their own
    // -------------------------------------------------------------------------
    #undef Rect
    #undef Point
    #undef BOOL              // int typedef that silently corrupts bool return types
    #undef TRUE
    #undef FALSE
    #undef min               // redundant if NOMINMAX, but be explicit
    #undef max

    // -------------------------------------------------------------------------
    // 4. Graphics / UI / COM
    //    LLGL and Vulkan headers especially hate these
    // -------------------------------------------------------------------------
    #undef interface         // COM keyword, breaks C++ class/concept design
    #undef OPAQUE            // Vulkan and LLGL use this as an identifier
    #undef TRANSPARENT       // same
    #undef DrawText          // GDI macro, A/W suffixed — nukes any DrawText method
    #undef DrawState         // GDI
    #undef CreateFont        // GDI — A/W macro that breaks font manager classes
    #undef LoadImage         // GDI — A/W macro, nukes asset loaders named LoadImage
    #undef LoadBitmap        // GDI
    #undef GetObject         // GDI — extremely common name, nukes asset/ECS code
    #undef SetPort           // nukes any networking or port abstractions

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
    #undef GetMessage        // A/W macro — conflicts with message queue classes
    #undef SendMessage       // same
    #undef PostMessage       // same
    #undef PeekMessage       // same — LLGL pumps its own event loop
    #undef CreateWindow      // A/W macro — stomps Window factory functions
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
    #undef LoadModule        // old Win16 relic, still defined in some SDK versions
    #undef FreeModule
    #undef GetModuleHandle   // A/W macro
    #undef GetModuleFileName // A/W macro

    // -------------------------------------------------------------------------
    // 8. Misc identifiers that appear in engine/physics/renderer namespaces
    // -------------------------------------------------------------------------
    #undef DIFFERENCE        // set-math name occasionally defined
    #undef DOMAIN            // math.h / <cmath> conflict on MSVC
    #undef VOID              // typedef void — corrupts template void specializations
    #undef pascal            // old calling convention keyword still lurking
    #undef cdecl
    #undef CDECL
#endif

#if defined(__unix__) || defined(__APPLE__) || defined(__linux__)
    #include <sys/mman.h>
    #include <unistd.h>
#endif

#if (__cplusplus < 202302L) && (!defined(_MSVC_LANG) || _MSVC_LANG < 202302L)
#error Project-Zahlen requires a compiler that supports C++23.
#endif

#ifdef _MSC_VER
#define ZHLN_RESTRICT __restrict
#else
#define ZHLN_RESTRICT __restrict__
#endif
