// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/RuntimePaths.hpp
//
// Where the process is allowed to read and write at runtime.
//
// This header exists because the engine used to answer "where?" with a
// build-tree path -- the pipeline cache at `build/cache/pipeline_cache.bin`,
// the asset pack at `build/data/base.pak`, the vendor crash dump at
// `gpu_crash_dump.bin` -- relative to the current working directory. That is
// correct for exactly one launch: the one CMake performs, with
// WORKING_DIRECTORY set to the source root (every target in the root
// CMakeLists), where `build/` is a real directory. Ship the same binary and the
// answers stop being true: launched from Finder the CWD is `/`, so the cache
// write fails silently and every run recompiles every pipeline; launched from a
// folder the user chose, a stray `build/` appears next to the executable.
//
// So there are two regimes, and which one applies is decided once, here:
//
//   * dev tree -- the process is running from the source tree it was built in
//     (IsDevTree(): the CWD is ZHLN::ProjectRoot, which is what every
//     CMake-launched workflow is, or the executable lives under
//     `<root>/build`). Everything stays where it always was: `build/cache/...`,
//     `build/data/...`. Nothing about a developer's day changes.
//   * anywhere else -- a distributed or hand-launched binary. Writable state
//     goes to the per-user cache directory, and shipped data is looked for
//     next to the executable (inside the app bundle's Resources on macOS)
//     before falling back to the CWD.
//
// Both regimes stay overridable: ZHLN_CACHE_DIR and ZHLN_DATA_DIR win over
// whatever is chosen here, matching the existing ZHLN_FONT_PATH precedent in
// CreativeWorksFactory.
//
// This header pulls <filesystem>, so it is not for the hot path or the error
// path -- include it from the handful of cold sites that resolve a real path
// (PipelineCache, GPUDiagnostics, Kernel's asset mount).
//
// Everything here uses the std::error_code overloads. The library builds with
// -fno-exceptions, so a throwing filesystem call is a terminate, and a missing
// $HOME or an unmounted /proc must not be able to do that.

#pragma once

#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Platform.hpp>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace ZHLN::RuntimePaths {

/// The application's name under the user's cache directory. One directory per
/// user per app: nothing here is shared with another game, and removing it is
/// always safe (every byte is a regenerable cache). Capitalized where the host
/// convention is (`~/Library/Caches`, `%LOCALAPPDATA%`), lowercase on the
/// FHS-style XDG path.
inline constexpr std::string_view kAppDirName = (isMac || isWindows) ? "Zahlen" : "zahlen";

/// The source tree this binary was built from, or empty when the build carried
/// none (a distribution build can drop ZHLN_PROJECT_ROOT entirely).
[[nodiscard]] inline auto SourceRoot() -> std::filesystem::path {
    if (ProjectRoot.empty()) {
        return {};
    }
    return std::filesystem::path(ProjectRoot);
}

/// True when `candidate` is `dir` or lies under it. Both sides are canonicalized
/// weakly, so components that do not exist yet (a `build/` before the first
/// build) still compare correctly.
[[nodiscard]] inline auto IsInside(const std::filesystem::path& candidate, const std::filesystem::path& dir) -> bool {
    if (candidate.empty() || dir.empty()) {
        return false;
    }
    std::error_code ec;
    const auto      lhs = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
        return false;
    }
    const auto rhs = std::filesystem::weakly_canonical(dir, ec);
    if (ec) {
        return false;
    }
    auto li = lhs.begin();
    for (auto ri = rhs.begin(); ri != rhs.end(); ++ri, ++li) {
        if ((li == lhs.end()) || (*li != *ri)) {
            return false;
        }
    }
    return true;
}

