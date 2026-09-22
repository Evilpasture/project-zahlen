// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/FileSystem/MappedFile.hpp
//
// Low-level memory-mapped file abstraction. No Vulkan, no Jolt, no ECS.
// Shared between runtime engine and offline tools (zcook) via zahlen_filesystem.

#pragma once

#include <cstddef>

namespace ZHLN::FS {

struct MappedFile {
    void*  data      = nullptr;
    size_t size      = 0;
    void*  osHandle  = nullptr;
    void*  osMapping = nullptr; // Windows only
};

[[nodiscard]] auto OpenMappedFile(const char* path) -> MappedFile;
void CloseMappedFile(MappedFile& file);

} // namespace ZHLN::FS
