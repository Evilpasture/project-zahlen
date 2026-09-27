// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <cstddef>

namespace ZHLN::FS {

struct MappedFile {
    void*  data      = nullptr;
    size_t size      = 0;
    void*  osHandle  = nullptr;
    void*  osMapping = nullptr;
};

[[nodiscard]] auto OpenMappedFile(const char* path) -> MappedFile;
void CloseMappedFile(MappedFile& file);

}
