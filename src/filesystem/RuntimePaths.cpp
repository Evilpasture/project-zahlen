// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/filesystem/RuntimePaths.cpp
// Moved from src/engine/RuntimePaths.cpp — low-level path queries belong to
// zahlen_filesystem, not to engine.

#include <Zahlen/FileSystem/Paths.hpp>

#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Platform.hpp>

#include <cstdlib>
#include <optional>
#include <string>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace ZHLN::FS::Paths {
namespace {

constexpr std::string_view kAppDirName = (isMac || isWindows) ? "Zahlen" : "zahlen";

[[nodiscard]] auto SourceRoot() -> std::filesystem::path {
    if (ProjectRoot.empty()) {
        return {};
    }
    return std::filesystem::path(ProjectRoot);
}

[[nodiscard]] auto EnvPath(const char* name) -> std::optional<std::filesystem::path> {
    if (const char* value = std::getenv(name); (value != nullptr) && (*value != '\0')) {
        return std::filesystem::path(value);
    }
    return std::nullopt;
}

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

[[nodiscard]] auto ResourceDir() -> std::filesystem::path {
    const auto exe = ExecutableDir();
    if constexpr (isMac) {
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
    if (auto found = exists(std::filesystem::path(relative))) {
        return found;
    }
    if (auto found = probe_dir(SourceRoot() / "build")) {
        return found;
    }
    return probe_dir("build");
}

} // namespace ZHLN::FS::Paths

// Back-compat: old RuntimePaths namespace forwards to FS::Paths
namespace ZHLN::RuntimePaths {
auto IsDevTree() -> bool { return FS::Paths::IsDevTree(); }
auto CacheDir() -> std::filesystem::path { return FS::Paths::CacheDir(); }
auto PipelineCacheFile() -> std::filesystem::path { return FS::Paths::PipelineCacheFile(); }
auto CrashDumpFile() -> std::filesystem::path { return FS::Paths::CrashDumpFile(); }
auto FindDataFile(std::string_view relative) -> std::optional<std::filesystem::path> { return FS::Paths::FindDataFile(relative); }
} // namespace ZHLN::RuntimePaths
