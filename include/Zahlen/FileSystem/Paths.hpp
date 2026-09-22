// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/FileSystem/Paths.hpp
//
// Where the running process may read and write. Shared between engine and
// offline tools (zcook, tests) via zahlen_filesystem.

#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace ZHLN::FS::Paths {

[[nodiscard]] auto IsDevTree() -> bool;
[[nodiscard]] auto CacheDir() -> std::filesystem::path;
[[nodiscard]] auto PipelineCacheFile() -> std::filesystem::path;
[[nodiscard]] auto CrashDumpFile() -> std::filesystem::path;
[[nodiscard]] auto FindDataFile(std::string_view relative) -> std::optional<std::filesystem::path>;

} // namespace ZHLN::FS::Paths