/// The directory the running executable lives in. Empty when the platform
/// query fails (no /proc, no bundle) -- callers fall back to the CWD.
///
/// macOS reports the path of the binary inside the bundle, i.e.
/// `Foo.app/Contents/MacOS` for a `Foo.app` launch; see ResourceDir() for the
/// directory shipped files actually belong in.
[[nodiscard]] inline auto ExecutableDir() -> std::filesystem::path {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0) {
        return {};
    }
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    std::error_code ec;
    const auto      canonical = std::filesystem::weakly_canonical(std::filesystem::path(buffer.c_str()), ec);
    return (ec ? std::filesystem::path(buffer.c_str()) : canonical).parent_path();
#elif defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            break;
        }
        // Truncated: grow and retry (long paths and \\?\ paths).
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path();
#elif defined(__linux__)
    std::string buffer(4096, '\0');
    const auto  written = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (written <= 0) {
        return {};
    }
    buffer.resize(static_cast<size_t>(written));
    std::error_code ec;
    const auto      canonical = std::filesystem::weakly_canonical(std::filesystem::path(buffer), ec);
    return (ec ? std::filesystem::path(buffer) : canonical).parent_path();
#else
    return {};
#endif
}

/// The preferred directory for shipped read-only data: the app bundle's
/// `Contents/Resources` on macOS when the binary runs from inside one, and the
/// executable's own directory everywhere else. `data/base.pak` is expected
/// under it.
///
/// This is a preference, not the only answer -- FindDataFile also probes
/// ExecutableDir() itself, because the build installs the pack with
/// `$<TARGET_FILE_DIR:zahlen>/data/base.pak`, which on macOS is
/// `Contents/MacOS/data/`, not `Contents/Resources/data/`.
[[nodiscard]] inline auto ResourceDir() -> std::filesystem::path {
    const auto exe = ExecutableDir();
    if constexpr (isMac) {
        // Foo.app/Contents/MacOS/Foo -> Foo.app/Contents/Resources. Only when
        // that directory exists: a bare command-line binary next to a MacOS
        // folder is not a bundle.
        if (!exe.empty() && (exe.filename() == "MacOS") && (exe.parent_path().filename() == "Contents")) {
            std::error_code ec;
            const auto      resources = exe.parent_path() / "Resources";
            if (std::filesystem::is_directory(resources, ec) && !ec) {
                return resources;
            }
        }
    }
    return exe;
}

/// True when this process is a developer running from the source tree it was
/// built in -- which is what decides between the historical `build/` locations
/// and the per-user ones.
///
/// Two things qualify, both of them dev-machine facts rather than user intent:
///
///   * the working directory is the source root -- what every CMake target
///     does, since they all run with WORKING_DIRECTORY = the source root;
///   * the executable sits under `<source root>/build` -- the same tree,
///     launched by hand from a terminal or a debugger instead of by CMake.
///
/// A distributed copy satisfies neither, even when the binary still carries
/// ZHLN_PROJECT_ROOT: on the receiving machine the executable is not under that
/// path, so it gets the per-user locations. `equivalent()` needs both paths to
/// exist, which also makes a stale compile-time root fail closed.
[[nodiscard]] inline auto IsDevTree() -> bool {
    const auto root = SourceRoot();
    if (root.empty()) {
        return false;
    }
    std::error_code ec;
    const auto      cwd = std::filesystem::current_path(ec);
    if (!ec && !cwd.empty() && std::filesystem::equivalent(cwd, root, ec) && !ec) {
        return true;
    }
    return IsInside(ExecutableDir(), root / "build");
}

