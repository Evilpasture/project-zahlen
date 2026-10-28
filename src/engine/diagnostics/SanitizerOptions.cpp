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

#if defined(ZHLN_SANITIZER_BUILD) || defined(__ASAN_ENABLED__) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
// NOLINTBEGIN(bugprone-reserved-identifier)
extern "C" {
[[gnu::visibility("default")]] auto __asan_default_options() -> const char*;
[[gnu::visibility("default")]] auto __lsan_default_options() -> const char*;
[[gnu::visibility("default")]] auto __ubsan_default_options() -> const char*;
[[gnu::visibility("default")]] auto __tsan_default_options() -> const char*;

[[gnu::visibility("default")]] auto __asan_default_options() -> const char* {
    return "protect_shadow_gap=0:detect_leaks=1:symbolize=1:halt_on_error=1";
}

[[gnu::visibility("default")]] auto __lsan_default_options() -> const char* {
    return "suppressions=" ZHLN_PROJECT_ROOT "/lsan.supp:print_suppressions=0";
}

[[gnu::visibility("default")]] auto __ubsan_default_options() -> const char* {
    return "suppressions=" ZHLN_PROJECT_ROOT "/ubsan.supp:print_stacktrace=1:halt_on_error=1";
}

[[gnu::visibility("default")]] auto __tsan_default_options() -> const char* {
    return "suppressions=" ZHLN_PROJECT_ROOT "/tsan.supp:halt_on_error=1";
}
}
// NOLINTEND(bugprone-reserved-identifier)
#endif
