// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/RuntimePaths.hpp
//
// Where the running process may read and write, in one of two regimes decided once
// per process: a dev tree (IsDevTree()) keeps the historical `build/cache` and
// `build/data` locations; anywhere else writable state goes to the per-user cache
// directory and shipped data is looked for next to the executable (the app bundle's
// Resources on macOS) before the working directory.
//
//   * Both regimes are overridable with ZHLN_CACHE_DIR / ZHLN_DATA_DIR, and
//     everything uses the std::error_code overloads: the library builds with
//     -fno-exceptions, so a missing $HOME must not terminate.
//   * Engine-internal by design -- renderer and RHI receive the answers as
//     configuration (RenderConfig::pipelineCachePath, Vk::DiagnosticConfig::crashDumpPath),
//     so this header is not in the public include surface and carries no ZHLN_API.
//   * Declarations only: the platform queries live in RuntimePaths.cpp, because they
//     need <mach-o/dyld.h>, <unistd.h> or <windows.h>.

#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace ZHLN::RuntimePaths {

// True when this process runs from the source tree it was built in -- what decides
// between the historical `build/` locations and the per-user ones. Two things
// qualify: the working directory is the source root (every CMake target runs with
// WORKING_DIRECTORY = the source root), or the executable sits under
// `<source root>/build`. A distributed copy satisfies neither, even when it still
// carries ZHLN_PROJECT_ROOT.
[[nodiscard]] auto IsDevTree() -> bool;

// The directory for runtime-writable state, created on demand by whoever writes into
// it.
//
//   ZHLN_CACHE_DIR            wins over everything, dev tree or not
//   dev tree                  <source root>/build/cache
//   macOS                     ~/Library/Caches/Zahlen
//   Linux/BSD                 $XDG_CACHE_HOME/zahlen, else ~/.cache/zahlen
//   Windows                   %LOCALAPPDATA%\Zahlen\Cache, else the profile
//   no home, no env           build/cache (the old behaviour, so a bare
//                             container still works)
[[nodiscard]] auto CacheDir() -> std::filesystem::path;

// The driver pipeline cache: losing it costs first-run compile time, never correctness.
[[nodiscard]] auto PipelineCacheFile() -> std::filesystem::path;

// The vendor GPU crash dump. Same directory as the cache, the one writable per-user
// location the engine has; the path is logged when the dump is written.
[[nodiscard]] auto CrashDumpFile() -> std::filesystem::path;

// Finds a shipped file by relative path, or returns nullopt. Search order:
//
//   1. $ZHLN_DATA_DIR/<relative>          explicit override
//   2. <ResourceDir>/<relative>           Contents/Resources in a macOS bundle
//   3. next to the executable             what the build installs (and what a
//                                         Windows or Linux distribution ships)
//   4. <relative>                         the working directory (dev)
//   5. <source root>/build/<relative>, then build/<relative>    (dev)
//
// In a dev tree the working directory *is* the source root and 1-3 do not exist, so
// a dev lookup resolves to what it always did.
[[nodiscard]] auto FindDataFile(std::string_view relative) -> std::optional<std::filesystem::path>;

} // namespace ZHLN::RuntimePaths
