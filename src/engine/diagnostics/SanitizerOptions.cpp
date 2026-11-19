// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Sanitizer option/suppression hooks, compiled as a standalone OBJECT target and
// linked into every executable that consumes zahlen_engine.
//
// Why in the executable and not the .so: under static compiler-rt the runtime
// carries weak `return "";` stubs for these hooks, and glibc resolves a dynamic
// symbol against the main executable first -- so strong definitions inside
// libzahlen_engine.so are shadowed and a sanitized consumer runs with no rules.
// The same is true of an installed toolchain, where a BUILD_INTERFACE source
// cannot ride the exported target; cmake/Toolchains.cmake installs this object
// and ZahlenEngineConfig.cmake attaches it.
//
// The rules arrive via #embed from lsan.supp / ubsan.supp (the way the default
// font does) so there is no path to resolve at startup. A suppressions=<path>
// naming a missing file is fatal to LeakSanitizer, and an installed prefix is
// relocated with mv, so the text travels inside the binary.

#if defined(ZHLN_SANITIZER_BUILD) || defined(__ASAN_ENABLED__) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
// NOLINTBEGIN(bugprone-reserved-identifier)

// Absolute paths from src/engine/CMakeLists.txt: moving a rule file cannot
// silently repoint the directive, and reaching this TU without them is a build
// error rather than a silently unsuppressed run.
#if !defined(ZHLN_LSAN_SUPPRESSIONS_PATH) || !defined(ZHLN_UBSAN_SUPPRESSIONS_PATH)
#error "SanitizerOptions.cpp is built only by zahlen_sanitizer_options; ZHLN_LSAN_SUPPRESSIONS_PATH and ZHLN_UBSAN_SUPPRESSIONS_PATH come from src/engine/CMakeLists.txt"
#endif

// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif

// The trailing 0 terminates the string the runtime reads until NUL. An empty
// .supp file degenerates to "{ , 0 }" and fails to compile, which is loud.
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

// print_suppressions stays 0 outside CI; the packaging test enables it through
// LSAN_OPTIONS so a rule that stops matching shows up in the log.
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
