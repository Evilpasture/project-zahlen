// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/filesystem/MappedFile.cpp
// Extracted from src/engine/Platform.cpp — low-level file mapping belongs to
// zahlen_filesystem, not to window/platform.

#include <Zahlen/FileSystem/MappedFile.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ZHLN::FS {

MappedFile OpenMappedFile(const char* path) {
    MappedFile file;
#if defined(_WIN32)
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return file;

    LARGE_INTEGER size;
    GetFileSizeEx(hFile, &size);
    file.size = static_cast<size_t>(size.QuadPart);

    HANDLE hMapping = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMapping) {
        CloseHandle(hFile);
        return file;
    }

    file.data      = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
    file.osHandle  = hFile;
    file.osMapping = hMapping;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return file;
    }

    struct stat sb {};
    if (fstat(fd, &sb) < 0) {
        close(fd);
        return file;
    }
    file.size = static_cast<size_t>(sb.st_size);

    file.data = mmap(nullptr, file.size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file.data == MAP_FAILED) {
        file.data = nullptr;
    }
    file.osHandle = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
#endif
    return file;
}

void CloseMappedFile(MappedFile& file) {
    if (file.data == nullptr) {
        return;
    }
#if defined(_WIN32)
    UnmapViewOfFile(file.data);
    CloseHandle(file.osMapping);
    CloseHandle(file.osHandle);
#else
    munmap(file.data, file.size);
    close(static_cast<int>(reinterpret_cast<intptr_t>(file.osHandle)));
#endif
    file.data = nullptr;
}

} // namespace ZHLN::FS
