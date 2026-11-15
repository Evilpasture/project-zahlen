// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/Common.h>
#include <filesystem>
#include <optional>
#include <string_view>

namespace ZHLN::FS::Paths {

[[nodiscard]] auto IsDevTree() -> bool;
[[nodiscard]] ZHLN_API auto CacheDir() -> std::filesystem::path;
[[nodiscard]] auto PipelineCacheFile() -> std::filesystem::path;
[[nodiscard]] auto CrashDumpFile() -> std::filesystem::path;
[[nodiscard]] ZHLN_API auto FindDataFile(std::string_view relative) -> std::optional<std::filesystem::path>;

}
