// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/RuntimePaths.hpp
//
// Where the running process is allowed to read and write.
//
// The engine answers that with two regimes, decided once per process:
//
//   * a dev tree -- the process is running from the source tree it was built
//     in (IsDevTree()). Everything stays where it always was: `build/cache/...`
//     for the pipeline cache, `build/data/...` for the asset pack.
//   * anywhere else -- a distributed or hand-launched binary. Writable state
//     goes to the per-user cache directory, and shipped data is looked for next
//     to the executable (inside the app bundle's Resources on macOS) before
//     falling back to the working directory.
//
// Both regimes stay overridable -- ZHLN_CACHE_DIR and ZHLN_DATA_DIR, following
// the ZHLN_FONT_PATH precedent in CreativeWorksFactory -- and everything here
// uses the std::error_code overloads, because the library builds with
// -fno-exceptions and a missing $HOME must not be able to terminate.
//
// Engine-internal, deliberately: this is the engine's own policy for where this
// process may read and write, and nothing below decides it. Renderer and RHI
// receive the answers as configuration -- RenderConfig::pipelineCachePath /
// ::crashDumpPath for the renderer, Vk::DiagnosticConfig::crashDumpPath for the
// fault dump -- so nothing else includes this header: it is not in the public
// include surface, not in the umbrella module, and it carries no ZHLN_API.
//
// Declarations only, with the platform queries in RuntimePaths.cpp: they need
// <mach-o/dyld.h>, <unistd.h> or <windows.h>, and no consumer of a path should
// have to pay for that. Only <filesystem> here, because the answers are paths.

#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace ZHLN::RuntimePaths {

/// True when this process is a developer running from the source tree it was
/// built in, which is what decides between the historical `build/` locations
/// and the per-user ones.
///
/// Two things qualify, both dev-machine facts rather than user intent: the
/// working directory is the source root (what every CMake target does, since
/// they run with WORKING_DIRECTORY = the source root), or the executable sits
/// under `<source root>/build` (the same tree, launched by hand instead of by
/// CMake).
///
/// A distributed copy satisfies neither, even when the binary still carries
/// ZHLN_PROJECT_ROOT: on the receiving machine the executable is not under that
/// path, so it gets the per-user locations.
[[nodiscard]] auto IsDevTree() -> bool;

/// The directory for runtime-writable state -- created on demand by whoever
/// writes into it.
///
///   ZHLN_CACHE_DIR            wins over everything, dev tree or not
///   dev tree                  <source root>/build/cache
///   macOS                     ~/Library/Caches/Zahlen
///   Linux/BSD                 $XDG_CACHE_HOME/zahlen, else ~/.cache/zahlen
///   Windows                   %LOCALAPPDATA%\Zahlen\Cache, else the profile
///   no home, no env           build/cache (the old behaviour, so a bare
///                             container still works)
[[nodiscard]] auto CacheDir() -> std::filesystem::path;

/// The driver pipeline cache. It is a cache: losing it costs first-run compile
/// time, never correctness.
[[nodiscard]] auto PipelineCacheFile() -> std::filesystem::path;

/// The vendor GPU crash dump. Same directory as the cache because that is the
/// one writable per-user location the engine has; the path used is logged when
/// the dump is written.
[[nodiscard]] auto CrashDumpFile() -> std::filesystem::path;

/// Finds a shipped file by relative path, or returns nullopt. Search order:
///
///   1. $ZHLN_DATA_DIR/<relative>          explicit override
///   2. <ResourceDir>/<relative>           Contents/Resources in a macOS bundle
///   3. next to the executable             what the build installs (and what a
///                                         Windows or Linux distribution ships)
///   4. <relative>                         the working directory (dev)
///   5. <source root>/build/<relative>, then build/<relative>    (dev)
///
/// 4 and 5 keep their existing order and meaning -- in a dev tree the working
/// directory *is* the source root, so 5 finds the same file it always did -- and
/// 1-3 do not exist in a dev tree, so a dev lookup resolves to exactly what it
/// resolved to before this header.
[[nodiscard]] auto FindDataFile(std::string_view relative) -> std::optional<std::filesystem::path>;

} // namespace ZHLN::RuntimePaths
