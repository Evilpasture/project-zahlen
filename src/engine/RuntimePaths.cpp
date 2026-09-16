// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/RuntimePaths.cpp
//
// Implementation of Zahlen/RuntimePaths.hpp. This is where the platform
// queries live, so that no consumer of the header needs <windows.h>,
// <unistd.h> or <mach-o/dyld.h> to resolve a path.
//
// The engine used to answer "where?" with a path relative to the working
// directory: the pipeline cache at `build/cache/pipeline_cache.bin`, the asset
// pack at `build/data/base.pak`, the vendor crash dump at `gpu_crash_dump.bin`.
// That is correct for exactly one launch -- the one CMake performs, since every
// target runs with WORKING_DIRECTORY set to the source root, where `build/` is
// a real directory -- and wrong for every other. Launched from Finder the
// working directory is `/`, so `create_directories`/`ofstream` fail and no
// cache ever persists (one log line, and every run recompiles every pipeline);
// launched from a folder the user picked, a stray `build/` tree appears there.
//
// So the regime is decided once, from facts about this process rather than
// intent: whether it is running out of the tree that produced it. In that tree
// the historical locations are exactly right and stay untouched -- including
// for out-of-tree builds, since the path is anchored to the source root the
// binary was built from rather than to the current directory. Everywhere else
// writable state goes to the per-user cache directory and shipped data is
// looked for beside the executable.
//
// Platform queries fail for ordinary reasons -- /proc not mounted, no bundle,
// a truncated buffer -- so each one returns something usable instead of
// terminating: the whole file is std::error_code based, matching the library's
// -fno-exceptions, and an unknown location degrades to the relative path the
// engine used before rather than to a crash.

#include <Zahlen/RuntimePaths.hpp>

#include <Zahlen/Config.hpp>
// <windows.h> on Windows, <unistd.h> (readlink) on Unix, macro hygiene for both.
#include <Zahlen/Core/Platform.hpp>

#include <cstdlib>
#include <optional>
#include <string>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace ZHLN::RuntimePaths {
namespace {

/// The application's name under the user's directory. One directory per user
/// per app: nothing here is shared with another game, and removing it is always
/// safe where it holds caches. Capitalized where the host convention is
/// (`~/Library/Caches`, `%LOCALAPPDATA%`), lowercase on the FHS-style XDG path.
constexpr std::string_view kAppDirName = (isMac || isWindows) ? "Zahlen" : "zahlen";

/// The source tree this binary was built from, or empty when the build carried
/// none (a distribution build can drop ZHLN_PROJECT_ROOT entirely).
[[nodiscard]] auto SourceRoot() -> std::filesystem::path {
    if (ProjectRoot.empty()) {
        return {};
    }
    return std::filesystem::path(ProjectRoot);
}

/// A non-empty environment variable as a path, or nullopt.
[[nodiscard]] auto EnvPath(const char* name) -> std::optional<std::filesystem::path> {
    if (const char* value = std::getenv(name); (value != nullptr) && (*value != '\0')) {
        return std::filesystem::path(value);
    }
    return std::nullopt;
}

/// True when `candidate` is `dir` or lies under it. Both sides are canonicalized
/// weakly, so components that do not exist yet (a `build/` before the first
/// build) still compare correctly.
[[nodiscard]] auto IsInside(const std::filesystem::path& candidate, const std::filesystem::path& dir) -> bool {
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

/// The directory the running executable lives in: the platform's own answer,
/// canonicalized so it can be compared and joined with confidence. Empty when
/// the query fails (no /proc, no bundle), which callers treat as "unknown".
///
/// On macOS this is the binary inside the bundle, i.e. `Foo.app/Contents/MacOS`
/// for a `Foo.app` launch; see ResourceDir() for the directory shipped files
/// belong in.
[[nodiscard]] auto ExecutableDir() -> std::filesystem::path {
#if defined(__APPLE__)
    uint32_t probe = 0;
    _NSGetExecutablePath(nullptr, &probe);
    if (probe == 0) {
        return {};
    }
    std::string buffer(probe, '\0');
    if (_NSGetExecutablePath(buffer.data(), &probe) != 0) {
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
/// executable's own directory everywhere else.
///
/// This is a preference, not the only answer -- FindDataFile also probes
/// ExecutableDir() itself, because the build installs the pack with
/// `$<TARGET_FILE_DIR:zahlen>/data/base.pak`, which on macOS is
/// `Contents/MacOS/data/`, not `Contents/Resources/data/`.
[[nodiscard]] auto ResourceDir() -> std::filesystem::path {
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

} // namespace

auto IsDevTree() -> bool {
    const auto root = SourceRoot();
    if (root.empty()) {
        return false;
    }
    std::error_code ec;
    const auto      cwd = std::filesystem::current_path(ec);
    // equivalent() needs both paths to exist, which also makes a stale
    // compile-time root fail closed.
    if (!ec && !cwd.empty() && std::filesystem::equivalent(cwd, root, ec) && !ec) {
        return true;
    }
    return IsInside(ExecutableDir(), root / "build");
}

auto CacheDir() -> std::filesystem::path {
    if (auto override = EnvPath("ZHLN_CACHE_DIR")) {
        return *override;
    }
    if (IsDevTree()) {
        return SourceRoot() / "build" / "cache";
    }
    if constexpr (isMac) {
        if (auto home = EnvPath("HOME")) {
            return *home / "Library" / "Caches" / kAppDirName;
        }
    } else if constexpr (isWindows) {
        if (auto local = EnvPath("LOCALAPPDATA")) {
            return *local / kAppDirName / "Cache";
        }
        if (auto profile = EnvPath("USERPROFILE")) {
            return *profile / "AppData" / "Local" / kAppDirName / "Cache";
        }
    } else {
        if (auto xdg = EnvPath("XDG_CACHE_HOME")) {
            return *xdg / kAppDirName;
        }
        if (auto home = EnvPath("HOME")) {
            return *home / ".cache" / kAppDirName;
        }
    }
    return std::filesystem::path("build") / "cache";
}

auto PipelineCacheFile() -> std::filesystem::path {
    return CacheDir() / "pipeline_cache.bin";
}

auto CrashDumpFile() -> std::filesystem::path {
    return CacheDir() / "gpu_crash_dump.bin";
}

auto FindDataFile(std::string_view relative) -> std::optional<std::filesystem::path> {
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

    if (auto override = EnvPath("ZHLN_DATA_DIR")) {
        if (auto found = probe_dir(*override)) {
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
