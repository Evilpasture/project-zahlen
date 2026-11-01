// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Compiled into a standalone OBJECT target and linked directly into executable
// targets consuming zahlen_engine. On ELF with static compiler-rt
// (libclang_rt.asan / libclang_rt.ubsan_standalone), the sanitizer runtime is
// linked into the main executable with weak default-options stubs (`return "";`).
// Because glibc dynamic symbol lookup searches the main executable before
// DT_NEEDED shared libraries regardless of STB_WEAK vs STB_GLOBAL, defining
// these hooks only inside libzahlen_engine.so leaves them shadowed by the
// executable's weak stubs unless the strong definitions also link into the
// executable itself.
//
// The same holds for a consumer of an installed toolchain: the hook object rides
// zahlen_engine as a $<BUILD_INTERFACE:...> source, which an exported target
// drops, and an exported INTERFACE source cannot carry a relocatable path. The
// toolchain therefore installs this object and ZahlenEngineConfig.cmake attaches it
// (see cmake/Toolchains.cmake and cmake/toolchain/PackageConfig.cmake.in). Without
// that, an asan-ubsan consumer runs with no rules at all and reports the
// driver's own one-time-init leaks as if they were the application's.
//
// The rules are #embed-ed from lsan.supp / ubsan.supp, the same way the default
// font and the cooked shader modules arrive, so there is no path to resolve at
// startup: an installed prefix is relocated with mv, the packaging test hides
// /workspace on purpose, and the payload image has no source tree at all. A
// suppressions=<path> flag naming a missing file is *fatal* (LeakSanitizer
// prints "failed to read suppressions file" and dies before reporting
// anything), which is why the text travels inside the binary instead.
// ZHLN_PROJECT_ROOT stays what ARCHITECTURE.md says it is: a way to recognize
// the tree a developer is running from, never a path a deployed binary needs.

#if defined(ZHLN_SANITIZER_BUILD) || defined(__ASAN_ENABLED__) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
// NOLINTBEGIN(bugprone-reserved-identifier)

// Both operands are compile definitions set in src/engine/CMakeLists.txt, the
// way src/gui exports its default font path: absolute, so moving a rule file
// cannot silently repoint the directive. Embedding is the only way the rules
// exist -- there is no file to find at run time and no fallback to an empty
// rule set, so a build that reached this TU without the definitions is a build
// error rather than a silently unsuppressed run. Missing or empty files are
// reported by the directive itself and by the terminator below.
#if !defined(ZHLN_LSAN_SUPPRESSIONS_PATH) || !defined(ZHLN_UBSAN_SUPPRESSIONS_PATH)
#error "SanitizerOptions.cpp is built only by zahlen_sanitizer_options; ZHLN_LSAN_SUPPRESSIONS_PATH and ZHLN_UBSAN_SUPPRESSIONS_PATH come from src/engine/CMakeLists.txt"
#endif

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif

// The trailing 0 is the string terminator: __lsan_default_suppressions() hands
// the runtime a const char*, and its parser reads until NUL. Written as a list
// element rather than as #embed suffix(, 0) so the directive stays the plain
// form used everywhere else in the tree. An empty .supp file would degenerate
// to "{ , 0 }" and fail to compile, which is the loud outcome we want.
constexpr unsigned char kLeakSuppressionsText[] = {
#embed ZHLN_LSAN_SUPPRESSIONS_PATH
    , 0};

constexpr unsigned char kUndefinedSuppressionsText[] = {
#embed ZHLN_UBSAN_SUPPRESSIONS_PATH
    , 0};

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
// NOLINTEND(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)

extern "C" {
[[gnu::visibility("default")]] auto __asan_default_options() -> const char*;
[[gnu::visibility("default")]] auto __lsan_default_options() -> const char*;
[[gnu::visibility("default")]] auto __lsan_default_suppressions() -> const char*;
[[gnu::visibility("default")]] auto __ubsan_default_options() -> const char*;
[[gnu::visibility("default")]] auto __ubsan_default_suppressions() -> const char*;
[[gnu::visibility("default")]] auto __tsan_default_options() -> const char*;

[[gnu::visibility("default")]] auto __asan_default_options() -> const char* {
    return "protect_shadow_gap=0:detect_leaks=1:symbolize=1:halt_on_error=1";
}

// print_suppressions stays 0 outside CI: the packaging test turns it on through
// LSAN_OPTIONS so a rule that stops matching is visible in the log.
[[gnu::visibility("default")]] auto __lsan_default_options() -> const char* {
    return "print_suppressions=0";
}

[[gnu::visibility("default")]] auto __lsan_default_suppressions() -> const char* {
    return reinterpret_cast<const char*>(kLeakSuppressionsText);
}

[[gnu::visibility("default")]] auto __ubsan_default_options() -> const char* {
    return "print_stacktrace=1:halt_on_error=1";
}

[[gnu::visibility("default")]] auto __ubsan_default_suppressions() -> const char* {
    return reinterpret_cast<const char*>(kUndefinedSuppressionsText);
}

[[gnu::visibility("default")]] auto __tsan_default_options() -> const char* {
    return "halt_on_error=1";
}
}
// NOLINTEND(bugprone-reserved-identifier)
#endif