/// The directory for runtime-writable state (pipeline cache, vendor crash
/// dumps). Created on demand by whoever writes into it.
///
///   ZHLN_CACHE_DIR            wins over everything, dev tree or not
///   dev tree                  <source root>/build/cache, next to the build
///                             that produced the binary
///   macOS                     ~/Library/Caches/<app>
///   Linux/BSD                 $XDG_CACHE_HOME/<app>, else ~/.cache/<app>
///   Windows                   %LOCALAPPDATA%\<app>\Cache, else the profile
///   no home, no env           build/cache (the old behaviour, so a bare
///                             container still works)
///
/// The dev-tree answer is absolute, not CWD-relative: it names the build tree
/// that produced this binary, which is the tree a developer wants the cache in
/// no matter where they launched from.
[[nodiscard]] inline auto CacheDir() -> std::filesystem::path {
    if (const char* env = std::getenv("ZHLN_CACHE_DIR"); (env != nullptr) && (*env != '\0')) {
        return std::filesystem::path(env);
    }
    if (IsDevTree()) {
        return SourceRoot() / "build" / "cache";
    }
    const auto env_path = [](const char* name) -> std::optional<std::filesystem::path> {
        if (const char* value = std::getenv(name); (value != nullptr) && (*value != '\0')) {
            return std::filesystem::path(value);
        }
        return std::nullopt;
    };
    if constexpr (isMac) {
        if (auto home = env_path("HOME")) {
            return *home / "Library" / "Caches" / kAppDirName;
        }
    } else if constexpr (isWindows) {
        if (auto local = env_path("LOCALAPPDATA")) {
            return *local / kAppDirName / "Cache";
        }
        if (auto profile = env_path("USERPROFILE")) {
            return *profile / "AppData" / "Local" / kAppDirName / "Cache";
        }
    } else {
        if (auto xdg = env_path("XDG_CACHE_HOME")) {
            return *xdg / kAppDirName;
        }
        if (auto home = env_path("HOME")) {
            return *home / ".cache" / kAppDirName;
        }
    }
    return std::filesystem::path("build") / "cache";
}

/// Where the driver pipeline cache lives. It is a cache: losing it costs
/// first-run compile time, never correctness.
[[nodiscard]] inline auto PipelineCacheFile() -> std::filesystem::path {
    return CacheDir() / "pipeline_cache.bin";
}

/// Where a vendor GPU crash dump is written. Same directory as the cache
/// because it is the one writable per-user location the engine has; the path
/// that was used is logged when the dump is written.
[[nodiscard]] inline auto CrashDumpFile() -> std::filesystem::path {
    return CacheDir() / "gpu_crash_dump.bin";
}

/// Finds a shipped file by relative path, or returns nullopt. Search order:
///
///   1. $ZHLN_DATA_DIR/<relative>          explicit override
///   2. <ResourceDir>/<relative>           Contents/Resources in a macOS bundle
///   3. <ExecutableDir>/<relative>         next to the executable (what the
///                                         build installs, and what a Windows
///                                         or Linux distribution ships)
///   4. <relative>                         the working directory (dev)
///   5. <source root>/build/<relative>, then build/<relative>    (dev)
///
/// 4 and 5 keep their existing order and meaning -- in a dev tree the working
/// directory *is* the source root, so 5 finds the same file it always did -- and
/// 1-3 do not exist in a dev tree, so a developer's lookup resolves to exactly
/// what it resolved to before this header.
[[nodiscard]] inline auto FindDataFile(std::string_view relative) -> std::optional<std::filesystem::path> {
    const auto exists = [](const std::filesystem::path& candidate) -> std::optional<std::filesystem::path> {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return candidate;
        }
        return std::nullopt;
    };
    const auto probe_dir = [&](const std::filesystem::path& dir) -> std::optional<std::filesystem::path> {
        if (dir.empty()) {
            return std::nullopt;
        }
        return exists(dir / relative);
    };

    if (const char* env = std::getenv("ZHLN_DATA_DIR"); (env != nullptr) && (*env != '\0')) {
        if (auto found = probe_dir(env)) {
            return found;
        }
    }
    const auto resources = ResourceDir();
    if (auto found = probe_dir(resources)) {
        return found;
    }
    const auto executable = ExecutableDir();
    if (executable != resources) {
        if (auto found = probe_dir(executable)) {
            return found;
        }
    }
    // The path exactly as asked for, i.e. relative to the working directory --
    // unchanged from what this lookup always was in a dev tree.
    if (auto found = exists(std::filesystem::path(relative))) {
        return found;
    }
    if (auto found = probe_dir(SourceRoot() / "build")) {
        return found;
    }
    return probe_dir("build");
}

} // namespace ZHLN::RuntimePaths
